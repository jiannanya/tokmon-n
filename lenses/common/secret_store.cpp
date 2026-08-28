#include "lenses/common/secret_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>

#include "tokmon/ids.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(TOKMON_HAS_LIBSECRET)
#include <libsecret/secret.h>
#endif

namespace tokmon::builtin {
namespace {

struct Binding {
  std::string value;
  std::string purpose;
  std::string act_hash;
  std::string target;
  GenerationId generation{0};
  MountEpoch epoch{0};
  std::chrono::steady_clock::time_point expires;
  ~Binding() { std::fill(value.begin(), value.end(), '\0'); }
};

struct SecretInput {
  std::string value;
  std::chrono::steady_clock::time_point expires;
  ~SecretInput() { std::fill(value.begin(), value.end(), '\0'); }
};

std::mutex binding_mutex;
std::map<std::string, Binding, std::less<>> bindings;
std::map<std::string, SecretInput, std::less<>> secret_inputs;

Result<void> validate_secret_input(const std::string_view id,
                                   const std::string_view purpose = {}) {
  if (id.empty() || id.size() > 240u || id.find('\0') != std::string_view::npos)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "secret id is empty, invalid or too long"));
  if (purpose.size() > 240u || purpose.find('\0') != std::string_view::npos)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "secret purpose is invalid or too long"));
  return {};
}

Result<void> validate_secret_value(const std::string_view value) {
  if (value.empty() || value.find('\0') != std::string_view::npos)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "secret value is empty or contains NUL"));
  return {};
}

#if defined(_WIN32)
Result<std::wstring> wide(const std::string_view input) {
  if (input.empty()) return std::wstring{};
  const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      input.data(), static_cast<int>(input.size()), nullptr, 0);
  if (size <= 0)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "secret id is not valid UTF-8"));
  std::wstring output(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
      static_cast<int>(input.size()), output.data(), size);
  return output;
}

std::string narrow(const wchar_t* input) {
  if (!input || *input == L'\0') return {};
  const auto length = static_cast<int>(std::wcslen(input));
  const auto size = WideCharToMultiByte(CP_UTF8, 0, input, length, nullptr, 0,
                                        nullptr, nullptr);
  std::string output(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, input, length, output.data(), size, nullptr, nullptr);
  return output;
}

Result<std::wstring> target_name(const std::string_view id) {
  return wide("Tokmon:" + std::string(id));
}
#elif defined(__APPLE__)
Error apple_keychain_error(const OSStatus status, const std::string_view operation) {
  return make_error(status == errSecItemNotFound ? ErrorCode::not_found
                                                  : ErrorCode::io_error,
      std::string(operation) + " failed with Keychain status " +
          std::to_string(status));
}

CFStringRef apple_string(const std::string_view value) {
  return CFStringCreateWithBytes(kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(value.data()),
      static_cast<CFIndex>(value.size()), kCFStringEncodingUTF8, false);
}

std::string apple_text(const CFTypeRef value) {
  if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
  const auto string = static_cast<CFStringRef>(value);
  const auto capacity = CFStringGetMaximumSizeForEncoding(
      CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
  std::string output(static_cast<std::size_t>(capacity), '\0');
  if (!CFStringGetCString(string, output.data(), capacity, kCFStringEncodingUTF8))
    return {};
  output.resize(std::strlen(output.c_str()));
  return output;
}

Result<CFMutableDictionaryRef> apple_keychain_query(const std::string_view id) {
  auto* account = apple_string(id);
  if (!account)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "secret id is not valid UTF-8"));
  auto* query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (!query) {
    CFRelease(account);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot allocate a Keychain query"));
  }
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, CFSTR("org.tokmon.secrets"));
  CFDictionarySetValue(query, kSecAttrAccount, account);
  CFRelease(account);
  return query;
}
#elif defined(TOKMON_HAS_LIBSECRET)
const SecretSchema tokmon_secret_schema = {
    "org.tokmon.Secret", SECRET_SCHEMA_NONE,
    {{"id", SECRET_SCHEMA_ATTRIBUTE_STRING},
     {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}};

Error libsecret_error(const std::string_view operation, GError* error,
                      const ErrorCode fallback = ErrorCode::io_error) {
  auto message = std::string(operation);
  if (error && error->message) message += ": " + std::string(error->message);
  if (error) g_error_free(error);
  return make_error(fallback, std::move(message));
}
#endif

Error keyring_unavailable() {
  return make_error(ErrorCode::unsupported,
      "the operating-system credential backend is unavailable");
}

}  // namespace

