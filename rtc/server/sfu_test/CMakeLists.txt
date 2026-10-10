# SFU 测试：链接统一的 sfu_server 静态库（frame_protoc 等依赖经 PUBLIC 传递）

add_executable(test_server test_server.cpp)
target_link_libraries(test_server PRIVATE sfu_server)

add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE sfu_server)

add_executable(test_e2e test_e2e.cpp)
target_link_libraries(test_e2e PRIVATE sfu_server)
