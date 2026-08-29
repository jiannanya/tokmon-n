#include "browser/browser_manager.hpp"

#include "lenses/common/process_runner.hpp"
#include "tokmon/hash.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace tokmon::desk {
namespace {

void add_if_present(std::vector<BrowserCandidate>& result, std::string name,
                    std::filesystem::path path) {
  std::error_code ec;
  if (!path.empty() && std::filesystem::is_regular_file(path, ec))
    result.push_back({std::move(name), std::move(path)});
}

std::filesystem::path env(const char* name) {
  const auto* value = std::getenv(name);
  return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
}

constexpr std::string_view runtime_version = "0.35.1";

struct RuntimeAsset {
  const char* name;
  const char* sha256;
};

RuntimeAsset runtime_asset() {
#if defined(_WIN32)
  return {"agent-browser-win32-x64.exe",
          "def2614c2c193518463ad9126718a1ff828a7bf217d7f75f156249c0dbb16c83"};
#elif defined(__APPLE__) && defined(__aarch64__)
  return {"agent-browser-darwin-arm64",
          "12be3313ec6d878d8fda62ca5c62b7013c1b6931bf57dd2678788654b01ffe95"};
#elif defined(__APPLE__)
  return {"agent-browser-darwin-x64",
          "6cafdc32d0cccbd892310adb7a36d7cd97807ab684338664fc08c7fdfeb2fef2"};
#elif defined(__aarch64__)
  return {"agent-browser-linux-arm64",
          "4c24f1fa2f704865a0c4d6f906bf8116931888681742bbf080c03dceb147ac9e"};
#else
  return {"agent-browser-linux-x64",
          "21874b7afbe12a225d01c7f3f7d635c2c2f740660f6ef5e7916737c60c4f1faf"};
#endif
}

std::string safe_name(std::string value) {
  for (char& ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if (!(std::isalnum(byte) || ch == '-' || ch == '_'))
      ch = '_';
  }
  if (value.empty())
    value = "default";
  return value;
}

std::optional<std::vector<std::uint8_t>> read_binary(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>());
}

std::filesystem::path runtime_executable(const std::filesystem::path& root) {
#if defined(_WIN32)
  return root / "browser" / "runtime" / runtime_version / "agent-browser.exe";
#else
  return root / "browser" / "runtime" / runtime_version / "agent-browser";
#endif
}

bool allowed_url(const std::string& value) {
  return value == "about:blank" || value.starts_with("https://") ||
         value.starts_with("http://");
}

void append_utf8(std::string& output, std::uint32_t value) {
  if (value <= 0x7fu) output.push_back(static_cast<char>(value));
  else if (value <= 0x7ffu) {
    output.push_back(static_cast<char>(0xc0u | (value >> 6u)));
    output.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
  } else {
    output.push_back(static_cast<char>(0xe0u | (value >> 12u)));
    output.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3fu)));
    output.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
  }
}

std::optional<std::string> json_string_field(const std::string_view json,
                                             const std::string_view key) {
  const std::string marker = "\"" + std::string(key) + "\":";
  auto position = json.find(marker);
  if (position == std::string_view::npos)
    return std::nullopt;
  position += marker.size();
  while (position < json.size() &&
         std::isspace(static_cast<unsigned char>(json[position])))
    ++position;
  if (position >= json.size() || json[position++] != '"')
    return std::nullopt;
  std::string output;
  while (position < json.size()) {
    const char character = json[position++];
    if (character == '"')
      return output;
    if (character != '\\') {
      output.push_back(character);
      continue;
    }
    if (position >= json.size())
      return std::nullopt;
    const char escaped = json[position++];
    switch (escaped) {
      case '"': output.push_back('"'); break;
      case '\\': output.push_back('\\'); break;
      case '/': output.push_back('/'); break;
      case 'b': output.push_back('\b'); break;
      case 'f': output.push_back('\f'); break;
      case 'n': output.push_back('\n'); break;
      case 'r': output.push_back('\r'); break;
      case 't': output.push_back('\t'); break;
      case 'u': {
        if (position + 4 > json.size()) return std::nullopt;
        std::uint32_t codepoint = 0;
        for (int digit = 0; digit < 4; ++digit) {
          const char hex = json[position++];
          codepoint <<= 4u;
          if (hex >= '0' && hex <= '9') codepoint |= hex - '0';
          else if (hex >= 'a' && hex <= 'f') codepoint |= hex - 'a' + 10;
          else if (hex >= 'A' && hex <= 'F') codepoint |= hex - 'A' + 10;
          else return std::nullopt;
        }
        append_utf8(output, codepoint);
        break;
      }
      default: return std::nullopt;
    }
  }
  return std::nullopt;
}

