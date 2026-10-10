// SPDX-License-Identifier: BSD-2-Clause
// DmaHeap.h - 简化版 DMA-buf 分配器
// 移植自 rpicam-apps，用于为 libcamera FrameBuffer 分配可被 V4L2 编码器
// 通过 fd 共享访问的内存（DMABUF）。

#pragma once

#include <cstddef>
#include <libcamera/base/unique_fd.h>

class DmaHeap {
public:
    DmaHeap();
    ~DmaHeap();

    bool isValid() const { return heap_fd_.isValid(); }

    // 分配一段 DMA-buf 内存，返回其 fd
    libcamera::UniqueFD Alloc(const char *name, std::size_t size) const;

private:
    libcamera::UniqueFD heap_fd_;
};
