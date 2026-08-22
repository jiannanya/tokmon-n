cmake_minimum_required(VERSION 3.25)

# yaml-cpp 0.8.0 still declares compatibility with very old CMake releases.
# CMake 4 removes those policy defaults unless consumers select a floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING
  "Oldest dependency policy version accepted by this build")

project(tokmon VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TOKMON_BUILD_UI "Build the Slint desktop application" ON)
option(TOKMON_BUILD_TESTS "Build Tokmon tests" ON)
option(TOKMON_BUILD_SCRIPT_SDK_TESTS "Enable Node.js and CPython SDK tests" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

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
  nyxia/base/logging.cpp
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
  lenses/common/lens_base.cpp
  lenses/common/process_runner.cpp
  protocol/snow_protocol.cpp
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
    spdlog::spdlog
    yaml-cpp::yaml-cpp
  PRIVATE
    tokmon_sqlite3)

target_compile_definitions(tokmon_core PRIVATE
  TOKMON_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  TOKMON_BUILD_BIN_DIR="${CMAKE_BINARY_DIR}/bin")

if(WIN32)
  target_link_libraries(tokmon_core PRIVATE bcrypt ws2_32)
else()
  target_link_libraries(tokmon_core PRIVATE dl pthread)
endif()

add_executable(tokmond apps/tokmond/main.cpp)
target_link_libraries(tokmond PRIVATE tokmon_core)

add_executable(tokmon apps/tokmon-cli/main.cpp)
target_link_libraries(tokmon PRIVATE tokmon_core)

add_executable(tokmon-launcher apps/tokmon-launcher/main.cpp)
target_link_libraries(tokmon-launcher PRIVATE tokmon_core)

add_executable(tokmon-lens-worker apps/tokmon-lens-worker/main.cpp)
target_link_libraries(tokmon-lens-worker PRIVATE tokmon_core)

set_target_properties(tokmond tokmon tokmon-launcher tokmon-lens-worker
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(TOKMON_LENS_NAMES
  ignis lemon iris rhea janus clotho aya textus enso techor styx fallen cista
  chora tracket nota cove snow termon calculator)

set(TOKMON_LENS_TARGETS)

foreach(lens IN LISTS TOKMON_LENS_NAMES)
  string(REPLACE "-" "_" lens_symbol "${lens}")
  add_library(tokmon-lens-${lens} SHARED lenses/common/named_lens_entry.cpp)
  target_link_libraries(tokmon-lens-${lens} PRIVATE tokmon_core)
  target_compile_definitions(tokmon-lens-${lens} PRIVATE
    TOKMON_LENS_SHORT_ID="${lens}")
  set_target_properties(tokmon-lens-${lens} PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/lenses"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/lenses")
  list(APPEND TOKMON_LENS_TARGETS tokmon-lens-${lens})
endforeach()

if(TOKMON_BUILD_UI)
  find_package(Slint 1.17.1 EXACT CONFIG QUIET)
  if(NOT Slint_FOUND)
    message(FATAL_ERROR
      "Slint 1.17.1 was not found. Set CMAKE_PREFIX_PATH to the project-local "
      "Slint C++ SDK. See scripts/bootstrap-slint.ps1.")
  endif()

  add_executable(tokmon-desktop WIN32
    apps/tokmon-desktop/main.cpp)
  target_link_libraries(tokmon-desktop PRIVATE tokmon_core Slint::Slint)
  if(MSVC)
    # Keep a GUI subsystem (no console window) while retaining the portable main().
    target_link_options(tokmon-desktop PRIVATE /ENTRY:mainCRTStartup)
  endif()
  slint_target_sources(tokmon-desktop
    apps/tokmon-desktop/ui/tokmon.slint)
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
endif()

include(CTest)
if(BUILD_TESTING AND TOKMON_BUILD_TESTS)
  add_subdirectory(tests)
endif()

install(TARGETS tokmond tokmon tokmon-launcher tokmon-lens-worker
  RUNTIME DESTINATION bin)
if(TARGET tokmon-desktop)
  install(TARGETS tokmon-desktop RUNTIME DESTINATION bin)
  install(DIRECTORY apps/tokmon-desktop/assets/ DESTINATION bin/assets)
  if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:tokmon-desktop> DESTINATION bin)
  endif()
endif()
install(TARGETS ${TOKMON_LENS_TARGETS}
  OPTIONAL RUNTIME DESTINATION bin/lenses LIBRARY DESTINATION bin/lenses)
install(DIRECTORY config/ DESTINATION share/tokmon/config)
install(DIRECTORY sdk/typescript/ DESTINATION share/tokmon/sdk/typescript
  PATTERN "examples" EXCLUDE)
install(DIRECTORY sdk/python/tokmon_lens_sdk DESTINATION share/tokmon/sdk/python
  PATTERN "__pycache__" EXCLUDE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/schemas")
  install(DIRECTORY schemas/ DESTINATION share/tokmon/schemas)
endif()