bool json_success(const std::string_view json) {
  return json.find("\"success\":true") != std::string_view::npos;
}

} // namespace

BrowserManager::BrowserManager(std::filesystem::path data_root)
    : data_root_(std::filesystem::absolute(std::move(data_root)).lexically_normal()) {}

void BrowserManager::remember_launch(
    const std::string& session, const std::filesystem::path& executable,
    const std::filesystem::path& profile, const bool headed) const {
  std::vector<std::string> arguments{
      "--executable-path", std::filesystem::absolute(executable).string(),
      "--profile", std::filesystem::absolute(profile).string()};
  if (headed)
    arguments.push_back("--headed");
  std::scoped_lock lock(launch_mutex_);
  launch_arguments_.insert_or_assign(safe_name(session), std::move(arguments));
}

std::vector<BrowserCandidate> BrowserManager::discover() const {
  std::vector<BrowserCandidate> result;
#if defined(_WIN32)
  const auto program_files = env("PROGRAMFILES");
  const auto program_files_x86 = env("PROGRAMFILES(X86)");
  const auto local = env("LOCALAPPDATA");
  add_if_present(result, "Google Chrome", program_files / "Google/Chrome/Application/chrome.exe");
  add_if_present(result, "Google Chrome", program_files_x86 / "Google/Chrome/Application/chrome.exe");
  add_if_present(result, "Google Chrome", local / "Google/Chrome/Application/chrome.exe");
  add_if_present(result, "Chromium", program_files / "Chromium/Application/chrome.exe");
  add_if_present(result, "Brave", program_files / "BraveSoftware/Brave-Browser/Application/brave.exe");
#elif defined(__APPLE__)
  add_if_present(result, "Google Chrome", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
  add_if_present(result, "Chromium", "/Applications/Chromium.app/Contents/MacOS/Chromium");
  add_if_present(result, "Brave", "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser");
#else
  for (const auto* path : {"/usr/bin/google-chrome", "/usr/bin/google-chrome-stable",
                           "/usr/bin/chromium", "/usr/bin/chromium-browser",
                           "/usr/bin/brave-browser"})
    add_if_present(result, std::filesystem::path(path).filename().string(), path);
#endif
  return result;
}

std::filesystem::path BrowserManager::profile_path(const std::string& session) const {
  const auto safe = safe_name(session);
  return data_root_ / "browser" / "profiles" / safe;
}

BrowserRuntimeStatus BrowserManager::runtime_status() const {
  BrowserRuntimeStatus result;
  result.version = std::string(runtime_version);
  result.executable = runtime_executable(data_root_);
  const auto bytes = read_binary(result.executable);
  if (!bytes) {
    result.error = "agent-browser runtime is not installed";
    return result;
  }
  const auto expected = runtime_asset().sha256;
  const auto digest = tokmon::sha256_hex(std::span<const std::uint8_t>(*bytes));
  if (digest != expected) {
    result.error = "agent-browser runtime checksum mismatch";
    return result;
  }
  result.installed = true;
  return result;
}

bool BrowserManager::install_runtime(std::string& error) const {
  if (runtime_status().installed)
    return true;
  const auto asset = runtime_asset();
  const auto destination = runtime_executable(data_root_);
  auto temporary = destination;
  temporary += ".download";
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    error = "cannot create browser runtime directory: " + ec.message();
    return false;
  }
  const std::string url = "https://github.com/vercel-labs/agent-browser/releases/download/v" +
                          std::string(runtime_version) + "/" + asset.name;
  auto download = tokmon::builtin::run_process(
      {"curl", "--proto", "=https", "--tlsv1.2", "--fail", "--location",
       "--silent", "--show-error", "--output", temporary.string(), url},
      destination.parent_path(), std::chrono::minutes(3), 256u * 1024u);
  if (!download || download->exit_code != 0) {
    std::filesystem::remove(temporary, ec);
    error = download ? download->stderr_text : download.error().describe();
    return false;
  }
  const auto bytes = read_binary(temporary);
  if (!bytes || tokmon::sha256_hex(std::span<const std::uint8_t>(*bytes)) !=
                    asset.sha256) {
    std::filesystem::remove(temporary, ec);
    error = "downloaded agent-browser runtime failed SHA-256 verification";
    return false;
  }
#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    ec = std::error_code(static_cast<int>(GetLastError()),
                         std::system_category());
