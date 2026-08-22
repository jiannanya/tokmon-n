#include "lenses/common/process_runner.hpp"

#include <algorithm>
#include <array>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace tokmon::builtin {
namespace {

void append_bounded(std::string& destination, const char* data,
                    const std::size_t size, const std::size_t limit,
                    bool& truncated) {
  const auto remaining = destination.size() < limit ? limit - destination.size() : 0u;
  const auto copied = std::min(size, remaining);
  destination.append(data, copied);
  truncated = truncated || copied != size;
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
                bool& truncated) {
  std::array<char, 4096> buffer{};
  while (true) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0)
      return;
    DWORD read = 0;
    const auto requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
    if (!ReadFile(pipe, buffer.data(), requested, &read, nullptr) || read == 0) return;
    append_bounded(destination, buffer.data(), read, limit, truncated);
  }
}

#else

void drain_fd(const int fd, std::string& destination, const std::size_t limit,
              bool& truncated) {
  std::array<char, 4096> buffer{};
  while (true) {
    const auto read_size = ::read(fd, buffer.data(), buffer.size());
    if (read_size > 0) {
      append_bounded(destination, buffer.data(), static_cast<std::size_t>(read_size),
                     limit, truncated);
      continue;
    }
    if (read_size < 0 && errno == EINTR) continue;
    return;
  }
}

#endif

}  // namespace

Result<ProcessOutput> run_process(const std::vector<std::string>& argv,
                                  const std::filesystem::path& cwd,
                                  const std::chrono::milliseconds timeout,
                                  const std::size_t max_output_bytes,
                                  const std::stop_token stop) {
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
  if (!CreatePipe(&stdout_read, &stdout_write, &security, 0) ||
      !CreatePipe(&stderr_read, &stderr_write, &security, 0)) {
    close_handle(stdout_read); close_handle(stdout_write);
    close_handle(stderr_read); close_handle(stderr_write);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot create process output pipes"));
  }
  SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

  std::wstring command;
  for (const auto& item : argv) {
    if (!command.empty()) command.push_back(L' ');
    const auto wide = widen(item);
    if (wide.empty() && !item.empty()) {
      close_handle(stdout_read); close_handle(stdout_write);
      close_handle(stderr_read); close_handle(stderr_write);
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "argv contains invalid UTF-8"));
    }
    command.append(quote_windows_argument(wide));
  }
  std::vector<wchar_t> command_buffer(command.begin(), command.end());
  command_buffer.push_back(L'\0');
  const auto wide_cwd = canonical_cwd.wstring();
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = stdout_write;
  startup.hStdError = stderr_write;
  PROCESS_INFORMATION process{};
  const auto created = CreateProcessW(nullptr, command_buffer.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, wide_cwd.c_str(), &startup, &process);
  close_handle(stdout_write); close_handle(stderr_write);
  if (!created) {
    close_handle(stdout_read); close_handle(stderr_read);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "CreateProcessW failed: " + std::to_string(GetLastError())));
  }

  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) {
    TerminateProcess(process.hProcess, 1);
    close_handle(process.hThread); close_handle(process.hProcess);
    close_handle(stdout_read); close_handle(stderr_read);
    return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
                                     "cannot create Windows Job Object"));
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits)) ||
      !AssignProcessToJobObject(job, process.hProcess)) {
    TerminateProcess(process.hProcess, 1);
    close_handle(job); close_handle(process.hThread); close_handle(process.hProcess);
    close_handle(stdout_read); close_handle(stderr_read);
    return tl::unexpected(make_error(ErrorCode::sandbox_rejected,
                                     "cannot contain process in Windows Job Object"));
  }
  ResumeThread(process.hThread);
  close_handle(process.hThread);
  output.sandbox_strength = "windows-job-object/process-tree";
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (WaitForSingleObject(process.hProcess, 10) == WAIT_TIMEOUT) {
    drain_pipe(stdout_read, output.stdout_text, max_output_bytes, output.stdout_truncated);
    drain_pipe(stderr_read, output.stderr_text, max_output_bytes, output.stderr_truncated);
    if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
      output.cancelled = stop.stop_requested();
      output.timed_out = !output.cancelled;
      TerminateJobObject(job, output.timed_out ? 124u : 125u);
      WaitForSingleObject(process.hProcess, 5'000);
      break;
    }
  }
  drain_pipe(stdout_read, output.stdout_text, max_output_bytes, output.stdout_truncated);
  drain_pipe(stderr_read, output.stderr_text, max_output_bytes, output.stderr_truncated);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  output.exit_code = static_cast<int>(exit_code);
  close_handle(job); close_handle(process.hProcess);
  close_handle(stdout_read); close_handle(stderr_read);
#else
  int stdout_pipe[2]{}; int stderr_pipe[2]{};
  if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0)
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
    ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
    ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
    std::vector<char*> arguments;
    arguments.reserve(argv.size() + 1u);
    for (const auto& item : argv) arguments.push_back(const_cast<char*>(item.c_str()));
    arguments.push_back(nullptr);
    ::execvp(arguments.front(), arguments.data());
    _exit(127);
  }
  ::close(stdout_pipe[1]); ::close(stderr_pipe[1]);
  ::fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
  ::fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);
  output.sandbox_strength = "posix-process-group/process-tree";
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (::waitpid(child, &status, WNOHANG) == 0) {
    drain_fd(stdout_pipe[0], output.stdout_text, max_output_bytes, output.stdout_truncated);
    drain_fd(stderr_pipe[0], output.stderr_text, max_output_bytes, output.stderr_truncated);
    if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
      output.cancelled = stop.stop_requested();
      output.timed_out = !output.cancelled;
      ::kill(-child, SIGKILL);
      ::waitpid(child, &status, 0);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  drain_fd(stdout_pipe[0], output.stdout_text, max_output_bytes, output.stdout_truncated);
  drain_fd(stderr_pipe[0], output.stderr_text, max_output_bytes, output.stderr_truncated);
  ::close(stdout_pipe[0]); ::close(stderr_pipe[0]);
  output.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
  return output;
}

}  // namespace tokmon::builtin
