cmake_minimum_required(VERSION 3.10)
project(hft_eb)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

set(HFT_BACKTEST_PACKAGE_DIR ${CMAKE_SOURCE_DIR}/hft_backtest)
set(HFT_BACKTEST_PACKAGE_LIB_DIR ${HFT_BACKTEST_PACKAGE_DIR}/lib)

function(hft_mark_backtest_package_lib target_name)
    if(TARGET ${target_name})
        set_target_properties(${target_name} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${HFT_BACKTEST_PACKAGE_LIB_DIR}
            BUILD_RPATH "\$ORIGIN"
        )
    endif()
endfunction()

# Dependencies
find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
execute_process(
    COMMAND ${Python3_EXECUTABLE} -m pybind11 --cmakedir
    OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT PYBIND11_CMAKE_DIR)
    execute_process(
        COMMAND python3 -m pybind11 --cmakedir
        OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(PYBIND11_CMAKE_DIR)
    set(pybind11_DIR ${PYBIND11_CMAKE_DIR} CACHE PATH "pybind11 CMake directory")
endif()
find_package(pybind11 CONFIG QUIET)
if(NOT TARGET yaml-cpp)
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(YAML_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/yaml-cpp
                   ${CMAKE_CURRENT_BINARY_DIR}/yaml-cpp-build)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
  if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/nlohmann_json/CMakeLists.txt)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/nlohmann_json
                     ${CMAKE_CURRENT_BINARY_DIR}/nlohmann_json-build)
  else()
    message(FATAL_ERROR "third_party/nlohmann_json is required. Please init/update submodules.")
  endif()
endif()
if(NOT TARGET spdlog::spdlog)
  if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/spdlog/CMakeLists.txt)
    set(SPDLOG_BUILD_PIC ON CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/spdlog
                     ${CMAKE_CURRENT_BINARY_DIR}/spdlog-build)
  else()
    message(FATAL_ERROR "third_party/spdlog is required. Please init/update submodules.")
  endif()
endif()
if(NOT TARGET absl::flat_hash_map)
  if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/abseil-cpp/CMakeLists.txt)
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
    set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/abseil-cpp
                     ${CMAKE_CURRENT_BINARY_DIR}/abseil-build)
  else()
    message(FATAL_ERROR "third_party/abseil-cpp is required. Please init/update submodules.")
  endif()
endif()
if(TARGET libzmq)
  set(ZMQ_LIBRARY libzmq)
