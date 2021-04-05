package benchmarktest

import (
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/asn1"
	"fmt"
	"os"
	"path"
	"testing"

	"github.com/bazelbuild/rules_go/go/tools/bazel"
	"github.com/miekg/pkcs11"
)

var (
	p        *pkcs11.Ctx
	hashOIDs = map[crypto.Hash]asn1.ObjectIdentifier{
		// From https://tools.ietf.org/html/rfc3447#appendix-B.1
		crypto.SHA256: asn1.ObjectIdentifier{2, 16, 840, 1, 101, 3, 4, 2, 1},
		crypto.SHA512: asn1.ObjectIdentifier{2, 16, 840, 1, 101, 3, 4, 2, 3},
	}
)

func init() {
	lib, err := bazel.Runfile("kmsp11/main/libkmsp11.so")
	if err != nil {
		errorString := fmt.Sprintf("error locating KMS PKCS11 .so library: %v", err)
		panic(errorString)
	}
	p = pkcs11.New(lib)
}

const configVar = "KMS_PKCS11_CONFIG"

type testEnv struct {
	tb                     testing.TB
	testDir, logDir        string
	envVarSet, initialized bool
}

func (env *testEnv) Close() {
	if env.initialized {
		if err := p.Finalize(); err != nil {
			env.tb.Errorf("error calling C_Finalize: %v", err)
		}
	}
	if env.envVarSet {
		if err := os.Unsetenv(configVar); err != nil {
			env.tb.Errorf("error unsetting %q: %v", configVar, err)
		}
	}

	if env.tb.Failed() {
		env.tb.Log("attempting to extract library logs to supplmement test failure information")
		entries, err := os.ReadDir(env.logDir)
		if err != nil {
			env.tb.Logf("failed to read from log directory: %v", err)
		}

		for _, entry := range entries {
			fullPath := path.Join(env.logDir, entry.Name())
			fileBytes, err := os.ReadFile(fullPath)
			if err != nil {
				env.tb.Logf("error reading file %q: %v", fullPath, err)
			}
			env.tb.Logf("contents of %q: %s", fullPath, string(fileBytes))
		}
	}

	os.RemoveAll(env.testDir)
}

const configTemplate = `---
kms_endpoint: staging-cloudkms.sandbox.googleapis.com
user_project_override: oss-tools-test
tokens:
  - key_ring: projects/oss-tools-test/locations/us-central1/keyRings/load-test
log_directory: %q
`

func initLibrary(b *testing.B) func() {
	b.Helper()
	env := &testEnv{tb: b}

	var err error
	if env.testDir, err = os.MkdirTemp("", "pkcs11-benchmark"); err != nil {
		b.Fatalf("error creating test directory: %v", err)
	}

	env.logDir = path.Join(env.testDir, "log")
	if err = os.Mkdir(env.logDir, 0755); err != nil {
		env.Close()
		b.Fatalf("error creating log directory: %v", err)
	}

	config := fmt.Sprintf(configTemplate, env.logDir)
	configFile := path.Join(env.testDir, "config.yaml")
	if err = os.WriteFile(configFile, []byte(config), 0644); err != nil {
		env.Close()
		b.Fatalf("error writing config file: %v", err)
	}

	if err = os.Setenv(configVar, configFile); err != nil {
		env.Close()
		b.Fatalf("error setting %q: %v", configVar, err)
	}
	env.envVarSet = true

	if err := p.Initialize(); err != nil {
		env.Close()
		b.Fatalf("error initializing: %v", err)
	}
	env.initialized = true

	return env.Close
}

func newSessionHandle(b *testing.B) (pkcs11.SessionHandle, func()) {
	b.Helper()

	// Slots are always assigned 0-index according to how they exist in YAML config,
	// hardcoding 0 here.
	session, err := p.OpenSession(0, pkcs11.CKF_SERIAL_SESSION)
	if err != nil {
		b.Fatalf("OpenSession: %v", err)
	}

	return session, func() {
		if err := p.CloseSession(session); err != nil {
			b.Fatal(err)
		}
	}
}

