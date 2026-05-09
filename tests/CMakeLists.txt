cmake_minimum_required(VERSION 3.10)
project(hft_eb_tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG -march=native -mtune=native")
endif()

find_package(Threads REQUIRED)

if(NOT TARGET absl::flat_hash_map)
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
    set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/abseil-cpp
                     ${CMAKE_CURRENT_BINARY_DIR}/abseil-build)
endif()

add_executable(queue_bench queue_bench.cpp)
target_include_directories(queue_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(queue_bench PRIVATE Threads::Threads)

add_executable(mmap_format_test mmap_format_test.cpp)
target_include_directories(mmap_format_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mmap_format_test PRIVATE Threads::Threads)

add_executable(function_pointer_bench function_pointer_bench.cpp)
target_include_directories(function_pointer_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(function_pointer_bench PRIVATE Threads::Threads)

add_executable(rvo_move_bench rvo_move_bench.cpp)
target_include_directories(rvo_move_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(rvo_move_bench PRIVATE Threads::Threads)

add_executable(hash_map_bench hash_map_bench.cpp)
target_include_directories(hash_map_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(hash_map_bench PRIVATE Threads::Threads absl::flat_hash_map absl::flat_hash_set)