elseif(EXISTS ${CMAKE_SOURCE_DIR}/third_party/libzmq/CMakeLists.txt)
  set(BUILD_SHARED ON CACHE BOOL "" FORCE)
  set(BUILD_STATIC OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
  set(WITH_DOC OFF CACHE BOOL "" FORCE)
  set(WITH_DOCS OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/libzmq
                   ${CMAKE_CURRENT_BINARY_DIR}/libzmq-build)
  set(ZMQ_LIBRARY libzmq)
else()
  message(FATAL_ERROR "third_party/libzmq is required. Please init/update submodules.")
endif()

# Global Includes
include_directories(include)
include_directories(engine/include)
include_directories(infra/include)
include_directories(core/include)

add_library(hft_hash_containers INTERFACE)
target_link_libraries(hft_hash_containers INTERFACE absl::flat_hash_map absl::flat_hash_set)

# ==========================================
# 0. 核心基础设施库 (共享单例)
# ==========================================
add_library(hft_logging STATIC engine/src/logging.cpp)
set_target_properties(hft_logging PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(hft_logging PUBLIC yaml-cpp spdlog::spdlog)

add_library(hft_core SHARED
    core/src/symbol_manager.cpp
    core/src/market_snapshot.cpp
    core/src/tick_window_snapshot.cpp
    core/src/core_state.cpp)
target_link_libraries(hft_core PRIVATE rt) # 显式链接实时库以支持 shm_open
target_link_libraries(hft_core PUBLIC hft_logging hft_hash_containers)

# 独立交易网关子系统
add_subdirectory(trade_gateway)
add_subdirectory(hft_md)

# 1. 编译插件 A: CTP (模拟)
add_library(mod_ctp SHARED modules/ctp/ctp_module.cpp)
target_include_directories(mod_ctp PRIVATE include)
target_link_libraries(mod_ctp PRIVATE hft_core -Wl,--no-as-needed pthread -Wl,--as-needed)

# 2. 编译插件 B: Strategy
add_library(mod_strategy SHARED modules/strategy/simple_strategy.cpp)
target_include_directories(mod_strategy PRIVATE include)
target_link_libraries(mod_strategy PRIVATE hft_core)

add_library(mod_t0_rb_strategy SHARED modules/strategy/t0_rb_strategy.cpp)
target_include_directories(mod_t0_rb_strategy PRIVATE include core/include)
target_link_libraries(mod_t0_rb_strategy PRIVATE hft_core)

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

# 2.14 编译插件 B-FactorNode: 500ms Mid Return
add_library(factor_mid_return_500ms SHARED modules/factor/mid_return_500ms_factor.cpp)
target_include_directories(factor_mid_return_500ms PRIVATE include core/include)
target_link_libraries(factor_mid_return_500ms PRIVATE hft_core)

# 2.14.1 编译插件 B-FactorNode: 500ms Weighted Mid Return
add_library(factor_weighted_mid_return_500ms SHARED modules/factor/weighted_mid_return_500ms_factor.cpp)
target_include_directories(factor_weighted_mid_return_500ms PRIVATE include core/include)
target_link_libraries(factor_weighted_mid_return_500ms PRIVATE hft_core)

# 2.15 编译插件 B-FactorNode: 500ms Quote Intensity
add_library(factor_quote_intensity_500ms SHARED modules/factor/quote_intensity_500ms_factor.cpp)
target_include_directories(factor_quote_intensity_500ms PRIVATE include core/include)
target_link_libraries(factor_quote_intensity_500ms PRIVATE hft_core)

# 2.16 编译插件 B-FactorNode: 500ms Trade Pressure
add_library(factor_trade_pressure_500ms SHARED modules/factor/trade_pressure_500ms_factor.cpp)
target_include_directories(factor_trade_pressure_500ms PRIVATE include core/include)
target_link_libraries(factor_trade_pressure_500ms PRIVATE hft_core)

# 3. 编译插件 C: SimpleTrade
add_library(mod_trade SHARED modules/trade/simple_trade.cpp)
target_include_directories(mod_trade PRIVATE include)
target_link_libraries(mod_trade PRIVATE hft_core)

# 3.0 编译插件 C-SimTrade: Simulated Exchange
add_library(mod_sim_trade SHARED modules/trade/sim_trade_module.cpp)
target_include_directories(mod_sim_trade PRIVATE include core/include)
target_link_libraries(mod_sim_trade PRIVATE hft_core)

# 3.0.1 编译插件 C-GatewayPoll: 独立 gateway 进程轮询桥接
add_library(mod_gateway_poll SHARED modules/trade/gateway_poll_module.cpp)
target_include_directories(mod_gateway_poll PRIVATE include core/include trade_gateway/include)
target_link_libraries(mod_gateway_poll PRIVATE hft_core rt)

# 3.1 CCAPI Binance USDT Futures Trade plugin is intentionally disabled.
# It requires CCAPI/RapidJSON/Boost/OpenSSL and should not be built by default.
# add_library(mod_ccapi_binance_usds_trade SHARED modules/trade/ccapi_binance_usds_trade.cpp)
# target_include_directories(mod_ccapi_binance_usds_trade PRIVATE include core/include)
# target_link_libraries(mod_ccapi_binance_usds_trade PRIVATE hft_core Threads::Threads)

# 4. 编译插件 D: Risk Control
add_library(mod_risk SHARED modules/risk/risk_module.cpp)
target_include_directories(mod_risk PRIVATE include)
target_link_libraries(mod_risk PRIVATE hft_core -Wl,--no-as-needed pthread -Wl,--as-needed)

# 4.1 编译插件 D-Portfolio: Portfolio Module
add_library(mod_portfolio SHARED modules/portfolio/portfolio_module.cpp)
target_include_directories(mod_portfolio PRIVATE include)
target_link_libraries(mod_portfolio PRIVATE hft_core yaml-cpp)

# 6. Monitor plugin is intentionally disabled.
# It requires IXWebSocket and should not be built by default.
# add_library(mod_monitor SHARED modules/monitor/monitor_module.cpp)
# target_include_directories(mod_monitor PRIVATE include)
# target_link_libraries(mod_monitor PRIVATE hft_core nlohmann_json::nlohmann_json ${ZMQ_LIBRARY})

# 6.1 编译插件 F-2: Signal CSV (EVENT_SIGNAL -> CSV)
add_library(mod_signal_csv SHARED modules/monitor/signal_csv_module.cpp)
target_include_directories(mod_signal_csv PRIVATE include)
target_link_libraries(mod_signal_csv PRIVATE hft_core -Wl,--no-as-needed pthread -Wl,--as-needed)

# 6.2 编译插件 F-3: Backtest Recorder
add_library(mod_backtest_recorder SHARED modules/monitor/backtest_recorder_module.cpp)
target_include_directories(mod_backtest_recorder PRIVATE include)
target_link_libraries(mod_backtest_recorder PRIVATE hft_core)

# 7. 编译插件 Test Harness (ZMQ 控制)
add_library(mod_test_harness SHARED modules/test_harness/test_harness_module.cpp)
target_include_directories(mod_test_harness PRIVATE include core/include)
target_link_libraries(mod_test_harness PRIVATE hft_core nlohmann_json::nlohmann_json ${ZMQ_LIBRARY} -Wl,--no-as-needed pthread -Wl,--as-needed)

# 8.0 编译插件 G-Test: Event Sampler
add_library(mod_event_sampler SHARED modules/test/event_sampler_module.cpp)
target_include_directories(mod_event_sampler PRIVATE include core/include)
target_link_libraries(mod_event_sampler PRIVATE hft_core nlohmann_json::nlohmann_json)

# 8. 编译插件 G: Replay (DataFeed)
add_library(mod_replay SHARED modules/replay/replay_module.cpp)
target_include_directories(mod_replay PRIVATE include core/include)
target_link_libraries(mod_replay PRIVATE hft_core yaml-cpp)


# 8.1 编译插件 G-Kline: Parquet 日线回放 — 显式使用 /usr 下头文件与 .so，避免 ArrowConfig 指到 conda
find_path(HFT_ARROW_INCLUDE NAMES arrow/api.h PATHS /usr/include NO_DEFAULT_PATH NO_CMAKE_PATH)
find_library(HFT_LIBARROW NAMES arrow PATHS /usr/lib/x86_64-linux-gnu /usr/lib NO_DEFAULT_PATH NO_CMAKE_PATH)
find_library(HFT_LIBPARQUET NAMES parquet PATHS /usr/lib/x86_64-linux-gnu /usr/lib NO_DEFAULT_PATH NO_CMAKE_PATH)
if (HFT_ARROW_INCLUDE AND HFT_LIBARROW AND HFT_LIBPARQUET)
    add_library(mod_kline_parquet_replay SHARED modules/kline/kline_parquet_replay_module.cpp)
    target_include_directories(mod_kline_parquet_replay PRIVATE include core/include ${HFT_ARROW_INCLUDE})
    target_link_libraries(mod_kline_parquet_replay PRIVATE hft_core ${HFT_LIBARROW} ${HFT_LIBPARQUET})
    message(STATUS "mod_kline_parquet_replay: ${HFT_LIBARROW} ${HFT_LIBPARQUET}")
else()
    message(STATUS "System Arrow/Parquet under /usr not found; skipping mod_kline_parquet_replay")
endif()

# 9. 编译插件 H: Kline (K线合成)
add_library(mod_kline SHARED modules/kline/kline_module.cpp)
target_include_directories(mod_kline PRIVATE include core/include)
target_link_libraries(mod_kline PRIVATE hft_core)

# 11. 编译插件 I: SweepTrader (扫单/TWAP)
add_library(mod_sweep_trader SHARED modules/sweep_trader/sweep_trader_module.cpp)
target_include_directories(mod_sweep_trader PRIVATE include core/include)
target_link_libraries(mod_sweep_trader PRIVATE hft_core)

# 7. 编译主程序 / 引擎库
add_library(hft_engine_lib SHARED engine/src/engine.cpp)
target_include_directories(hft_engine_lib PRIVATE include engine/include infra/include)
target_link_libraries(hft_engine_lib PRIVATE hft_core hft_logging dl pthread yaml-cpp)
set_target_properties(hft_engine_lib PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${HFT_BACKTEST_PACKAGE_LIB_DIR}
    BUILD_RPATH "\$ORIGIN"
)

add_executable(hft_engine engine/src/main.cpp)
target_include_directories(hft_engine PRIVATE include engine/include infra/include)
target_link_libraries(hft_engine PRIVATE hft_engine_lib)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")

if(pybind11_FOUND)
    pybind11_add_module(_core MODULE hft_backtest/native/hft_backtest_core.cpp)
    target_include_directories(_core PRIVATE include engine/include infra/include core/include)
    target_link_libraries(_core PRIVATE hft_engine_lib hft_core hft_logging)
    set_target_properties(_core PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/hft_backtest
        BUILD_RPATH "\$ORIGIN/lib"
    )
endif()

hft_mark_backtest_package_lib(yaml-cpp)
hft_mark_backtest_package_lib(hft_core)
hft_mark_backtest_package_lib(hft_engine_lib)
hft_mark_backtest_package_lib(mod_py_strategy)
hft_mark_backtest_package_lib(mod_replay)
hft_mark_backtest_package_lib(mod_risk)
hft_mark_backtest_package_lib(mod_sim_trade)
hft_mark_backtest_package_lib(mod_backtest_recorder)
if(TARGET mod_kline_parquet_replay)
    hft_mark_backtest_package_lib(mod_kline_parquet_replay)
endif()
