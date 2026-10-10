cmake_minimum_required(VERSION 3.14)
project(pi4_air LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# ── rtc_client_sdk（推流 SDK）─────────────────────────────────────────────────
# 路径: apps/demos/pi4_air -> apps/demos -> apps -> tj_rtc -> rtc/client_sdk/rtc_client_sdk
if(NOT TARGET rtc_client_sdk)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../rtc/client_sdk/rtc_client_sdk
                     ${CMAKE_BINARY_DIR}/rtc_client_sdk_build)
endif()

# ── libcamera（树莓派相机框架）──────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)

# ── 视频采集/编码组件复用 raspberry_pi_pusher/src ───────────────────────────
set(PUSHER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../raspberry_pi_pusher/src)

# ── 可执行程序 ──────────────────────────────────────────────────────────────
add_executable(pi4_air
    src/pi4_air.cpp
    ${PUSHER_SRC}/DmaHeap.cpp
    ${PUSHER_SRC}/LibcameraVideoSource.cpp
    ${PUSHER_SRC}/H264V4l2Encoder.cpp
    ${PUSHER_SRC}/NaluSplitter.cpp
)

target_include_directories(pi4_air PRIVATE
    ${PUSHER_SRC}
)

target_link_libraries(pi4_air PRIVATE
    rtc_client_sdk
    PkgConfig::LIBCAMERA
    Threads::Threads
)

target_compile_options(pi4_air PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)