cmake_minimum_required(VERSION 3.20)
project(PerformanceDetective VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# DETECTIVE_SANITIZE: "" | "address" (ASan+UBSan) | "thread" (TSan)
set(DETECTIVE_SANITIZE "" CACHE STRING "Sanitizer configuration for the engine and tests")
option(DETECTIVE_BUILD_TESTS "Build the test suite" ON)

set(DETECTIVE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Wunused -Woverloaded-virtual -Wnull-dereference -Wformat=2)

set(DETECTIVE_SAN_FLAGS "")
if(DETECTIVE_SANITIZE STREQUAL "address")
  set(DETECTIVE_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
elseif(DETECTIVE_SANITIZE STREQUAL "thread")
  set(DETECTIVE_SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------- core engine
add_library(detective_core STATIC
  src/platform/process_runner.cpp
  src/sampling/perf_sampler.cpp
  src/symbols/symbol_resolver.cpp
  src/analysis/statistics.cpp
  src/analysis/call_tree.cpp
  src/analysis/flame_graph.cpp
  src/analysis/bottleneck_detector.cpp
  src/analysis/optimizer.cpp
  src/analysis/comparison.cpp
  src/analysis/instrumentation_report.cpp
  src/analysis/memory_report.cpp
  src/core/session.cpp
  src/core/profiler.cpp
  src/export/serialize.cpp
)
target_include_directories(detective_core PUBLIC include)
target_include_directories(detective_core SYSTEM PUBLIC third_party)
target_compile_options(detective_core PRIVATE ${DETECTIVE_WARNINGS} ${DETECTIVE_SAN_FLAGS})
target_link_options(detective_core PUBLIC ${DETECTIVE_SAN_FLAGS})
target_link_libraries(detective_core PUBLIC Threads::Threads)

add_library(detective_server STATIC src/server/api_server.cpp)
target_link_libraries(detective_server PUBLIC detective_core)
target_compile_options(detective_server PRIVATE ${DETECTIVE_WARNINGS} ${DETECTIVE_SAN_FLAGS})

add_executable(detective src/cli/main.cpp)
target_link_libraries(detective PRIVATE detective_server)
target_compile_options(detective PRIVATE ${DETECTIVE_WARNINGS} ${DETECTIVE_SAN_FLAGS})
target_compile_definitions(detective PRIVATE
  DETECTIVE_DEFAULT_FRONTEND="${CMAKE_SOURCE_DIR}/frontend"
  DETECTIVE_DEFAULT_ALLOC_LIB="$<TARGET_FILE:detective_alloc>"
  DETECTIVE_DEFAULT_EXAMPLES="${CMAKE_BINARY_DIR}/examples")

# ------------------------------------------- instrumentation library (Mode B)
add_library(detective_instr STATIC src/instrument/instrument.cpp)
target_include_directories(detective_instr PUBLIC include)
target_compile_options(detective_instr PRIVATE ${DETECTIVE_WARNINGS} -fno-omit-frame-pointer)
target_link_libraries(detective_instr PUBLIC Threads::Threads)

# ------------------------------------ allocation tracker (LD_PRELOAD shim)
add_library(detective_alloc SHARED src/alloc/alloc_shim.cpp)
target_compile_options(detective_alloc PRIVATE -O2 -g -fno-omit-frame-pointer -fvisibility=default -Wall -Wextra)
set_target_properties(detective_alloc PROPERTIES OUTPUT_NAME detective_alloc)

# ---------------------------------------------------------- example workloads
set(EXAMPLE_FLAGS -O2 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer)
foreach(ex inefficient_loop allocation_stress recursive_workload contention_demo data_processing)
  add_executable(${ex} examples/${ex}.cpp)
  target_compile_options(${ex} PRIVATE ${EXAMPLE_FLAGS})
  target_link_libraries(${ex} PRIVATE Threads::Threads)
  set_target_properties(${ex} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)
endforeach()

add_executable(instrumented_demo examples/instrumented_demo.cpp)
target_compile_options(instrumented_demo PRIVATE ${EXAMPLE_FLAGS})
target_link_libraries(instrumented_demo PRIVATE detective_instr)
set_target_properties(instrumented_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)

# Stripped copy of Workload A, used to test the missing-symbols path.
add_custom_command(TARGET inefficient_loop POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} --strip-all $<TARGET_FILE:inefficient_loop> ${CMAKE_BINARY_DIR}/examples/inefficient_loop_stripped
  COMMENT "Creating stripped copy of inefficient_loop")

# -------------------------------------------------------------------- tests
if(DETECTIVE_BUILD_TESTS)
  enable_testing()
  add_library(catch2_main STATIC third_party/catch_amalgamated.cpp)
  target_include_directories(catch2_main SYSTEM PUBLIC third_party)
  target_compile_options(catch2_main PRIVATE ${DETECTIVE_SAN_FLAGS})
  target_link_options(catch2_main PUBLIC ${DETECTIVE_SAN_FLAGS})

  add_executable(fixture_target tests/fixtures/fixture_target.cpp)
  target_compile_options(fixture_target PRIVATE -O1 -g -fno-omit-frame-pointer)

  add_executable(unit_tests
    tests/unit/test_statistics.cpp
    tests/unit/test_call_tree.cpp
    tests/unit/test_flame_graph.cpp
    tests/unit/test_analysis.cpp
    tests/unit/test_serialize.cpp
    tests/unit/test_session.cpp
    tests/unit/test_elf.cpp
    tests/unit/test_instrumentation.cpp)
  target_link_libraries(unit_tests PRIVATE detective_server detective_instr catch2_main)
  target_compile_options(unit_tests PRIVATE ${DETECTIVE_SAN_FLAGS})
  target_compile_definitions(unit_tests PRIVATE DETECTIVE_SELF_EXE_HINT="$<TARGET_FILE:fixture_target>")

  add_executable(integration_tests
    tests/integration/test_process_runner.cpp
    tests/integration/test_profiling.cpp
    tests/integration/test_api.cpp)
  target_link_libraries(integration_tests PRIVATE detective_server catch2_main)
  target_compile_options(integration_tests PRIVATE ${DETECTIVE_SAN_FLAGS})
  target_compile_definitions(integration_tests PRIVATE
    FIXTURE_TARGET="$<TARGET_FILE:fixture_target>"
    EXAMPLES_DIR="${CMAKE_BINARY_DIR}/examples"
    INSTR_DEMO="$<TARGET_FILE:instrumented_demo>"
    ALLOC_LIB="$<TARGET_FILE:detective_alloc>")
  add_dependencies(integration_tests fixture_target inefficient_loop recursive_workload
    allocation_stress contention_demo instrumented_demo detective_alloc)

  add_test(NAME unit_tests COMMAND unit_tests)
  add_test(NAME integration_tests COMMAND integration_tests)
  add_test(NAME frontend_tests COMMAND node --test ${CMAKE_SOURCE_DIR}/frontend/tests/flamegraph.test.mjs)
endif()

# ------------------------------------------------------------ engine benchmark
add_executable(engine_bench bench/engine_bench.cpp)
target_link_libraries(engine_bench PRIVATE detective_core detective_instr)
target_compile_options(engine_bench PRIVATE -fno-omit-frame-pointer)
