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
  STARTUPINFOW desktop_start{sizeof(desktop_start)};
  PROCESS_INFORMATION desktop_process{};
  std::wstring desktop_command = L"\"" + desktop.wstring() + L"\"";
  if (!CreateProcessW(nullptr, desktop_command.data(), nullptr, nullptr, FALSE,
                      0, nullptr, directory.c_str(), &desktop_start, &desktop_process)) {
    std::cerr << "failed to start tokmon-desktop\n"; return 2;
  }
  CloseHandle(desktop_process.hThread);
  WaitForSingleObject(desktop_process.hProcess, INFINITE);
  CloseHandle(desktop_process.hProcess);
  return 0;
#else
  std::cerr << "tokmon-launcher is an optional Windows compatibility shortcut; "
               "run tokmon-desktop directly on this platform\n";
  return 2;
#endif
}
