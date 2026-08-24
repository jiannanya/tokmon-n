#pragma once

#include <sstream>

#include <chtest.hpp>

// chTest's boolean macros pass their expression to a bool parameter.  That
// rejects useful boolean-testable types such as std::shared_ptr and
// tl::expected because their conversion is explicit.  Preserve chTest's
// routing semantics while making the contextual conversion explicit.
#undef CHECK
#define CHECK(EXPRESSION)                                                       \
  if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery ||             \
      (::chtest::route().mode == ::chtest::SubcaseMode::Active &&                \
       ::chtest::route().in_subcase))                                            \
  ::chtest::record_check(static_cast<bool>(EXPRESSION), __FILE__, __LINE__,       \
                         #EXPRESSION, "", false, ::chtest::current_quiet())

#undef REQUIRE
#define REQUIRE(EXPRESSION)                                                     \
  if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery ||             \
      (::chtest::route().mode == ::chtest::SubcaseMode::Active &&                \
       ::chtest::route().in_subcase))                                            \
  ::chtest::record_check(static_cast<bool>(EXPRESSION), __FILE__, __LINE__,       \
                         #EXPRESSION, "", true, ::chtest::current_quiet())

#define REQUIRE_FALSE(EXPRESSION) REQUIRE(!(EXPRESSION))
#define INFO(EXPRESSION)                                                        \
  do {                                                                          \
    std::ostringstream tokmon_test_context_stream;                              \
    tokmon_test_context_stream << EXPRESSION;                                   \
  } while (false)
#define CAPTURE(EXPRESSION) INFO(#EXPRESSION " = " << (EXPRESSION))

// These dynamic sections are already inside explicit loops in Tokmon's tests.
#define DYNAMIC_SECTION(NAME) if ((void)(NAME), true)
