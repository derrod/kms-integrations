#include "absl/status/status.h"
#include "absl/types/optional.h"
#include "kmsp11/config/config.h"
#include "kmsp11/cryptoki.h"
#include "kmsp11/main/function_list.h"
#include "kmsp11/mechanism.h"
#include "kmsp11/provider.h"
#include "kmsp11/util/errors.h"
#include "kmsp11/util/status_macros.h"

namespace kmsp11 {

static CK_FUNCTION_LIST function_list = NewFunctionList();
static std::unique_ptr<Provider> provider;

StatusOr<Provider*> GetProvider() {
  if (!provider) {
    return NotInitializedError(SOURCE_LOCATION);
  }
  return provider.get();
}

StatusOr<Token*> GetToken(CK_SLOT_ID slot_id) {
  ASSIGN_OR_RETURN(Provider * provider, GetProvider());
  return provider->TokenAt(slot_id);
}

StatusOr<std::shared_ptr<Session>> GetSession(
    CK_SESSION_HANDLE session_handle) {
  ASSIGN_OR_RETURN(Provider * provider, GetProvider());
  return provider->GetSession(session_handle);
}

// Initialize the library.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002322
absl::Status Initialize(CK_VOID_PTR pInitArgs) {
  if (provider) {
    return FailedPreconditionError("the library is already initialized",
                                   CKR_CRYPTOKI_ALREADY_INITIALIZED,
                                   SOURCE_LOCATION);
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
absl::Status Finalize(CK_VOID_PTR pReserved) {
  if (!provider) {
    return NotInitializedError(SOURCE_LOCATION);
  }
  provider = nullptr;
  return absl::OkStatus();
}

// Get basic information about the library.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002324
absl::Status GetInfo(CK_INFO_PTR pInfo) {
  ASSIGN_OR_RETURN(const Provider* provider, GetProvider());
  if (!pInfo) {
    return NullArgumentError("pInfo", SOURCE_LOCATION);
  }

  *pInfo = provider->info();
  return absl::OkStatus();
}

// Get pointers to the functions exposed in this library.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc319313512
absl::Status GetFunctionList(CK_FUNCTION_LIST_PTR_PTR ppFunctionList) {
  // Note that GetFunctionList is the only Cryptoki function that may be called
  // before the library is initialized.
  if (!ppFunctionList) {
    return NullArgumentError("ppFunctionList", SOURCE_LOCATION);
  }
  *ppFunctionList = &function_list;
  return absl::OkStatus();
}

// Get the list of slots in this library.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002327
// Note that tokenPresent is always ignored in our library, since we do not have
// removable tokens.
absl::Status GetSlotList(CK_BBOOL tokenPresent, CK_SLOT_ID_PTR pSlotList,
                         CK_ULONG_PTR pulCount) {
  ASSIGN_OR_RETURN(const Provider* provider, GetProvider());
  if (!pulCount) {
    return NullArgumentError("pulCount", SOURCE_LOCATION);
  }

  if (!pSlotList) {
    *pulCount = provider->token_count();
    return absl::OkStatus();
  }

  if (*pulCount < provider->token_count()) {
    absl::Status result =
        OutOfRangeError(absl::StrFormat("*pulCount=%d but there are %d tokens",
                                        *pulCount, provider->token_count()),
                        SOURCE_LOCATION);
    *pulCount = provider->token_count();
    return result;
  }

  for (int i = 0; i < provider->token_count(); i++) {
    pSlotList[i] = i;
  }
  *pulCount = provider->token_count();
  return absl::OkStatus();
}

// Get information about a slot in the system.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002328
absl::Status GetSlotInfo(CK_SLOT_ID slotID, CK_SLOT_INFO_PTR pInfo) {
  ASSIGN_OR_RETURN(const Token* token, GetToken(slotID));
  if (!pInfo) {
    return NullArgumentError("pInfo", SOURCE_LOCATION);
  }

  *pInfo = token->slot_info();
  return absl::OkStatus();
}

// Get information about a token in the system.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002329
absl::Status GetTokenInfo(CK_SLOT_ID slotID, CK_TOKEN_INFO_PTR pInfo) {
  ASSIGN_OR_RETURN(const Token* token, GetToken(slotID));
  if (!pInfo) {
    return NullArgumentError("pInfo", SOURCE_LOCATION);
  }

  *pInfo = token->token_info();
  return absl::OkStatus();
}

// Open a session between an application and a token in a particular slot.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002337
// Note that `pApplication` and `notify` are always ignored in our library,
// which does not support notifications.
absl::Status OpenSession(CK_SLOT_ID slotID, CK_FLAGS flags,
                         CK_VOID_PTR pApplication, CK_NOTIFY notify,
                         CK_SESSION_HANDLE_PTR phSession) {
  ASSIGN_OR_RETURN(Provider * provider, GetProvider());

  if ((flags & CKF_SERIAL_SESSION) != CKF_SERIAL_SESSION) {
    return NewError(absl::StatusCode::kInvalidArgument,
                    "parallel sessions are not supported",
                    CKR_SESSION_PARALLEL_NOT_SUPPORTED, SOURCE_LOCATION);
  }
  if ((flags & CKF_RW_SESSION) == CKF_RW_SESSION) {
    return NewError(absl::StatusCode::kInvalidArgument,
                    "this library does not support read-write sessions",
                    CKR_TOKEN_WRITE_PROTECTED, SOURCE_LOCATION);
  }

  ASSIGN_OR_RETURN(*phSession, provider->OpenSession(slotID));
  return absl::OkStatus();
}

// Close a session between an application and a token.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc383864884
absl::Status CloseSession(CK_SESSION_HANDLE hSession) {
  ASSIGN_OR_RETURN(Provider * provider, GetProvider());
  return provider->CloseSession(hSession);
}

// Get information about a session.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002340
absl::Status GetSessionInfo(CK_SESSION_HANDLE hSession,
                            CK_SESSION_INFO_PTR pInfo) {
  ASSIGN_OR_RETURN(Provider * provider, GetProvider());
  ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   provider->GetSession(hSession));
  *pInfo = session->token()->session_info();
  return absl::OkStatus();
}

// Log a user into a token.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002343
// Note that pPin and ulPinLen are always ignored in this library.
absl::Status Login(CK_SESSION_HANDLE hSession, CK_USER_TYPE userType,
                   CK_UTF8CHAR_PTR pPin, CK_ULONG ulPinLen) {
  ASSIGN_OR_RETURN(Provider * provider, GetProvider());
  ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   provider->GetSession(hSession));
  return session->token()->Login(userType);
}

// Log a user out from a token.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002344
absl::Status Logout(CK_SESSION_HANDLE hSession) {
  ASSIGN_OR_RETURN(Provider * provider, GetProvider());
  ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   provider->GetSession(hSession));
  return session->token()->Logout();
}

