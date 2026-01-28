cmake_minimum_required(VERSION 3.10)
project(hft_eb)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# 7. 编译主程序
add_executable(hft_engine src/main.cpp src/engine.cpp)
target_include_directories(hft_engine PRIVATE include)
# 引入项目现有的 rapidjson
target_include_directories(hft_engine PRIVATE "${CMAKE_SOURCE_DIR}/../gateway_ctp/include") 
target_link_libraries(hft_engine PRIVATE dl pthread)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")
