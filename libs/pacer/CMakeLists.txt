cmake_minimum_required(VERSION 3.10)
project(pacer_lib)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_PACER_STANDALONE ON)
else()
    set(_PACER_STANDALONE OFF)
endif()
option(BUILD_TESTS "Build pacer tests" ${_PACER_STANDALONE})
option(BUILD_DEMOS "Build pacer examples" ${_PACER_STANDALONE})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_library(pacer_lib STATIC Pacer.cpp)

target_include_directories(pacer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(BUILD_TESTS)
    add_executable(test_pacer test_pacer.cpp)
    target_link_libraries(test_pacer PRIVATE pacer_lib Threads::Threads)
endif()

if(BUILD_DEMOS)
    add_executable(sfu_pacer_example sfu_pacer_example.cpp)
    target_link_libraries(sfu_pacer_example PRIVATE pacer_lib Threads::Threads)
endif()
