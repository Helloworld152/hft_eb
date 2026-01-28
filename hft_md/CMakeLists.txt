cmake_minimum_required(VERSION 3.10)
project(hft_md)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Include paths
include_directories(include)
include_directories(../core/include)
include_directories(../third_party/ctp/include)
include_directories(${CMAKE_SOURCE_DIR}/../../gateway_ctp/include)

# Library paths
link_directories(../third_party/ctp/lib)

# Source files
set(SOURCES src/main.cpp)

# Executable
add_executable(hft_recorder ${SOURCES})

# Links
target_link_libraries(hft_recorder thostmduserapi_se pthread)

# Tool: Data Reader
add_executable(hft_reader tools/read_dat.cpp)

# Installation/Output info
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Output dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
