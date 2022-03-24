// Copyright 2021 Google LLC
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

#include "kmsp11/operation/kms_digesting_signer.h"

#include "fakekms/cpp/fakekms.h"
#include "kmsp11/object.h"
#include "kmsp11/operation/rsassa_pkcs1.h"
#include "kmsp11/operation/rsassa_pss.h"
#include "kmsp11/test/resource_helpers.h"
#include "kmsp11/test/test_status_macros.h"
#include "kmsp11/util/crypto_utils.h"

namespace kmsp11 {
namespace {

using ::testing::AllOf;

TEST(NewSignerTest, ParamInvalid) {
  ASSERT_OK_AND_ASSIGN(
      KeyPair kp, NewMockKeyPair(kms_v1::CryptoKeyVersion::EC_SIGN_P256_SHA256,
                                 "ec_p256_public.pem"));
  std::shared_ptr<Object> key = std::make_shared<Object>(kp.private_key);

  char buf[1];
  CK_MECHANISM mechanism{CKM_ECDSA_SHA256, buf, sizeof(buf)};
  EXPECT_THAT(KmsDigestingSigner::New(key, &mechanism),
              StatusRvIs(CKR_MECHANISM_PARAM_INVALID));
}

TEST(NewSignerTest, FailureWrongKeyType) {
  ASSERT_OK_AND_ASSIGN(
      KeyPair kp,
      NewMockKeyPair(kms_v1::CryptoKeyVersion::RSA_DECRYPT_OAEP_2048_SHA256,
                     "rsa_2048_public.pem"));
  std::shared_ptr<Object> key = std::make_shared<Object>(kp.private_key);

  CK_MECHANISM mechanism{CKM_ECDSA_SHA256, nullptr, 0};
  EXPECT_THAT(KmsDigestingSigner::New(key, &mechanism),
              StatusRvIs(CKR_KEY_TYPE_INCONSISTENT));
}

TEST(NewSignerTest, FailureWrongObjectClass) {
  ASSERT_OK_AND_ASSIGN(
      KeyPair kp, NewMockKeyPair(kms_v1::CryptoKeyVersion::EC_SIGN_P256_SHA256,
                                 "ec_p256_public.pem"));
  std::shared_ptr<Object> key = std::make_shared<Object>(kp.public_key);

  CK_MECHANISM mechanism{CKM_ECDSA_SHA256, nullptr, 0};
  EXPECT_THAT(KmsDigestingSigner::New(key, &mechanism),
              StatusRvIs(CKR_KEY_FUNCTION_NOT_PERMITTED));
}

TEST(NewSignerTest, FailureWrongMechanism) {
  ASSERT_OK_AND_ASSIGN(
      KeyPair kp, NewMockKeyPair(kms_v1::CryptoKeyVersion::EC_SIGN_P256_SHA256,
                                 "ec_p256_public.pem"));
  std::shared_ptr<Object> key = std::make_shared<Object>(kp.public_key);

  CK_MECHANISM mechanism{CKM_AES_MAC, nullptr, 0};
  EXPECT_THAT(KmsDigestingSigner::New(key, &mechanism),
              StatusRvIs(CKR_GENERAL_ERROR));
}

class KmsDigestingSignerTest : public testing::Test {
 protected:
  void SetUp(google::cloud::kms::v1::CryptoKeyVersion::CryptoKeyVersionAlgorithm
                 algorithm) {
    ASSERT_OK_AND_ASSIGN(fake_server_, fakekms::Server::New());
    client_ = std::make_unique<KmsClient>(fake_server_->listen_addr(),
                                          grpc::InsecureChannelCredentials(),
                                          absl::Seconds(1));

    auto fake_client = fake_server_->NewClient();

    kms_v1::KeyRing kr;
    kr = CreateKeyRingOrDie(fake_client.get(), kTestLocation, RandomId(), kr);

    kms_v1::CryptoKey ck;
    ck.set_purpose(kms_v1::CryptoKey::ASYMMETRIC_SIGN);
    ck.mutable_version_template()->set_algorithm(algorithm);
    ck = CreateCryptoKeyOrDie(fake_client.get(), kr.name(), "ck", ck, true);

    kms_v1::CryptoKeyVersion ckv;
    ckv = CreateCryptoKeyVersionOrDie(fake_client.get(), ck.name(), ckv);
    ckv = WaitForEnablement(fake_client.get(), ckv);

    kms_key_name_ = ckv.name();

    kms_v1::PublicKey pub_proto = GetPublicKey(fake_client.get(), ckv);
    ASSERT_OK_AND_ASSIGN(public_key_, ParseX509PublicKeyPem(pub_proto.pem()));

    ASSERT_OK_AND_ASSIGN(KeyPair kp,
                         Object::NewKeyPair(ckv, public_key_.get()));
    pub_ = std::make_shared<Object>(kp.public_key);
    prv_ = std::make_shared<Object>(kp.private_key);
  }

  std::unique_ptr<fakekms::Server> fake_server_;
  std::unique_ptr<KmsClient> client_;
  std::string kms_key_name_;
  bssl::UniquePtr<EVP_PKEY> public_key_;
  std::shared_ptr<Object> pub_, prv_;
};

TEST_F(KmsDigestingSignerTest, SignSuccess) {
  SetUp(kms_v1::CryptoKeyVersion::EC_SIGN_P256_SHA256);
  std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};

  CK_MECHANISM mech{CKM_ECDSA_SHA256, nullptr, 0};
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SignerInterface> signer,
                       KmsDigestingSigner::New(prv_, &mech));
  std::vector<uint8_t> sig(signer->signature_length());
  EXPECT_OK(signer->Sign(client_.get(), data, absl::MakeSpan(sig)));

