cmake_minimum_required(VERSION 3.10)
project(hft_eb)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

include(FetchContent)

# CCAPI (needed by Binance trade plugin)
FetchContent_Declare(
  ccapi
  GIT_REPOSITORY https://github.com/crypto-chassis/ccapi.git
  GIT_TAG        v7.8.0
)
FetchContent_MakeAvailable(ccapi)

# ==========================================
# 编译优化配置
# ==========================================
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # 基础优化级别（Release 模式默认 -O3）
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
    
    # 架构特定优化：针对当前 CPU 优化（重要：提升 10-20%）
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=native -mtune=native")
    
    # 链接时优化（LTO）：跨模块优化（重要：提升 5-15%，但编译慢）
    # 注意：LTO 会增加编译时间，但能显著提升性能
    option(ENABLE_LTO "Enable Link Time Optimization" OFF)
    if(ENABLE_LTO)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto")
        set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-flto")
        set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "-flto")
    endif()
    
    # 内联优化：更激进的内联
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -finline-functions")
    
    # 循环优化：自动循环展开
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -funroll-loops")
    
    # 其他优化
    # -fomit-frame-pointer: 省略帧指针，提升性能但影响调试
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fomit-frame-pointer")
    
    # -fstrict-aliasing: 严格别名优化（需要代码保证无别名冲突）
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fstrict-aliasing")
    
    # 数学优化（谨慎使用：可能改变浮点精度）
    # 如果对浮点精度要求不高，可以启用 -ffast-math
    option(ENABLE_FAST_MATH "Enable fast math optimizations" OFF)
    if(ENABLE_FAST_MATH)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math -fno-math-errno")
        message(STATUS "  - Fast math: enabled (may affect floating-point precision)")
    endif()
    
    # 警告级别（Release 模式减少警告噪音）
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Wno-unused-variable -Wno-unused-parameter")
    
    message(STATUS "Release build: Optimizations enabled")
    message(STATUS "  - Architecture: native")
    if(ENABLE_LTO)
        message(STATUS "  - LTO: enabled")
    endif()
endif()

# Debug 模式配置
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fno-omit-frame-pointer")
    # Sanitizers 可选（会增加运行开销）
    option(ENABLE_SANITIZERS "Enable sanitizers in Debug mode" OFF)
    if(ENABLE_SANITIZERS)
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fsanitize=undefined")
        message(STATUS "Debug build: Sanitizers enabled")
    endif()
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Dependencies
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Python3 COMPONENTS Development REQUIRED)
find_path(RAPIDJSON_INCLUDE_DIR rapidjson/document.h)
if (NOT RAPIDJSON_INCLUDE_DIR)
  message(FATAL_ERROR "RapidJSON headers not found. Please install rapidjson (include path should contain rapidjson/document.h).")
endif()

# Global Includes
include_directories(include)
include_directories(core/include)

# ==========================================
# 0. 核心基础设施库 (共享单例)
# ==========================================
add_library(hft_core SHARED core/src/symbol_manager.cpp core/src/market_snapshot.cpp)
target_link_libraries(hft_core PRIVATE rt) # 显式链接实时库以支持 shm_open

# 1. 编译插件 A: CTP (模拟)
add_library(mod_ctp SHARED modules/ctp/ctp_module.cpp)
target_include_directories(mod_ctp PRIVATE include)
target_link_libraries(mod_ctp PRIVATE hft_core -Wl,--no-as-needed pthread -Wl,--as-needed)

# 1.1 编译插件 A-Real: CTP (实盘)
add_library(mod_ctp_real SHARED modules/ctp_real/ctp_real_module.cpp)
target_include_directories(mod_ctp_real PRIVATE include third_party/ctp/include)
target_link_directories(mod_ctp_real PRIVATE ${CMAKE_SOURCE_DIR}/third_party/ctp/lib)
target_link_libraries(mod_ctp_real PRIVATE hft_core thostmduserapi_se thosttraderapi_se -Wl,--no-as-needed pthread -Wl,--as-needed)

# 2. 编译插件 B: Strategy
add_library(mod_strategy SHARED modules/strategy/simple_strategy.cpp)
target_include_directories(mod_strategy PRIVATE include)
target_link_libraries(mod_strategy PRIVATE hft_core)

add_library(mod_py_strategy SHARED modules/py_strategy/py_strategy_module.cpp)
target_include_directories(mod_py_strategy PRIVATE include core/include ${Python3_INCLUDE_DIRS})
target_link_libraries(mod_py_strategy PRIVATE hft_core Python3::Python)

