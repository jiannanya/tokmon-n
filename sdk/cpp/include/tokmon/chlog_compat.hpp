#pragma once

#include <format>

// VS 2019's final C++20 STL shipped std::format but exposed the checked format
// string type under its implementation name. chlog supports this toolchain once
// the standard spelling is supplied.
#if defined(_MSC_VER) && _MSC_VER < 1930
namespace std {
template <typename... Arguments>
using format_string = _Fmt_string<Arguments...>;
}  // namespace std
#endif
