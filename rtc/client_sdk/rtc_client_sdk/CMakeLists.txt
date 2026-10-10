cmake_minimum_required(VERSION 3.14)
project(rtc_client_sdk)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_SDK_STANDALONE ON)
else()
    set(_SDK_STANDALONE OFF)
endif()
option(BUILD_DEMOS "Build SDK example" ${_SDK_STANDALONE})

# tj_rtc 根目录: client_sdk/rtc_client_sdk -> client_sdk -> tj_rtc
get_filename_component(TJ_RTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# 守卫式引入依赖（支持被单独 add_subdirectory，如 raspberry_pi_pusher）
if(NOT TARGET sfu_protocol)
    add_subdirectory(${TJ_RTC_ROOT}/common/sfu_protocol ${CMAKE_BINARY_DIR}/_deps/sfu_protocol)
endif()
if(NOT TARGET frame_protoc)
    add_subdirectory(${TJ_RTC_ROOT}/libs/frame_protoc ${CMAKE_BINARY_DIR}/_deps/frame_protoc)
endif()
if(NOT TARGET pacer_lib)
    add_subdirectory(${TJ_RTC_ROOT}/libs/pacer ${CMAKE_BINARY_DIR}/_deps/pacer)
endif()
if(NOT TARGET mem_pool_lib)
    add_subdirectory(${TJ_RTC_ROOT}/libs/mem_pool ${CMAKE_BINARY_DIR}/_deps/mem_pool)
endif()

# ── SDK 库 ────────────────────────────────────────────────────────────────────

set(SDK_HEADERS
    include/Publisher.h
    include/Subscriber.h
    include/SimulatedAudioSource.h
    include/P2PManager.h
    include/P2PDeduplicator.h
)

set(SDK_SOURCES
    src/Publisher.cpp
    src/Subscriber.cpp
    src/P2PManager.cpp
)

add_library(rtc_client_sdk STATIC ${SDK_SOURCES})

target_include_directories(rtc_client_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rtc_client_sdk PUBLIC
    sfu_protocol
    frame_protoc
    pacer_lib
    mem_pool_lib
    Threads::Threads
)

target_compile_options(rtc_client_sdk PRIVATE -Wall)

# ── 示例程序 ──────────────────────────────────────────────────────────────────

if(BUILD_DEMOS)
    add_executable(rtc_sdk_example examples/example_usage.cpp)
    target_link_libraries(rtc_sdk_example PRIVATE rtc_client_sdk)

    add_executable(test_ctrl_channel examples/test_ctrl_channel.cpp)
    target_link_libraries(test_ctrl_channel PRIVATE rtc_client_sdk)

    add_executable(test_pli examples/test_pli.cpp)
    target_link_libraries(test_pli PRIVATE rtc_client_sdk)
endif()

# ── 安装 ──────────────────────────────────────────────────────────────────────

install(TARGETS rtc_client_sdk ARCHIVE DESTINATION lib)
install(FILES ${SDK_HEADERS} DESTINATION include/rtc_client_sdk)
install(FILES
    ${TJ_RTC_ROOT}/common/sfu_protocol/SfuProtocol.h
    ${TJ_RTC_ROOT}/common/sfu_protocol/SfuSubscribeProtocol.h
    ${TJ_RTC_ROOT}/common/sfu_protocol/SfuFrameType.h
    DESTINATION include/rtc_client_sdk
)
