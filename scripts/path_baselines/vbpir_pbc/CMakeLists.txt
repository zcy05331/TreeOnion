cmake_minimum_required(VERSION 3.16)

if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Build for the local x86_64 SEAL install")
endif()

project(vbpir_path_adapter LANGUAGES CXX)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(VBPIR_ROOT "${REPO_ROOT}/.benchmarks/vbpir-pbc" CACHE PATH "Staged VBPIR_PBC source root")
set(DEFAULT_SEAL_DIR
    "${REPO_ROOT}/../generic-superrecord/.benchmarks/seal-install-x86_64/lib/cmake/SEAL-4.0")
if(NOT DEFINED SEAL_DIR AND EXISTS "${DEFAULT_SEAL_DIR}/SEALConfig.cmake")
    set(SEAL_DIR "${DEFAULT_SEAL_DIR}" CACHE PATH "SEAL package configuration directory")
endif()

find_package(SEAL 4.0 REQUIRED)

set(VBPIR_SOURCES
    "${VBPIR_ROOT}/src/batchpirclient.cpp"
    "${VBPIR_ROOT}/src/batchpirparams.cpp"
    "${VBPIR_ROOT}/src/batchpirserver.cpp"
    "${VBPIR_ROOT}/src/client.cpp"
    "${VBPIR_ROOT}/src/pirparams.cpp"
    "${VBPIR_ROOT}/src/server.cpp"
)

add_executable(vbpir_path_adapter
    path_adapter.cpp
    path_execution.cpp
    path_input.cpp
    path_report.cpp
    ${VBPIR_SOURCES}
)

target_compile_features(vbpir_path_adapter PRIVATE cxx_std_17)
target_compile_options(vbpir_path_adapter PRIVATE -O2)
target_include_directories(vbpir_path_adapter PRIVATE
    "${VBPIR_ROOT}"
    "${VBPIR_ROOT}/header"
    "${REPO_ROOT}/scripts/path_baselines/common"
)
target_link_libraries(vbpir_path_adapter PRIVATE SEAL::seal)
