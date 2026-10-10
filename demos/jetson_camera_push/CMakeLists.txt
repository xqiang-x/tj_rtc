cmake_minimum_required(VERSION 3.14)
project(jetson_camera_push LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# ── GStreamer（nvarguscamerasrc / nvv4l2h264enc / appsink）───────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0)

# ── rtc_client_sdk（推流 SDK）────────────────────────────────────────────────
# 路径: demos/jetson_camera_push -> demos -> tj_rtc -> client_sdk/rtc_client_sdk
if(NOT TARGET rtc_client_sdk)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../client_sdk/rtc_client_sdk
                     ${CMAKE_BINARY_DIR}/rtc_client_sdk_build)
endif()

# ── 可执行程序 ────────────────────────────────────────────────────────────────
# 复用 raspberry_pi_pusher 的 NaluSplitter（纯 C++，无外部依赖）
add_executable(jetson_camera_push
    src/main.cpp
    src/GstH264Source.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../raspberry_pi_pusher/src/NaluSplitter.cpp
)

target_include_directories(jetson_camera_push PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../raspberry_pi_pusher/src
)

target_link_libraries(jetson_camera_push PRIVATE
    rtc_client_sdk
    PkgConfig::GST
    Threads::Threads
)

target_compile_options(jetson_camera_push PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)