#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
std::filesystem::path executable_directory(const char* argv0) {
  std::error_code error;
  auto path = std::filesystem::absolute(argv0, error);
  return error ? std::filesystem::current_path() : path.parent_path();
}
}

int main(int argc, char** argv) {
  const auto directory = executable_directory(argv[0]);
  const auto daemon = directory / "tokmond.exe";
  const auto desktop = directory / "tokmon-desktop.exe";
  if (argc > 1 && std::string(argv[1]) == "--check") {
    const bool valid = std::filesystem::exists(daemon) && std::filesystem::exists(desktop);
    std::cout << "tokmond=" << daemon.string() << "\ndesktop=" << desktop.string() << '\n';
    return valid ? 0 : 1;
  }
#if defined(_WIN32)
  STARTUPINFOW daemon_start{sizeof(daemon_start)};
  PROCESS_INFORMATION daemon_process{};
  std::wstring daemon_command = L"\"" + daemon.wstring() + L"\"";
  if (!CreateProcessW(nullptr, daemon_command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, directory.c_str(), &daemon_start,
                      &daemon_process)) {
    std::cerr << "failed to start tokmond\n"; return 2;
  }
  CloseHandle(daemon_process.hThread);
  STARTUPINFOW desktop_start{sizeof(desktop_start)};
  PROCESS_INFORMATION desktop_process{};
  std::wstring desktop_command = L"\"" + desktop.wstring() + L"\"";
  if (!CreateProcessW(nullptr, desktop_command.data(), nullptr, nullptr, FALSE,
                      0, nullptr, directory.c_str(), &desktop_start, &desktop_process)) {
    TerminateProcess(daemon_process.hProcess, 1);
    CloseHandle(daemon_process.hProcess);
    std::cerr << "failed to start tokmon-desktop\n"; return 2;
  }
  CloseHandle(desktop_process.hThread);
  WaitForSingleObject(desktop_process.hProcess, INFINITE);
  CloseHandle(desktop_process.hProcess);
  TerminateProcess(daemon_process.hProcess, 0);
  CloseHandle(daemon_process.hProcess);
  return 0;
#else
  std::cerr << "launcher handoff is currently available on Windows; run tokmond and "
               "tokmon-desktop directly on this platform\n";
  return 2;
#endif
}

