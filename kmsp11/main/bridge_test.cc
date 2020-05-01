#include "kmsp11/main/bridge.h"

#include <fstream>

#include "gmock/gmock.h"
#include "kmsp11/config/config.h"
#include "kmsp11/test/matchers.h"
#include "kmsp11/test/test_status_macros.h"
#include "kmsp11/util/cleanup.h"
#include "kmsp11/util/platform.h"

namespace kmsp11 {
namespace {

class BridgeTest : public testing::Test {
 protected:
  BridgeTest() : config_file_(std::tmpnam(nullptr)), init_args_({0}) {
    std::ofstream(config_file_) << "---";
    init_args_.pReserved = const_cast<char*>(config_file_.c_str());
  }
  ~BridgeTest() { std::remove(config_file_.c_str()); }

  std::string config_file_;
  CK_C_INITIALIZE_ARGS init_args_;
};

TEST_F(BridgeTest, InitializeFromArgs) {
  EXPECT_OK(Initialize(&init_args_));
  EXPECT_OK(Finalize(nullptr));
}

TEST_F(BridgeTest, InitializeFailsOnSecondCall) {
  EXPECT_OK(Initialize(&init_args_));
  EXPECT_THAT(Initialize(&init_args_),
              StatusRvIs(CKR_CRYPTOKI_ALREADY_INITIALIZED));
  // Finalize so that other tests see an uninitialized state
  EXPECT_OK(Finalize(nullptr));
}

TEST_F(BridgeTest, InitializeFromEnvironment) {
  SetEnvVariable(kConfigEnvVariable, config_file_);
  Cleanup c([]() { ClearEnvVariable(kConfigEnvVariable); });

  EXPECT_OK(Initialize(nullptr));
  // Finalize so that other tests see an uninitialized state
  EXPECT_OK(Finalize(nullptr));
}

TEST_F(BridgeTest, InitArgsWithoutReservedLoadsFromEnv) {
  SetEnvVariable(kConfigEnvVariable, config_file_);
  Cleanup c([]() { ClearEnvVariable(kConfigEnvVariable); });

  CK_C_INITIALIZE_ARGS init_args = {0};
  EXPECT_OK(Initialize(&init_args));
  // Finalize so that other tests see an uninitialized state
  EXPECT_OK(Finalize(nullptr));
}

TEST_F(BridgeTest, InitializeFailsWithoutConfig) {
  EXPECT_THAT(Initialize(nullptr),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(BridgeTest, InitializeFailsWithArgsNoConfig) {
  CK_C_INITIALIZE_ARGS init_args = {0};
  EXPECT_THAT(Initialize(&init_args),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(BridgeTest, FinalizeFailsWithoutInitialize) {
  EXPECT_THAT(Finalize(nullptr), StatusRvIs(CKR_CRYPTOKI_NOT_INITIALIZED));
}

TEST_F(BridgeTest, GetInfoSuccess) {
  EXPECT_OK(Initialize(&init_args_));
  CK_INFO info;
  EXPECT_OK(GetInfo(&info));
  EXPECT_OK(Finalize(nullptr));
}

TEST_F(BridgeTest, GetInfoFailsWithoutInitialize) {
  EXPECT_THAT(GetInfo(nullptr), StatusRvIs(CKR_CRYPTOKI_NOT_INITIALIZED));
}

TEST_F(BridgeTest, GetFunctionListSuccess) {
  CK_FUNCTION_LIST* function_list;
  EXPECT_OK(GetFunctionList(&function_list));
}

TEST_F(BridgeTest, FunctionListValidPointers) {
  CK_FUNCTION_LIST* f;
  EXPECT_OK(GetFunctionList(&f));
  EXPECT_EQ(f->C_Initialize(&init_args_), CKR_OK);
  CK_INFO info;
  EXPECT_EQ(f->C_GetInfo(&info), CKR_OK);
  EXPECT_EQ(f->C_Finalize(nullptr), CKR_OK);
}

}  // namespace
}  // namespace kmsp11
