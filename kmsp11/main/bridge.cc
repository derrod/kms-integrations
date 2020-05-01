#include "absl/status/status.h"
#include "absl/types/optional.h"
#include "kmsp11/config/config.h"
#include "kmsp11/cryptoki.h"
#include "kmsp11/main/function_list.h"
#include "kmsp11/provider.h"
#include "kmsp11/util/errors.h"
#include "kmsp11/util/status_macros.h"

namespace kmsp11 {

static CK_FUNCTION_LIST function_list = NewFunctionList();
static absl::optional<Provider> provider;

StatusOr<Provider*> GetProvider() {
  if (!provider.has_value()) {
    return NotInitializedError(SOURCE_LOCATION);
  }
  return &provider.value();
}

// Initialize the library.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002322
absl::Status Initialize(CK_VOID_PTR pInitArgs) {
  if (provider.has_value()) {
    return NewError(absl::StatusCode::kFailedPrecondition,
                    "the library is already initialized",
                    CKR_CRYPTOKI_ALREADY_INITIALIZED, SOURCE_LOCATION);
  }

  auto* init_args = static_cast<CK_C_INITIALIZE_ARGS*>(pInitArgs);

  LibraryConfig config;
  if (init_args && init_args->pReserved) {
    // This behavior isn't part of the spec, but there are numerous libraries
    // in the wild that allow specifying a config file in pInitArgs->pReserved.
    // There's also support for providing config this way in the OpenSSL engine:
    // https://github.com/OpenSC/libp11/blob/4084f83ee5ea51353facf151126b7d6d739d0784/src/eng_front.c#L62
    ASSIGN_OR_RETURN(
        config, LoadConfigFromFile(static_cast<char*>(init_args->pReserved)));
  } else {
    ASSIGN_OR_RETURN(config, LoadConfigFromEnvironment());
  }

  ASSIGN_OR_RETURN(provider, Provider::New(config));
  return absl::OkStatus();
}

// Shut down the library.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc383864872
absl::Status Finalize(CK_VOID_PTR reserved) {
  if (!provider.has_value()) {
    return NotInitializedError(SOURCE_LOCATION);
  }
  provider = absl::nullopt;
  return absl::OkStatus();
}

// Get basic information about the library.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002324
absl::Status GetInfo(CK_INFO_PTR info) {
  ASSIGN_OR_RETURN(Provider * provider, GetProvider());
  *info = provider->info();
  return absl::OkStatus();
}

// Get pointers to the functions exposed in this library.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc319313512
absl::Status GetFunctionList(CK_FUNCTION_LIST_PTR_PTR ppFunctionList) {
  // Note that GetFunctionList is the only Cryptoki function that may be called
  // before the library is initialized.
  *ppFunctionList = &function_list;
  return absl::OkStatus();
}

}  // namespace kmsp11
