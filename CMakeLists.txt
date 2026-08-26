cmake_minimum_required(VERSION 3.25)

project(tokmon VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TOKMON_BUILD_DESKTOP "Build the optional Slint desktop application" OFF)
option(TOKMON_BUILD_EXTERNAL_LENSES
  "Build and install standalone C-ABI Lens plugin modules" OFF)
option(TOKMON_BUILD_TESTS "Build Tokmon tests" ON)
option(TOKMON_BUILD_SCRIPT_SDK_TESTS "Enable Node.js and CPython SDK tests" ON)

include(CTest)
# C-ABI contract tests need the module targets, but test-only modules are never
# installed unless the external Lens package was explicitly requested.
set(TOKMON_NEED_LENS_MODULES ${TOKMON_BUILD_EXTERNAL_LENSES})
if(BUILD_TESTING AND TOKMON_BUILD_TESTS)
  set(TOKMON_NEED_LENS_MODULES ON)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)
include(ReleaseOptimization)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_library(tokmon_core STATIC
  nyxia/base/cbor.cpp
  nyxia/base/error.cpp
  nyxia/base/hash.cpp
  nyxia/base/ids.cpp
  nyxia/base/json.cpp
  nyxia/base/logging.cpp
  nyxia/base/redaction.cpp
  nyxia/base/yaml.cpp
  nyxia/base/value_types.cpp
  nyxia/config/config.cpp
  nyxia/engine/act_pipeline.cpp
  nyxia/engine/ray_tracing_engine.cpp
  nyxia/light_path/light_path.cpp
  nyxia/loader/c_abi_loader.cpp
  nyxia/loader/manifest_io.cpp
  nyxia/mount/lens_mount.cpp
  nyxia/runtime/runtime.cpp
  nyxia/storage/photon_store.cpp
  nyxia/worker/worker_protocol.cpp
  nyxia/worker/worker_lens_proxy.cpp
  lenses/aya/aya_lens.cpp
  lenses/calculator/calculator_lens.cpp
  lenses/chora/chora_lens.cpp
  lenses/cista/cista_lens.cpp
  lenses/clotho/clotho_lens.cpp
  lenses/cove/cove_lens.cpp
  lenses/enso/enso_lens.cpp
  lenses/fallen/fallen_lens.cpp
  lenses/ignis/ignis_lens.cpp
  lenses/iris/iris_lens.cpp
  lenses/janus/janus_lens.cpp
  lenses/lemon/lemon_lens.cpp
  lenses/nota/nota_lens.cpp
  lenses/rhea/rhea_lens.cpp
  lenses/snow/snow_lens.cpp
  lenses/styx/styx_lens.cpp
  lenses/techor/techor_lens.cpp
  lenses/termon/termon_lens.cpp
  lenses/textus/textus_lens.cpp
  lenses/tracket/tracket_lens.cpp
  lenses/common/builtin_registry.cpp
  lenses/common/http_client.cpp
  lenses/common/lens_base.cpp
  lenses/common/process_runner.cpp
  lenses/common/prometheus_endpoint.cpp
  lenses/common/pty_session.cpp
  lenses/common/secret_store.cpp
  lenses/common/schema_validator.cpp
  protocol/snow_protocol.cpp
  protocol/slash_commands.cpp
  protocol/daemon_lifecycle.cpp
  protocol/snow_transport.cpp)

target_include_directories(tokmon_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk/cpp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk/c"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${sqlite3_SOURCE_DIR}")

target_link_libraries(tokmon_core
  PUBLIC
    tl::expected
    chlog::chlog
    chjson
    chyaml::chyaml
  PRIVATE
    tokmon_sqlite3)

