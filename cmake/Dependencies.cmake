include(FetchContent)

set(FETCHCONTENT_QUIET OFF)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPECTED_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(tl_expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected.git
  GIT_TAG v1.2.0
  GIT_SHALLOW TRUE)
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.15.3
  GIT_SHALLOW TRUE)
FetchContent_Declare(yaml_cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
  GIT_SHALLOW TRUE)
FetchContent_Declare(sqlite3
  URL https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(tl_expected spdlog yaml_cpp sqlite3)

if(NOT TARGET tokmon_sqlite3)
  add_library(tokmon_sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
  target_include_directories(tokmon_sqlite3 PUBLIC "${sqlite3_SOURCE_DIR}")
  target_compile_definitions(tokmon_sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1)
  if(MSVC)
    target_compile_options(tokmon_sqlite3 PRIVATE /W0)
  else()
    target_compile_options(tokmon_sqlite3 PRIVATE -w)
  endif()
endif()

if(TOKMON_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()