func findKey(b *testing.B, session pkcs11.SessionHandle, template []*pkcs11.Attribute) pkcs11.ObjectHandle {
	b.Helper()
	if err := p.FindObjectsInit(session, template); err != nil {
		b.Fatalf("FindObjectsInit: %v", err)
	}
	// Find a maximum of 2 objects, to catch multiple matches and produce a warning.
	obj, _, err := p.FindObjects(session, 2)
	if err != nil {
		b.Fatalf("FindObjects: %v", err)
	}
	if err := p.FindObjectsFinal(session); err != nil {
		b.Fatalf("FindObjectsFinal: %v", err)
	}
	switch len(obj) {
	case 0:
		b.Fatal("findKey: could not find a valid key")
	case 2:
		b.Error("findKey: found multiple matches, returning first match")
	default:
		break
	}

	return obj[0]
}

func getKey(b *testing.B, template []*pkcs11.Attribute) pkcs11.ObjectHandle {
	session, closeSession := newSessionHandle(b)
	defer closeSession()
	key := findKey(b, session, template)

	return key
}

func generateInput(b *testing.B, length int) []byte {
	b.Helper()
	data := make([]byte, 32)
	if _, err := rand.Read(data); err != nil {
		b.Fatalf("failed to generate data: %v", err)
	}

	return data
}

func generateDigestInfo(b *testing.B, algorithm crypto.Hash) []byte {
	b.Helper()

	oid, ok := hashOIDs[algorithm]
	if !ok {
		b.Fatal("generateDigestInfo: invalid DigestAlgorithm")
	}

	digest := generateInput(b, algorithm.Size())

	var digestInfo = struct {
		DigestAlgorithm pkix.AlgorithmIdentifier
		Digest          []byte
	}{
		DigestAlgorithm: pkix.AlgorithmIdentifier{Algorithm: oid},
		Digest:          digest,
	}
	result, err := asn1.Marshal(digestInfo)
	if err != nil {
		b.Fatalf("failed to marshal digestInfo: %v", err)
	}

	return result
}

func BenchmarkECDSA(b *testing.B) {
	finalize := initLibrary(b)
	defer finalize()
	template := []*pkcs11.Attribute{pkcs11.NewAttribute(pkcs11.CKA_CLASS, pkcs11.CKO_PRIVATE_KEY),
		pkcs11.NewAttribute(pkcs11.CKA_LABEL, "load-test-ec")}
	key := getKey(b, template)
	b.RunParallel(func(pb *testing.PB) {
		session, closeSession := newSessionHandle(b)
		defer closeSession()

		for pb.Next() {
			digest := generateInput(b, 32)
			p.SignInit(session, []*pkcs11.Mechanism{pkcs11.NewMechanism(pkcs11.CKM_ECDSA, nil)}, key)
			if _, err := p.Sign(session, digest); err != nil {
				b.Fatalf("failed to sign: %v", err)
			}
		}
	})
}

func BenchmarkRSAPKCS1Sign(b *testing.B) {
	finalize := initLibrary(b)
	defer finalize()
	template := []*pkcs11.Attribute{pkcs11.NewAttribute(pkcs11.CKA_CLASS, pkcs11.CKO_PRIVATE_KEY),
		pkcs11.NewAttribute(pkcs11.CKA_LABEL, "load-test-rsa-pkcs")}
	key := getKey(b, template)
	b.RunParallel(func(pb *testing.PB) {
		session, closeSession := newSessionHandle(b)
		defer closeSession()

		for pb.Next() {
			digest := generateDigestInfo(b, crypto.SHA256)
			p.SignInit(session, []*pkcs11.Mechanism{pkcs11.NewMechanism(pkcs11.CKM_RSA_PKCS, nil)}, key)
			if _, err := p.Sign(session, digest); err != nil {
				b.Fatalf("failed to sign: %v", err)
			}
		}
	})
}

