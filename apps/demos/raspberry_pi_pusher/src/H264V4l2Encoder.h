// SPDX-License-Identifier: BSD-2-Clause
// H264V4l2Encoder.h - 基于树莓派硬件 V4L2 编码器的 H.264 编码（/dev/video11）
//
// 设计目标：
//  - 接受 DMABUF 输入（与 LibcameraVideoSource 配合使用）
//  - 输出 H.264 ES 流（带 SPS/PPS，开启 REPEAT_SEQ_HEADER）
//  - 通过回调把编码后的 NALU 缓冲区交给上层

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

class H264V4l2Encoder {
public:
    struct Config {
        unsigned int width = 1280;
        unsigned int height = 720;
        unsigned int stride = 0;     // 由相机的 StreamConfiguration::stride 提供
        unsigned int fps = 30;
        unsigned int bitrate_bps = 2'000'000;
        unsigned int intra_period = 30;  // GOP 长度
        // SMPTE170M（标清/Rec601）= 0/默认；Rec709（高清）= 1
        bool rec709_colorspace = true;
    };

    // 编码完成的一段 H.264 数据回调
    //  data        : 缓冲区指针（由编码器内部 mmap 的 capture buffer，仅在回调期间有效）
    //  size        : 字节数
    //  timestamp_us: 输入时附带的时间戳
    //  keyframe    : V4L2 标志位（IDR）
    using OutputReadyCb = std::function<void(const uint8_t *data, std::size_t size,
                                             int64_t timestamp_us, bool keyframe)>;
    using InputDoneCb = std::function<void()>;  // 一次输入消费完毕

    H264V4l2Encoder();
    ~H264V4l2Encoder();

    H264V4l2Encoder(const H264V4l2Encoder &) = delete;
    H264V4l2Encoder &operator=(const H264V4l2Encoder &) = delete;

    void SetOnOutput(OutputReadyCb cb) { on_output_ = std::move(cb); }
    void SetOnInputDone(InputDoneCb cb) { on_input_done_ = std::move(cb); }

    bool Start(const Config &cfg);
    void Stop();

    // 提交一帧 YUV420 到编码器（DMABUF）
    //  fd       : libcamera FrameBuffer planes()[0].fd.get()
    //  size     : 缓冲区字节数
    //  ts_us    : 时间戳（微秒）
    bool EncodeBuffer(int fd, std::size_t size, int64_t ts_us);

private:
    static constexpr int kNumOutputBuffers = 6;
    static constexpr int kNumCaptureBuffers = 12;

    void pollThread();
    void outputThread();

    Config cfg_{};
    int fd_ = -1;
    std::atomic_bool running_{false};
    std::atomic_bool abort_poll_{false};
    std::atomic_bool abort_output_{false};

    struct CaptureBuffer {
        void *mem = nullptr;
        std::size_t size = 0;
    };
    CaptureBuffer cap_buffers_[kNumCaptureBuffers]{};
    int num_capture_buffers_ = 0;

    std::mutex input_avail_mtx_;
    std::queue<int> input_avail_;  // 可用的 OUTPUT 队列 index

    struct OutputItem {
        void *mem;
        std::size_t bytes_used;
        std::size_t length;
        unsigned int index;
        bool keyframe;
        int64_t timestamp_us;
    };
    std::mutex out_mtx_;
    std::condition_variable out_cv_;
    std::queue<OutputItem> out_queue_;

    std::thread poll_thread_;
    std::thread output_thread_;

    OutputReadyCb on_output_;
    InputDoneCb on_input_done_;
};
