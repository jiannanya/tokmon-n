#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tokmon/error.hpp"
#include "tokmon/ids.hpp"

namespace tokmon::builtin {

struct SecretMetadata {
  std::string id;
  std::string purpose;
  std::int64_t last_rotated_ms{0};
};

std::string_view keyring_backend() noexcept;
bool keyring_supported() noexcept;
std::string model_credential_id(std::string_view configuration_name);
Result<void> keyring_write(std::string_view id, std::string_view purpose,
                           std::string_view value);
Result<std::string> keyring_read(std::string_view id);
Result<void> keyring_delete(std::string_view id);
Result<std::vector<SecretMetadata>> keyring_list();

Result<std::string> create_secret_input_handle(
    std::string_view value, std::chrono::milliseconds lifetime);
Result<std::string> consume_secret_input_handle(std::string_view handle);
void revoke_secret_input_handle(std::string_view handle) noexcept;

Result<std::string>
create_secret_binding(std::string_view secret_id, std::string_view purpose,
                      std::string_view act_hash, std::string_view target,
                      GenerationId generation, MountEpoch epoch,
                      std::chrono::milliseconds lifetime);
Result<std::string>
create_model_secret_binding(std::string_view configuration_name,
                            std::string_view fallback_environment,
                            std::string_view purpose,
                            std::string_view act_hash,
                            std::string_view target,
                            GenerationId generation, MountEpoch epoch,
                            std::chrono::milliseconds lifetime);
Result<std::string>
resolve_secret_binding(std::string_view binding_id, std::string_view purpose,
                       std::string_view act_hash, std::string_view target,
                       GenerationId generation, MountEpoch epoch);
void revoke_secret_binding(std::string_view binding_id) noexcept;

} // namespace tokmon::builtin
