cmake_minimum_required(VERSION 3.13)
project(kspir_pbc LANGUAGES CXX)

set(KSPIR_DIR "" CACHE PATH "Patched mmingluo/kspir checkout")
if(NOT KSPIR_DIR)
  message(FATAL_ERROR "-DKSPIR_DIR=<patched kspir checkout> is required")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# KsPIR's own Release flags (-O2 -march=native), for the library and the adapter.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=native")

find_package(HEXL 1.2.6 REQUIRED)
add_subdirectory(${KSPIR_DIR}/src kspir)
set_target_properties(pir PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_executable(kspir_pbc main.cpp)
set_target_properties(kspir_pbc PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(kspir_pbc PRIVATE ${KSPIR_DIR}/src)
target_link_libraries(kspir_pbc PRIVATE pir HEXL::hexl)
