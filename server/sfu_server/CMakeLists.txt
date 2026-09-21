# 合并后的完整 SFU 服务器：框架层（EvPool/ConnManager/ServerBase/KvStore）+ 业务层（SfuServer）

find_package(Threads REQUIRED)

add_library(sfu_server STATIC
    SfuKvStore.cpp
    SfuEvPool.cpp
    SfuServerBase.cpp
    SfuConnManager.cpp
    SfuServer.cpp
    StunService.cpp
)

target_include_directories(sfu_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBEV_INCLUDE_DIR}
    ${GLOG_INCLUDE_DIR}
)

target_link_libraries(sfu_server PUBLIC
    sfu_protocol
    frame_protoc
    pacer_lib
    mem_pool_lib
    ${LIBEV_LIBRARY}
    ${GLOG_LIBRARY}
    ${UNWIND_LIBRARY}
    Threads::Threads
)

target_compile_options(sfu_server PRIVATE -Wall -Wextra -Wno-unused-parameter)

# 完整服务器可执行文件
add_executable(sfu_server_full main.cpp)
target_link_libraries(sfu_server_full PRIVATE sfu_server)

# 服务端框架用法示例
if(BUILD_DEMOS)
    add_executable(sfu_server_example example_usage.cpp)
    target_link_libraries(sfu_server_example PRIVATE sfu_server)
endif()
