include(FetchContent)

if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

set(FETCHCONTENT_QUIET OFF)
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPECTED_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
set(CHLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CHLOG_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CHJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CHJSON_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CHJSON_BUILD_COMPARE_BENCH OFF CACHE BOOL "" FORCE)
set(CHHTTP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CHHTTP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CHMD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CHMD_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CHMD_BUILD_FUZZER OFF CACHE BOOL "" FORCE)
set(CHMD_ENABLE_SANITIZERS OFF CACHE BOOL "" FORCE)
set(CHYAML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CHYAML_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CHYAML_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
set(CHYAML_BUILD_SIZE_PROBE OFF CACHE BOOL "" FORCE)
set(CHYAML_BUILD_COMPARISON OFF CACHE BOOL "" FORCE)
set(CHYAML_ENABLE_IPO ON CACHE BOOL "" FORCE)
set(CHYAML_OPTIMIZE_FOR SPEED CACHE STRING "" FORCE)
set(CHTEST_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(tl_expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected.git
  GIT_TAG v1.2.0
  GIT_SHALLOW TRUE)
FetchContent_Declare(chlog
  GIT_REPOSITORY https://github.com/jiannanya/chlog.git
  GIT_TAG d63ceda126cc6165c8cf1101ae5f16db8978882d
  GIT_SHALLOW TRUE)
FetchContent_Declare(chyaml
  GIT_REPOSITORY https://github.com/jiannanya/chyaml.git
  GIT_TAG 0586cd91a7b497feb7df1de90da37c6d1728cf1d
  GIT_SHALLOW TRUE)
FetchContent_Declare(chjson
  GIT_REPOSITORY https://github.com/jiannanya/chjson.git
  GIT_TAG f98fc8d8b228559ec584a331deab911eff6df8ab
  GIT_SUBMODULES ""
  GIT_SHALLOW TRUE)
FetchContent_Declare(chhttp
  GIT_REPOSITORY https://github.com/jiannanya/chhttp.git
  GIT_TAG 0e11978d228dd1e7be728378c8898f67ab4cf36b
  GIT_SUBMODULES ""
  GIT_SHALLOW TRUE)
FetchContent_Declare(sqlite3
  URL https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(md4c
  GIT_REPOSITORY https://github.com/mity/md4c.git
  GIT_TAG release-0.5.2
  GIT_SHALLOW TRUE)
if(TOKMON_BUILD_DESK)
  FetchContent_Declare(chmd
    GIT_REPOSITORY https://github.com/jiannanya/chmd.git
    GIT_TAG 332c0257409263254cbd06b5b60edf844edaf516
    GIT_SHALLOW TRUE)
endif()

# Add fetched projects below an EXCLUDE_FROM_ALL directory boundary.  Tokmon
# links their targets normally, while their developer headers/CMake packages do
# not leak into Tokmon's runtime install tree.
macro(tokmon_make_dependency_available dependency)
  FetchContent_GetProperties(${dependency})
  if(NOT ${dependency}_POPULATED)
    FetchContent_Populate(${dependency})
  endif()
  if(EXISTS "${${dependency}_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${${dependency}_SOURCE_DIR}"
      "${${dependency}_BINARY_DIR}" EXCLUDE_FROM_ALL)
  endif()
endmacro()

tokmon_make_dependency_available(tl_expected)
tokmon_make_dependency_available(chlog)
tokmon_make_dependency_available(chyaml)
tokmon_make_dependency_available(chjson)
tokmon_make_dependency_available(chhttp)
if(TOKMON_BUILD_DESK)
  tokmon_make_dependency_available(chmd)
endif()
tokmon_make_dependency_available(sqlite3)

# Only fetch md4c's sources for the legacy Slint tokmon-desktop. Its own CMake
# tree is skipped because tokmon_md4c compiles the parser TU directly.
FetchContent_GetProperties(md4c)
if(NOT md4c_POPULATED)
  FetchContent_Populate(md4c)
endif()

if(NOT TARGET tokmon_md4c)
  add_library(tokmon_md4c STATIC "${md4c_SOURCE_DIR}/src/md4c.c")
  target_include_directories(tokmon_md4c PUBLIC "${md4c_SOURCE_DIR}/src")
  target_compile_definitions(tokmon_md4c PUBLIC $<$<CONFIG:Release>:MD4C_USE_UTF8>)
  if(MSVC)
    target_compile_options(tokmon_md4c PRIVATE /W0)
  else()
    target_compile_options(tokmon_md4c PRIVATE -w)
  endif()
endif()

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
  FetchContent_Declare(chtest
    GIT_REPOSITORY https://github.com/jiannanya/chtest.git
    GIT_TAG 497a52ce53a06855cc7993c338e81e67862866e4
    GIT_SHALLOW TRUE)
  tokmon_make_dependency_available(chtest)
endif()