func BenchmarkRSAPSSSign(b *testing.B) {
	finalize := initLibrary(b)
	defer finalize()
	params := pkcs11.NewPSSParams(pkcs11.CKM_SHA256, pkcs11.CKG_MGF1_SHA256, 32)
	template := []*pkcs11.Attribute{pkcs11.NewAttribute(pkcs11.CKA_CLASS, pkcs11.CKO_PRIVATE_KEY),
		pkcs11.NewAttribute(pkcs11.CKA_LABEL, "load-test-rsa-pss")}
	key := getKey(b, template)
	b.RunParallel(func(pb *testing.PB) {
		session, closeSession := newSessionHandle(b)
		defer closeSession()

		for pb.Next() {
			digest := generateInput(b, 32)
			p.SignInit(session, []*pkcs11.Mechanism{pkcs11.NewMechanism(pkcs11.CKM_RSA_PKCS_PSS, params)}, key)
			if _, err := p.Sign(session, digest); err != nil {
				b.Fatalf("failed to sign: %v", err)
			}
		}
	})
}

func BenchmarkRSAOAEPEncrypt(b *testing.B) {
	finalize := initLibrary(b)
	defer finalize()
	params := pkcs11.NewOAEPParams(pkcs11.CKM_SHA256, pkcs11.CKG_MGF1_SHA256, pkcs11.CKZ_DATA_SPECIFIED, nil)
	template := []*pkcs11.Attribute{pkcs11.NewAttribute(pkcs11.CKA_CLASS, pkcs11.CKO_PUBLIC_KEY),
		pkcs11.NewAttribute(pkcs11.CKA_LABEL, "load-test-rsa-oaep")}
	key := getKey(b, template)
	b.RunParallel(func(pb *testing.PB) {
		session, closeSession := newSessionHandle(b)
		defer closeSession()

		for pb.Next() {
			message := generateInput(b, 190)
			p.EncryptInit(session, []*pkcs11.Mechanism{pkcs11.NewMechanism(pkcs11.CKM_RSA_PKCS_OAEP, params)}, key)
			if _, err := p.Encrypt(session, message); err != nil {
				b.Fatalf("failed to encrypt: %v", err)
			}
		}
	})
}

func BenchmarkRSAOAEPDecrypt(b *testing.B) {
	finalize := initLibrary(b)
	defer finalize()

	params := pkcs11.NewOAEPParams(pkcs11.CKM_SHA256, pkcs11.CKG_MGF1_SHA256, pkcs11.CKZ_DATA_SPECIFIED, nil)
	template := []*pkcs11.Attribute{pkcs11.NewAttribute(pkcs11.CKA_CLASS, pkcs11.CKO_PUBLIC_KEY),
		pkcs11.NewAttribute(pkcs11.CKA_LABEL, "load-test-rsa-oaep")}
	setupSession, closeSession := newSessionHandle(b)
	key := findKey(b, setupSession, template)
	attr, err := p.GetAttributeValue(setupSession, key, []*pkcs11.Attribute{pkcs11.NewAttribute(pkcs11.CKA_PUBLIC_KEY_INFO, nil)})
	if err != nil {
		b.Fatalf("err %s\n", err)
	}
	closeSession()

	message := generateInput(b, 190)
	pub, err := x509.ParsePKIXPublicKey(attr[0].Value)
	if err != nil {
		b.Fatalf("failed to parse RSA encoded public key: %v", err)
	}
	rsaKey, ok := pub.(*rsa.PublicKey)
	if !ok {
		b.Fatalf("provided key of type '%T' is not a *rsa.PublicKey", pub)
	}

	ciphertext, err := rsa.EncryptOAEP(crypto.SHA256.New(), rand.Reader, rsaKey, message, nil)
	if err != nil {
		b.Fatalf("failed to encrypt: %v", err)
	}

	template = []*pkcs11.Attribute{pkcs11.NewAttribute(pkcs11.CKA_CLASS, pkcs11.CKO_PRIVATE_KEY),
		pkcs11.NewAttribute(pkcs11.CKA_LABEL, "load-test-rsa-oaep")}
	key = getKey(b, template)
	b.RunParallel(func(pb *testing.PB) {
		session, closeSession := newSessionHandle(b)
		defer closeSession()

		for pb.Next() {
			p.DecryptInit(session, []*pkcs11.Mechanism{pkcs11.NewMechanism(pkcs11.CKM_RSA_PKCS_OAEP, params)}, key)
			if _, err := p.Decrypt(session, ciphertext); err != nil {
				b.Fatalf("failed to decrypt: %v", err)
			}
		}
	})
}
