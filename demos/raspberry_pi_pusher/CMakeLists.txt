cmake_minimum_required(VERSION 3.14)
project(raspberry_pi_pusher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# ── rtc_client_sdk（推流 SDK）─────────────────────────────────────────────────
# 直接以子目录方式集成，无需安装
# 路径: demos/raspberry_pi_pusher -> demos -> tj_rtc -> client_sdk/rtc_client_sdk
if(NOT TARGET rtc_client_sdk)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../client_sdk/rtc_client_sdk
                     ${CMAKE_BINARY_DIR}/rtc_client_sdk_build)
endif()

# ── libcamera（树莓派相机框架）──────────────────────────────────────────────
# 通过 pkg-config 查找系统安装的 libcamera
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)

# ── 可执行程序 ──────────────────────────────────────────────────────────────
add_executable(raspberry_pi_pusher
    src/main.cpp
    src/DmaHeap.cpp
    src/LibcameraVideoSource.cpp
    src/H264V4l2Encoder.cpp
    src/NaluSplitter.cpp
)

target_include_directories(raspberry_pi_pusher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(raspberry_pi_pusher PRIVATE
    rtc_client_sdk
    PkgConfig::LIBCAMERA
    Threads::Threads
)

target_compile_options(raspberry_pi_pusher PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)
