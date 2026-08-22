#include "lenses/common/pty_session.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace tokmon::builtin {
namespace {

void append_ring(std::string& destination, const char* data, const std::size_t size,
                 const std::size_t limit, bool& truncated) {
  if (size >= limit) {
    destination.assign(data + size - limit, limit);
    truncated = true;
    return;
  }
  if (destination.size() + size > limit) {
    destination.erase(0, destination.size() + size - limit);
    truncated = true;
  }
  destination.append(data, size);
}

#if defined(_WIN32)
std::wstring widen(const std::string_view text) {
  if (text.empty()) return {};
  const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size);
  return result;
}

std::string narrow(const std::wstring_view text) {
  if (text.empty()) return {};
  const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0,
                                        nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
  return result;
}

std::wstring quote(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
    return argument;
  std::wstring result(1, L'\"');
  std::size_t slashes = 0;
  for (const auto character : argument) {
    if (character == L'\\') ++slashes;
    else if (character == L'\"') {
      result.append(slashes * 2u + 1u, L'\\');
      result.push_back(L'\"');
      slashes = 0;
    } else {
      result.append(slashes, L'\\');
      slashes = 0;
      result.push_back(character);
    }
  }
  result.append(slashes * 2u, L'\\');
  result.push_back(L'\"');
  return result;
}

void close_handle(HANDLE& handle) {
  if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  handle = nullptr;
}
#endif

}  // namespace

struct PtySession::Impl {
  PtyOptions options;
  std::mutex mutex;
  std::string output;
  bool truncated{false};
  bool running{true};
  int exit_code{-1};
  std::chrono::steady_clock::time_point last_activity{std::chrono::steady_clock::now()};
  std::jthread reader;
  std::jthread watchdog;
#if defined(_WIN32)
  HPCON console{nullptr};
  HANDLE input{nullptr};
  HANDLE output_pipe{nullptr};
  HANDLE process{nullptr};
  HANDLE job{nullptr};
#else
  int master{-1};
  pid_t process{-1};
#endif
};

PtySession::PtySession(std::unique_ptr<Impl> implementation)
    : impl_(std::move(implementation)) {}

PtySession::~PtySession() {
  if (!impl_) return;
  (void)close(std::chrono::milliseconds(100));
}

Result<std::shared_ptr<PtySession>> PtySession::open(PtyOptions options) {
  if (options.argv.empty() || options.argv.front().empty() ||
      options.columns == 0 || options.rows == 0 || options.max_output_bytes == 0)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "PTY argv, dimensions and output bound are required"));
  std::error_code error;
  options.cwd = std::filesystem::weakly_canonical(options.cwd, error);
  if (error || !std::filesystem::is_directory(options.cwd))
    return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
                                     "PTY cwd is not an existing directory"));
  auto implementation = std::make_unique<Impl>();
  implementation->options = std::move(options);
