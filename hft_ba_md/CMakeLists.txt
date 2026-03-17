cmake_minimum_required(VERSION 3.10)
project(hft_ba_md)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Include paths
include_directories(include)
include_directories(../core/include)
# Library paths
link_directories(${CMAKE_SOURCE_DIR}/../bin)

# Source files
set(BA_SOURCES
    src/main.cpp
    src/Recorder.cpp
)

# Executable
add_executable(hft_ba_recorder ${BA_SOURCES})

# Links
# hft_core provides MarketSnapshot
# CCAPI + OpenSSL
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)

target_link_libraries(
  hft_ba_recorder
  hft_core
  Boost::system
  OpenSSL::SSL
  OpenSSL::Crypto
  nlohmann_json::nlohmann_json
  yaml-cpp
  Threads::Threads
)

target_compile_definitions(
  hft_ba_recorder
  PRIVATE
  CCAPI_ENABLE_SERVICE_MARKET_DATA
  CCAPI_ENABLE_EXCHANGE_BINANCE
)

# Installation/Output info
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Output dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# ==========================================
# External Dependencies (reuse root build/_deps if available)
# ==========================================
include(FetchContent)
set(HFT_ROOT_DEPS_DIR "${CMAKE_SOURCE_DIR}/../build/_deps")
if (EXISTS "${HFT_ROOT_DEPS_DIR}/yaml-cpp-src" AND
    EXISTS "${HFT_ROOT_DEPS_DIR}/json-src")

  add_subdirectory(${HFT_ROOT_DEPS_DIR}/yaml-cpp-src ${CMAKE_BINARY_DIR}/yaml-cpp-build)
  add_subdirectory(${HFT_ROOT_DEPS_DIR}/json-src ${CMAKE_BINARY_DIR}/json-build)

else()
  # YAML-CPP
  FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
  )

  # JSON (nlohmann)
  FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
  )

  FetchContent_MakeAvailable(yaml-cpp json)
endif()

# CCAPI
FetchContent_Declare(
  ccapi
  GIT_REPOSITORY https://github.com/crypto-chassis/ccapi.git
  GIT_TAG        v7.8.0
)
FetchContent_MakeAvailable(ccapi)

# RapidJSON (system provided)
find_path(RAPIDJSON_INCLUDE_DIR rapidjson/document.h)
if (NOT RAPIDJSON_INCLUDE_DIR)
  message(FATAL_ERROR "RapidJSON headers not found. Please install rapidjson (include path should contain rapidjson/document.h).")
endif()

target_include_directories(
  hft_ba_recorder
  PRIVATE
  ${ccapi_SOURCE_DIR}/include
  ${RAPIDJSON_INCLUDE_DIR}
)
