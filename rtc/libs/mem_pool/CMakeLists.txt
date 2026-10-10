cmake_minimum_required(VERSION 3.10)
project(mem_pool_lib)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_MEM_POOL_STANDALONE ON)
else()
    set(_MEM_POOL_STANDALONE OFF)
endif()
option(BUILD_TESTS "Build mem_pool tests" ${_MEM_POOL_STANDALONE})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(mem_pool_lib STATIC MemPool.cpp)

target_include_directories(mem_pool_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(BUILD_TESTS)
    add_executable(test_mempool test_mempool.cpp)
    target_link_libraries(test_mempool PRIVATE mem_pool_lib)
endif()