#if defined(_WIN32)
  // Windows process initialization needs its system directory even when user
  // environment inheritance is disabled.  This fixed bootstrap contains no
  // user data or credentials and remains visible in the reported SandboxPlan.
  if (!implementation->options.environment.contains("SystemRoot")) {
    std::array<wchar_t, MAX_PATH + 1> windows_directory{};
    const auto length = GetWindowsDirectoryW(windows_directory.data(),
                                             static_cast<UINT>(windows_directory.size()));
    if (length != 0 && length < windows_directory.size()) {
      const auto root = narrow(std::wstring_view(windows_directory.data(), length));
      implementation->options.environment["SystemRoot"] = root;
      implementation->options.environment["WINDIR"] = root;
    }
  }
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE input_read = nullptr;
  HANDLE output_write = nullptr;
  if (!CreatePipe(&input_read, &implementation->input, &security, 0) ||
      !CreatePipe(&implementation->output_pipe, &output_write, &security, 0)) {
    close_handle(input_read); close_handle(output_write);
    close_handle(implementation->input); close_handle(implementation->output_pipe);
    return tl::unexpected(make_error(ErrorCode::io_error, "cannot create ConPTY pipes"));
  }
  SetHandleInformation(implementation->input, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(implementation->output_pipe, HANDLE_FLAG_INHERIT, 0);
  const COORD dimensions{static_cast<SHORT>(implementation->options.columns),
                         static_cast<SHORT>(implementation->options.rows)};
  const auto console_result = CreatePseudoConsole(dimensions, input_read, output_write, 0,
                                                   &implementation->console);
  if (FAILED(console_result)) {
    close_handle(input_read); close_handle(output_write);
    return tl::unexpected(make_error(ErrorCode::unsupported,
                                     "Windows ConPTY is unavailable"));
  }

  SIZE_T attribute_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  std::vector<std::byte> attribute_storage(attribute_size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) ||
      !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                 implementation->console, sizeof(implementation->console),
                                 nullptr, nullptr)) {
    DeleteProcThreadAttributeList(attributes);
    ClosePseudoConsole(implementation->console);
    implementation->console = nullptr;
    close_handle(input_read); close_handle(output_write);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot initialize ConPTY process attributes"));
  }
  std::wstring command;
  for (const auto& argument : implementation->options.argv) {
    if (!command.empty()) command.push_back(L' ');
    const auto value = widen(argument);
    if (value.empty() && !argument.empty()) {
      DeleteProcThreadAttributeList(attributes);
      close_handle(input_read); close_handle(output_write);
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "PTY argv contains invalid UTF-8"));
    }
    command += quote(value);
  }
  std::vector<wchar_t> command_buffer(command.begin(), command.end());
  command_buffer.push_back(L'\0');
  std::vector<wchar_t> environment;
  for (const auto& [name, value] : implementation->options.environment) {
    const auto item = widen(name + "=" + value);
    environment.insert(environment.end(), item.begin(), item.end());
    environment.push_back(L'\0');
  }
  environment.push_back(L'\0');
  if (environment.size() == 1u) environment.push_back(L'\0');
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  // Explicitly clear inherited standard handles.  This is required when the
  // host itself is running with redirected stdout/stderr (CTest, services):
  // ConPTY supplies the actual terminal handles through the process attribute.
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = nullptr;
  startup.StartupInfo.hStdOutput = nullptr;
  startup.StartupInfo.hStdError = nullptr;
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process{};
  const auto created = CreateProcessW(nullptr, command_buffer.data(), nullptr, nullptr, FALSE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
      environment.data(), implementation->options.cwd.wstring().c_str(),
      &startup.StartupInfo, &process);
  DeleteProcThreadAttributeList(attributes);
  close_handle(input_read); close_handle(output_write);
  if (!created) {
    ClosePseudoConsole(implementation->console);
    implementation->console = nullptr;
    return tl::unexpected(make_error(ErrorCode::io_error,
        "cannot start ConPTY process: " + std::to_string(GetLastError())));
  }
  implementation->process = process.hProcess;
  implementation->job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (implementation->options.max_memory_bytes != 0) {
    limits.ProcessMemoryLimit = implementation->options.max_memory_bytes;
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
  }
  if (implementation->options.max_processes != 0) {
    limits.BasicLimitInformation.ActiveProcessLimit = static_cast<DWORD>(
        std::min<std::size_t>(implementation->options.max_processes, MAXDWORD));
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
  }
  if (implementation->options.max_cpu_time > std::chrono::milliseconds::zero()) {
    limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
        implementation->options.max_cpu_time.count() * 10'000;
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
  }
  if (!implementation->job ||
      !SetInformationJobObject(implementation->job, JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits)) ||
      !AssignProcessToJobObject(implementation->job, implementation->process)) {
    TerminateProcess(implementation->process, 1);
    return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
                                     "cannot contain ConPTY in a Job Object"));
  }
  ResumeThread(process.hThread);
  close_handle(process.hThread);
