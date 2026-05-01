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
find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)

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
  CCAPI_ENABLE_EXCHANGE_BINANCE_USDS_FUTURES
)

# Installation/Output info
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Output dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# ==========================================
# CCAPI include path
# ==========================================
find_path(CCAPI_INCLUDE_DIR ccapi_cpp/ccapi_session.h)
if (NOT CCAPI_INCLUDE_DIR)
  message(FATAL_ERROR "CCAPI headers not found. Install CCAPI system-wide or pass -DCCAPI_INCLUDE_DIR=/path/to/ccapi/include.")
endif()

# RapidJSON (system provided)
find_path(RAPIDJSON_INCLUDE_DIR rapidjson/document.h)
if (NOT RAPIDJSON_INCLUDE_DIR)
  message(FATAL_ERROR "RapidJSON headers not found. Please install rapidjson (include path should contain rapidjson/document.h).")
endif()

target_include_directories(
  hft_ba_recorder
  PRIVATE
  ${CCAPI_INCLUDE_DIR}
  ${RAPIDJSON_INCLUDE_DIR}
)
