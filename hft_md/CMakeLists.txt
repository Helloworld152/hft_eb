add_executable(hft_recorder
    src/main.cpp
    src/Recorder.cpp)

target_include_directories(hft_recorder
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/engine/include
        ${CMAKE_SOURCE_DIR}/infra/include
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/third_party/ctp/include)

target_link_directories(hft_recorder
    PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/ctp/lib)

target_link_libraries(hft_recorder
    PRIVATE
        hft_core
        hft_logging
        thostmduserapi_se
        pthread
        yaml-cpp)

add_executable(hft_reader tools/read_dat.cpp)
target_include_directories(hft_reader
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/engine/include
        ${CMAKE_SOURCE_DIR}/infra/include
        ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(hft_reader PRIVATE hft_core pthread)