std::string_view keyring_backend() noexcept {
#if defined(_WIN32)
  return "windows-credential-manager";
#elif defined(__APPLE__)
  return "macos-keychain";
#elif defined(TOKMON_HAS_LIBSECRET)
  return "linux-secret-service";
#else
  return "unsupported";
#endif
}

bool keyring_supported() noexcept {
#if defined(_WIN32) || defined(__APPLE__) || defined(TOKMON_HAS_LIBSECRET)
  return true;
#else
  return false;
#endif
}

std::string model_credential_id(const std::string_view configuration_name) {
  return "model-secret-library/" + std::string(configuration_name);
}

Result<void> keyring_write(const std::string_view id, const std::string_view purpose,
                           const std::string_view value) {
  if (auto checked = validate_secret_input(id, purpose); !checked) return checked;
  if (auto checked = validate_secret_value(value); !checked) return checked;
#if defined(_WIN32)
  auto target = target_name(id);
  auto comment = wide(purpose);
  if (!target) return tl::unexpected(target.error());
  if (!comment) return tl::unexpected(comment.error());
  if (value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "secret value is too large"));
  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = target->data();
  credential.Comment = comment->empty() ? nullptr : comment->data();
  credential.CredentialBlobSize = static_cast<DWORD>(value.size());
  credential.CredentialBlob = reinterpret_cast<LPBYTE>(
      const_cast<char*>(value.data()));
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
  credential.UserName = const_cast<wchar_t*>(L"tokmon");
  if (!CredWriteW(&credential, 0))
    return tl::unexpected(make_error(ErrorCode::io_error,
        "Windows Credential Manager rejected the secret"));
  return {};
#elif defined(__APPLE__)
  auto query = apple_keychain_query(id);
  if (!query) return tl::unexpected(query.error());
  auto* data = CFDataCreate(kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(value.data()),
      static_cast<CFIndex>(value.size()));
  auto* comment = apple_string(purpose);
  const auto label_text = "Tokmon: " + std::string(id);
  auto* label = apple_string(label_text);
  if (!data || !comment || !label) {
    if (data) CFRelease(data);
    if (comment) CFRelease(comment);
    if (label) CFRelease(label);
    CFRelease(*query);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot allocate Keychain attributes"));
  }
  CFDictionarySetValue(*query, kSecAttrLabel, label);
  CFDictionarySetValue(*query, kSecAttrComment, comment);
  CFDictionarySetValue(*query, kSecValueData, data);
  auto status = SecItemAdd(*query, nullptr);
  if (status == errSecDuplicateItem) {
    CFDictionaryRemoveValue(*query, kSecAttrLabel);
    CFDictionaryRemoveValue(*query, kSecAttrComment);
    CFDictionaryRemoveValue(*query, kSecValueData);
    auto* changes = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!changes) {
      CFRelease(data);
      CFRelease(comment);
      CFRelease(label);
      CFRelease(*query);
      return tl::unexpected(make_error(ErrorCode::io_error,
                                       "cannot allocate Keychain update attributes"));
    }
    CFDictionarySetValue(changes, kSecAttrLabel, label);
    CFDictionarySetValue(changes, kSecAttrComment, comment);
    CFDictionarySetValue(changes, kSecValueData, data);
    status = SecItemUpdate(*query, changes);
    CFRelease(changes);
  }
  CFRelease(data);
  CFRelease(comment);
  CFRelease(label);
  CFRelease(*query);
  if (status != errSecSuccess)
    return tl::unexpected(apple_keychain_error(status, "Keychain write"));
  return {};
#elif defined(TOKMON_HAS_LIBSECRET)
  GError* error = nullptr;
  const auto label = purpose.empty() ? std::string("Tokmon secret")
                                     : std::string(purpose);
  const auto password = std::string(value);
  const auto identifier = std::string(id);
  if (!secret_password_store_sync(&tokmon_secret_schema, SECRET_COLLECTION_DEFAULT,
          label.c_str(), password.c_str(), nullptr, &error,
          "id", identifier.c_str(), nullptr))
    return tl::unexpected(libsecret_error("Secret Service write failed", error));
  return {};
#else
  (void)id; (void)purpose; (void)value;
  return tl::unexpected(keyring_unavailable());
#endif
}

