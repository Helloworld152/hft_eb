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
link_directories(${CMAKE_SOURCE_DIR}/../bin) # 为了找到 libhft_core.so

find_package(yaml-cpp REQUIRED)

# Source files
set(SOURCES
    src/main.cpp
    src/Recorder.cpp
)

# Executable
add_executable(hft_recorder ${SOURCES})

# Links
# 链接 hft_core 以获得 MarketSnapshot 和 SymbolManager 支持
target_link_libraries(hft_recorder hft_core thostmduserapi_se pthread yaml-cpp)

# Tool: Data Reader
add_executable(hft_reader tools/read_dat.cpp)
target_link_libraries(hft_reader hft_core pthread)

# Installation/Output info
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Output dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
