#pragma once

#include "tokmon/cbor.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace tokmon::desk {

class BrowserController;
class DeskViewModel;

// Owns desktop settings, their RmlUi-bound form model, and conversion to/from
// persisted CBOR. Daemon transport remains an injected concern of the shell.
class SettingsController {
public:
  SettingsController(DeskViewModel& view_model, BrowserController& browser,
                     int default_ui_scale);

  void load(const tokmon::cbor::Value& stored);
  void show(std::string page, const std::filesystem::path& workspace);
  void apply_shared(const tokmon::cbor::Value& payload);
  void apply_providers(const tokmon::cbor::Value& payload);
  void reset(const std::filesystem::path& workspace);
  void set_status(std::string status);

  [[nodiscard]] tokmon::cbor::Value& values() noexcept { return values_; }
  [[nodiscard]] tokmon::cbor::Value& providers_payload() noexcept { return providers_; }
  [[nodiscard]] const tokmon::cbor::Value& values() const noexcept { return values_; }
  [[nodiscard]] const tokmon::cbor::Value& providers_payload() const noexcept {
    return providers_;
  }
  [[nodiscard]] tokmon::cbor::Value shared_values();
  [[nodiscard]] tokmon::cbor::Value provider_configuration() const;
  [[nodiscard]] tokmon::cbor::Value provider_test() const;
  [[nodiscard]] tokmon::cbor::Value provider_secret() const;

  [[nodiscard]] const std::string& page() const noexcept { return page_; }
  [[nodiscard]] const std::string& provider() const noexcept { return provider_; }
  [[nodiscard]] const std::string& model() const noexcept { return model_; }
  [[nodiscard]] const std::string& effort() const noexcept { return effort_; }
  [[nodiscard]] const std::string& access() const noexcept { return access_; }
  void select_provider(std::string provider);
  void select_model(std::string model);
  void select_effort(std::string effort);
  void select_access(std::string access);
  void toggle(std::string_view key);
  void choose(std::string_view key, std::string value);

  [[nodiscard]] std::string string(std::string_view key,
                                   std::string_view fallback = {}) const;
  [[nodiscard]] std::int64_t integer(std::string_view key,
                                     std::int64_t fallback) const;
  [[nodiscard]] bool boolean(std::string_view key, bool fallback) const;
  void set(std::string key, tokmon::cbor::Value value);

private:
  void values_to_view(const std::filesystem::path& workspace);
  void view_to_values();
  void sync_shell();

  DeskViewModel& view_model_;
  BrowserController& browser_;
  int default_ui_scale_{125};
  tokmon::cbor::Value values_{tokmon::cbor::Value::Map{}};
  tokmon::cbor::Value providers_{tokmon::cbor::Value::Map{}};
  std::string page_{"general"};
  std::string provider_;
  std::string model_;
  std::string effort_{"高"};
  std::string access_{"full"};
};

} // namespace tokmon::desk