Result<std::string> keyring_read(const std::string_view id) {
  if (auto checked = validate_secret_input(id); !checked)
    return tl::unexpected(checked.error());
#if defined(_WIN32)
  auto target = target_name(id);
  if (!target) return tl::unexpected(target.error());
  PCREDENTIALW credential = nullptr;
  if (!CredReadW(target->c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
    const auto code = GetLastError();
    return tl::unexpected(make_error(code == ERROR_NOT_FOUND ? ErrorCode::not_found
                                                              : ErrorCode::io_error,
        code == ERROR_NOT_FOUND ? "secret reference was not found"
                                : "Windows Credential Manager read failed"));
  }
  std::string value(reinterpret_cast<const char*>(credential->CredentialBlob),
                    credential->CredentialBlobSize);
  CredFree(credential);
  return value;
#elif defined(__APPLE__)
  auto query = apple_keychain_query(id);
  if (!query) return tl::unexpected(query.error());
  CFDictionarySetValue(*query, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(*query, kSecMatchLimit, kSecMatchLimitOne);
  CFTypeRef found = nullptr;
  const auto status = SecItemCopyMatching(*query, &found);
  CFRelease(*query);
  if (status != errSecSuccess) {
    if (found) CFRelease(found);
    return tl::unexpected(apple_keychain_error(status, "Keychain read"));
  }
  if (!found || CFGetTypeID(found) != CFDataGetTypeID()) {
    if (found) CFRelease(found);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "Keychain returned an invalid secret"));
  }
  const auto data = static_cast<CFDataRef>(found);
  std::string value(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                    static_cast<std::size_t>(CFDataGetLength(data)));
  CFRelease(found);
  return value;
#elif defined(TOKMON_HAS_LIBSECRET)
  GError* error = nullptr;
  const auto identifier = std::string(id);
  auto* password = secret_password_lookup_sync(&tokmon_secret_schema, nullptr, &error,
      "id", identifier.c_str(), nullptr);
  if (!password) {
    if (error)
      return tl::unexpected(libsecret_error("Secret Service read failed", error));
    return tl::unexpected(make_error(ErrorCode::not_found,
                                     "secret reference was not found"));
  }
  std::string value(password);
  secret_password_free(password);
  return value;
#else
  (void)id;
  return tl::unexpected(keyring_unavailable());
#endif
}

Result<void> keyring_delete(const std::string_view id) {
  if (auto checked = validate_secret_input(id); !checked) return checked;
#if defined(_WIN32)
  auto target = target_name(id);
  if (!target) return tl::unexpected(target.error());
  if (!CredDeleteW(target->c_str(), CRED_TYPE_GENERIC, 0)) {
    const auto code = GetLastError();
    if (code == ERROR_NOT_FOUND) return {};
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "Windows Credential Manager delete failed"));
  }
  return {};
#elif defined(__APPLE__)
  auto query = apple_keychain_query(id);
  if (!query) return tl::unexpected(query.error());
  const auto status = SecItemDelete(*query);
  CFRelease(*query);
  if (status != errSecSuccess && status != errSecItemNotFound)
    return tl::unexpected(apple_keychain_error(status, "Keychain delete"));
  return {};
#elif defined(TOKMON_HAS_LIBSECRET)
  GError* error = nullptr;
  const auto identifier = std::string(id);
  (void)secret_password_clear_sync(&tokmon_secret_schema, nullptr, &error,
      "id", identifier.c_str(), nullptr);
  if (error)
    return tl::unexpected(libsecret_error("Secret Service delete failed", error));
  return {};
#else
  (void)id;
  return tl::unexpected(keyring_unavailable());
#endif
}

Result<std::vector<SecretMetadata>> keyring_list() {
#if defined(_WIN32)
  DWORD count = 0;
  PCREDENTIALW* credentials = nullptr;
  if (!CredEnumerateW(L"Tokmon:*", 0, &count, &credentials)) {
    if (GetLastError() == ERROR_NOT_FOUND) return std::vector<SecretMetadata>{};
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "Windows Credential Manager enumeration failed"));
  }
  std::vector<SecretMetadata> result;
  for (DWORD index = 0; index < count; ++index) {
    const auto* item = credentials[index];
    auto target = narrow(item->TargetName);
    if (!target.starts_with("Tokmon:")) continue;
    const auto ticks = (static_cast<std::uint64_t>(item->LastWritten.dwHighDateTime) << 32u) |
        item->LastWritten.dwLowDateTime;
    const auto unix_ms = ticks > 116444736000000000ULL
        ? static_cast<std::int64_t>((ticks - 116444736000000000ULL) / 10000ULL) : 0;
    result.push_back(SecretMetadata{target.substr(7), narrow(item->Comment), unix_ms});
  }
  CredFree(credentials);
  std::ranges::sort(result, {}, &SecretMetadata::id);
  return result;
