// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package contract

import (
	"context"
	"hash/crc32"
	"testing"

	"github.com/google/go-cmp/cmp"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/testing/protocmp"
	"google.golang.org/protobuf/types/known/wrapperspb"

	kmspb "google.golang.org/genproto/googleapis/cloud/kms/v1"
	fmpb "google.golang.org/genproto/protobuf/field_mask"
)

var ignoreCiphertextAndIVAndTagLength = protocmp.IgnoreFields(new(kmspb.RawEncryptResponse),
	protoreflect.Name("ciphertext"), protoreflect.Name("ciphertext_crc32c"), protoreflect.Name("initialization_vector"), protoreflect.Name("initialization_vector_crc32c"))

func TestRawEncryptDecrypt(t *testing.T) {
	ctx := context.Background()
	kr := client.CreateTestKR(ctx, t, &kmspb.CreateKeyRingRequest{Parent: location})
	ck := client.CreateTestCK(ctx, t, &kmspb.CreateCryptoKeyRequest{
		Parent: kr.Name,
		CryptoKey: &kmspb.CryptoKey{
			Purpose: kmspb.CryptoKey_RAW_ENCRYPT_DECRYPT,
			VersionTemplate: &kmspb.CryptoKeyVersionTemplate{
				ProtectionLevel: kmspb.ProtectionLevel_HSM,
				Algorithm:       kmspb.CryptoKeyVersion_AES_256_GCM,
			},
		},
		SkipInitialVersionCreation: true,
	})
	ckv := client.CreateTestCKVAndWait(ctx, t, &kmspb.CreateCryptoKeyVersionRequest{
		Parent: ck.Name,
	})

	plaintext := []byte("Here is some data to encrypt")
	aad := []byte("aad")

	gotEncrypt, err := client.RawEncrypt(ctx, &kmspb.RawEncryptRequest{
		Name:                        ckv.Name,
		Plaintext:                   plaintext,
		AdditionalAuthenticatedData: aad,
	})
	if err != nil {
		t.Fatal(err)
	}

	wantEncrypt := &kmspb.RawEncryptResponse{
		Name:            ckv.Name,
		ProtectionLevel: kmspb.ProtectionLevel_HSM,
		TagLength:       16,
	}

	opts := append(ProtoDiffOpts(), ignoreCiphertextAndIVAndTagLength)
	if diff := cmp.Diff(wantEncrypt, gotEncrypt, opts...); diff != "" {
		t.Errorf("proto mismatch (-want +got): %s", diff)
	}

	gotDecrypt, err := client.RawDecrypt(ctx, &kmspb.RawDecryptRequest{
		Name:                        ckv.Name,
		Ciphertext:                  gotEncrypt.Ciphertext,
		AdditionalAuthenticatedData: aad,
		InitializationVector:        gotEncrypt.InitializationVector,
	})
	if err != nil {
		t.Fatal(err)
	}

	wantDecrypt := &kmspb.RawDecryptResponse{
		Plaintext:       plaintext,
		PlaintextCrc32C: wrapperspb.Int64(int64(crc32.Checksum(plaintext, crc32CTable))),
		ProtectionLevel: kmspb.ProtectionLevel_HSM,
	}

	if diff := cmp.Diff(wantDecrypt, gotDecrypt, ProtoDiffOpts()...); diff != "" {
		t.Errorf("proto mismatch (-want +got): %s", diff)
	}
}

func TestRawEncryptNotFound(t *testing.T) {
	ctx := context.Background()

	_, err := client.RawEncrypt(ctx, &kmspb.RawEncryptRequest{
		Name: location + "/keyRings/foo/cryptoKeys/bar/cryptoKeyVersions/1",
	})
	if status.Code(err) != codes.NotFound {
		t.Errorf("err=%v, want code=%s", err, codes.NotFound)
	}
}

func TestRawDecryptNotFound(t *testing.T) {
	ctx := context.Background()

	_, err := client.RawDecrypt(ctx, &kmspb.RawDecryptRequest{
		Name:                        location + "/keyRings/foo/cryptoKeys/bar/cryptoKeyVersions/1",
		Ciphertext:                  []byte("testdata"),
		AdditionalAuthenticatedData: []byte("aad"),
		InitializationVector:        []byte("iv"),
	})
	if status.Code(err) != codes.NotFound {
		t.Errorf("err=%v, want code=%s", err, codes.NotFound)
	}
}