#else
  std::filesystem::rename(temporary, destination, ec);
#endif
  if (ec) {
    error = "cannot install verified agent-browser runtime: " + ec.message();
    return false;
  }
#if !defined(_WIN32)
  std::filesystem::permissions(
      destination,
      std::filesystem::perms::owner_exec |
          std::filesystem::perms::group_exec |
          std::filesystem::perms::others_exec,
      std::filesystem::perm_options::add, ec);
#endif
  std::ofstream manifest(destination.parent_path() / "runtime.manifest",
                         std::ios::binary | std::ios::trunc);
  manifest << "version=" << runtime_version << "\nsha256=" << asset.sha256 << '\n';
  return true;
}

bool BrowserManager::run(const std::string& session,
                         std::vector<std::string> command,
                         std::string& output, std::string& error,
                         const std::chrono::milliseconds timeout) const {
  const auto status = runtime_status();
  if (!status.installed) {
    error = status.error;
    return false;
  }
  const auto runtime = status.executable.parent_path();
  const auto config = runtime / "tokmon-agent-browser.json";
  if (!std::filesystem::exists(config)) {
    std::ofstream safe_config(config, std::ios::binary | std::ios::trunc);
    safe_config << "{}\n";
  }
  std::vector<std::string> argv{status.executable.string(), "--config",
                                config.string(), "--namespace", "tokmon-desk",
                                "--session", safe_name(session), "--json"};
  {
    std::scoped_lock lock(launch_mutex_);
    const auto found = launch_arguments_.find(safe_name(session));
    if (found != launch_arguments_.end())
      argv.insert(argv.end(), found->second.begin(), found->second.end());
  }
  // agent-browser includes launch options in its daemon reuse hash. Supplying
  // the same options on every command keeps navigation, inspection and input
  // attached to the exact Chrome tab opened for this Tokmon session.
  argv.push_back("--pin-tab");
  argv.insert(argv.end(), std::make_move_iterator(command.begin()),
              std::make_move_iterator(command.end()));
  tokmon::builtin::ProcessRequest request;
  request.argv = std::move(argv);
  request.cwd = runtime;
  request.timeout = timeout;
  request.max_output_bytes = 8u * 1024u * 1024u;
  request.allow_background_children = true;
  const auto result = tokmon::builtin::run_process(std::move(request));
  if (!result) {
    error = result.error().describe();
    return false;
  }
  output = result->stdout_text;
  if (result->exit_code != 0) {
    error = result->stderr_text.empty() ? output : result->stderr_text;
    return false;
  }
  if (!json_success(output)) {
    error = json_string_field(output, "error").value_or(
        output.empty() ? "agent-browser returned an invalid response" : output);
    return false;
  }
  return true;
}

bool BrowserManager::launch_agent_browser(const std::filesystem::path& executable,
                                          const std::string& session, bool headed,
                                          std::string& error) const {
  std::error_code ec;
  const auto profile = profile_path(session);
  std::filesystem::create_directories(profile, ec);
  if (ec) {
    error = "cannot create Tokmon browser profile: " + ec.message();
    return false;
  }
  remember_launch(session, executable, profile, headed);
  std::string output;
  return run(session, {"open", "about:blank"}, output, error,
             std::chrono::seconds(45));
}