# 2.1 编译插件 B-Tree: Strategy Tree Container
add_library(mod_strategy_tree SHARED modules/strategy/strategy_tree_module.cpp)
target_include_directories(mod_strategy_tree PRIVATE include)
target_link_libraries(mod_strategy_tree PRIVATE hft_core dl yaml-cpp)

add_library(mod_strategy_tree_parallel SHARED modules/strategy/strategy_tree_parallel_module.cpp)
target_include_directories(mod_strategy_tree_parallel PRIVATE include)
target_link_libraries(mod_strategy_tree_parallel PRIVATE hft_core dl yaml-cpp)

# 2.2 编译插件 B-Leaf: Grid Strategy (Leaf)
add_library(strat_grid SHARED modules/strategy/grid_strategy.cpp)
target_include_directories(strat_grid PRIVATE include)
target_link_libraries(strat_grid PRIVATE hft_core yaml-cpp)


# 2.2.1 编译插件 B-Leaf: Cross-Section Combiner (Leaf)
add_library(strat_cs_combiner SHARED modules/strategy/cs_combiner_node.cpp)
target_include_directories(strat_cs_combiner PRIVATE include)
target_link_libraries(strat_cs_combiner PRIVATE hft_core yaml-cpp)

# 2.3 编译插件 B-Leaf: Price Jump Factor (Leaf)
add_library(strat_price_jump SHARED modules/strategy/price_jump_node.cpp)
target_include_directories(strat_price_jump PRIVATE include)
target_link_libraries(strat_price_jump PRIVATE hft_core)

# 2.4 编译插件 B-Leaf: Stat Arb Strategy (Leaf)
add_library(strat_stat_arb SHARED modules/strategy/stat_arb_node.cpp)
target_include_directories(strat_stat_arb PRIVATE include)
target_link_libraries(strat_stat_arb PRIVATE hft_core)

# 2.5 编译插件 B-Leaf: SMA Factor (Leaf)
add_library(strat_sma SHARED modules/strategy/sma_factor_node.cpp)
target_include_directories(strat_sma PRIVATE include)
target_link_libraries(strat_sma PRIVATE hft_core)

# 2.6 编译插件 B-Leaf: Imbalance Factor (Leaf)
add_library(strat_imbalance SHARED modules/strategy/imbalance_node.cpp)
target_include_directories(strat_imbalance PRIVATE include)
target_link_libraries(strat_imbalance PRIVATE hft_core)

# 2.7 编译插件 B-Factor: Factor DAG Module
add_library(mod_factor_dag SHARED modules/factor/factor_dag_module.cpp)
target_include_directories(mod_factor_dag PRIVATE include core/include)
target_link_libraries(mod_factor_dag PRIVATE hft_core dl yaml-cpp)

# 2.8 编译插件 B-FactorNode: Last Price
add_library(factor_last_price SHARED modules/factor/last_price_factor.cpp)
target_include_directories(factor_last_price PRIVATE include core/include)
target_link_libraries(factor_last_price PRIVATE hft_core)

# 2.9 编译插件 B-FactorNode: SMA
add_library(factor_sma SHARED modules/factor/sma_factor.cpp)
target_include_directories(factor_sma PRIVATE include core/include)
target_link_libraries(factor_sma PRIVATE hft_core)

# 2.10 编译插件 B-FactorNode: Spread
add_library(factor_spread SHARED modules/factor/spread_factor.cpp)
target_include_directories(factor_spread PRIVATE include core/include)
target_link_libraries(factor_spread PRIVATE hft_core)

# 2.11 编译插件 B-FactorNode: Imbalance
add_library(factor_imbalance SHARED modules/factor/imbalance_factor.cpp)
target_include_directories(factor_imbalance PRIVATE include core/include)
target_link_libraries(factor_imbalance PRIVATE hft_core)

# 2.12 编译插件 B-FactorNode: Volatility
add_library(factor_volatility SHARED modules/factor/volatility_factor.cpp)
target_include_directories(factor_volatility PRIVATE include core/include)
target_link_libraries(factor_volatility PRIVATE hft_core)

# 2.13 编译插件 B-FactorNode: Combiner
add_library(factor_combiner SHARED modules/factor/combiner_factor.cpp)
target_include_directories(factor_combiner PRIVATE include core/include)
target_link_libraries(factor_combiner PRIVATE hft_core yaml-cpp)

# 3. 编译插件 C: SimpleTrade
add_library(mod_trade SHARED modules/trade/simple_trade.cpp)
target_include_directories(mod_trade PRIVATE include)
target_link_libraries(mod_trade PRIVATE hft_core)

