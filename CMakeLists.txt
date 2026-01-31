cmake_minimum_required(VERSION 3.10)
project(hft_eb)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
    option(ENABLE_LTO "Enable Link Time Optimization" ON)
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

# Global Includes
include_directories(include)
include_directories(core/include)

# 1. 编译插件 A: CTP (模拟)
add_library(mod_ctp SHARED modules/ctp/ctp_module.cpp)
target_include_directories(mod_ctp PRIVATE include)
# 强制链接 pthread，防止 --as-needed 把它优化掉
target_link_libraries(mod_ctp PRIVATE -Wl,--no-as-needed pthread -Wl,--as-needed)

# 1.1 编译插件 A-Real: CTP (实盘)
add_library(mod_ctp_real SHARED modules/ctp_real/ctp_real_module.cpp)
target_include_directories(mod_ctp_real PRIVATE include third_party/ctp/include)
target_link_directories(mod_ctp_real PRIVATE ${CMAKE_SOURCE_DIR}/third_party/ctp/lib)
target_link_libraries(mod_ctp_real PRIVATE thostmduserapi_se thosttraderapi_se -Wl,--no-as-needed pthread -Wl,--as-needed)

# 2. 编译插件 B: Strategy
add_library(mod_strategy SHARED modules/strategy/simple_strategy.cpp)
target_include_directories(mod_strategy PRIVATE include)

# 2.1 编译插件 B-Tree: Strategy Tree Container
add_library(mod_strategy_tree SHARED modules/strategy/strategy_tree_module.cpp)
target_include_directories(mod_strategy_tree PRIVATE include)
target_link_libraries(mod_strategy_tree PRIVATE dl yaml-cpp)

# 2.2 编译插件 B-Leaf: Grid Strategy (Leaf)
add_library(strat_grid SHARED modules/strategy/grid_strategy.cpp)
target_include_directories(strat_grid PRIVATE include)
target_link_libraries(strat_grid PRIVATE yaml-cpp)

# 2.3 编译插件 B-Leaf: Price Jump Factor (Leaf)
add_library(strat_price_jump SHARED modules/strategy/price_jump_node.cpp)
target_include_directories(strat_price_jump PRIVATE include)

# 2.4 编译插件 B-Leaf: Stat Arb Strategy (Leaf)
add_library(strat_stat_arb SHARED modules/strategy/stat_arb_node.cpp)
target_include_directories(strat_stat_arb PRIVATE include)

# 2.5 编译插件 B-Leaf: SMA Factor (Leaf)
add_library(strat_sma SHARED modules/strategy/sma_factor_node.cpp)
target_include_directories(strat_sma PRIVATE include)

# 2.6 编译插件 B-Leaf: Imbalance Factor (Leaf)
add_library(strat_imbalance SHARED modules/strategy/imbalance_node.cpp)
target_include_directories(strat_imbalance PRIVATE include)

# 3. 编译插件 C: SimpleTrade
add_library(mod_trade SHARED modules/trade/simple_trade.cpp)
target_include_directories(mod_trade PRIVATE include)

# 4. 编译插件 D: Risk Control
add_library(mod_risk SHARED modules/risk/risk_module.cpp)
target_include_directories(mod_risk PRIVATE include)
target_link_libraries(mod_risk PRIVATE -Wl,--no-as-needed pthread -Wl,--as-needed)

# 5. 编译插件 E: Position Manager
add_library(mod_position SHARED modules/position/position_module.cpp)
target_include_directories(mod_position PRIVATE include)

# 6. 编译插件 F: Monitor (ZMQ)
add_library(mod_monitor SHARED modules/monitor/monitor_module.cpp)
target_include_directories(mod_monitor PRIVATE include)
# Link against system libzmq (assuming installed via apt/yum)
target_link_libraries(mod_monitor PRIVATE -Wl,--no-as-needed pthread zmq -Wl,--as-needed)

# 8. 编译插件 G: Replay (DataFeed)
add_library(mod_replay SHARED modules/replay/replay_module.cpp)
target_include_directories(mod_replay PRIVATE include core/include)
target_link_libraries(mod_replay PRIVATE yaml-cpp)

# 9. 编译插件 H: Kline (K线合成)
add_library(mod_kline SHARED modules/kline/kline_module.cpp)
target_include_directories(mod_kline PRIVATE include core/include)

# 7. 编译主程序
add_executable(hft_engine src/main.cpp src/engine.cpp)
target_include_directories(hft_engine PRIVATE include)
target_link_libraries(hft_engine PRIVATE dl pthread yaml-cpp)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")

# 10. 编译工具: read_kline
add_executable(read_kline tools/read_kline.cpp)
target_include_directories(read_kline PRIVATE core/include)
target_link_libraries(read_kline PRIVATE pthread)

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

