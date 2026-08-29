#include "lenses/common/process_runner.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace tokmon::builtin {
namespace {

void append_bounded(std::string& destination, const char* data,
                    const std::size_t size, const std::size_t limit,
                    bool& truncated,
                    const std::function<void(std::string_view)>& observer) {
  if (observer && size != 0u) observer(std::string_view(data, size));
  if (size >= limit) {
    destination.assign(data + (size - limit), limit);
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

std::wstring widen(const std::string& text) {
  if (text.empty()) return {};
  const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size);
  return result;
}

std::wstring quote_windows_argument(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
    return argument;
  std::wstring quoted(1, L'\"');
  std::size_t backslashes = 0;
  for (const auto character : argument) {
    if (character == L'\\') {
      ++backslashes;
    } else if (character == L'\"') {
      quoted.append(backslashes * 2u + 1u, L'\\');
      quoted.push_back(L'\"');
      backslashes = 0;
    } else {
      quoted.append(backslashes, L'\\');
      backslashes = 0;
      quoted.push_back(character);
    }
  }
  quoted.append(backslashes * 2u, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}

void close_handle(HANDLE& handle) {
  if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  handle = nullptr;
}

void drain_pipe(HANDLE pipe, std::string& destination, const std::size_t limit,
                bool& truncated,
                const std::function<void(std::string_view)>& observer) {
  std::array<char, 4096> buffer{};
  while (true) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0)
      return;
    DWORD read = 0;
    const auto requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
    if (!ReadFile(pipe, buffer.data(), requested, &read, nullptr) || read == 0) return;
    append_bounded(destination, buffer.data(), read, limit, truncated, observer);
  }
}

#else

void drain_fd(const int fd, std::string& destination, const std::size_t limit,
              bool& truncated,
              const std::function<void(std::string_view)>& observer) {
  std::array<char, 4096> buffer{};
  while (true) {
    const auto read_size = ::read(fd, buffer.data(), buffer.size());
    if (read_size > 0) {
      append_bounded(destination, buffer.data(), static_cast<std::size_t>(read_size),
                     limit, truncated, observer);
      continue;
    }
    if (read_size < 0 && errno == EINTR) continue;
    return;
  }
}

#endif

}  // namespace

Result<ProcessOutput> run_process(ProcessRequest request) {
  const auto& argv = request.argv;
  const auto& cwd = request.cwd;
  const auto timeout = request.timeout;
  const auto max_output_bytes = request.max_output_bytes;
  const auto stop = request.stop;
  if (argv.empty() || argv.front().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "process argv must not be empty"));
  if (timeout <= std::chrono::milliseconds::zero() || max_output_bytes == 0)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "process limits must be positive"));
  std::error_code path_error;
  const auto canonical_cwd = std::filesystem::weakly_canonical(cwd, path_error);
  if (path_error || !std::filesystem::is_directory(canonical_cwd))
    return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
                                     "process cwd is not an existing directory"));

  ProcessOutput output;