#elif defined(__APPLE__)
  auto* query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (!query)
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot allocate a Keychain list query"));
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, CFSTR("org.tokmon.secrets"));
  CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitAll);
  CFTypeRef found = nullptr;
  const auto status = SecItemCopyMatching(query, &found);
  CFRelease(query);
  if (status == errSecItemNotFound) return std::vector<SecretMetadata>{};
  if (status != errSecSuccess)
    return tl::unexpected(apple_keychain_error(status, "Keychain enumeration"));
  std::vector<SecretMetadata> result;
  const auto append = [&result](const CFDictionaryRef attributes) {
    const auto id = apple_text(CFDictionaryGetValue(attributes, kSecAttrAccount));
    if (id.empty()) return;
    const auto purpose = apple_text(CFDictionaryGetValue(attributes, kSecAttrComment));
    std::int64_t modified_ms = 0;
    const auto modified = CFDictionaryGetValue(attributes, kSecAttrModificationDate);
    if (modified && CFGetTypeID(modified) == CFDateGetTypeID()) {
      const auto unix_seconds = CFDateGetAbsoluteTime(static_cast<CFDateRef>(modified)) +
          kCFAbsoluteTimeIntervalSince1970;
      modified_ms = static_cast<std::int64_t>(unix_seconds * 1000.0);
    }
    result.push_back(SecretMetadata{id, purpose, modified_ms});
  };
  if (found && CFGetTypeID(found) == CFArrayGetTypeID()) {
    const auto items = static_cast<CFArrayRef>(found);
    for (CFIndex index = 0; index < CFArrayGetCount(items); ++index) {
      const auto item = CFArrayGetValueAtIndex(items, index);
      if (item && CFGetTypeID(item) == CFDictionaryGetTypeID())
        append(static_cast<CFDictionaryRef>(item));
    }
  } else if (found && CFGetTypeID(found) == CFDictionaryGetTypeID()) {
    append(static_cast<CFDictionaryRef>(found));
  } else {
    if (found) CFRelease(found);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "Keychain returned invalid enumeration data"));
  }
  if (found) CFRelease(found);
  std::ranges::sort(result, {}, &SecretMetadata::id);
  return result;
#elif defined(TOKMON_HAS_LIBSECRET)
  auto* attributes = g_hash_table_new(g_str_hash, g_str_equal);
  if (!attributes)
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot allocate Secret Service attributes"));
  GError* error = nullptr;
  auto* items = secret_password_searchv_sync(&tokmon_secret_schema, attributes,
      SECRET_SEARCH_ALL, nullptr, &error);
  g_hash_table_unref(attributes);
  if (error) {
    if (items) g_list_free_full(items, g_object_unref);
    return tl::unexpected(libsecret_error("Secret Service enumeration failed", error));
  }
  std::vector<SecretMetadata> result;
  for (auto* iterator = items; iterator; iterator = iterator->next) {
    auto* item = SECRET_RETRIEVABLE(iterator->data);
    auto* item_attributes = secret_retrievable_get_attributes(item);
    const auto* id = item_attributes
        ? static_cast<const char*>(g_hash_table_lookup(item_attributes, "id")) : nullptr;
    auto* label = secret_retrievable_get_label(item);
    const auto modified = secret_retrievable_get_modified(item);
    if (id && *id) {
      const auto maximum_seconds = static_cast<std::uint64_t>(
          std::numeric_limits<std::int64_t>::max() / 1000);
      const auto modified_ms = modified > maximum_seconds
          ? std::numeric_limits<std::int64_t>::max()
          : static_cast<std::int64_t>(modified * 1000u);
      result.push_back(SecretMetadata{id, label ? label : "", modified_ms});
    }
    if (label) g_free(label);
    if (item_attributes) g_hash_table_unref(item_attributes);
  }
  if (items) g_list_free_full(items, g_object_unref);
  std::ranges::sort(result, {}, &SecretMetadata::id);
  return result;
#else
  return tl::unexpected(keyring_unavailable());
#endif
}

Result<std::string> create_secret_input_handle(
    const std::string_view value, const std::chrono::milliseconds lifetime) {
  if (auto checked = validate_secret_value(value); !checked)
    return tl::unexpected(checked.error());
  if (lifetime <= std::chrono::milliseconds::zero() ||
      lifetime > std::chrono::seconds(30))
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "secret input lifetime is invalid"));
  const auto now = std::chrono::steady_clock::now();
  const auto handle = make_id("secret-input");
  std::scoped_lock lock(binding_mutex);
  std::erase_if(secret_inputs,
                [now](const auto& item) { return item.second.expires <= now; });
  secret_inputs.emplace(handle, SecretInput{std::string(value), now + lifetime});
  return handle;
}