# 3.0 编译插件 C-SimTrade: Simulated Exchange
add_library(mod_sim_trade SHARED modules/trade/sim_trade_module.cpp)
target_include_directories(mod_sim_trade PRIVATE include core/include)
target_link_libraries(mod_sim_trade PRIVATE hft_core)

# 3.1 编译插件 C-CCAPI: Binance USDT Futures Trade
add_library(mod_ccapi_binance_usds_trade SHARED modules/trade/ccapi_binance_usds_trade.cpp)
target_include_directories(mod_ccapi_binance_usds_trade PRIVATE include core/include ${ccapi_SOURCE_DIR}/include ${RAPIDJSON_INCLUDE_DIR})
target_link_libraries(mod_ccapi_binance_usds_trade PRIVATE hft_core Boost::system OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
target_compile_definitions(mod_ccapi_binance_usds_trade PRIVATE CCAPI_ENABLE_SERVICE_EXECUTION_MANAGEMENT CCAPI_ENABLE_EXCHANGE_BINANCE_USDS_FUTURES)

# 4. 编译插件 D: Risk Control
add_library(mod_risk SHARED modules/risk/risk_module.cpp)
target_include_directories(mod_risk PRIVATE include)
target_link_libraries(mod_risk PRIVATE hft_core -Wl,--no-as-needed pthread -Wl,--as-needed)

# 4.1 编译插件 D-Portfolio: Portfolio Module
add_library(mod_portfolio SHARED modules/portfolio/portfolio_module.cpp)
target_include_directories(mod_portfolio PRIVATE include)
target_link_libraries(mod_portfolio PRIVATE hft_core yaml-cpp)

# 5. 编译插件 E: Position Manager
add_library(mod_position SHARED modules/position/position_module.cpp)
target_include_directories(mod_position PRIVATE include)
target_link_libraries(mod_position PRIVATE hft_core nlohmann_json::nlohmann_json)

# 5.1 编译插件 E-Order: Order Manager
add_library(mod_order SHARED modules/order/order_manager_module.cpp)
target_include_directories(mod_order PRIVATE include core/include)
target_link_libraries(mod_order PRIVATE hft_core)

# 6. 编译插件 F: Monitor (WebSocket + ZMQ)
add_library(mod_monitor SHARED modules/monitor/monitor_module.cpp)
target_include_directories(mod_monitor PRIVATE include)
target_link_libraries(mod_monitor PRIVATE hft_core ixwebsocket nlohmann_json::nlohmann_json zmq -Wl,--no-as-needed pthread -Wl,--as-needed)

# 6.1 编译插件 F-2: Signal CSV (EVENT_SIGNAL -> CSV)
add_library(mod_signal_csv SHARED modules/monitor/signal_csv_module.cpp)
target_include_directories(mod_signal_csv PRIVATE include)
target_link_libraries(mod_signal_csv PRIVATE hft_core -Wl,--no-as-needed pthread -Wl,--as-needed)

# 7. 编译插件 Test Harness (ZMQ 控制)
add_library(mod_test_harness SHARED modules/test_harness/test_harness_module.cpp)
target_include_directories(mod_test_harness PRIVATE include core/include)
target_link_libraries(mod_test_harness PRIVATE hft_core nlohmann_json::nlohmann_json zmq -Wl,--no-as-needed pthread -Wl,--as-needed)

# 8. 编译插件 G: Replay (DataFeed)
add_library(mod_replay SHARED modules/replay/replay_module.cpp)
target_include_directories(mod_replay PRIVATE include core/include)
target_link_libraries(mod_replay PRIVATE hft_core yaml-cpp)

# 9. 编译插件 H: Kline (K线合成)
add_library(mod_kline SHARED modules/kline/kline_module.cpp)
target_include_directories(mod_kline PRIVATE include core/include)
target_link_libraries(mod_kline PRIVATE hft_core)

# 11. 编译插件 I: SweepTrader (扫单/TWAP)
add_library(mod_sweep_trader SHARED modules/sweep_trader/sweep_trader_module.cpp)
target_include_directories(mod_sweep_trader PRIVATE include core/include)
target_link_libraries(mod_sweep_trader PRIVATE hft_core)

# 7. 编译主程序
add_executable(hft_engine src/main.cpp src/engine.cpp)
target_include_directories(hft_engine PRIVATE include)
target_link_libraries(hft_engine PRIVATE hft_core dl pthread yaml-cpp)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")

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

# IXWebSocket
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG        v11.4.4
)

# JSON (nlohmann)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)

FetchContent_MakeAvailable(yaml-cpp ixwebsocket json)
