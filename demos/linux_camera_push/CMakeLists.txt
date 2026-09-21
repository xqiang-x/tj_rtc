cmake_minimum_required(VERSION 3.14)
project(linux_camera_push LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# ── FFmpeg 库（采集 + 编码，替代 fork ffmpeg 子进程）─────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libavdevice libswscale
)

# ── rtc_client_sdk（推流 SDK）─────────────────────────────────────────────────
# 路径: demos/linux_camera_push -> demos -> tj_rtc -> client_sdk/rtc_client_sdk
if(NOT TARGET rtc_client_sdk)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../client_sdk/rtc_client_sdk
                     ${CMAKE_BINARY_DIR}/rtc_client_sdk_build)
endif()

# ── 可执行程序 ────────────────────────────────────────────────────────────────
# 复用 raspberry_pi_pusher 的 NaluSplitter（纯 C++，无外部依赖）
add_executable(linux_camera_push
    src/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../raspberry_pi_pusher/src/NaluSplitter.cpp
)

target_include_directories(linux_camera_push PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../raspberry_pi_pusher/src
)

target_link_libraries(linux_camera_push PRIVATE
    rtc_client_sdk
    Threads::Threads
    PkgConfig::FFMPEG
)

# ── 极简拉流验证工具 ──────────────────────────────────────────────────────────
add_executable(pull_probe
    src/pull_probe.cpp
)

target_link_libraries(pull_probe PRIVATE
    rtc_client_sdk
    Threads::Threads
)

target_compile_options(linux_camera_push PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)
