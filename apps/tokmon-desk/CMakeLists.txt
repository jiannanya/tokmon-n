find_package(SDL3 CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)
find_package(unofficial-skia CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(harfbuzz CONFIG REQUIRED)
find_package(unofficial-tree-sitter CONFIG REQUIRED)
find_package(libgit2 CONFIG QUIET)
if(UNIX AND NOT APPLE)
  find_package(Vulkan REQUIRED)
endif()

# Zep is kept behind a Tokmon adapter. Its gap buffer is header-only, so we
# populate the pinned source without pulling Zep's demo, Qt, or ImGui targets
# into the desktop build.
include(FetchContent)
FetchContent_Declare(tokmon_zep
  GIT_REPOSITORY https://github.com/Rezonality/zep.git
  GIT_TAG 7aa7ed940d5c889dc6b27a2529a740c0bb2ad6af
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR __tokmon_source_only__)
FetchContent_MakeAvailable(tokmon_zep)
add_library(tokmon_zep_core INTERFACE)
target_include_directories(tokmon_zep_core INTERFACE
  "${tokmon_zep_SOURCE_DIR}/include")

# Pin the generated C++ grammar independently from tree-sitter core. Grammar
# types stay inside SyntaxService and never cross the editor adapter boundary.
FetchContent_Declare(tokmon_tree_sitter_cpp
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
  GIT_TAG 8b5b49eb196bec7040441bee33b2c9a4838d6967
  GIT_SHALLOW FALSE
  GIT_SUBMODULES ""
  GIT_SUBMODULES_RECURSE FALSE
  SOURCE_SUBDIR __tokmon_source_only__)
FetchContent_MakeAvailable(tokmon_tree_sitter_cpp)
add_library(tokmon_tree_sitter_cpp STATIC
  "${tokmon_tree_sitter_cpp_SOURCE_DIR}/src/parser.c"
  "${tokmon_tree_sitter_cpp_SOURCE_DIR}/src/scanner.c")
target_include_directories(tokmon_tree_sitter_cpp PRIVATE
  "${tokmon_tree_sitter_cpp_SOURCE_DIR}/src")
if(MSVC)
  target_compile_options(tokmon_tree_sitter_cpp PRIVATE
    /wd4003 /wd4100 /wd4127 /wd4242 /wd4244 /wd4267 /wd4456 /wd4702 /wd4706)
else()
  target_compile_options(tokmon_tree_sitter_cpp PRIVATE -w)
endif()

# Every editor grammar is pinned independently. Generated parsers are built
# directly; grammar CLIs and language runtimes are not part of the product.
function(tokmon_desk_add_tree_sitter_grammar target content_name repository revision subdir)
  FetchContent_Declare(${content_name}
    GIT_REPOSITORY "${repository}"
    GIT_TAG "${revision}"
    GIT_SHALLOW FALSE
    GIT_SUBMODULES ""
    GIT_SUBMODULES_RECURSE FALSE
    SOURCE_SUBDIR __tokmon_source_only__)
  FetchContent_MakeAvailable(${content_name})
  set(grammar_root "${${content_name}_SOURCE_DIR}/${subdir}")
  set(grammar_sources "${grammar_root}/src/parser.c")
  foreach(scanner IN ITEMS scanner.c scanner.cc)
    if(EXISTS "${grammar_root}/src/${scanner}")
      list(APPEND grammar_sources "${grammar_root}/src/${scanner}")
    endif()
  endforeach()
  add_library(${target} STATIC ${grammar_sources})
  target_include_directories(${target} PRIVATE "${grammar_root}/src")
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /wd4003 /wd4100 /wd4127 /wd4242 /wd4244 /wd4267 /wd4456 /wd4702 /wd4706)
  else()
    target_compile_options(${target} PRIVATE -w)
  endif()
  set_property(TARGET ${target} PROPERTY POSITION_INDEPENDENT_CODE ON)
  set(${content_name}_SOURCE_DIR "${${content_name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

tokmon_desk_add_tree_sitter_grammar(tokmon_tree_sitter_rust tokmon_ts_rust
  https://github.com/tree-sitter/tree-sitter-rust.git
  77a3747266f4d621d0757825e6b11edcbf991ca5 .)
tokmon_desk_add_tree_sitter_grammar(tokmon_tree_sitter_javascript tokmon_ts_javascript
  https://github.com/tree-sitter/tree-sitter-javascript.git
  44c892e0be055ac465d5eeddae6d3e194424e7de .)
tokmon_desk_add_tree_sitter_grammar(tokmon_tree_sitter_typescript tokmon_ts_typescript
  https://github.com/tree-sitter/tree-sitter-typescript.git
  f975a621f4e7f532fe322e13c4f79495e0a7b2e7 typescript)
tokmon_desk_add_tree_sitter_grammar(tokmon_tree_sitter_tsx tokmon_ts_typescript
  https://github.com/tree-sitter/tree-sitter-typescript.git
  f975a621f4e7f532fe322e13c4f79495e0a7b2e7 tsx)
tokmon_desk_add_tree_sitter_grammar(tokmon_tree_sitter_python tokmon_ts_python
  https://github.com/tree-sitter/tree-sitter-python.git
  293fdc02038ee2bf0e2e206711b69c90ac0d413f .)
tokmon_desk_add_tree_sitter_grammar(tokmon_tree_sitter_json tokmon_ts_json
  https://github.com/tree-sitter/tree-sitter-json.git
  ee35a6ebefcef0c5c416c0d1ccec7370cfca5a24 .)
tokmon_desk_add_tree_sitter_grammar(tokmon_tree_sitter_yaml tokmon_ts_yaml
  https://github.com/tree-sitter-grammars/tree-sitter-yaml.git
  7708026449bed86239b1cd5bce6e3c34dbca6415 .)
tokmon_desk_add_tree_sitter_grammar(tokmon_tree_sitter_toml tokmon_ts_toml
  https://github.com/tree-sitter-grammars/tree-sitter-toml.git
  64b56832c2cffe41758f28e05c756a3a98d16f41 .)
tokmon_desk_add_tree_sitter_grammar(tokmon_tree_sitter_markdown tokmon_ts_markdown
  https://github.com/tree-sitter-grammars/tree-sitter-markdown.git
  f969cd3ae3f9fbd4e43205431d0ae286014c05b5 tree-sitter-markdown)
tokmon_desk_add_tree_sitter_grammar(tokmon_tree_sitter_bash tokmon_ts_bash
  https://github.com/tree-sitter/tree-sitter-bash.git
  a06c2e4415e9bc0346c6b86d401879ffb44058f7 .)
tokmon_desk_add_tree_sitter_grammar(tokmon_tree_sitter_cmake tokmon_ts_cmake
  https://github.com/uyha/tree-sitter-cmake.git
  ca627bb5828616b6246aafdc3c3222789e728e37 .)

# libghostty-vt is the only Ghostty component linked by tokmon-desk. Pinning
# an exact upstream revision keeps the C ABI and VT semantics reproducible.
# SIMD is disabled so the static archive has no extra runtime dependencies on
# Windows, macOS, or Linux.
set(TOKMON_DESK_ZIG_VERSION "0.16.0" CACHE STRING
  "Pinned Zig toolchain version used to build libghostty-vt")

# Ghostty's CMake adapter invokes Zig at build time. Prefer an explicitly
# supplied ZIG_EXECUTABLE, then PATH, and finally the repository-local tool
# cache used by the reproducible Windows build. Zig remains a build-only tool
# and is never installed with tokmon-desk.
if(NOT ZIG_EXECUTABLE)
  find_program(ZIG_EXECUTABLE NAMES zig)
endif()
if(NOT ZIG_EXECUTABLE AND WIN32)
  set(_tokmon_zig_candidate
    "${CMAKE_SOURCE_DIR}/build/tools/zig-x86_64-windows-${TOKMON_DESK_ZIG_VERSION}/zig.exe")
  if(EXISTS "${_tokmon_zig_candidate}")
    set(ZIG_EXECUTABLE "${_tokmon_zig_candidate}" CACHE FILEPATH
      "Pinned Zig executable used to build libghostty-vt" FORCE)
  endif()
endif()
if(NOT ZIG_EXECUTABLE)
  message(FATAL_ERROR
    "tokmon-desk requires Zig ${TOKMON_DESK_ZIG_VERSION} at build time for "
    "libghostty-vt. Install that exact version or configure with "
    "-DZIG_EXECUTABLE=/absolute/path/to/zig. Zig is not a runtime dependency.")
endif()
execute_process(
  COMMAND "${ZIG_EXECUTABLE}" version
  RESULT_VARIABLE _tokmon_zig_result
  OUTPUT_VARIABLE _tokmon_zig_actual
  ERROR_VARIABLE _tokmon_zig_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _tokmon_zig_result EQUAL 0 OR
   NOT _tokmon_zig_actual STREQUAL TOKMON_DESK_ZIG_VERSION)
  message(FATAL_ERROR
    "libghostty-vt requires Zig ${TOKMON_DESK_ZIG_VERSION}, but "
    "${ZIG_EXECUTABLE} reported '${_tokmon_zig_actual}'. ${_tokmon_zig_error}")
endif()
message(STATUS "tokmon-desk: using pinned Zig ${_tokmon_zig_actual}: ${ZIG_EXECUTABLE}")
unset(_tokmon_zig_candidate)
unset(_tokmon_zig_result)
unset(_tokmon_zig_actual)
unset(_tokmon_zig_error)
set(GHOSTTY_ZIG_BUILD_FLAGS "-Dsimd=false" CACHE STRING
  "Additional flags for tokmon-desk's pinned libghostty-vt build" FORCE)
FetchContent_Declare(tokmon_ghostty_vt
  GIT_REPOSITORY https://github.com/ghostty-org/ghostty.git
  GIT_TAG 4540d499ae463ad7b90f28f6f852f64f844c160f
  GIT_SHALLOW FALSE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(tokmon_ghostty_vt)

# Zig 0.16 resolves lazy transitive packages into Ghostty's repository-local
# zig-pkg cache. Make that network step explicit during configure so a first
# build from a pristine checkout cannot accidentally depend on another build's
# cache. The fetched package hashes are pinned by Ghostty's build.zig.zon.
execute_process(
  COMMAND "${ZIG_EXECUTABLE}" build --fetch
  WORKING_DIRECTORY "${tokmon_ghostty_vt_SOURCE_DIR}"
  RESULT_VARIABLE _tokmon_ghostty_fetch_result
  OUTPUT_VARIABLE _tokmon_ghostty_fetch_output
  ERROR_VARIABLE _tokmon_ghostty_fetch_error)
if(NOT _tokmon_ghostty_fetch_result EQUAL 0)
  message(FATAL_ERROR
    "Unable to fetch the pinned libghostty-vt Zig packages. "
    "${_tokmon_ghostty_fetch_output}${_tokmon_ghostty_fetch_error}")
endif()
unset(_tokmon_ghostty_fetch_result)
unset(_tokmon_ghostty_fetch_output)
unset(_tokmon_ghostty_fetch_error)

# Keep the distributable self-contained from a licensing perspective. Vcpkg
# records one canonical copyright file for each direct and transitive package;
# FetchContent dependencies need their license files installed explicitly.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  set(TOKMON_DESK_VCPKG_SHARE_DIR
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
  if(EXISTS "${TOKMON_DESK_VCPKG_SHARE_DIR}")
    file(GLOB TOKMON_DESK_VCPKG_LICENSE_FILES
      LIST_DIRECTORIES FALSE
      "${TOKMON_DESK_VCPKG_SHARE_DIR}/*/copyright")
    foreach(license_file IN LISTS TOKMON_DESK_VCPKG_LICENSE_FILES)
      get_filename_component(license_dir "${license_file}" DIRECTORY)
      get_filename_component(component "${license_dir}" NAME)
      install(FILES "${license_file}"
        DESTINATION "bin/licenses/vcpkg/${component}")
    endforeach()
  endif()
endif()

function(tokmon_desk_install_fetched_license component source_file)
  if(EXISTS "${source_file}")
    install(FILES "${source_file}"
      DESTINATION "bin/licenses/${component}"
      RENAME copyright)
  endif()
endfunction()
tokmon_desk_install_fetched_license(zep "${tokmon_zep_SOURCE_DIR}/LICENSE")
tokmon_desk_install_fetched_license(tree-sitter-cpp
  "${tokmon_tree_sitter_cpp_SOURCE_DIR}/LICENSE")
foreach(grammar IN ITEMS rust javascript typescript python json yaml toml markdown bash cmake)
  set(grammar_source_variable "tokmon_ts_${grammar}_SOURCE_DIR")
  tokmon_desk_install_fetched_license("tree-sitter-${grammar}"
    "${${grammar_source_variable}}/LICENSE")
endforeach()
tokmon_desk_install_fetched_license(tree-sitter-tsx
  "${tokmon_ts_typescript_SOURCE_DIR}/LICENSE")
tokmon_desk_install_fetched_license(ghostty
  "${tokmon_ghostty_vt_SOURCE_DIR}/LICENSE")
tokmon_desk_install_fetched_license(chmd "${chmd_SOURCE_DIR}/LICENSE")

set(TOKMON_DESK_CORE_SOURCES
  src/app/app_state.cpp
  src/platform/desk_app_paths.cpp
  src/platform/desk_resource_paths.cpp
  src/state/desk_state_store.cpp
  src/state/document_recovery.cpp
  src/fonts/font_manager.cpp
  src/markdown/markdown_ast.cpp
  src/workspace/workspace_service.cpp
  src/editor/document_store.cpp
  src/editor/grapheme.cpp
  src/editor/syntax_service.cpp
  src/review/git_service.cpp
  src/review/desktop_change_tracker.cpp
  src/terminal/terminal_service.cpp
  src/browser/browser_manager.cpp
  src/integration/daemon_client.cpp)
list(APPEND TOKMON_DESK_CORE_SOURCES src/ui/navigation_model.cpp)

add_library(tokmon_desk_core STATIC ${TOKMON_DESK_CORE_SOURCES})
target_include_directories(tokmon_desk_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}")
target_link_libraries(tokmon_desk_core
  PUBLIC
    tokmon_core
    chmd::chmd
    Freetype::Freetype
    harfbuzz::harfbuzz
    unofficial::tree-sitter::tree-sitter
    tokmon_tree_sitter_cpp
    tokmon_tree_sitter_rust
    tokmon_tree_sitter_javascript
    tokmon_tree_sitter_typescript
    tokmon_tree_sitter_tsx
    tokmon_tree_sitter_python
    tokmon_tree_sitter_json
    tokmon_tree_sitter_yaml
    tokmon_tree_sitter_toml
    tokmon_tree_sitter_markdown
    tokmon_tree_sitter_bash
    tokmon_tree_sitter_cmake
    tokmon_zep_core
    ghostty-vt-static)
target_compile_definitions(tokmon_desk_core PUBLIC
  TOKMON_DESK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  TOKMON_DESK_FONT_FILE="${TOKMON_MISANS_FONT_FILE}")
if(UNIX AND NOT APPLE)
  find_library(TOKMON_DESK_UTIL_LIBRARY util REQUIRED)
  target_link_libraries(tokmon_desk_core PUBLIC "${TOKMON_DESK_UTIL_LIBRARY}")
endif()
if(APPLE)
  target_link_libraries(tokmon_desk_core PUBLIC "-framework CoreServices")
endif()

if(TARGET libgit2::libgit2)
  target_link_libraries(tokmon_desk_core PUBLIC libgit2::libgit2)
  target_compile_definitions(tokmon_desk_core PUBLIC TOKMON_DESK_HAS_LIBGIT2=1)
elseif(TARGET libgit2::libgit2package)
  target_link_libraries(tokmon_desk_core PUBLIC libgit2::libgit2package)
  target_compile_definitions(tokmon_desk_core PUBLIC TOKMON_DESK_HAS_LIBGIT2=1)
elseif(TARGET libgit2package)
  target_link_libraries(tokmon_desk_core PUBLIC libgit2package)
  target_compile_definitions(tokmon_desk_core PUBLIC TOKMON_DESK_HAS_LIBGIT2=1)
endif()

add_executable(tokmon-desk WIN32
  main.cpp
  src/app/desk_app.cpp
  src/platform/sdl_platform.cpp
  src/render/skia_device.cpp
  src/render/rml_render_interface_skia.cpp
  src/ui/elements/element_code_surface.cpp
  src/ui/elements/element_diff_surface.cpp
  src/ui/elements/element_file_tree.cpp
  src/ui/elements/element_terminal.cpp
  src/ui/desk_view_model.cpp
  src/ui/modules/browser_controller.cpp
  src/ui/modules/settings_controller.cpp
  src/ui/modules/desk_renderer.cpp
  src/ui/modules/trajectory_controller.cpp
  src/ui/modules/terminal_controller.cpp
  src/ui/desk_controller.cpp)
add_dependencies(tokmon-desk tokmon)
if(WIN32)
  target_sources(tokmon-desk PRIVATE src/render/skia_device_d3d12.cpp)
elseif(APPLE)
  target_sources(tokmon-desk PRIVATE src/render/skia_device_metal.mm)
elseif(UNIX)
  target_sources(tokmon-desk PRIVATE src/render/skia_device_vulkan.cpp)
endif()
if(APPLE)
  target_link_libraries(tokmon-desk PRIVATE
    "-framework AppKit" "-framework Metal" "-framework QuartzCore"
    "-framework CoreServices")
endif()
target_include_directories(tokmon-desk PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}")
target_link_libraries(tokmon-desk PRIVATE
  tokmon_desk_core
  SDL3::SDL3
  RmlUi::RmlUi
    unofficial::skia::skia)
if(UNIX AND NOT APPLE)
  target_link_libraries(tokmon-desk PRIVATE Vulkan::Vulkan)
endif()
if(TARGET RmlUi::SVG)
  target_link_libraries(tokmon-desk PRIVATE RmlUi::SVG)
  target_compile_definitions(tokmon-desk PRIVATE TOKMON_DESK_HAS_RMLUI_SVG=1)
endif()
if(WIN32)
  target_link_libraries(tokmon-desk PRIVATE comdlg32 shell32 ole32 user32 d3d12 dxgi)
  if(MSVC)
    target_link_options(tokmon-desk PRIVATE /ENTRY:mainCRTStartup)
  endif()
endif()

set_target_properties(tokmon-desk PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Resource changes must reach an incremental build even when no C++ object is
# relinked. A tracked stamp keeps no-op builds cheap while CONFIGURE_DEPENDS
# makes newly added UI resources part of the dependency graph.
file(GLOB_RECURSE TOKMON_DESK_RESOURCE_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/rml/*")
set(TOKMON_DESK_RESOURCE_STAMP
  "${CMAKE_CURRENT_BINARY_DIR}/tokmon-desk-resources.stamp")
add_custom_command(OUTPUT "${TOKMON_DESK_RESOURCE_STAMP}"
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "$<TARGET_FILE_DIR:tokmon-desk>"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    "$<TARGET_FILE_DIR:tokmon-desk>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/rml"
    "$<TARGET_FILE_DIR:tokmon-desk>/rml"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "$<TARGET_FILE_DIR:tokmon-desk>/THIRD_PARTY_NOTICES.md"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "$<TARGET_FILE_DIR:tokmon-desk>/THIRD_PARTY_NOTICES.txt"
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "$<TARGET_FILE_DIR:tokmon-desk>/assets/fonts"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${TOKMON_MISANS_FONT_FILE}"
    "$<TARGET_FILE_DIR:tokmon-desk>/assets/fonts/MiSansVF.ttf"
  COMMAND ${CMAKE_COMMAND} -E touch "${TOKMON_DESK_RESOURCE_STAMP}"
  DEPENDS ${TOKMON_DESK_RESOURCE_FILES}
          "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
          "${TOKMON_MISANS_FONT_FILE}"
  VERBATIM)
add_custom_target(tokmon-desk-resources
  DEPENDS "${TOKMON_DESK_RESOURCE_STAMP}")
add_dependencies(tokmon-desk tokmon-desk-resources)
if(WIN32)
  add_custom_command(TARGET tokmon-desk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:tokmon-desk>
      $<TARGET_FILE_DIR:tokmon-desk>
    COMMAND_EXPAND_LISTS)
endif()

if(BUILD_TESTING AND TOKMON_BUILD_TESTS)
  add_executable(tokmon-desk-tests tests/desk_tests.cpp)
  target_link_libraries(tokmon-desk-tests PRIVATE tokmon_desk_core)
  set_target_properties(tokmon-desk-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  add_test(NAME tokmon-desk-core COMMAND tokmon-desk-tests)
endif()