#else
  winsize dimensions{implementation->options.rows, implementation->options.columns, 0, 0};
  const auto child = ::forkpty(&implementation->master, nullptr, nullptr, &dimensions);
  if (child < 0)
    return tl::unexpected(make_error(ErrorCode::io_error, "forkpty failed"));
  if (child == 0) {
    ::setpgid(0, 0);
    ::chdir(implementation->options.cwd.c_str());
    ::clearenv();
    for (const auto& [name, value] : implementation->options.environment)
      ::setenv(name.c_str(), value.c_str(), 1);
    if (implementation->options.max_memory_bytes != 0) {
      const rlimit limit{implementation->options.max_memory_bytes,
                         implementation->options.max_memory_bytes};
      ::setrlimit(RLIMIT_AS, &limit);
    }
    if (implementation->options.max_cpu_time > std::chrono::milliseconds::zero()) {
      const auto seconds = static_cast<rlim_t>(std::max<std::int64_t>(
          1, (implementation->options.max_cpu_time.count() + 999) / 1000));
      const rlimit limit{seconds, seconds};
      ::setrlimit(RLIMIT_CPU, &limit);
    }
    std::vector<char*> arguments;
    for (auto& argument : implementation->options.argv) arguments.push_back(argument.data());
    arguments.push_back(nullptr);
    ::execvp(arguments.front(), arguments.data());
    _exit(127);
  }
  implementation->process = child;
  ::fcntl(implementation->master, F_SETFL, O_NONBLOCK);
#endif
  auto session = std::shared_ptr<PtySession>(new PtySession(std::move(implementation)));
  auto* state = session->impl_.get();
  state->reader = std::jthread([state](std::stop_token stop) {
    std::array<char, 4096> buffer{};
    while (!stop.stop_requested()) {
#if defined(_WIN32)
      DWORD read = 0;
      if (!ReadFile(state->output_pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                    &read, nullptr) || read == 0) break;
      const auto count = static_cast<std::size_t>(read);
#else
      const auto read = ::read(state->master, buffer.data(), buffer.size());
      if (read < 0 && (errno == EAGAIN || errno == EINTR)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      if (read <= 0) break;
      const auto count = static_cast<std::size_t>(read);
#endif
      std::scoped_lock lock(state->mutex);
      append_ring(state->output, buffer.data(), count,
                  state->options.max_output_bytes, state->truncated);
      state->last_activity = std::chrono::steady_clock::now();
    }
  });
  state->watchdog = std::jthread([state](std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::scoped_lock lock(state->mutex);
      if (!state->running) return;
      if (state->options.idle_timeout <= std::chrono::milliseconds::zero() ||
          std::chrono::steady_clock::now() - state->last_activity <
              state->options.idle_timeout) continue;
#if defined(_WIN32)
      TerminateJobObject(state->job, 124u);
#else
      ::kill(-state->process, SIGKILL);
#endif
      return;
    }
  });
  return session;
}

Result<void> PtySession::write(const std::string_view input) {
  if (input.find('\0') != std::string_view::npos)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "PTY input must be NUL-free UTF-8"));
  std::scoped_lock lock(impl_->mutex);
  if (!impl_->running)
    return tl::unexpected(make_error(ErrorCode::invalid_state, "PTY session has exited"));
#if defined(_WIN32)
  DWORD written = 0;
  if (!WriteFile(impl_->input, input.data(), static_cast<DWORD>(input.size()), &written, nullptr) ||
      written != input.size())
#else
  const auto written = ::write(impl_->master, input.data(), input.size());
  if (written < 0 || static_cast<std::size_t>(written) != input.size())
#endif
    return tl::unexpected(make_error(ErrorCode::io_error, "cannot write PTY input"));
  impl_->last_activity = std::chrono::steady_clock::now();
  return {};
}

Result<void> PtySession::resize(const std::uint16_t columns, const std::uint16_t rows) {
  if (columns == 0 || rows == 0)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "PTY dimensions must be positive"));
