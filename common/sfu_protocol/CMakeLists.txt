cmake_minimum_required(VERSION 3.10)

# SFU 共享协议头（header-only），服务端与客户端 SDK 共用
add_library(sfu_protocol INTERFACE)
target_include_directories(sfu_protocol INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
