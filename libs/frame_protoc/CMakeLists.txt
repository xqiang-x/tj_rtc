cmake_minimum_required(VERSION 3.14)
project(frame_protoc VERSION 1.0.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_FRAME_PROTOC_STANDALONE ON)
else()
    set(_FRAME_PROTOC_STANDALONE OFF)
endif()
option(BUILD_TESTS "Build frame_protoc tests" ${_FRAME_PROTOC_STANDALONE})
option(BUILD_DEMOS "Build frame_protoc demos (requires ffmpeg, SDL2)" ${_FRAME_PROTOC_STANDALONE})

# FEC 帧协议库（含 cm256 纠删码源码）
add_library(frame_protoc STATIC
    src/protocol.cpp
    src/sender.cpp
    src/receiver.cpp
    src/proxy_receiver.cpp
    src/bitrate_controller.cpp
    cm256cc/cm256.cpp
    cm256cc/gf256.cpp
)

target_include_directories(frame_protoc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/cm256cc
)

target_compile_definitions(frame_protoc PUBLIC NO_RESTRICT)

# SIMD 按架构分支（PUBLIC：使用 cm256 内联代码的消费方也需要这些宏）
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_compile_options(frame_protoc PUBLIC -mssse3)
    target_compile_definitions(frame_protoc PUBLIC USE_SSSE3)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    target_compile_definitions(frame_protoc PUBLIC USE_NEON)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|ARM")
    target_compile_options(frame_protoc PUBLIC -mfpu=neon)
    target_compile_definitions(frame_protoc PUBLIC USE_NEON)
endif()

if(BUILD_TESTS)
    add_executable(fec_test src/test/test_main.cpp)
    target_link_libraries(fec_test PRIVATE frame_protoc)

    add_executable(cm256_test cm256cc/unit_test/cm256_test.cpp)
    target_link_libraries(cm256_test PRIVATE frame_protoc)
endif()

# Demo（可选，需要 ffmpeg + SDL2）
if(BUILD_DEMOS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(AVFORMAT IMPORTED_TARGET libavformat)
        pkg_check_modules(AVCODEC IMPORTED_TARGET libavcodec)
        pkg_check_modules(AVUTIL IMPORTED_TARGET libavutil)
        pkg_check_modules(SWSCALE IMPORTED_TARGET libswscale)
        pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
    endif()

    set(FFMPEG_FOUND FALSE)
    if(AVFORMAT_FOUND AND AVCODEC_FOUND AND AVUTIL_FOUND AND SWSCALE_FOUND)
        set(FFMPEG_FOUND TRUE)
    endif()

    if(FFMPEG_FOUND AND SDL2_FOUND)
        message(STATUS "frame_protoc demos enabled (ffmpeg + SDL2 found)")

        add_executable(fec_demo_client demo/client.cpp)
        target_link_libraries(fec_demo_client PRIVATE
            frame_protoc
            PkgConfig::AVFORMAT
            PkgConfig::AVCODEC
            PkgConfig::AVUTIL
        )

        add_executable(fec_demo_server demo/server.cpp)
        target_link_libraries(fec_demo_server PRIVATE
            frame_protoc
            PkgConfig::AVCODEC
            PkgConfig::AVUTIL
            PkgConfig::SWSCALE
            PkgConfig::SDL2
        )

        # macOS 静态 ffmpeg 需要的系统框架
        if(APPLE)
            set(_MAC_FFMPEG_FRAMEWORKS
                "-framework CoreFoundation"
                "-framework Security"
                "-framework AudioToolbox"
                "-framework VideoToolbox"
                "-framework CoreMedia"
                "-framework CoreVideo"
                "-framework CoreServices"
                bz2 z iconv
            )
            target_link_libraries(fec_demo_client PRIVATE ${_MAC_FFMPEG_FRAMEWORKS})
            target_link_libraries(fec_demo_server PRIVATE ${_MAC_FFMPEG_FRAMEWORKS})
        endif()
    else()
        if(NOT FFMPEG_FOUND)
            message(STATUS "frame_protoc demos skipped: ffmpeg not found")
        endif()
        if(NOT SDL2_FOUND)
            message(STATUS "frame_protoc demos skipped: SDL2 not found")
        endif()
    endif()
endif()
