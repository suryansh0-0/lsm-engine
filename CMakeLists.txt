cmake_minimum_required(VERSION 3.14)
project(lsm_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

add_library(lsm STATIC src/wal.cpp src/sstable.cpp src/db.cpp)
target_include_directories(lsm PUBLIC include)

add_executable(lsm_tests tests/test_lsm.cpp)
target_link_libraries(lsm_tests lsm)

add_executable(lsm_bench bench/bench.cpp)
target_link_libraries(lsm_bench lsm)

add_executable(lsm_cli tools/lsm_cli.cpp)
target_link_libraries(lsm_cli lsm)

enable_testing()
add_test(NAME all COMMAND lsm_tests)