// Get a list of mechanisms supported in a token.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002331
absl::Status GetMechanismList(CK_SLOT_ID slotID,
                              CK_MECHANISM_TYPE_PTR pMechanismList,
                              CK_ULONG_PTR pulCount) {
  RETURN_IF_ERROR(GetToken(slotID).status());  // ensure slotID is valid
  if (!pulCount) {
    return NullArgumentError("pulCount", SOURCE_LOCATION);
  }

  absl::Span<const CK_MECHANISM_TYPE> types = Mechanisms();

  if (!pMechanismList) {
    *pulCount = types.size();
    return absl::OkStatus();
  }

  if (*pulCount < types.size()) {
    absl::Status result = OutOfRangeError(
        absl::StrFormat("*pulCount=%d but there are %d mechanisms", *pulCount,
                        types.size()),
        SOURCE_LOCATION);
    *pulCount = types.size();
    return result;
  }

  for (int i = 0; i < types.size(); i++) {
    pMechanismList[i] = types[i];
  }
  *pulCount = types.size();
  return absl::OkStatus();
}

// Get information about a mechanism supported in a token.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002332
absl::Status GetMechanismInfo(CK_SLOT_ID slotID, CK_MECHANISM_TYPE type,
                              CK_MECHANISM_INFO_PTR pInfo) {
  RETURN_IF_ERROR(GetToken(slotID).status());  // ensure slotID is valid
  if (!pInfo) {
    return NullArgumentError("pInfo", SOURCE_LOCATION);
  }
  ASSIGN_OR_RETURN(*pInfo, MechanismInfo(type));
  return absl::OkStatus();
}

