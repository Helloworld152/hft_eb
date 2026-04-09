# 支持：① 仓库根目录 add_subdirectory(trade_gateway) ② 仅在 trade_gateway 下 cmake -S . -B build
# 原先用 CMAKE_SOURCE_DIR 指向 third_party 在 ② 中会错（会变成 trade_gateway/third_party）
get_filename_component(HFT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_executable(hft_trade_gateway
    src/main.cpp
    src/gateway_runtime.cpp
    src/gateway_config.cpp
    src/ctp_gateway_adapter.cpp)

target_include_directories(hft_trade_gateway
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${HFT_REPO_ROOT}/core/include
        ${HFT_REPO_ROOT}/third_party/ctp/include)

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
