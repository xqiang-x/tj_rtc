cmake_minimum_required(VERSION 3.14)
project(mac_pull_test)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Homebrew prefix（Apple Silicon: /opt/homebrew，Intel Mac: /usr/local）
if(NOT DEFINED HOMEBREW_PREFIX)
    if(EXISTS "/opt/homebrew")
        set(HOMEBREW_PREFIX "/opt/homebrew")
    else()
        set(HOMEBREW_PREFIX "/usr/local")
    endif()
endif()
list(APPEND CMAKE_PREFIX_PATH "${HOMEBREW_PREFIX}")

find_package(Threads REQUIRED)
find_package(SDL2 REQUIRED)

# macOS 系统框架
find_library(FW_VIDEOTOOLBOX  VideoToolbox  REQUIRED)
find_library(FW_COREMEDIA     CoreMedia     REQUIRED)
find_library(FW_COREVIDEO     CoreVideo     REQUIRED)
find_library(FW_COREFOUNDATION CoreFoundation REQUIRED)

# 守卫式引入 rtc_client_sdk（支持独立构建）
# 路径: demos/mac_pull_test -> demos -> tj_rtc -> client_sdk/rtc_client_sdk
if(NOT TARGET rtc_client_sdk)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../client_sdk/rtc_client_sdk
                     ${CMAKE_BINARY_DIR}/rtc_client_sdk_build)
endif()

add_executable(mac_pull_test
    main.cpp
    H264Decoder.cpp
    SdlRenderer.cpp
)

target_include_directories(mac_pull_test PRIVATE
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(mac_pull_test PRIVATE
    rtc_client_sdk
    Threads::Threads
    ${SDL2_LIBRARIES}
    ${FW_VIDEOTOOLBOX}
    ${FW_COREMEDIA}
    ${FW_COREVIDEO}
    ${FW_COREFOUNDATION}
)

target_compile_options(mac_pull_test PRIVATE
    -Wall
    -Wno-deprecated-declarations
)