  uint8_t digest[32];
  SHA256(data.data(), data.size(), digest);
  EXPECT_OK(EcdsaVerifyP1363(EVP_PKEY_get0_EC_KEY(public_key_.get()),
                             EVP_sha256(), digest, sig));
}

TEST_F(KmsDigestingSignerTest, SignSignatureLengthInvalid) {
  SetUp(kms_v1::CryptoKeyVersion::EC_SIGN_P256_SHA256);
  uint8_t data[123], sig[97];

  CK_MECHANISM mech{CKM_ECDSA_SHA256, nullptr, 0};
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SignerInterface> signer,
                       KmsDigestingSigner::New(prv_, &mech));

  EXPECT_THAT(signer->Sign(client_.get(), data, absl::MakeSpan(sig)),
              AllOf(StatusIs(absl::StatusCode::kInternal),
                    StatusRvIs(CKR_GENERAL_ERROR)));
}

TEST_F(KmsDigestingSignerTest, SignMultiPartSuccess) {
  SetUp(kms_v1::CryptoKeyVersion::EC_SIGN_P256_SHA256);
  std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
  std::vector<uint8_t> data_part1 = {0xDE, 0xAD};
  std::vector<uint8_t> data_part2 = {0xBE, 0xEF};

  CK_MECHANISM mech{CKM_ECDSA_SHA256, nullptr, 0};
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SignerInterface> signer,
                       KmsDigestingSigner::New(prv_, &mech));
  std::vector<uint8_t> sig(signer->signature_length());
  EXPECT_OK(signer->SignUpdate(client_.get(), data_part1));
  EXPECT_OK(signer->SignUpdate(client_.get(), data_part2));
  EXPECT_OK(signer->SignFinal(client_.get(), absl::MakeSpan(sig)));

  uint8_t digest[32];
  SHA256(data.data(), data.size(), digest);
  EXPECT_OK(EcdsaVerifyP1363(EVP_PKEY_get0_EC_KEY(public_key_.get()),
                             EVP_sha256(), digest, sig));
}

TEST_F(KmsDigestingSignerTest, SignFinalWithoutUpdateFails) {
  SetUp(kms_v1::CryptoKeyVersion::EC_SIGN_P256_SHA256);
  std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};

  CK_MECHANISM mech{CKM_ECDSA_SHA256, nullptr, 0};
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SignerInterface> signer,
                       KmsDigestingSigner::New(prv_, &mech));
  std::vector<uint8_t> sig(signer->signature_length());
  EXPECT_THAT(signer->SignFinal(client_.get(), absl::MakeSpan(sig)),
              AllOf(StatusIs(absl::StatusCode::kFailedPrecondition),
                    StatusRvIs(CKR_FUNCTION_FAILED)));
}

TEST_F(KmsDigestingSignerTest, SignSinglePartAfterUpdateFails) {
  SetUp(kms_v1::CryptoKeyVersion::EC_SIGN_P256_SHA256);
  std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};

  CK_MECHANISM mech{CKM_ECDSA_SHA256, nullptr, 0};
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SignerInterface> signer,
                       KmsDigestingSigner::New(prv_, &mech));
  std::vector<uint8_t> sig(signer->signature_length());
  EXPECT_OK(signer->SignUpdate(client_.get(), data));
  EXPECT_THAT(signer->Sign(client_.get(), data, absl::MakeSpan(sig)),
              AllOf(StatusIs(absl::StatusCode::kFailedPrecondition),
                    StatusRvIs(CKR_FUNCTION_FAILED)));
}

TEST_F(KmsDigestingSignerTest, RsaPkcs1SignSuccess) {
  SetUp(kms_v1::CryptoKeyVersion::RSA_SIGN_PKCS1_2048_SHA256);
  std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};

  CK_MECHANISM mech{CKM_SHA256_RSA_PKCS, nullptr, 0};
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SignerInterface> signer,
                       KmsDigestingSigner::New(prv_, &mech));
  std::vector<uint8_t> sig(signer->signature_length());
  EXPECT_OK(signer->Sign(client_.get(), data, absl::MakeSpan(sig)));

  uint8_t digest[32];
  std::vector<uint8_t> prehashed_sig(signer->signature_length());
  SHA256(data.data(), data.size(), digest);
  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> digest_info,
                       BuildRsaDigestInfo(NID_sha256, digest));
  CK_MECHANISM inner_mech{CKM_RSA_PKCS, nullptr, 0};
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SignerInterface> inner_signer,
                       RsaPkcs1Signer::New(prv_, &inner_mech));
  EXPECT_OK(inner_signer->Sign(client_.get(), digest_info,
                               absl::MakeSpan(prehashed_sig)));
  EXPECT_EQ(prehashed_sig, sig);
}

TEST_F(KmsDigestingSignerTest, RsaPssSignSuccess) {
  SetUp(kms_v1::CryptoKeyVersion::RSA_SIGN_PSS_2048_SHA256);
  std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
  CK_RSA_PKCS_PSS_PARAMS params{CKM_SHA256, CKG_MGF1_SHA256, 32};
  CK_MECHANISM mech{CKM_SHA256_RSA_PKCS_PSS, &params, sizeof(params)};

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SignerInterface> signer,
                       KmsDigestingSigner::New(prv_, &mech));
  std::vector<uint8_t> sig(signer->signature_length());
  EXPECT_OK(signer->Sign(client_.get(), data, absl::MakeSpan(sig)));

  uint8_t digest[32];
  SHA256(data.data(), data.size(), digest);
  EXPECT_OK(RsaVerifyPss(public_key_.get(), EVP_sha256(), digest, sig));
}

}  // namespace
}  // namespace kmsp11