// SPDX-License-Identifier: BSD-2-Clause
// H264V4l2Encoder.cpp
//
// 编码核心逻辑参照 rpicam-apps/encoder/h264_encoder.cpp，做了简化：
//  - 固定 YUV420 输入
//  - 不暴露 profile/level/QP 等高级选项
//  - 强制开启 REPEAT_SEQ_HEADER（每个 IDR 前发送 SPS+PPS）

#include "H264V4l2Encoder.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char *kTag = "[H264Encoder]";
constexpr const char *kDevice = "/dev/video11";

int xioctl(int fd, unsigned long ctl, void *arg) {
    int ret;
    int tries = 10;
    do {
        ret = ::ioctl(fd, ctl, arg);
    } while (ret == -1 && errno == EINTR && tries-- > 0);
    return ret;
}
}  // namespace

H264V4l2Encoder::H264V4l2Encoder() = default;

H264V4l2Encoder::~H264V4l2Encoder() {
    Stop();
}

bool H264V4l2Encoder::Start(const Config &cfg) {
    if (running_.load()) return true;
    cfg_ = cfg;

    fd_ = ::open(kDevice, O_RDWR, 0);
    if (fd_ < 0) {
        std::fprintf(stderr, "%s 无法打开 %s\n", kTag, kDevice);
        return false;
    }

    // ── 设置编码参数 ────────────────────────────────────────
    v4l2_control ctrl{};
    ctrl.id = V4L2_CID_MPEG_VIDEO_BITRATE;
    ctrl.value = static_cast<int>(cfg_.bitrate_bps);
    if (xioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        std::fprintf(stderr, "%s 设置 bitrate 失败\n", kTag);
        return false;
    }

    ctrl = {};
    ctrl.id = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD;
    ctrl.value = static_cast<int>(cfg_.intra_period);
    if (xioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        std::fprintf(stderr, "%s 设置 I 帧周期失败\n", kTag);
        return false;
    }

    // 始终开启 inline header：每个 IDR 前会重复 SPS/PPS，方便 SDK 拆包推 VIDEO_PARAMS
    ctrl = {};
    ctrl.id = V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER;
    ctrl.value = 1;
    if (xioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        std::fprintf(stderr, "%s 设置 REPEAT_SEQ_HEADER 失败\n", kTag);
        return false;
    }

    // ── 输出（编码器输入）：YUV420 ──────────────────────────
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = cfg_.width;
    fmt.fmt.pix_mp.height = cfg_.height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = cfg_.stride ? cfg_.stride : cfg_.width;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.colorspace =
        cfg_.rec709_colorspace ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SMPTE170M;
    fmt.fmt.pix_mp.num_planes = 1;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        std::fprintf(stderr, "%s 设置输入格式失败\n", kTag);
        return false;
    }

    // ── 捕获（编码器输出）：H264 ────────────────────────────
    fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = cfg_.width;
    fmt.fmt.pix_mp.height = cfg_.height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_DEFAULT;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 0;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 512 << 10;  // 512KB
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        std::fprintf(stderr, "%s 设置输出格式失败\n", kTag);
        return false;
    }

    // ── 帧率 ───────────────────────────────────────────────
    if (cfg_.fps > 0) {
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        parm.parm.output.timeperframe.numerator = 1;
        parm.parm.output.timeperframe.denominator = cfg_.fps;
        if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
            std::fprintf(stderr, "%s 设置帧率失败\n", kTag);
            return false;
        }
    }

    // ── 申请缓冲 ───────────────────────────────────────────
    v4l2_requestbuffers reqbufs{};
    reqbufs.count = kNumOutputBuffers;
    reqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    reqbufs.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(fd_, VIDIOC_REQBUFS, &reqbufs) < 0) {
        std::fprintf(stderr, "%s 申请输入缓冲失败\n", kTag);
        return false;
    }
    for (unsigned int i = 0; i < reqbufs.count; ++i) input_avail_.push(i);

    reqbufs = {};
    reqbufs.count = kNumCaptureBuffers;
    reqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    reqbufs.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &reqbufs) < 0) {
        std::fprintf(stderr, "%s 申请输出缓冲失败\n", kTag);
        return false;
    }
    num_capture_buffers_ = reqbufs.count;

    for (int i = 0; i < num_capture_buffers_; ++i) {
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = planes;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            std::fprintf(stderr, "%s QUERYBUF 失败 i=%d\n", kTag, i);
            return false;
        }
        cap_buffers_[i].mem = ::mmap(0, buf.m.planes[0].length, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd_, buf.m.planes[0].m.mem_offset);
        if (cap_buffers_[i].mem == MAP_FAILED) {
            std::fprintf(stderr, "%s mmap capture 失败 i=%d\n", kTag, i);
            return false;
        }
        cap_buffers_[i].size = buf.m.planes[0].length;
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            std::fprintf(stderr, "%s QBUF capture 失败 i=%d\n", kTag, i);
            return false;
        }
    }

    // ── 启动流 ─────────────────────────────────────────────
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        std::fprintf(stderr, "%s STREAMON output 失败\n", kTag);
        return false;
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        std::fprintf(stderr, "%s STREAMON capture 失败\n", kTag);
        return false;
    }

    abort_poll_.store(false);
    abort_output_.store(false);
    running_.store(true);
    output_thread_ = std::thread(&H264V4l2Encoder::outputThread, this);
    poll_thread_ = std::thread(&H264V4l2Encoder::pollThread, this);

    std::fprintf(stdout, "%s 启动成功 %ux%u stride=%u %u fps %u bps GOP=%u\n", kTag,
                 cfg_.width, cfg_.height, cfg_.stride, cfg_.fps, cfg_.bitrate_bps,
                 cfg_.intra_period);
    return true;
}

