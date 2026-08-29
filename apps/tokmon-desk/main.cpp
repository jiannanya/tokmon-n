#include "app/desk_app.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>
#endif

int main(int argc, char** argv) {
  return tokmon::desk::run_desk(argc, argv);
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  int wide_count = 0;
  auto** wide_arguments = CommandLineToArgvW(GetCommandLineW(), &wide_count);
  if (!wide_arguments)
    return tokmon::desk::run_desk(0, nullptr);
  std::vector<std::string> utf8_arguments;
  std::vector<char*> arguments;
  utf8_arguments.reserve(static_cast<std::size_t>(wide_count));
  arguments.reserve(static_cast<std::size_t>(wide_count));
  for (int index = 0; index < wide_count; ++index) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide_arguments[index], -1,
                                         nullptr, 0, nullptr, nullptr);
    std::string value(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
      WideCharToMultiByte(CP_UTF8, 0, wide_arguments[index], -1,
                          value.data(), size, nullptr, nullptr);
      value.resize(static_cast<std::size_t>(size - 1));
    }
    utf8_arguments.push_back(std::move(value));
  }
  LocalFree(wide_arguments);
  for (auto& value : utf8_arguments)
    arguments.push_back(value.data());
  return tokmon::desk::run_desk(wide_count, arguments.data());
}
#endif
