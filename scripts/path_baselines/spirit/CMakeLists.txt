cmake_minimum_required(VERSION 3.16)
project(spirit_path_adapter CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED SPIRIT_DIR)
  set(SPIRIT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../.benchmarks/SPIRIT")
endif()

if(NOT EXISTS "${SPIRIT_DIR}/src/SPIRIT/pir/batch_protocol.h")
  message(FATAL_ERROR "SPIRIT_DIR does not point to the public SPIRIT checkout: ${SPIRIT_DIR}")
endif()

function(validate_full_hex_commit value label)
  string(LENGTH "${value}" COMMIT_LENGTH)
  if(NOT COMMIT_LENGTH EQUAL 40 OR NOT value MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "${label} must be a full 40-hex commit")
  endif()
endfunction()

set(EXPLICIT_SPIRIT_SOURCE_COMMIT FALSE)
if(DEFINED SPIRIT_SOURCE_COMMIT)
  set(EXPLICIT_SPIRIT_SOURCE_COMMIT TRUE)
endif()

execute_process(
  COMMAND git -C "${SPIRIT_DIR}" rev-parse --is-inside-work-tree
  OUTPUT_VARIABLE SPIRIT_IS_GIT_WORKTREE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(SPIRIT_IS_GIT_WORKTREE STREQUAL "true")
  execute_process(
    COMMAND git -C "${SPIRIT_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE SPIRIT_GIT_HEAD
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  validate_full_hex_commit("${SPIRIT_GIT_HEAD}" "SPIRIT_DIR git HEAD")
  if(EXPLICIT_SPIRIT_SOURCE_COMMIT)
    validate_full_hex_commit("${SPIRIT_SOURCE_COMMIT}" "SPIRIT_SOURCE_COMMIT")
    string(TOLOWER "${SPIRIT_SOURCE_COMMIT}" SPIRIT_SOURCE_COMMIT_LOWER)
    string(TOLOWER "${SPIRIT_GIT_HEAD}" SPIRIT_GIT_HEAD_LOWER)
    if(NOT SPIRIT_SOURCE_COMMIT_LOWER STREQUAL SPIRIT_GIT_HEAD_LOWER)
      message(FATAL_ERROR "SPIRIT_SOURCE_COMMIT does not match git HEAD for SPIRIT_DIR")
    endif()
    set(SPIRIT_SOURCE_COMMIT "${SPIRIT_SOURCE_COMMIT_LOWER}")
  else()
    string(TOLOWER "${SPIRIT_GIT_HEAD}" SPIRIT_SOURCE_COMMIT)
  endif()
else()
  if(NOT EXPLICIT_SPIRIT_SOURCE_COMMIT)
    message(FATAL_ERROR "SPIRIT_DIR has no git metadata; pass -DSPIRIT_SOURCE_COMMIT=<full 40-hex upstream commit>")
  endif()
  validate_full_hex_commit("${SPIRIT_SOURCE_COMMIT}" "SPIRIT_SOURCE_COMMIT")
  string(TOLOWER "${SPIRIT_SOURCE_COMMIT}" SPIRIT_SOURCE_COMMIT)
endif()

# SPIRIT calls its fork's offline/online Galois and split-modswitch APIs.
# A system SEAL package does not provide those interfaces.
set(SEAL_BUILD_DEPS ON CACHE BOOL "" FORCE)
set(SEAL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SEAL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SEAL_USE_ZLIB OFF CACHE BOOL "" FORCE)
set(SEAL_USE_ZSTD OFF CACHE BOOL "" FORCE)
set(SEAL_THROW_ON_TRANSPARENT_CIPHERTEXT OFF CACHE BOOL "" FORCE)
add_subdirectory("${SPIRIT_DIR}/thirdparties/SEAL" "${CMAKE_CURRENT_BINARY_DIR}/seal" EXCLUDE_FROM_ALL)
set(SPIRIT_SEAL_TARGET seal)

find_package(OpenMP QUIET)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(INSTRUMENTED_SPIRIT_DIR "${CMAKE_CURRENT_BINARY_DIR}/spirit_instrumented")
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/patch_spirit_core.py" "${SPIRIT_DIR}" "${INSTRUMENTED_SPIRIT_DIR}"
  RESULT_VARIABLE PATCH_SPIRIT_RESULT)
if(NOT PATCH_SPIRIT_RESULT EQUAL 0)
  message(FATAL_ERROR "failed to prepare patched SPIRIT core")
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/patch_spirit_rc.py" "${INSTRUMENTED_SPIRIT_DIR}"
  RESULT_VARIABLE PATCH_SPIRIT_RC_RESULT)
if(NOT PATCH_SPIRIT_RC_RESULT EQUAL 0)
  message(FATAL_ERROR "failed to prepare SPIRIT RC public layout")
endif()

add_library(spirit_core STATIC
  "${INSTRUMENTED_SPIRIT_DIR}/SPIRIT/he/lwe_matrix.cpp"
  "${INSTRUMENTED_SPIRIT_DIR}/SPIRIT/he/lwe_pack.cpp"
  "${INSTRUMENTED_SPIRIT_DIR}/SPIRIT/he/lwe_ciphertext.cpp"
  "${INSTRUMENTED_SPIRIT_DIR}/SPIRIT/he/packlwes.cpp"
  "${INSTRUMENTED_SPIRIT_DIR}/SPIRIT/he/utils.cpp"
  "${INSTRUMENTED_SPIRIT_DIR}/SPIRIT/he/query_unpack.cpp"
  "${INSTRUMENTED_SPIRIT_DIR}/SPIRIT/he/rgsw_ciphertext.cpp"
  "${INSTRUMENTED_SPIRIT_DIR}/SPIRIT/pir/matmul.cpp"
  "${INSTRUMENTED_SPIRIT_DIR}/SPIRIT/pir/batch_protocol.cpp"
  "${INSTRUMENTED_SPIRIT_DIR}/SPIRIT/pir/pbc.cpp"
  "${INSTRUMENTED_SPIRIT_DIR}/SPIRIT/pir/public_matrix.cpp"
  "${INSTRUMENTED_SPIRIT_DIR}/SPIRIT/pir/protocol.cpp")
target_include_directories(spirit_core PUBLIC "${INSTRUMENTED_SPIRIT_DIR}")
target_link_libraries(spirit_core PUBLIC "${SPIRIT_SEAL_TARGET}")
if(OpenMP_FOUND)
  target_link_libraries(spirit_core PUBLIC OpenMP::OpenMP_CXX)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  target_compile_options(spirit_core PRIVATE -O3 -funroll-loops -march=native -mprefer-vector-width=512)
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  target_compile_options(spirit_core PRIVATE -O3 -funroll-loops -msse4.2)
else()
  target_compile_options(spirit_core PRIVATE -O3 -funroll-loops)
endif()

add_executable(spirit_path_adapter
  spirit_path_adapter.cpp
  spirit_adapter_input.cpp
  spirit_adapter_report.cpp
  spirit_adapter_backend.cpp
  spirit_rc.cpp
  spirit_rc_selector_pack.cpp)
target_link_libraries(spirit_path_adapter PRIVATE spirit_core)
target_compile_definitions(spirit_path_adapter PRIVATE
  SPIRIT_SOURCE_COMMIT="${SPIRIT_SOURCE_COMMIT}")

add_executable(spirit_rc_test spirit_rc_test.cpp spirit_rc.cpp spirit_rc_selector_pack.cpp)
target_link_libraries(spirit_rc_test PRIVATE spirit_core)
enable_testing()
add_test(NAME spirit_rc_dense_polynomials COMMAND spirit_rc_test)
add_executable(spirit_rc_pipeline_test spirit_rc_pipeline_test.cpp spirit_rc.cpp spirit_rc_selector_pack.cpp)
target_link_libraries(spirit_rc_pipeline_test PRIVATE spirit_core)
add_test(NAME spirit_rc_h8_full_path COMMAND spirit_rc_pipeline_test)
add_executable(spirit_rc_selector_pack_test spirit_rc_selector_pack_test.cpp spirit_rc_selector_pack.cpp)
target_link_libraries(spirit_rc_selector_pack_test PRIVATE spirit_core)
add_test(NAME spirit_rc_packed_selectors COMMAND spirit_rc_selector_pack_test)
