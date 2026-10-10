# SFU 服务端（需要 libev）

# libev 探测（统一入口，替代各子工程五花八门的查找方式）
find_path(LIBEV_INCLUDE_DIR ev.h)
find_library(LIBEV_LIBRARY NAMES ev)
if(NOT LIBEV_INCLUDE_DIR OR NOT LIBEV_LIBRARY)
    message(FATAL_ERROR "libev not found. Install with: apt install libev-dev")
endif()
message(STATUS "libev: ${LIBEV_LIBRARY} (include: ${LIBEV_INCLUDE_DIR})")

# glog 探测
find_path(GLOG_INCLUDE_DIR glog/logging.h)
find_library(GLOG_LIBRARY NAMES glog)
if(NOT GLOG_INCLUDE_DIR OR NOT GLOG_LIBRARY)
    message(FATAL_ERROR "glog not found. Install from source: https://github.com/google/glog")
endif()
message(STATUS "glog: ${GLOG_LIBRARY} (include: ${GLOG_INCLUDE_DIR})")

# glog 静态库依赖 libunwind（栈回溯）
find_library(UNWIND_LIBRARY NAMES unwind)
if(UNWIND_LIBRARY)
    message(STATUS "libunwind: ${UNWIND_LIBRARY}")
else()
    message(WARNING "libunwind not found; linking glog may fail if it needs stack-trace symbols")
endif()

add_subdirectory(sfu_server)

if(BUILD_TESTS)
    add_subdirectory(sfu_test)
endif()