// Get the values of the supplied attributes for the given object.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002350
absl::Status GetAttributeValue(CK_SESSION_HANDLE hSession,
                               CK_OBJECT_HANDLE hObject,
                               CK_ATTRIBUTE_PTR pTemplate, CK_ULONG ulCount) {
  ASSIGN_OR_RETURN(std::shared_ptr<Session> session, GetSession(hSession));
  ASSIGN_OR_RETURN(std::shared_ptr<Object> object,
                   session->token()->GetObject(hObject));
  if (!pTemplate) {
    return NullArgumentError("pTemplate", SOURCE_LOCATION);
  }

  absl::Status result = absl::OkStatus();
  for (CK_ATTRIBUTE& attr : absl::MakeSpan(pTemplate, ulCount)) {
    StatusOr<absl::string_view> value_or =
        object->attributes().Value(attr.type);

    // C_GetAttributeValue cases 1 and 2
    if (!value_or.ok()) {
      result = value_or.status();
      attr.ulValueLen = CK_UNAVAILABLE_INFORMATION;
      continue;
    }

    absl::string_view value = value_or.value();

    // C_GetAttributeValue case 3
    if (!attr.pValue) {
      attr.ulValueLen = value.size();
      continue;
    }

    // C_GetAttributeValue case 4
    if (attr.ulValueLen >= value.size()) {
      std::copy(value.begin(), value.end(), static_cast<char*>(attr.pValue));
      attr.ulValueLen = value.size();
      continue;
    }

    // C_GetAttributeValue case 5
    attr.ulValueLen = CK_UNAVAILABLE_INFORMATION;
    result = OutOfRangeError(
        absl::StrFormat(
            "attribute %#X is of length %d, received buffer of length %d",
            attr.type, value.size(), attr.ulValueLen),
        SOURCE_LOCATION);
  }

  return result;
}

// Begin an object browsing operation.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002352
absl::Status FindObjectsInit(CK_SESSION_HANDLE hSession,
                             CK_ATTRIBUTE_PTR pTemplate, CK_ULONG ulCount) {
  ASSIGN_OR_RETURN(std::shared_ptr<Session> session, GetSession(hSession));

  if (ulCount > 0 && !pTemplate) {
    return NullArgumentError("pTemplate", SOURCE_LOCATION);
  }

  return session->FindObjectsInit(absl::MakeConstSpan(pTemplate, ulCount));
}

// Continue an object browsing operation.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002353
absl::Status FindObjects(CK_SESSION_HANDLE hSession,
                         CK_OBJECT_HANDLE_PTR phObject,
                         CK_ULONG ulMaxObjectCount,
                         CK_ULONG_PTR pulObjectCount) {
  ASSIGN_OR_RETURN(std::shared_ptr<Session> session, GetSession(hSession));

  if (!phObject) {
    return NullArgumentError("phObject", SOURCE_LOCATION);
  }
  if (!pulObjectCount) {
    return NullArgumentError("pulObjectCount", SOURCE_LOCATION);
  }

  ASSIGN_OR_RETURN(absl::Span<const CK_OBJECT_HANDLE> handles,
                   session->FindObjects(ulMaxObjectCount));

  std::copy(handles.begin(), handles.end(), phObject);
  *pulObjectCount = handles.size();
  return absl::OkStatus();
}

// End an object browsing operation.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002354
absl::Status FindObjectsFinal(CK_SESSION_HANDLE hSession) {
  ASSIGN_OR_RETURN(std::shared_ptr<Session> session, GetSession(hSession));
  return session->FindObjectsFinal();
}

// Begin a decrypt operation.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002361
absl::Status DecryptInit(CK_SESSION_HANDLE hSession,
                         CK_MECHANISM_PTR pMechanism, CK_OBJECT_HANDLE hKey) {
  ASSIGN_OR_RETURN(std::shared_ptr<Session> session, GetSession(hSession));

  StatusOr<std::shared_ptr<Object>> key_or = session->token()->GetObject(hKey);
  if (!key_or.ok()) {
    if (GetCkRv(key_or.status()) == CKR_OBJECT_HANDLE_INVALID) {
      absl::Status result(key_or.status());
      SetErrorRv(result, CKR_KEY_HANDLE_INVALID);
      return result;
    }
    return key_or.status();
  }

  if (!pMechanism) {
    return NullArgumentError("pMechanism", SOURCE_LOCATION);
  }
  return session->DecryptInit(key_or.value(), pMechanism);
}