target_compile_definitions(tokmon_core PRIVATE
  TOKMON_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(WIN32)
  target_link_libraries(tokmon_core PRIVATE bcrypt ws2_32 advapi32 crypt32)
else()
  target_link_libraries(tokmon_core PRIVATE dl pthread)
endif()

add_executable(tokmon
  apps/tokmon/main.cpp
  apps/tokmon-cli/main.cpp
  apps/tokmond/main.cpp
  apps/tokmon-lens-worker/main.cpp)
target_include_directories(tokmon PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tokmon PRIVATE tokmon_core)

set(TOKMON_RUNTIME_TARGETS tokmon)
set(TOKMON_WORKER_TARGET tokmon)

set_target_properties(${TOKMON_RUNTIME_TARGETS}
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(TOKMON_LENS_NAMES
  ignis lemon iris rhea janus clotho aya textus enso techor styx fallen cista
  chora tracket nota cove snow termon calculator)

set(TOKMON_LENS_TARGETS)

if(TOKMON_NEED_LENS_MODULES)
  foreach(lens IN LISTS TOKMON_LENS_NAMES)
    string(SUBSTRING "${lens}" 0 1 lens_class_initial)
    string(TOUPPER "${lens_class_initial}" lens_class_initial)
    string(SUBSTRING "${lens}" 1 -1 lens_class_suffix)
    set(lens_class "${lens_class_initial}${lens_class_suffix}Lens")
    add_library(tokmon-lens-${lens} SHARED lenses/common/named_lens_entry.cpp)
    target_include_directories(tokmon-lens-${lens} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(tokmon-lens-${lens} PRIVATE tokmon_core)
    target_compile_definitions(tokmon-lens-${lens} PRIVATE
      TOKMON_LENS_SHORT_ID="${lens}"
      TOKMON_LENS_HEADER="lenses/${lens}/${lens}_lens.hpp"
      TOKMON_LENS_TYPE=tokmon::builtin::${lens_class})
    set_target_properties(tokmon-lens-${lens} PROPERTIES
      PREFIX ""
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/lenses"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/lenses")
    list(APPEND TOKMON_LENS_TARGETS tokmon-lens-${lens})
  endforeach()
endif()

if(TOKMON_BUILD_DESKTOP)
  find_package(Slint 1.17.1 EXACT CONFIG QUIET)
  if(NOT Slint_FOUND)
    message(FATAL_ERROR
      "Slint 1.17.1 was not found. Set CMAKE_PREFIX_PATH to the project-local "
      "Slint C++ SDK. See scripts/bootstrap-slint.ps1.")
  endif()

  add_executable(tokmon-desktop WIN32
    apps/tokmon-desktop/main.cpp
    apps/tokmon-desktop/src/desktop_application.cpp
    apps/tokmon-desktop/src/navigation_state.cpp
    apps/tokmon-desktop/src/platform_utils.cpp
    apps/tokmon-desktop/src/ui_controller.cpp
    apps/tokmon-desktop/src/ui_projection.cpp)
  target_link_libraries(tokmon-desktop PRIVATE tokmon_core Slint::Slint)
  if(WIN32)
    target_link_libraries(tokmon-desktop PRIVATE comdlg32 shell32 ole32)
  endif()
  if(MSVC)
    # Keep a GUI subsystem (no console window) while retaining the portable main().
    target_link_options(tokmon-desktop PRIVATE /ENTRY:mainCRTStartup)
  endif()
  slint_target_sources(tokmon-desktop
    apps/tokmon-desktop/ui/tokmon.slint)
  # These desktop implementation units include the generated Slint header.
  # Keep the object dependency explicit on MSVC as localized /showIncludes
  # output may otherwise leave them built against an older component ABI.
  set_property(SOURCE
    apps/tokmon-desktop/src/desktop_application.cpp
    apps/tokmon-desktop/src/navigation_state.cpp
    apps/tokmon-desktop/src/ui_controller.cpp
    apps/tokmon-desktop/src/ui_projection.cpp
    APPEND PROPERTY
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/tokmon.h")
  set_target_properties(tokmon-desktop PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  add_custom_command(TARGET tokmon-desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/apps/tokmon-desktop/assets"
      "$<TARGET_FILE_DIR:tokmon-desktop>/assets")
  if(WIN32)
    add_custom_command(TARGET tokmon-desktop POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:tokmon-desktop>
        $<TARGET_FILE_DIR:tokmon-desktop>
      COMMAND_EXPAND_LISTS)
  endif()

  if(BUILD_TESTING)
    add_executable(tokmon-desktop-title-edit-tests
      tests/desktop/title_edit_tests.cpp)
    target_link_libraries(tokmon-desktop-title-edit-tests PRIVATE Slint::Slint)
    target_compile_definitions(tokmon-desktop-title-edit-tests PRIVATE
      SLINT_FEATURE_EXPERIMENTAL)
    # ElementHandle-based pointer tests need the Slint compiler's element IDs.
    # Enable that metadata only for this test component, keeping the production
    # desktop component unchanged.
    get_target_property(_tokmon_slint_features Slint::Slint SLINT_ENABLED_FEATURES)
    set_property(TARGET Slint::Slint APPEND PROPERTY
      SLINT_ENABLED_FEATURES SYSTEM_TESTING)
    slint_target_sources(tokmon-desktop-title-edit-tests
      tests/desktop/title_edit_harness.slint)
    set_property(TARGET Slint::Slint PROPERTY
      SLINT_ENABLED_FEATURES "${_tokmon_slint_features}")
    set_target_properties(tokmon-desktop-title-edit-tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(WIN32)
      add_custom_command(TARGET tokmon-desktop-title-edit-tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_RUNTIME_DLLS:tokmon-desktop-title-edit-tests>
          $<TARGET_FILE_DIR:tokmon-desktop-title-edit-tests>
        COMMAND_EXPAND_LISTS)
    endif()
    add_test(NAME tokmon-desktop-title-edit
      COMMAND tokmon-desktop-title-edit-tests)
  endif()
endif()

set(TOKMON_RELEASE_TARGETS
  tokmon_sqlite3 tokmon_core ${TOKMON_RUNTIME_TARGETS} ${TOKMON_LENS_TARGETS})
if(TARGET tokmon-desktop)
  list(APPEND TOKMON_RELEASE_TARGETS tokmon-desktop)
endif()
foreach(target IN LISTS TOKMON_RELEASE_TARGETS)
  tokmon_optimize_release_target("${target}")
endforeach()

message(STATUS "Tokmon unified runtime: tokmon")
message(STATUS "Tokmon desktop: ${TOKMON_BUILD_DESKTOP}")
message(STATUS "Tokmon external Lenses: ${TOKMON_BUILD_EXTERNAL_LENSES}")

if(BUILD_TESTING AND TOKMON_BUILD_TESTS)
  add_subdirectory(tests)
endif()

install(TARGETS ${TOKMON_RUNTIME_TARGETS} RUNTIME DESTINATION bin)
if(TARGET tokmon-desktop)
  install(TARGETS tokmon-desktop RUNTIME DESTINATION bin)
  install(DIRECTORY apps/tokmon-desktop/assets/ DESTINATION bin/assets)
  if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:tokmon-desktop> DESTINATION bin)
  endif()
endif()
if(TOKMON_BUILD_EXTERNAL_LENSES AND TOKMON_LENS_TARGETS)
  install(TARGETS ${TOKMON_LENS_TARGETS}
    OPTIONAL RUNTIME DESTINATION bin/lenses LIBRARY DESTINATION bin/lenses)
endif()
install(DIRECTORY config/ DESTINATION share/tokmon/config)
install(DIRECTORY sdk/typescript/ DESTINATION share/tokmon/sdk/typescript
  PATTERN "examples" EXCLUDE)
install(DIRECTORY sdk/python/tokmon_lens_sdk DESTINATION share/tokmon/sdk/python
  PATTERN "__pycache__" EXCLUDE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/schemas")
  install(DIRECTORY schemas/ DESTINATION share/tokmon/schemas)
endif()
