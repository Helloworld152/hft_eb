cmake_minimum_required(VERSION 3.10)

# 支持：① 仓库根目录 add_subdirectory(trade_gateway) ② 仅在 trade_gateway 下 cmake -S . -B build
# 原先用 CMAKE_SOURCE_DIR 指向 third_party 在 ② 中会错（会变成 trade_gateway/third_party）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(hft_trade_gateway_standalone)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(HFT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

find_package(Threads REQUIRED)
find_package(yaml-cpp REQUIRED)

if(NOT TARGET hft_core)
    add_library(hft_core SHARED
        ${HFT_REPO_ROOT}/core/src/symbol_manager.cpp
        ${HFT_REPO_ROOT}/core/src/market_snapshot.cpp
        ${HFT_REPO_ROOT}/core/src/tick_window_snapshot.cpp
        ${HFT_REPO_ROOT}/core/src/core_state.cpp)
    target_include_directories(hft_core
        PUBLIC
            ${HFT_REPO_ROOT}/core/include)
    target_link_libraries(hft_core PRIVATE rt)
endif()

add_executable(hft_trade_gateway
    src/main.cpp
    src/gateway_adapter_factory.cpp
    src/gateway_runtime.cpp
    src/gateway_config.cpp
    src/ctp_gateway_adapter.cpp)

add_executable(hft_trade_gateway_ping
    src/gateway_ping.cpp)

target_include_directories(hft_trade_gateway
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${HFT_REPO_ROOT}/core/include
        ${HFT_REPO_ROOT}/third_party/ctp/include)

target_include_directories(hft_trade_gateway_ping
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${HFT_REPO_ROOT}/core/include)

target_link_directories(hft_trade_gateway
    PRIVATE
        ${HFT_REPO_ROOT}/third_party/ctp/lib)

target_link_libraries(hft_trade_gateway
    PRIVATE
        hft_core
        Threads::Threads
        yaml-cpp
        rt
        thosttraderapi_se
        -Wl,--no-as-needed
        pthread
        -Wl,--as-needed)

target_link_libraries(hft_trade_gateway_ping
    PRIVATE
        Threads::Threads
        rt)
