// SPDX-License-Identifier: BSD-2-Clause
// LibcameraVideoSource.cpp

#include "LibcameraVideoSource.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include <libcamera/control_ids.h>
#include <libcamera/controls.h>
#include <libcamera/formats.h>
#include <libcamera/property_ids.h>

namespace {
constexpr const char *kTag = "[VideoSource]";
}

LibcameraVideoSource::LibcameraVideoSource() = default;

LibcameraVideoSource::~LibcameraVideoSource() {
    Stop();
}

bool LibcameraVideoSource::Start(const Config &cfg) {
    if (started_.load()) return true;

    cfg_ = cfg;

    if (!dma_heap_.isValid()) {
        std::fprintf(stderr, "%s DmaHeap 初始化失败\n", kTag);
        return false;
    }

    if (!openCamera()) return false;
    if (!configureStream()) return false;
    if (!allocateBuffers()) return false;
    if (!makeRequests()) return false;

    // 把 requestComplete 挂到相机
    camera_->requestCompleted.connect(this, &LibcameraVideoSource::requestComplete);

    // 设置帧率
    libcamera::ControlList controls;
    int64_t frame_dur = 1'000'000 / static_cast<int64_t>(cfg_.fps);  // 微秒
    controls.set(libcamera::controls::FrameDurationLimits,
                 libcamera::Span<const int64_t, 2>({frame_dur, frame_dur}));

    if (camera_->start(&controls) < 0) {
        std::fprintf(stderr, "%s camera->start 失败\n", kTag);
        return false;
    }
    for (auto &req : requests_) {
        if (camera_->queueRequest(req.get()) < 0) {
            std::fprintf(stderr, "%s queueRequest 失败\n", kTag);
            return false;
        }
    }

    started_.store(true);
    std::fprintf(stdout, "%s 启动成功 %ux%u stride=%u fps=%u buf=%u\n", kTag,
                 stream_width_, stream_height_, stream_stride_, cfg_.fps,
                 cfg_.buffer_count);
    return true;
}

void LibcameraVideoSource::Stop() {
    if (!started_.exchange(false)) {
        // 即使没成功 start，也要清理已经获取的资源
    }

    if (camera_) {
        camera_->stop();
        camera_->requestCompleted.disconnect(this);
    }

    requests_.clear();

    // 解除 mmap
    for (auto &kv : mapped_) {
        if (kv.second.data())
            ::munmap(kv.second.data(), kv.second.size());
    }
    mapped_.clear();
    frame_buffers_.clear();

    if (camera_acquired_ && camera_) {
        camera_->release();
        camera_acquired_ = false;
    }
    camera_.reset();
    if (camera_manager_) {
        camera_manager_->stop();
        camera_manager_.reset();
    }
}

bool LibcameraVideoSource::openCamera() {
    camera_manager_ = std::make_unique<libcamera::CameraManager>();
    if (camera_manager_->start() != 0) {
        std::fprintf(stderr, "%s CameraManager 启动失败\n", kTag);
        return false;
    }
    auto cameras = camera_manager_->cameras();
    // 过滤 USB 摄像头（rpicam-apps 同样过滤）
    cameras.erase(
        std::remove_if(cameras.begin(), cameras.end(),
                       [](auto &c) { return c->id().find("/usb") != std::string::npos; }),
        cameras.end());
    if (cameras.empty()) {
        std::fprintf(stderr, "%s 未发现可用相机\n", kTag);
        return false;
    }
    camera_ = camera_manager_->get(cameras[0]->id());
    if (!camera_) {
        std::fprintf(stderr, "%s 无法获取相机 %s\n", kTag, cameras[0]->id().c_str());
        return false;
    }
    if (camera_->acquire() != 0) {
        std::fprintf(stderr, "%s acquire 失败\n", kTag);
        return false;
    }
    camera_acquired_ = true;
    std::fprintf(stdout, "%s 已获取相机: %s\n", kTag, camera_->id().c_str());
    return true;
}

