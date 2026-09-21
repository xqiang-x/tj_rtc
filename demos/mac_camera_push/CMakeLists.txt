cmake_minimum_required(VERSION 3.10)
project(mac_camera_push LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS}  -Wall -Wno-unused-parameter")
set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -Wall -Wno-unused-parameter")

# 守卫式引入 rtc_client_sdk（支持独立构建）
# 路径: demos/mac_camera_push -> demos -> tj_rtc -> client_sdk/rtc_client_sdk
if(NOT TARGET rtc_client_sdk)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../client_sdk/rtc_client_sdk
                     ${CMAKE_BINARY_DIR}/rtc_client_sdk_build)
endif()

add_executable(mac_camera_push
    main.mm
    MacCameraCapture.mm
    H264Encoder.mm
)

target_link_libraries(mac_camera_push PRIVATE
    rtc_client_sdk
    pthread
    "-framework AVFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework VideoToolbox"
    "-framework Foundation"
    "-framework CoreFoundation"
)

# 需要 macOS 10.14+（AVFoundation 摄像头权限 API）
set_target_properties(mac_camera_push PROPERTIES
    MACOSX_BUNDLE FALSE
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.demo.mac_camera_push"
)

message(STATUS "==============================================")
message(STATUS "  mac_camera_push: Mac 摄像头 → H.264 → SFU")
message(STATUS "  用法: ./mac_camera_push [server_ip] [port] [stream] [user]")
message(STATUS "  默认: 127.0.0.1 9200 stream1 publisher1")
message(STATUS "  注意: 需要在 Info.plist 或系统偏好中授予摄像头权限")
message(STATUS "==============================================")