#if defined(_WIN32)
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE stdout_read = nullptr; HANDLE stdout_write = nullptr;
  HANDLE stderr_read = nullptr; HANDLE stderr_write = nullptr;
  HANDLE stdin_read = nullptr; HANDLE stdin_write = nullptr;
  if (!CreatePipe(&stdout_read, &stdout_write, &security, 0) ||
      !CreatePipe(&stderr_read, &stderr_write, &security, 0) ||
      (!request.stdin_text.empty() &&
       !CreatePipe(&stdin_read, &stdin_write, &security, 0))) {
    close_handle(stdout_read); close_handle(stdout_write);
    close_handle(stderr_read); close_handle(stderr_write);
    close_handle(stdin_read); close_handle(stdin_write);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot create process output pipes"));
  }
  SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
  if (stdin_write) SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

  std::wstring command;
  for (const auto& item : argv) {
    if (!command.empty()) command.push_back(L' ');
    const auto wide = widen(item);
    if (wide.empty() && !item.empty()) {
      close_handle(stdout_read); close_handle(stdout_write);
      close_handle(stderr_read); close_handle(stderr_write);
      close_handle(stdin_read); close_handle(stdin_write);
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "argv contains invalid UTF-8"));
    }
    command.append(quote_windows_argument(wide));
  }
  std::vector<wchar_t> command_buffer(command.begin(), command.end());
  command_buffer.push_back(L'\0');
  std::vector<wchar_t> environment_block;
  if (!request.environment.empty() || !request.inherit_environment) {
    for (const auto& [key, value] : request.environment) {
      const auto item = widen(key + "=" + value);
      environment_block.insert(environment_block.end(), item.begin(), item.end());
      environment_block.push_back(L'\0');
    }
    environment_block.push_back(L'\0');
    if (environment_block.size() == 1u) environment_block.push_back(L'\0');
  }
  const auto wide_cwd = canonical_cwd.wstring();
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = stdin_read ? stdin_read : GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = stdout_write;
  startup.hStdError = stderr_write;
  PROCESS_INFORMATION process{};
  const auto created = CreateProcessW(nullptr, command_buffer.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED |
          CREATE_UNICODE_ENVIRONMENT,
      environment_block.empty() ? nullptr : environment_block.data(),
      wide_cwd.c_str(), &startup, &process);
  close_handle(stdout_write); close_handle(stderr_write);
  close_handle(stdin_read);
  if (!created) {
    close_handle(stdout_read); close_handle(stderr_read);
    close_handle(stdin_write);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "CreateProcessW failed: " + std::to_string(GetLastError())));
  }

  HANDLE job = nullptr;
  if (!request.allow_background_children) {
    job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
      TerminateProcess(process.hProcess, 1);
      close_handle(process.hThread); close_handle(process.hProcess);
      close_handle(stdout_read); close_handle(stderr_read);
      close_handle(stdin_write);
      return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
                                       "cannot create Windows Job Object"));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (request.max_memory_bytes != 0) {
      limits.ProcessMemoryLimit = request.max_memory_bytes;
      limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    }
    if (request.max_cpu_time > std::chrono::milliseconds::zero()) {
      limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
          request.max_cpu_time.count() * 10'000;
      limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
    }
    if (request.max_processes != 0) {
      limits.BasicLimitInformation.ActiveProcessLimit =
          static_cast<DWORD>(std::min<std::size_t>(request.max_processes, MAXDWORD));
      limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    }
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, process.hProcess)) {
      TerminateProcess(process.hProcess, 1);
      close_handle(job); close_handle(process.hThread); close_handle(process.hProcess);
      close_handle(stdout_read); close_handle(stderr_read);
      close_handle(stdin_write);
      return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
                                       "cannot contain process in Windows Job Object"));
    }
  }
  ResumeThread(process.hThread);
  close_handle(process.hThread);
  std::jthread stdin_writer;
  if (stdin_write) {
    const auto input = std::move(request.stdin_text);
    stdin_writer = std::jthread([stdin_write, input]() mutable {
      std::size_t offset = 0;
      while (offset < input.size()) {
        DWORD written = 0;
        const auto remaining = std::min<std::size_t>(
            input.size() - offset, static_cast<std::size_t>(64u * 1024u));
        if (!WriteFile(stdin_write, input.data() + offset,
                       static_cast<DWORD>(remaining), &written, nullptr) || written == 0)
          break;
        offset += written;
      }
      auto handle = stdin_write;
      close_handle(handle);
    });
    stdin_write = nullptr;
  }
  output.sandbox_strength = request.allow_background_children
      ? "windows-process/background-children-allowed"
      : "windows-job-object/process-tree";
  const auto started_at = std::chrono::steady_clock::now();
  auto last_output_at = started_at;
  auto previous_stdout_size = output.stdout_text.size();
  bool saw_stdout = false;
  const auto deadline = started_at + timeout;
  while (WaitForSingleObject(process.hProcess, 10) == WAIT_TIMEOUT) {
    drain_pipe(stdout_read, output.stdout_text, max_output_bytes, output.stdout_truncated,
               request.on_stdout);
    drain_pipe(stderr_read, output.stderr_text, max_output_bytes, output.stderr_truncated,
               request.on_stderr);
    const auto now = std::chrono::steady_clock::now();
    if (output.stdout_text.size() != previous_stdout_size) {
      previous_stdout_size = output.stdout_text.size(); saw_stdout = true; last_output_at = now;
    }
    const auto first_expired = !saw_stdout &&
        request.first_output_timeout > std::chrono::milliseconds::zero() &&
        now - started_at >= request.first_output_timeout;
    const auto idle_expired = saw_stdout &&
        request.idle_output_timeout > std::chrono::milliseconds::zero() &&
        now - last_output_at >= request.idle_output_timeout;
    if (stop.stop_requested() || now >= deadline || first_expired || idle_expired) {
      output.cancelled = stop.stop_requested();
      output.timed_out = !output.cancelled;
      if (first_expired) output.timeout_reason = "first-output";
      else if (idle_expired) output.timeout_reason = "idle-output";
      else if (output.timed_out) output.timeout_reason = "total";
      output.cooperative_stop_attempted = true;
      (void)GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process.dwProcessId);
      if (WaitForSingleObject(process.hProcess, 250) == WAIT_TIMEOUT) {
        output.forced_tree_termination = true;
        if (job)
          TerminateJobObject(job, output.timed_out ? 124u : 125u);
        else
          TerminateProcess(process.hProcess, output.timed_out ? 124u : 125u);
        WaitForSingleObject(process.hProcess, 5'000);
      }
      break;
    }
  }
  drain_pipe(stdout_read, output.stdout_text, max_output_bytes, output.stdout_truncated,
             request.on_stdout);
  drain_pipe(stderr_read, output.stderr_text, max_output_bytes, output.stderr_truncated,
             request.on_stderr);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  output.exit_code = static_cast<int>(exit_code);
  close_handle(job); close_handle(process.hProcess);
  close_handle(stdout_read); close_handle(stderr_read);
  close_handle(stdin_write);
