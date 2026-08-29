#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tokmon::desk {

struct BrowserCandidate {
  std::string name;
  std::filesystem::path executable;
};

struct BrowserRuntimeStatus {
  bool installed{false};
  std::string version;
  std::filesystem::path executable;
  std::string error;
};

struct BrowserSessionState {
  bool running{false};
  std::string session;
  std::string url;
  std::string title;
  std::string accessibility_snapshot;
  std::filesystem::path preview_image;
  std::string error;
};

class BrowserManager final {
 public:
  explicit BrowserManager(std::filesystem::path data_root = {});
  [[nodiscard]] std::vector<BrowserCandidate> discover() const;
  [[nodiscard]] std::filesystem::path profile_path(const std::string& session) const;
  [[nodiscard]] BrowserRuntimeStatus runtime_status() const;
  [[nodiscard]] bool install_runtime(std::string& error) const;
  [[nodiscard]] bool launch_agent_browser(const std::filesystem::path& executable,
                                          const std::string& session,
                                          bool headed, std::string& error) const;
  [[nodiscard]] BrowserSessionState open(
      const std::filesystem::path& executable, const std::string& session,
      std::string url, bool headed) const;
  [[nodiscard]] BrowserSessionState refresh(const std::string& session) const;
  [[nodiscard]] BrowserSessionState back(const std::string& session) const;
  [[nodiscard]] BrowserSessionState forward(const std::string& session) const;
  [[nodiscard]] BrowserSessionState reload(const std::string& session) const;
  [[nodiscard]] bool click(const std::string& session,
                           const std::string& selector,
                           std::string& error) const;
  [[nodiscard]] bool fill(const std::string& session,
                          const std::string& selector,
                          const std::string& value,
                          std::string& error) const;
  [[nodiscard]] bool close(const std::string& session,
                           std::string& error) const;

 private:
  std::filesystem::path data_root_;
  mutable std::mutex launch_mutex_;
  mutable std::unordered_map<std::string, std::vector<std::string>>
      launch_arguments_;
  void remember_launch(const std::string& session,
                       const std::filesystem::path& executable,
                       const std::filesystem::path& profile,
                       bool headed) const;
  [[nodiscard]] bool run(const std::string& session,
                         std::vector<std::string> command,
                         std::string& output,
                         std::string& error,
                         std::chrono::milliseconds timeout =
                             std::chrono::seconds(30)) const;
  [[nodiscard]] BrowserSessionState navigate_command(
      const std::string& session, std::string command) const;
};

} // namespace tokmon::desk