BrowserSessionState BrowserManager::open(
    const std::filesystem::path& executable, const std::string& session,
    std::string url, const bool headed) const {
  BrowserSessionState state;
  state.session = safe_name(session);
  if (!allowed_url(url)) {
    state.error = "only http://, https://, and about:blank URLs are allowed";
    return state;
  }
  std::error_code ec;
  const auto profile = profile_path(state.session);
  std::filesystem::create_directories(profile, ec);
  if (ec) {
    state.error = "cannot create isolated browser profile: " + ec.message();
    return state;
  }
  remember_launch(state.session, executable, profile, headed);
  std::string output;
  if (!run(state.session, {"open", std::move(url)}, output, state.error,
           std::chrono::seconds(45)))
    return state;
  // The native command returns when navigation has been issued. Give the
  // document a short, browser-side settle point before snapshot/screenshot so
  // a newly launched Chrome does not expose its initial about:blank page.
  if (!run(state.session, {"wait", "300"}, output, state.error,
           std::chrono::seconds(10)))
    return state;
  return refresh(state.session);
}

BrowserSessionState BrowserManager::refresh(const std::string& session) const {
  BrowserSessionState state;
  state.session = safe_name(session);
  std::string output;
  if (!run(state.session, {"get", "url"}, output, state.error))
    return state;
  state.url = json_string_field(output, "url").value_or(std::string{});
  if (!run(state.session, {"get", "title"}, output, state.error))
    return state;
  state.title = json_string_field(output, "title").value_or(std::string{});
  if (!run(state.session, {"snapshot", "--compact"},
           output, state.error))
    return state;
  state.accessibility_snapshot =
      json_string_field(output, "snapshot").value_or(std::string{});
  const auto preview_directory = data_root_ / "browser" / "previews";
  std::error_code ec;
  std::filesystem::create_directories(preview_directory, ec);
  // RmlUi keeps the currently displayed image mapped on Windows. Reusing the
  // same path makes Chromium's next screenshot fail with ERROR_USER_MAPPED_FILE
  // (1224), so every refresh publishes an immutable preview asset.
  static std::atomic<std::uint64_t> preview_sequence{0};
  const auto generation =
      std::chrono::system_clock::now().time_since_epoch().count();
  state.preview_image = preview_directory /
      (state.session + "-" + std::to_string(generation) + "-" +
       std::to_string(preview_sequence.fetch_add(1, std::memory_order_relaxed)) +
       ".png");
  if (!run(state.session, {"screenshot", state.preview_image.string()}, output,
           state.error))
    return state;
  state.running = true;
  return state;
}

BrowserSessionState BrowserManager::navigate_command(
    const std::string& session, std::string command) const {
  BrowserSessionState state;
  state.session = safe_name(session);
  std::string output;
  if (!run(state.session, {std::move(command)}, output, state.error))
    return state;
  if (!run(state.session, {"wait", "150"}, output, state.error,
           std::chrono::seconds(10)))
    return state;
  return refresh(state.session);
}

BrowserSessionState BrowserManager::back(const std::string& session) const {
  return navigate_command(session, "back");
}

BrowserSessionState BrowserManager::forward(const std::string& session) const {
  return navigate_command(session, "forward");
}

BrowserSessionState BrowserManager::reload(const std::string& session) const {
  return navigate_command(session, "reload");
}

bool BrowserManager::click(const std::string& session,
                           const std::string& selector,
                           std::string& error) const {
  if (selector.empty()) { error = "browser selector is empty"; return false; }
  std::string output;
  return run(session, {"click", selector}, output, error);
}

bool BrowserManager::fill(const std::string& session,
                          const std::string& selector,
                          const std::string& value,
                          std::string& error) const {
  if (selector.empty()) { error = "browser selector is empty"; return false; }
  std::string output;
  return run(session, {"fill", selector, value}, output, error);
}

bool BrowserManager::close(const std::string& session,
                           std::string& error) const {
  std::string output;
  return run(session, {"close"}, output, error, std::chrono::seconds(15));
}

} // namespace tokmon::desk