// Complete a decrypt operation.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002362
absl::Status Decrypt(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pEncryptedData,
                     CK_ULONG ulEncryptedDataLen, CK_BYTE_PTR pData,
                     CK_ULONG_PTR pulDataLen) {
  ASSIGN_OR_RETURN(std::shared_ptr<Session> session, GetSession(hSession));
  if (!pEncryptedData) {
    return NullArgumentError("pEncryptedData", SOURCE_LOCATION);
  }
  if (!pulDataLen) {
    return NullArgumentError("pulDataLen", SOURCE_LOCATION);
  }

  ASSIGN_OR_RETURN(absl::Span<const uint8_t> plaintext,
                   session->Decrypt(absl::MakeConstSpan(pEncryptedData,
                                                        ulEncryptedDataLen)));

  if (!pData) {
    *pulDataLen = plaintext.size();
    return absl::OkStatus();
  }

  if (*pulDataLen < plaintext.size()) {
    absl::Status result = OutOfRangeError(
        absl::StrFormat(
            "plaintext of length %d cannot fit in buffer of length %d",
            plaintext.size(), *pulDataLen),
        SOURCE_LOCATION);
    *pulDataLen = plaintext.size();
    return result;
  }

  std::copy(plaintext.begin(), plaintext.end(), pData);
  *pulDataLen = plaintext.size();

  session->ReleaseOperation();
  return absl::OkStatus();
}

// Begin an encrypt operation.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002356
absl::Status EncryptInit(CK_SESSION_HANDLE hSession,
                         CK_MECHANISM_PTR pMechanism, CK_OBJECT_HANDLE hKey) {
  ASSIGN_OR_RETURN(std::shared_ptr<Session> session, GetSession(hSession));

  StatusOr<std::shared_ptr<Object>> key_or = session->token()->GetObject(hKey);
  if (!key_or.ok()) {
    if (GetCkRv(key_or.status()) == CKR_OBJECT_HANDLE_INVALID) {
      absl::Status result(key_or.status());
      SetErrorRv(result, CKR_KEY_HANDLE_INVALID);
      return result;
    }
    return key_or.status();
  }

  if (!pMechanism) {
    return NullArgumentError("pMechanism", SOURCE_LOCATION);
  }
  return session->EncryptInit(key_or.value(), pMechanism);
}

// Complete an encrypt operation.
// http://docs.oasis-open.org/pkcs11/pkcs11-base/v2.40/pkcs11-base-v2.40.html#_Toc235002357
absl::Status Encrypt(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pData,
                     CK_ULONG ulDataLen, CK_BYTE_PTR pEncryptedData,
                     CK_ULONG_PTR pulEncryptedDataLen) {
  ASSIGN_OR_RETURN(std::shared_ptr<Session> session, GetSession(hSession));
  if (!pData) {
    return NullArgumentError("pData", SOURCE_LOCATION);
  }
  if (!pulEncryptedDataLen) {
    return NullArgumentError("pulEncryptedDataLen", SOURCE_LOCATION);
  }

  ASSIGN_OR_RETURN(absl::Span<const uint8_t> ciphertext,
                   session->Encrypt(absl::MakeConstSpan(pData, ulDataLen)));

  if (!pEncryptedData) {
    *pulEncryptedDataLen = ciphertext.size();
    return absl::OkStatus();
  }

  if (*pulEncryptedDataLen < ciphertext.size()) {
    absl::Status result = OutOfRangeError(
        absl::StrFormat(
            "ciphertext of length %d cannot fit in buffer of length %d",
            ciphertext.size(), *pulEncryptedDataLen),
        SOURCE_LOCATION);
    *pulEncryptedDataLen = ciphertext.size();
    return result;
  }

  std::copy(ciphertext.begin(), ciphertext.end(), pEncryptedData);
  *pulEncryptedDataLen = ciphertext.size();

  session->ReleaseOperation();
  return absl::OkStatus();
}

}  // namespace kmsp11
