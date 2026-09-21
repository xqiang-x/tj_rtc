// SPDX-License-Identifier: BSD-2-Clause
// LibcameraVideoSource.h - 简化版 libcamera 视频采集（YUV420）
//
// 设计目标：
//  - 打开第一颗相机，仅配置一路 Video stream（YUV420）
//  - 使用 DmaHeap 自行分配 DMA-buf FrameBuffer，方便交给 V4L2 H264 编码器
//  - 通过回调把 (fd, size, mem, stride, timestamp_us) 暴露给上层
//
// 注意：这里特意不实现 rpicam-apps 中的 Pre/Post-processing、Preview、ZSL 等高级
// 功能，只保留最小可用流程。

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <libcamera/base/span.h>
#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/framebuffer.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>

#include "DmaHeap.h"

class LibcameraVideoSource {
public:
    struct Config {
        unsigned int width = 1280;
        unsigned int height = 720;
        unsigned int fps = 30;
        unsigned int buffer_count = 6;  // 采集环形缓冲数
    };

    struct Frame {
        int dmabuf_fd = -1;       // 用于 V4L2 编码器（DMABUF）
        void *mem = nullptr;      // mmap 后的 CPU 指针（备用）
        std::size_t size = 0;     // 帧大小（字节）
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int stride = 0;
        int64_t timestamp_us = 0;
    };

    // FrameReady 回调：上层应同步消费数据，回调结束后归还到 libcamera
    using FrameReadyCb = std::function<void(const Frame &)>;

    LibcameraVideoSource();
    ~LibcameraVideoSource();

    LibcameraVideoSource(const LibcameraVideoSource &) = delete;
    LibcameraVideoSource &operator=(const LibcameraVideoSource &) = delete;

    void SetOnFrame(FrameReadyCb cb) { on_frame_ = std::move(cb); }

    // 打开 + 配置 + 启动
    bool Start(const Config &cfg);
    // 停止 + 释放
    void Stop();

    // 已配置的实际流参数（在 Start 之后有效）
    unsigned int Width() const { return stream_width_; }
    unsigned int Height() const { return stream_height_; }
    unsigned int Stride() const { return stream_stride_; }

private:
    bool openCamera();
    bool configureStream();
    bool allocateBuffers();
    bool makeRequests();
    void requestComplete(libcamera::Request *request);

    Config cfg_;

    DmaHeap dma_heap_;
    std::unique_ptr<libcamera::CameraManager> camera_manager_;
    std::shared_ptr<libcamera::Camera> camera_;
    bool camera_acquired_ = false;
    std::unique_ptr<libcamera::CameraConfiguration> configuration_;

    libcamera::Stream *video_stream_ = nullptr;
    unsigned int stream_width_ = 0;
    unsigned int stream_height_ = 0;
    unsigned int stream_stride_ = 0;
    std::size_t frame_size_ = 0;

    // FrameBuffer 与其 mmap 映射
    std::vector<std::unique_ptr<libcamera::FrameBuffer>> frame_buffers_;
    std::map<libcamera::FrameBuffer *, libcamera::Span<uint8_t>> mapped_;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;

    std::atomic_bool started_{false};
    FrameReadyCb on_frame_;
};
