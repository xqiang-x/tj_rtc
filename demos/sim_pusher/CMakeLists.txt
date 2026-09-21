cmake_minimum_required(VERSION 3.14)
project(sim_pusher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(X264 REQUIRED IMPORTED_TARGET x264)

if(NOT TARGET rtc_client_sdk)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../client_sdk/rtc_client_sdk
                     ${CMAKE_BINARY_DIR}/rtc_client_sdk_build)
endif()

add_executable(sim_pusher
    src/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../raspberry_pi_pusher/src/NaluSplitter.cpp
)

target_include_directories(sim_pusher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../raspberry_pi_pusher/src
)

target_link_libraries(sim_pusher PRIVATE
    rtc_client_sdk
    PkgConfig::X264
    Threads::Threads
)

target_compile_options(sim_pusher PRIVATE -Wall -Wextra -Wno-unused-parameter)
