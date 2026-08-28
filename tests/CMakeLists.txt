add_executable(tokmon-tests
  main.cpp
  unit/core_tests.cpp
  unit/builtin_lens_tests.cpp
  ../lenses/ignis/tests/ignis_contract.cpp
  ../lenses/lemon/tests/lemon_contract.cpp
  ../lenses/iris/tests/iris_contract.cpp
  ../lenses/rhea/tests/rhea_contract.cpp
  ../lenses/janus/tests/janus_contract.cpp
  ../lenses/clotho/tests/clotho_contract.cpp
  ../lenses/aya/tests/aya_contract.cpp
  ../lenses/textus/tests/textus_contract.cpp
  ../lenses/enso/tests/enso_contract.cpp
  ../lenses/techor/tests/techor_contract.cpp
  ../lenses/styx/tests/styx_contract.cpp
  ../lenses/fallen/tests/fallen_contract.cpp
  ../lenses/cista/tests/cista_contract.cpp
  ../lenses/chora/tests/chora_contract.cpp
  ../lenses/tracket/tests/tracket_contract.cpp
  ../lenses/nota/tests/nota_contract.cpp
  ../lenses/cove/tests/cove_contract.cpp
  ../lenses/snow/tests/snow_contract.cpp
  ../lenses/termon/tests/termon_contract.cpp
  ../lenses/calculator/tests/calculator_contract.cpp)

target_link_libraries(tokmon-tests PRIVATE
  chhttp::chhttp
  tokmon_core
  tokmon_sqlite3
  chtest)
target_include_directories(tokmon-tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_compile_definitions(tokmon-tests PRIVATE
  TOKMON_TEST_LENS_PATH="$<TARGET_FILE:tokmon-lens-calculator>"
  TOKMON_TEST_LENS_DIR="$<TARGET_FILE_DIR:tokmon-lens-calculator>"
  TOKMON_WORKER_EXECUTABLE="$<TARGET_FILE:${TOKMON_WORKER_TARGET}>"
  TOKMON_CLI_EXECUTABLE="$<TARGET_FILE:tokmon>"
  TOKMON_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  TOKMON_CMAKE_COMMAND="${CMAKE_COMMAND}")
add_dependencies(tokmon-tests ${TOKMON_LENS_TARGETS} ${TOKMON_WORKER_TARGET} tokmon)

add_test(NAME tokmon-tests COMMAND tokmon-tests --quiet --no-color)

if(TOKMON_BUILD_SCRIPT_SDK_TESTS)
  find_program(TOKMON_NODE_EXECUTABLE NAMES node)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(TOKMON_NODE_EXECUTABLE)
    execute_process(COMMAND "${TOKMON_NODE_EXECUTABLE}" -p "process.versions.node"
      OUTPUT_VARIABLE TOKMON_NODE_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
    target_compile_definitions(tokmon-tests PRIVATE
      TOKMON_NODE_EXECUTABLE="${TOKMON_NODE_EXECUTABLE}"
      TOKMON_NODE_VERSION="${TOKMON_NODE_VERSION}")
    add_test(NAME sdk-node-contract
      COMMAND "${TOKMON_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/sdk_contract_node.mjs"
        "${PROJECT_SOURCE_DIR}/sdk/typescript")
  endif()
  if(Python3_Interpreter_FOUND)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c
      "import platform; print(platform.python_version())"
      OUTPUT_VARIABLE TOKMON_PYTHON_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
    target_compile_definitions(tokmon-tests PRIVATE
      TOKMON_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
      TOKMON_PYTHON_VERSION="${TOKMON_PYTHON_VERSION}")
    add_test(NAME sdk-cpython-contract
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/sdk_contract_python.py"
        "${PROJECT_SOURCE_DIR}/sdk/python")
  endif()
endif()