func TestRawEncryptWrongPurpose(t *testing.T) {
	ctx := context.Background()
	kr := client.CreateTestKR(ctx, t, &kmspb.CreateKeyRingRequest{Parent: location})
	ck := client.CreateTestCK(ctx, t, &kmspb.CreateCryptoKeyRequest{
		Parent: kr.Name,
		CryptoKey: &kmspb.CryptoKey{
			Purpose: kmspb.CryptoKey_ENCRYPT_DECRYPT,
		},
	})

	_, err := client.RawEncrypt(ctx, &kmspb.RawEncryptRequest{
		Name:                        ck.Primary.Name,
		Plaintext:                   []byte("testdata"),
		AdditionalAuthenticatedData: []byte("aad"),
	})
	if status.Code(err) != codes.FailedPrecondition {
		t.Errorf("err=%v, want code=%s", err, codes.FailedPrecondition)
	}
}

func TestRawDecryptWrongPurpose(t *testing.T) {
	ctx := context.Background()
	kr := client.CreateTestKR(ctx, t, &kmspb.CreateKeyRingRequest{Parent: location})
	ck := client.CreateTestCK(ctx, t, &kmspb.CreateCryptoKeyRequest{
		Parent: kr.Name,
		CryptoKey: &kmspb.CryptoKey{
			Purpose: kmspb.CryptoKey_ENCRYPT_DECRYPT,
		},
	})

	_, err := client.RawDecrypt(ctx, &kmspb.RawDecryptRequest{
		Name:                        ck.Primary.Name,
		Ciphertext:                  []byte("testdata"),
		AdditionalAuthenticatedData: []byte("aad"),
		InitializationVector:        []byte("iv"),
	})
	if status.Code(err) != codes.FailedPrecondition {
		t.Errorf("err=%v, want code=%s", err, codes.FailedPrecondition)
	}
}

func TestRawEncryptKeyDisabled(t *testing.T) {
	ctx := context.Background()
	kr := client.CreateTestKR(ctx, t, &kmspb.CreateKeyRingRequest{Parent: location})
	ck := client.CreateTestCK(ctx, t, &kmspb.CreateCryptoKeyRequest{
		Parent: kr.Name,
		CryptoKey: &kmspb.CryptoKey{
			Purpose: kmspb.CryptoKey_RAW_ENCRYPT_DECRYPT,
			VersionTemplate: &kmspb.CryptoKeyVersionTemplate{
				Algorithm: kmspb.CryptoKeyVersion_AES_256_GCM,
			},
		},
		SkipInitialVersionCreation: true,
	})
	ckv := client.CreateTestCKVAndWait(ctx, t, &kmspb.CreateCryptoKeyVersionRequest{
		Parent: ck.Name,
	})

	ckv.State = kmspb.CryptoKeyVersion_DISABLED

	_, err := client.UpdateCryptoKeyVersion(ctx, &kmspb.UpdateCryptoKeyVersionRequest{
		CryptoKeyVersion: ckv,
		UpdateMask:       &fmpb.FieldMask{Paths: []string{"state"}},
	})
	if err != nil {
		t.Fatal(err)
	}

	_, err = client.RawEncrypt(ctx, &kmspb.RawEncryptRequest{
		Name: ckv.Name,
	})
	if status.Code(err) != codes.FailedPrecondition {
		t.Errorf("err=%v, want code=%s", err, codes.FailedPrecondition)
	}
}

func TestRawDecryptKeyDisabled(t *testing.T) {
	ctx := context.Background()
	kr := client.CreateTestKR(ctx, t, &kmspb.CreateKeyRingRequest{Parent: location})
	ck := client.CreateTestCK(ctx, t, &kmspb.CreateCryptoKeyRequest{
		Parent: kr.Name,
		CryptoKey: &kmspb.CryptoKey{
			Purpose: kmspb.CryptoKey_RAW_ENCRYPT_DECRYPT,
			VersionTemplate: &kmspb.CryptoKeyVersionTemplate{
				Algorithm: kmspb.CryptoKeyVersion_AES_256_GCM,
			},
		},
		SkipInitialVersionCreation: true,
	})
	ckv := client.CreateTestCKVAndWait(ctx, t, &kmspb.CreateCryptoKeyVersionRequest{
		Parent: ck.Name,
	})

	ckv.State = kmspb.CryptoKeyVersion_DISABLED

	_, err := client.UpdateCryptoKeyVersion(ctx, &kmspb.UpdateCryptoKeyVersionRequest{
		CryptoKeyVersion: ckv,
		UpdateMask:       &fmpb.FieldMask{Paths: []string{"state"}},
	})
	if err != nil {
		t.Fatal(err)
	}

	_, err = client.RawDecrypt(ctx, &kmspb.RawDecryptRequest{
		Name:                        ckv.Name,
		Ciphertext:                  []byte("testdata"),
		AdditionalAuthenticatedData: []byte("aad"),
		InitializationVector:        []byte("iv"),
	})
	if status.Code(err) != codes.FailedPrecondition {
		t.Errorf("err=%v, want code=%s", err, codes.FailedPrecondition)
	}
}