#else
  int stdout_pipe[2]{}; int stderr_pipe[2]{}; int stdin_pipe[2]{-1, -1};
  if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0 ||
      (!request.stdin_text.empty() && ::pipe(stdin_pipe) != 0))
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot create process output pipes"));
  const auto child = ::fork();
  if (child < 0)
    return tl::unexpected(make_error(ErrorCode::io_error, "fork failed"));
  if (child == 0) {
    ::setpgid(0, 0);
    ::chdir(canonical_cwd.c_str());
    ::dup2(stdout_pipe[1], STDOUT_FILENO);
    ::dup2(stderr_pipe[1], STDERR_FILENO);
    if (stdin_pipe[0] >= 0) ::dup2(stdin_pipe[0], STDIN_FILENO);
    ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
    ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
    if (stdin_pipe[0] >= 0) {
      ::close(stdin_pipe[0]);
      ::close(stdin_pipe[1]);
    }
    if (!request.inherit_environment) ::clearenv();
    for (const auto& [key, value] : request.environment)
      ::setenv(key.c_str(), value.c_str(), 1);
    if (request.max_memory_bytes != 0) {
      const rlimit limit{static_cast<rlim_t>(request.max_memory_bytes),
                         static_cast<rlim_t>(request.max_memory_bytes)};
      ::setrlimit(RLIMIT_AS, &limit);
    }
    if (request.max_cpu_time > std::chrono::milliseconds::zero()) {
      const auto seconds = static_cast<rlim_t>(
          std::max<std::int64_t>(1, (request.max_cpu_time.count() + 999) / 1000));
      const rlimit limit{seconds, seconds};
      ::setrlimit(RLIMIT_CPU, &limit);
    }
#if defined(RLIMIT_NPROC)
    if (request.max_processes != 0) {
      const rlimit limit{static_cast<rlim_t>(request.max_processes),
                         static_cast<rlim_t>(request.max_processes)};
      ::setrlimit(RLIMIT_NPROC, &limit);
    }
#endif
    std::vector<char*> arguments;
    arguments.reserve(argv.size() + 1u);
    for (const auto& item : argv) arguments.push_back(const_cast<char*>(item.c_str()));
    arguments.push_back(nullptr);
    ::execvp(arguments.front(), arguments.data());
    _exit(127);
  }
  ::close(stdout_pipe[1]); ::close(stderr_pipe[1]);
  if (stdin_pipe[0] >= 0) ::close(stdin_pipe[0]);
  ::fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
  ::fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);
  std::jthread stdin_writer;
  if (stdin_pipe[1] >= 0) {
    const auto input = std::move(request.stdin_text);
    const auto write_fd = stdin_pipe[1];
    stdin_pipe[1] = -1;
    stdin_writer = std::jthread([write_fd, input]() {
      std::size_t offset = 0;
      while (offset < input.size()) {
        const auto written = ::write(write_fd, input.data() + offset, input.size() - offset);
        if (written > 0) offset += static_cast<std::size_t>(written);
        else if (written < 0 && errno == EINTR) continue;
        else break;
      }
      ::close(write_fd);
    });
  }
  output.sandbox_strength = "posix-process-group/process-tree";
  const auto started_at = std::chrono::steady_clock::now();
  auto last_output_at = started_at;
  auto previous_stdout_size = output.stdout_text.size();
  bool saw_stdout = false;
  const auto deadline = started_at + timeout;
  int status = 0;
  while (::waitpid(child, &status, WNOHANG) == 0) {
    drain_fd(stdout_pipe[0], output.stdout_text, max_output_bytes, output.stdout_truncated,
             request.on_stdout);
    drain_fd(stderr_pipe[0], output.stderr_text, max_output_bytes, output.stderr_truncated,
             request.on_stderr);
    const auto now = std::chrono::steady_clock::now();
    if (output.stdout_text.size() != previous_stdout_size) {
      previous_stdout_size = output.stdout_text.size(); saw_stdout = true; last_output_at = now;
    }
    const auto first_expired = !saw_stdout &&
        request.first_output_timeout > std::chrono::milliseconds::zero() &&
        now - started_at >= request.first_output_timeout;
    const auto idle_expired = saw_stdout &&
        request.idle_output_timeout > std::chrono::milliseconds::zero() &&
        now - last_output_at >= request.idle_output_timeout;
    if (stop.stop_requested() || now >= deadline || first_expired || idle_expired) {
      output.cancelled = stop.stop_requested();
      output.timed_out = !output.cancelled;
      if (first_expired) output.timeout_reason = "first-output";
      else if (idle_expired) output.timeout_reason = "idle-output";
      else if (output.timed_out) output.timeout_reason = "total";
      output.cooperative_stop_attempted = true;
      (void)::kill(-child, SIGTERM);
      const auto grace_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(250);
      bool exited = false;
      while (std::chrono::steady_clock::now() < grace_deadline) {
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) { exited = true; break; }
        if (waited < 0 && errno != EINTR) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (!exited) {
        output.forced_tree_termination = true;
        (void)::kill(-child, SIGKILL);
        (void)::waitpid(child, &status, 0);
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  drain_fd(stdout_pipe[0], output.stdout_text, max_output_bytes, output.stdout_truncated,
           request.on_stdout);
  drain_fd(stderr_pipe[0], output.stderr_text, max_output_bytes, output.stderr_truncated,
           request.on_stderr);
  ::close(stdout_pipe[0]); ::close(stderr_pipe[0]);
  output.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
  return output;
}

Result<ProcessOutput> run_process(const std::vector<std::string>& argv,
                                  const std::filesystem::path& cwd,
                                  const std::chrono::milliseconds timeout,
                                  const std::size_t max_output_bytes,
                                  const std::stop_token stop) {
  return run_process(ProcessRequest{.argv = argv, .cwd = cwd, .timeout = timeout,
      .max_output_bytes = max_output_bytes, .stop = stop});
}

}  // namespace tokmon::builtin