bool LibcameraVideoSource::configureStream() {
    configuration_ = camera_->generateConfiguration({libcamera::StreamRole::VideoRecording});
    if (!configuration_) {
        std::fprintf(stderr, "%s generateConfiguration 失败\n", kTag);
        return false;
    }
    auto &scfg = configuration_->at(0);
    scfg.size = libcamera::Size(cfg_.width, cfg_.height);
    scfg.pixelFormat = libcamera::formats::YUV420;
    scfg.bufferCount = cfg_.buffer_count;

    auto status = configuration_->validate();
    if (status == libcamera::CameraConfiguration::Invalid) {
        std::fprintf(stderr, "%s 配置非法\n", kTag);
        return false;
    }
    if (status == libcamera::CameraConfiguration::Adjusted) {
        std::fprintf(stdout, "%s 配置被自动调整为 %s\n", kTag,
                     scfg.toString().c_str());
    }
    if (camera_->configure(configuration_.get()) < 0) {
        std::fprintf(stderr, "%s configure 失败\n", kTag);
        return false;
    }

    video_stream_ = scfg.stream();
    stream_width_ = scfg.size.width;
    stream_height_ = scfg.size.height;
    stream_stride_ = scfg.stride;
    frame_size_ = scfg.frameSize;
    return true;
}

bool LibcameraVideoSource::allocateBuffers() {
    for (unsigned int i = 0; i < cfg_.buffer_count; ++i) {
        std::string name = "raspberry_pi_pusher_" + std::to_string(i);
        libcamera::UniqueFD fd = dma_heap_.Alloc(name.c_str(), frame_size_);
        if (!fd.isValid()) {
            std::fprintf(stderr, "%s DMA 缓冲分配失败\n", kTag);
            return false;
        }

        std::vector<libcamera::FrameBuffer::Plane> planes(1);
        planes[0].fd = libcamera::SharedFD(std::move(fd));
        planes[0].offset = 0;
        planes[0].length = frame_size_;

        auto fb = std::make_unique<libcamera::FrameBuffer>(planes);
        void *mem = ::mmap(nullptr, frame_size_, PROT_READ | PROT_WRITE, MAP_SHARED,
                           planes[0].fd.get(), 0);
        if (mem == MAP_FAILED) {
            std::fprintf(stderr, "%s mmap 失败\n", kTag);
            return false;
        }
        mapped_[fb.get()] =
            libcamera::Span<uint8_t>(static_cast<uint8_t *>(mem), frame_size_);
        frame_buffers_.push_back(std::move(fb));
    }
    return true;
}

bool LibcameraVideoSource::makeRequests() {
    for (auto &fb : frame_buffers_) {
        auto request = camera_->createRequest();
        if (!request) {
            std::fprintf(stderr, "%s createRequest 失败\n", kTag);
            return false;
        }
        if (request->addBuffer(video_stream_, fb.get()) < 0) {
            std::fprintf(stderr, "%s addBuffer 失败\n", kTag);
            return false;
        }
        requests_.push_back(std::move(request));
    }
    return true;
}

void LibcameraVideoSource::requestComplete(libcamera::Request *request) {
    if (request->status() == libcamera::Request::RequestCancelled) return;

    auto it = request->buffers().find(video_stream_);
    if (it == request->buffers().end()) {
        request->reuse(libcamera::Request::ReuseBuffers);
        camera_->queueRequest(request);
        return;
    }
    libcamera::FrameBuffer *fb = it->second;

    Frame frame{};
    frame.dmabuf_fd = fb->planes()[0].fd.get();
    auto map_it = mapped_.find(fb);
    frame.mem = (map_it != mapped_.end()) ? map_it->second.data() : nullptr;
    frame.size = fb->planes()[0].length;
    frame.width = stream_width_;
    frame.height = stream_height_;
    frame.stride = stream_stride_;
    frame.timestamp_us = static_cast<int64_t>(fb->metadata().timestamp / 1000);

    if (on_frame_ && started_.load()) {
        on_frame_(frame);
    }

    // 归还 request 继续采集
    request->reuse(libcamera::Request::ReuseBuffers);
    if (started_.load()) camera_->queueRequest(request);
}