void H264V4l2Encoder::Stop() {
    if (!running_.exchange(false)) return;

    abort_poll_.store(true);
    if (poll_thread_.joinable()) poll_thread_.join();
    abort_output_.store(true);
    out_cv_.notify_all();
    if (output_thread_.joinable()) output_thread_.join();

    if (fd_ >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);

        for (int i = 0; i < num_capture_buffers_; ++i) {
            if (cap_buffers_[i].mem) {
                ::munmap(cap_buffers_[i].mem, cap_buffers_[i].size);
                cap_buffers_[i].mem = nullptr;
            }
        }

        v4l2_requestbuffers rb{};
        rb.count = 0;
        rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        rb.memory = V4L2_MEMORY_DMABUF;
        xioctl(fd_, VIDIOC_REQBUFS, &rb);
        rb = {};
        rb.count = 0;
        rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        rb.memory = V4L2_MEMORY_MMAP;
        xioctl(fd_, VIDIOC_REQBUFS, &rb);

        ::close(fd_);
        fd_ = -1;
    }

    while (!input_avail_.empty()) input_avail_.pop();
    while (!out_queue_.empty()) out_queue_.pop();
}

bool H264V4l2Encoder::EncodeBuffer(int fd, std::size_t size, int64_t ts_us) {
    if (!running_.load()) return false;

    int index;
    {
        std::lock_guard<std::mutex> lk(input_avail_mtx_);
        if (input_avail_.empty()) {
            std::fprintf(stderr, "%s 输入队列已满，丢弃一帧\n", kTag);
            return false;
        }
        index = input_avail_.front();
        input_avail_.pop();
    }

    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.index = index;
    buf.field = V4L2_FIELD_NONE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.length = 1;
    buf.timestamp.tv_sec = ts_us / 1000000;
    buf.timestamp.tv_usec = ts_us % 1000000;
    buf.m.planes = planes;
    buf.m.planes[0].m.fd = fd;
    buf.m.planes[0].bytesused = size;
    buf.m.planes[0].length = size;
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        std::fprintf(stderr, "%s QBUF input 失败\n", kTag);
        return false;
    }
    return true;
}

void H264V4l2Encoder::pollThread() {
    while (true) {
        pollfd p{fd_, POLLIN, 0};
        int ret = ::poll(&p, 1, 200);
        {
            std::lock_guard<std::mutex> lk(input_avail_mtx_);
            if (abort_poll_.load() &&
                static_cast<int>(input_avail_.size()) == kNumOutputBuffers)
                break;
        }
        if (ret == -1) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "%s poll 失败 errno=%d\n", kTag, errno);
            break;
        }
        if (!(p.revents & POLLIN)) continue;

        // OUTPUT 端（输入帧）已被消费 → 归还 index
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.length = 1;
        buf.m.planes = planes;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) == 0) {
            {
                std::lock_guard<std::mutex> lk(input_avail_mtx_);
                input_avail_.push(buf.index);
            }
            if (on_input_done_) on_input_done_();
        }

        // CAPTURE 端（编码后输出）
        std::memset(planes, 0, sizeof(planes));
        buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = 1;
        buf.m.planes = planes;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) == 0) {
            int64_t ts = buf.timestamp.tv_sec * 1'000'000LL + buf.timestamp.tv_usec;
            OutputItem item{cap_buffers_[buf.index].mem,
                            buf.m.planes[0].bytesused,
                            buf.m.planes[0].length,
                            buf.index,
                            !!(buf.flags & V4L2_BUF_FLAG_KEYFRAME),
                            ts};
            std::lock_guard<std::mutex> lk(out_mtx_);
            out_queue_.push(item);
            out_cv_.notify_one();
        }
    }
}

void H264V4l2Encoder::outputThread() {
    while (true) {
        OutputItem item;
        {
            std::unique_lock<std::mutex> lk(out_mtx_);
            using namespace std::chrono_literals;
            while (out_queue_.empty()) {
                if (abort_output_.load()) return;
                out_cv_.wait_for(lk, 200ms);
            }
            item = out_queue_.front();
            out_queue_.pop();
        }

        if (on_output_) {
            on_output_(static_cast<const uint8_t *>(item.mem), item.bytes_used,
                       item.timestamp_us, item.keyframe);
        }

        // 把 capture buffer 重新入队
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = item.index;
        buf.length = 1;
        buf.m.planes = planes;
        buf.m.planes[0].bytesused = 0;
        buf.m.planes[0].length = item.length;
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            std::fprintf(stderr, "%s 重新入队 capture 失败\n", kTag);
        }
    }
}
