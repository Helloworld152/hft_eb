cmake_minimum_required(VERSION 3.10)

# 支持：① 仓库根目录 add_subdirectory(hft_md) ② 仅在 hft_md 下 cmake -S . -B build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(hft_md_standalone)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

get_filename_component(HFT_EB_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(NOT TARGET yaml-cpp)
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    add_subdirectory(${HFT_EB_ROOT_DIR}/third_party/yaml-cpp
                     ${CMAKE_CURRENT_BINARY_DIR}/yaml-cpp-build)
endif()

if(NOT TARGET spdlog::spdlog)
    if(EXISTS ${HFT_EB_ROOT_DIR}/third_party/spdlog/CMakeLists.txt)
        set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
        add_subdirectory(${HFT_EB_ROOT_DIR}/third_party/spdlog
                         ${CMAKE_CURRENT_BINARY_DIR}/spdlog-build)
    else()
        message(FATAL_ERROR "third_party/spdlog is required. Please init/update submodules.")
    endif()
endif()

find_package(Threads REQUIRED)

add_executable(hft_recorder
    src/main.cpp
    src/Recorder.cpp
    ../engine/src/logging.cpp)

add_executable(hft_reader tools/read_dat.cpp)

set_target_properties(hft_recorder PROPERTIES
    BUILD_RPATH "${HFT_EB_ROOT_DIR}/bin"
    INSTALL_RPATH "${HFT_EB_ROOT_DIR}/bin")

set_target_properties(hft_reader PROPERTIES
    BUILD_RPATH "${HFT_EB_ROOT_DIR}/bin"
    INSTALL_RPATH "${HFT_EB_ROOT_DIR}/bin")

target_include_directories(hft_recorder
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${HFT_EB_ROOT_DIR}/include
        ${HFT_EB_ROOT_DIR}/engine/include
        ${HFT_EB_ROOT_DIR}/infra/include
        ${HFT_EB_ROOT_DIR}/core/include
        ${HFT_EB_ROOT_DIR}/third_party/ctp/include)

target_include_directories(hft_reader
    PRIVATE
        ${HFT_EB_ROOT_DIR}/include
        ${HFT_EB_ROOT_DIR}/engine/include
        ${HFT_EB_ROOT_DIR}/infra/include
        ${HFT_EB_ROOT_DIR}/core/include)

if(NOT TARGET hft_core)
    target_link_directories(hft_recorder
        PRIVATE
            ${HFT_EB_ROOT_DIR}/bin
            ${HFT_EB_ROOT_DIR}/third_party/ctp/lib)
    target_link_directories(hft_reader
        PRIVATE
            ${HFT_EB_ROOT_DIR}/bin)
else()
    target_link_directories(hft_recorder
        PRIVATE
            ${HFT_EB_ROOT_DIR}/third_party/ctp/lib)
endif()

target_link_libraries(hft_recorder
    PRIVATE
        hft_core
        spdlog::spdlog
        Threads::Threads
        yaml-cpp
        thostmduserapi_se
        pthread)

target_link_libraries(hft_reader
    PRIVATE
        hft_core
        spdlog::spdlog
        Threads::Threads
        pthread)
