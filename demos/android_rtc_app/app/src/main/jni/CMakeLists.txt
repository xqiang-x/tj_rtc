cmake_minimum_required(VERSION 3.22)
project(android_rtc)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Android log 库
find_library(log-lib log)

# tj_rtc 项目根目录
# jni -> main -> src -> app -> android_rtc_app -> demos -> tj_rtc (6 个 ..)
set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../)

# 依赖目录
set(RTC_SDK_DIR ${PROJECT_ROOT}/client_sdk/rtc_client_sdk)
set(SFU_PROTOCOL_DIR ${PROJECT_ROOT}/common/sfu_protocol)
set(FRAME_PROTOC_DIR ${PROJECT_ROOT}/libs/frame_protoc)
set(PACER_DIR ${PROJECT_ROOT}/libs/pacer)
set(MEM_POOL_DIR ${PROJECT_ROOT}/libs/mem_pool)

# 编译所有源文件
add_library(rtc_native SHARED
    # JNI 桥接层
    RtcJniBridge.cpp

    # rtc_client_sdk (客户端代码)
    ${RTC_SDK_DIR}/src/Publisher.cpp
    ${RTC_SDK_DIR}/src/Subscriber.cpp
    ${RTC_SDK_DIR}/src/P2PManager.cpp

    # frame_protoc (FEC)
    ${FRAME_PROTOC_DIR}/src/protocol.cpp
    ${FRAME_PROTOC_DIR}/src/sender.cpp
    ${FRAME_PROTOC_DIR}/src/receiver.cpp
    ${FRAME_PROTOC_DIR}/src/proxy_receiver.cpp
    ${FRAME_PROTOC_DIR}/src/bitrate_controller.cpp
    ${FRAME_PROTOC_DIR}/cm256cc/gf256.cpp
    ${FRAME_PROTOC_DIR}/cm256cc/cm256.cpp

    # Pacer (带宽控制)
    ${PACER_DIR}/Pacer.cpp

    # MemPool (内存池)
    ${MEM_POOL_DIR}/MemPool.cpp

    # 注意：不包含 sfu_server 代码（需要 libev，Android 不支持）
    # 只编译客户端所需的代码
)

# 头文件目录
target_include_directories(rtc_native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RTC_SDK_DIR}/include
    ${SFU_PROTOCOL_DIR}  # SfuProtocol.h / SfuSubscribeProtocol.h / SfuFrameType.h
    ${FRAME_PROTOC_DIR}/src
    ${FRAME_PROTOC_DIR}/cm256cc
    ${PACER_DIR}
    ${MEM_POOL_DIR}
)

# 链接库
target_link_libraries(rtc_native
    ${log-lib}
    android
    EGL
    GLESv2
)

# 16 KB 页面对齐（Android 15 要求）
target_link_options(rtc_native PRIVATE
    "-Wl,-z,max-page-size=16384"
)

# 编译选项
target_compile_options(rtc_native PRIVATE
    -Wall
    -Wno-unused-parameter
    -Wno-unused-variable
)

# Android 特定设置
target_compile_definitions(rtc_native PRIVATE
    __ANDROID__
    NO_RESTRICT  # 禁用 restrict 关键字（Android 兼容性）
)

# SIMD：Android ABI 均为 ARM，使用 NEON 路径
if(ANDROID_ABI MATCHES "arm64-v8a|armeabi-v7a")
    target_compile_definitions(rtc_native PRIVATE USE_NEON)
    if(ANDROID_ABI STREQUAL "armeabi-v7a")
        target_compile_options(rtc_native PRIVATE -mfpu=neon)
    endif()
elseif(ANDROID_ABI MATCHES "x86")
    target_compile_definitions(rtc_native PRIVATE USE_SSSE3)
    target_compile_options(rtc_native PRIVATE -mssse3)
endif()