Result<std::string> consume_secret_input_handle(const std::string_view handle) {
  std::scoped_lock lock(binding_mutex);
  const auto found = secret_inputs.find(handle);
  if (found == secret_inputs.end())
    return tl::unexpected(make_error(ErrorCode::not_found,
                                     "secret input is unavailable or already consumed"));
  if (found->second.expires <= std::chrono::steady_clock::now()) {
    secret_inputs.erase(found);
    return tl::unexpected(make_error(ErrorCode::not_found,
                                     "secret input expired before it was consumed"));
  }
  auto value = std::move(found->second.value);
  secret_inputs.erase(found);
  return value;
}

void revoke_secret_input_handle(const std::string_view handle) noexcept {
  std::scoped_lock lock(binding_mutex);
  if (const auto found = secret_inputs.find(handle); found != secret_inputs.end())
    secret_inputs.erase(found);
}

Result<std::string> create_secret_binding(const std::string_view secret_id,
    const std::string_view purpose, const std::string_view act_hash,
    const std::string_view target, const GenerationId generation, const MountEpoch epoch,
    const std::chrono::milliseconds lifetime) {
  if (purpose.empty() || act_hash.size() != 64u || target.empty() || generation == 0 ||
      epoch == 0 || lifetime <= std::chrono::milliseconds::zero() ||
      lifetime > std::chrono::minutes(5))
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "invalid secret binding scope"));
  auto value = keyring_read(secret_id);
  if (!value) return tl::unexpected(value.error());
  auto id = make_id("secret-binding");
  std::scoped_lock lock(binding_mutex);
  const auto now = std::chrono::steady_clock::now();
  std::erase_if(bindings, [now](const auto& item) { return item.second.expires <= now; });
  bindings.emplace(id, Binding{std::move(*value), std::string(purpose),
      std::string(act_hash), std::string(target), generation, epoch, now + lifetime});
  return id;
}

Result<std::string> create_model_secret_binding(
    const std::string_view configuration_name,
    const std::string_view fallback_environment,
    const std::string_view purpose, const std::string_view act_hash,
    const std::string_view target, const GenerationId generation,
    const MountEpoch epoch, const std::chrono::milliseconds lifetime) {
  if (configuration_name.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "model configuration name is required for a secret binding"));
  const auto credential_id = model_credential_id(configuration_name);
  auto value = keyring_read(credential_id);
  if (!value && value.error().code == ErrorCode::not_found &&
      !fallback_environment.empty()) {
    const auto environment_name = std::string(fallback_environment);
    if (const auto* environment = std::getenv(environment_name.c_str());
        environment && *environment != '\0')
      value = std::string(environment);
  }
  if (!value && value.error().code == ErrorCode::not_found)
    return tl::unexpected(make_error(ErrorCode::not_found,
        "model API credential is not configured for " +
        std::string(configuration_name)));
  if (!value) return tl::unexpected(value.error());
  if (purpose.empty() || act_hash.size() != 64u || target.empty() || generation == 0 ||
      epoch == 0 || lifetime <= std::chrono::milliseconds::zero() ||
      lifetime > std::chrono::minutes(5)) {
    std::fill(value->begin(), value->end(), '\0');
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "invalid model secret binding scope"));
  }
  auto id = make_id("secret-binding");
  std::scoped_lock lock(binding_mutex);
  const auto now = std::chrono::steady_clock::now();
  std::erase_if(bindings, [now](const auto& item) { return item.second.expires <= now; });
  bindings.emplace(id, Binding{std::move(*value), std::string(purpose),
      std::string(act_hash), std::string(target), generation, epoch, now + lifetime});
  return id;
}

Result<std::string> resolve_secret_binding(const std::string_view binding_id,
    const std::string_view purpose, const std::string_view act_hash,
    const std::string_view target, const GenerationId generation, const MountEpoch epoch) {
  std::scoped_lock lock(binding_mutex);
  const auto found = bindings.find(binding_id);
  if (found == bindings.end())
    return tl::unexpected(make_error(ErrorCode::not_found,
                                     "secret binding is unavailable or already consumed"));
  const auto& binding = found->second;
  if (binding.expires <= std::chrono::steady_clock::now() ||
      binding.purpose != purpose || binding.act_hash != act_hash ||
      binding.target != target || binding.generation != generation || binding.epoch != epoch) {
    bindings.erase(found);
    return tl::unexpected(make_error(ErrorCode::permission_denied,
                                     "secret binding scope no longer matches the Act"));
  }
  auto value = binding.value;
  bindings.erase(found);
  return value;
}

void revoke_secret_binding(const std::string_view binding_id) noexcept {
  std::scoped_lock lock(binding_mutex);
  bindings.erase(std::string(binding_id));
}

}  // namespace tokmon::builtin
