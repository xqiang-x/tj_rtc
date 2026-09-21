// SPDX-License-Identifier: BSD-2-Clause
// DmaHeap.cpp

#include "DmaHeap.h"

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>
#include <vector>

namespace {
// 优先使用 vidbuf_cached（Pi 5 的系统堆，Pi 4 的 CMA），否则回退到 linux,cma
const std::vector<const char *> kHeapNames{
    "/dev/dma_heap/vidbuf_cached",
    "/dev/dma_heap/linux,cma",
};
}  // namespace

DmaHeap::DmaHeap() {
    for (const char *name : kHeapNames) {
        int fd = ::open(name, O_RDWR | O_CLOEXEC, 0);
        if (fd < 0) continue;
        heap_fd_ = libcamera::UniqueFD(fd);
        return;
    }
    std::fprintf(stderr, "[DmaHeap] 无法打开任何 dma_heap 设备\n");
}

DmaHeap::~DmaHeap() = default;

libcamera::UniqueFD DmaHeap::Alloc(const char *name, std::size_t size) const {
    if (!name || !heap_fd_.isValid()) return {};

    struct dma_heap_allocation_data alloc {};
    alloc.len = size;
    alloc.fd_flags = O_CLOEXEC | O_RDWR;

    if (::ioctl(heap_fd_.get(), DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        std::fprintf(stderr, "[DmaHeap] 分配失败 name=%s size=%zu\n", name, size);
        return {};
    }

    libcamera::UniqueFD allocated(alloc.fd);
    if (::ioctl(allocated.get(), DMA_BUF_SET_NAME, name) < 0) {
        std::fprintf(stderr, "[DmaHeap] 命名失败 name=%s\n", name);
    }
    return allocated;
}
