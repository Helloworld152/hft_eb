cmake_minimum_required(VERSION 3.10)

# 支持：① 仓库根目录 add_subdirectory(trade_gateway) ② 仅在 trade_gateway 下 cmake -S . -B build
# 原先用 CMAKE_SOURCE_DIR 指向 third_party 在 ② 中会错（会变成 trade_gateway/third_party）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(hft_trade_gateway_standalone)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

get_filename_component(HFT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

find_package(Threads REQUIRED)
if(NOT TARGET yaml-cpp)
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    add_subdirectory(${HFT_REPO_ROOT}/third_party/yaml-cpp
                     ${CMAKE_CURRENT_BINARY_DIR}/yaml-cpp-build)
endif()
if(NOT TARGET spdlog::spdlog)
    if(EXISTS ${HFT_REPO_ROOT}/third_party/spdlog/CMakeLists.txt)
        set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
        add_subdirectory(${HFT_REPO_ROOT}/third_party/spdlog
                         ${CMAKE_CURRENT_BINARY_DIR}/spdlog-build)
    else()
        message(FATAL_ERROR "third_party/spdlog is required. Please init/update submodules.")
    endif()
endif()

add_executable(hft_trade_gateway
    src/main.cpp
    ../engine/src/logging.cpp
    src/gateway_adapter_factory.cpp
    src/gateway_runtime.cpp
    src/gateway_config.cpp
    src/ctp_gateway_adapter.cpp)

add_executable(hft_trade_gateway_ping
    src/gateway_ping.cpp)

set_target_properties(hft_trade_gateway PROPERTIES
    BUILD_RPATH "${HFT_REPO_ROOT}/bin"
    INSTALL_RPATH "${HFT_REPO_ROOT}/bin")

target_include_directories(hft_trade_gateway
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${HFT_REPO_ROOT}/include
        ${HFT_REPO_ROOT}/engine/include
        ${HFT_REPO_ROOT}/infra/include
        ${HFT_REPO_ROOT}/core/include
        ${HFT_REPO_ROOT}/third_party/ctp/include)

target_include_directories(hft_trade_gateway_ping
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${HFT_REPO_ROOT}/engine/include
        ${HFT_REPO_ROOT}/infra/include
        ${HFT_REPO_ROOT}/core/include)

target_link_directories(hft_trade_gateway
    PRIVATE
        ${HFT_REPO_ROOT}/bin
        ${HFT_REPO_ROOT}/third_party/ctp/lib)

target_link_libraries(hft_trade_gateway
    PRIVATE
        hft_core
        spdlog::spdlog
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
