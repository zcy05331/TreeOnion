cmake_minimum_required(VERSION 3.16)
project(treepir_sealpir_adapter CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED SEALPIRPLUS_DIR)
  set(SEALPIRPLUS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.benchmarks/TreePIR/SealPIRplus")
endif()

find_package(SEAL 4.0 REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(INSTRUMENTED_SEALPIR_DIR "${CMAKE_CURRENT_BINARY_DIR}/sealpirplus_instrumented")
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/patch_sealpir_core.py" "${SEALPIRPLUS_DIR}" "${INSTRUMENTED_SEALPIR_DIR}"
  RESULT_VARIABLE PATCH_SEALPIR_RESULT)
if(NOT PATCH_SEALPIR_RESULT EQUAL 0)
  message(FATAL_ERROR "failed to prepare instrumented SealPIRplus core")
endif()

add_library(sealpirplus_core STATIC
  "${INSTRUMENTED_SEALPIR_DIR}/pir.cpp"
  "${INSTRUMENTED_SEALPIR_DIR}/pir_client.cpp"
  "${INSTRUMENTED_SEALPIR_DIR}/pir_server.cpp")
target_include_directories(sealpirplus_core PUBLIC "${INSTRUMENTED_SEALPIR_DIR}")
target_link_libraries(sealpirplus_core PUBLIC SEAL::seal)

add_executable(treepir_sealpir_adapter treepir_sealpir_adapter.cpp)
target_link_libraries(treepir_sealpir_adapter PRIVATE sealpirplus_core)