#if defined(_WIN32)
  if (FAILED(ResizePseudoConsole(impl_->console,
          COORD{static_cast<SHORT>(columns), static_cast<SHORT>(rows)})))
#else
  winsize dimensions{rows, columns, 0, 0};
  if (::ioctl(impl_->master, TIOCSWINSZ, &dimensions) != 0)
#endif
    return tl::unexpected(make_error(ErrorCode::io_error, "cannot resize PTY"));
  impl_->options.columns = columns;
  impl_->options.rows = rows;
  return {};
}

PtySnapshot PtySession::take_output() {
  std::scoped_lock lock(impl_->mutex);
#if defined(_WIN32)
  if (impl_->running && WaitForSingleObject(impl_->process, 0) == WAIT_OBJECT_0) {
    DWORD exit_code = 1;
    GetExitCodeProcess(impl_->process, &exit_code);
    impl_->exit_code = static_cast<int>(exit_code);
    impl_->running = false;
  }
#else
  if (impl_->running) {
    int status = 0;
    if (::waitpid(impl_->process, &status, WNOHANG) == impl_->process) {
      impl_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
      impl_->running = false;
    }
  }
#endif
  PtySnapshot snapshot{.output = std::move(impl_->output), .truncated = impl_->truncated,
      .running = impl_->running, .exit_code = impl_->exit_code,
#if defined(_WIN32)
      .sandbox_strength = "windows-conpty/job-object"
#else
      .sandbox_strength = "unix-pty/process-group"
#endif
  };
  impl_->output.clear();
  impl_->truncated = false;
  return snapshot;
}

Result<PtySnapshot> PtySession::close(const std::chrono::milliseconds grace) {
  if (!impl_) return PtySnapshot{};
  {
    std::scoped_lock lock(impl_->mutex);
#if defined(_WIN32)
    if (impl_->running && WaitForSingleObject(impl_->process, 0) == WAIT_OBJECT_0) {
      DWORD exit_code = 1;
      GetExitCodeProcess(impl_->process, &exit_code);
      impl_->exit_code = static_cast<int>(exit_code);
      impl_->running = false;
    }
#endif
    if (impl_->running) {
#if defined(_WIN32)
      close_handle(impl_->input);
#else
      ::kill(-impl_->process, SIGHUP);
#endif
    }
  }
  const auto deadline = std::chrono::steady_clock::now() + std::max(grace,
      std::chrono::milliseconds::zero());
  while (std::chrono::steady_clock::now() < deadline) {
#if defined(_WIN32)
    if (WaitForSingleObject(impl_->process, 0) == WAIT_OBJECT_0) break;
#else
    int status = 0;
    if (::waitpid(impl_->process, &status, WNOHANG) == impl_->process) {
      std::scoped_lock lock(impl_->mutex);
      impl_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
      impl_->running = false;
      break;
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->running) {
#if defined(_WIN32)
      TerminateJobObject(impl_->job, 125u);
      WaitForSingleObject(impl_->process, 5'000);
      DWORD exit_code = 125;
      GetExitCodeProcess(impl_->process, &exit_code);
      impl_->exit_code = static_cast<int>(exit_code);
#else
      ::kill(-impl_->process, SIGKILL);
      int status = 0;
      ::waitpid(impl_->process, &status, 0);
      impl_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
      impl_->running = false;
    }
  }
  impl_->reader.request_stop();
  impl_->watchdog.request_stop();
#if defined(_WIN32)
  if (impl_->console) ClosePseudoConsole(impl_->console);
  impl_->console = nullptr;
  close_handle(impl_->output_pipe); close_handle(impl_->process); close_handle(impl_->job);
#else
  if (impl_->master >= 0) ::close(impl_->master);
  impl_->master = -1;
#endif
  if (impl_->reader.joinable()) impl_->reader.join();
  if (impl_->watchdog.joinable()) impl_->watchdog.join();
  return take_output();
}

}  // namespace tokmon::builtin
