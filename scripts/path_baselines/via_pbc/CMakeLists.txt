cmake_minimum_required(VERSION 3.13)
project(via_pbc LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# The VIA authors' flags (-O3 -march=native); the IFMA kernels need AVX512-IFMA.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")

find_package(HEXL 1.2.6 REQUIRED)
add_executable(via_pbc main.cpp)
set_target_properties(via_pbc PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
# pbc.hpp is shared with the KsPIR adapter, the ring arithmetic with the VIA-B adapter.
target_include_directories(via_pbc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../kspir_pbc ${CMAKE_CURRENT_SOURCE_DIR}/../via_b)
target_link_libraries(via_pbc PRIVATE HEXL::hexl)
