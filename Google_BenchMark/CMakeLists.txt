cmake_minimum_required(VERSION 3.13)
project(Google_BenchMark)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

if(NOT TARGET absl::flat_hash_map)
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
    set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/abseil-cpp
                     ${CMAKE_CURRENT_BINARY_DIR}/abseil-build)
endif()

if(NOT TARGET benchmark::benchmark)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/benchmark
                     ${CMAKE_CURRENT_BINARY_DIR}/benchmark-build)
endif()

add_executable(hash_map_benchmark hash_map_benchmark.cpp)
target_link_libraries(hash_map_benchmark
    PRIVATE
    Threads::Threads
    absl::flat_hash_map
    benchmark::benchmark_main)
target_include_directories(hash_map_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(queue_benchmark queue_benchmark.cpp)
target_link_libraries(queue_benchmark
    PRIVATE
    Threads::Threads
    benchmark::benchmark_main)
target_include_directories(queue_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(queue_latency queue_latency.cpp)
target_link_libraries(queue_latency PRIVATE Threads::Threads)
target_include_directories(queue_latency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
