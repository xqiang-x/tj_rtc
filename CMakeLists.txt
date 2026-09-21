cmake_minimum_required(VERSION 3.16)
project(tj_rtc VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 可执行文件统一输出到 build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(BUILD_SERVER "Build SFU server (lib + executable)" ON)
option(BUILD_SDK    "Build rtc_client_sdk"                ON)
option(BUILD_DEMOS  "Build demos and runnable examples"   ON)
option(BUILD_TESTS  "Build tests"                         ON)

find_package(Threads REQUIRED)

# 共用协议头 + 基础库（无条件构建）
add_subdirectory(common/sfu_protocol)
add_subdirectory(libs/frame_protoc)
add_subdirectory(libs/pacer)
add_subdirectory(libs/mem_pool)

if(BUILD_SERVER)
    add_subdirectory(server)
endif()

if(BUILD_SDK)
    add_subdirectory(client_sdk/rtc_client_sdk)
endif()

if(BUILD_DEMOS)
    add_subdirectory(demos)
endif()
