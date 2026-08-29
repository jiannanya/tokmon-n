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
  GIT_SHALLOW FALSE)
FetchContent_GetProperties(tokmon_zep)
if(NOT tokmon_zep_POPULATED)
  FetchContent_Populate(tokmon_zep)
endif()
add_library(tokmon_zep_core INTERFACE)
target_include_directories(tokmon_zep_core INTERFACE
  "${tokmon_zep_SOURCE_DIR}/include")

# Pin the generated C++ grammar independently from tree-sitter core. Grammar
# types stay inside SyntaxService and never cross the editor adapter boundary.
FetchContent_Declare(tokmon_tree_sitter_cpp
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
  GIT_TAG 8b5b49eb196bec7040441bee33b2c9a4838d6967
  GIT_SHALLOW FALSE)
FetchContent_GetProperties(tokmon_tree_sitter_cpp)
if(NOT tokmon_tree_sitter_cpp_POPULATED)
  FetchContent_Populate(tokmon_tree_sitter_cpp)
endif()
add_library(tokmon_tree_sitter_cpp STATIC
  "${tokmon_tree_sitter_cpp_SOURCE_DIR}/src/parser.c"
  "${tokmon_tree_sitter_cpp_SOURCE_DIR}/src/scanner.c")
target_include_directories(tokmon_tree_sitter_cpp PRIVATE
  "${tokmon_tree_sitter_cpp_SOURCE_DIR}/src")

# libghostty-vt is the only Ghostty component linked by tokmon-desk. Pinning
# an exact upstream revision keeps the C ABI and VT semantics reproducible.
# SIMD is disabled so the static archive has no extra runtime dependencies on
# Windows, macOS, or Linux.
set(GHOSTTY_ZIG_BUILD_FLAGS "-Dsimd=false" CACHE STRING
  "Additional flags for tokmon-desk's pinned libghostty-vt build" FORCE)
FetchContent_Declare(tokmon_ghostty_vt
  GIT_REPOSITORY https://github.com/ghostty-org/ghostty.git
  GIT_TAG 4540d499ae463ad7b90f28f6f852f64f844c160f
  GIT_SHALLOW FALSE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(tokmon_ghostty_vt)

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
tokmon_desk_install_fetched_license(ghostty
  "${tokmon_ghostty_vt_SOURCE_DIR}/LICENSE")
tokmon_desk_install_fetched_license(md4c "${md4c_SOURCE_DIR}/LICENSE.md")

set(TOKMON_DESK_CORE_SOURCES
  src/app/app_state.cpp
  src/platform/desk_app_paths.cpp
  src/fonts/font_manager.cpp
  src/markdown/markdown_ast.cpp
  src/workspace/workspace_service.cpp
  src/editor/document_store.cpp
  src/editor/syntax_service.cpp
  src/review/git_service.cpp
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
    tokmon_md4c
    Freetype::Freetype
    harfbuzz::harfbuzz
    unofficial::tree-sitter::tree-sitter
    tokmon_tree_sitter_cpp
    tokmon_zep_core
    ghostty-vt-static)
target_compile_definitions(tokmon_desk_core PUBLIC
  TOKMON_DESK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
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

add_custom_command(TARGET tokmon-desk POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    "$<TARGET_FILE_DIR:tokmon-desk>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/rml"
    "$<TARGET_FILE_DIR:tokmon-desk>/rml"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "$<TARGET_FILE_DIR:tokmon-desk>/THIRD_PARTY_NOTICES.md"
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "$<TARGET_FILE_DIR:tokmon-desk>/assets/fonts"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${TOKMON_MISANS_FONT_FILE}"
    "$<TARGET_FILE_DIR:tokmon-desk>/assets/fonts/MiSansVF.ttf")
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
