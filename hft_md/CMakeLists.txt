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

# Add symbol_manager source (needed by Recorder)
set(SOURCES ${SOURCES} ../core/src/symbol_manager.cpp)

# Executable
add_executable(hft_recorder ${SOURCES})

# Links
target_link_libraries(hft_recorder thostmduserapi_se pthread yaml-cpp)

# Tool: Data Reader
add_executable(hft_reader tools/read_dat.cpp)

# Installation/Output info
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Output dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# ==========================================
# External Dependencies
# ==========================================
include(FetchContent)

# YAML-CPP
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG        0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

