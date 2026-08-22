#include "lenses/common/secret_store.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>

#include "tokmon/ids.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
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

std::mutex binding_mutex;
std::map<std::string, Binding, std::less<>> bindings;

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
  if (id.empty() || id.size() > 240u || id.find('\0') != std::string_view::npos)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "secret id is empty or too long"));
  return wide("Tokmon:" + std::string(id));
}
#endif

Error keyring_unavailable() {
  return make_error(ErrorCode::unsupported,
      "the operating-system credential backend is unavailable");
}

}  // namespace

Result<void> keyring_write(const std::string_view id, const std::string_view purpose,
                           const std::string_view value) {
#if defined(_WIN32)
  auto target = target_name(id);
  auto comment = wide(purpose);
  if (!target) return tl::unexpected(target.error());
  if (!comment) return tl::unexpected(comment.error());
  if (value.empty() || value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "secret value is empty or too large"));
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
#else
  (void)id; (void)purpose; (void)value;
  return tl::unexpected(keyring_unavailable());
#endif
}

Result<std::string> keyring_read(const std::string_view id) {
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
#else
  (void)id;
  return tl::unexpected(keyring_unavailable());
#endif
}

Result<void> keyring_delete(const std::string_view id) {
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
#else
  return tl::unexpected(keyring_unavailable());
#endif
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
