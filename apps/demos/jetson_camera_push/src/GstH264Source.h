// SPDX-License-Identifier: BSD-2-Clause
// GstH264Source.h —— Jetson 硬编视频源（nvarguscamerasrc → nvvidconv → nvv4l2h264enc → appsink）
//
// 管线：
//   nvarguscamerasrc(sensor-id)                  Argus/ISP 采集（NV12, NVMM）
//     → nvvidconv                                缩放 + 转 I420（NVMM）
//     → nvv4l2h264enc                            Tegra NVENC 硬件编码
//     → h264parse                                字节流/AU 对齐
//     → appsink                                  拉取编码后访问单元（Annex-B）
//
// 注意：本机 IMX219 的 v4l2 直采路径（nvv4l2camerasrc/v4l2src）实测仅 4K@21
// 可出帧，其余模式无帧；Argus 路径六种模式全部可用，故采用 Argus。
// 编码器无运行时 force-idr（idrinterval 仅 NULL/READY 可改），PLI 恢复依赖
// iframeinterval/idrinterval 设定的 GOP 周期。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

class GstH264Source {
public:
    // 回调数据为 H.264 Annex-B 访问单元（自带起始码；应为完整一帧）
    using OnAuCb = std::function<void(const uint8_t* data, std::size_t size)>;

    struct Config {
        int sensor_id = 0;      // Argus 传感器编号（本机 0 = video0 的 IMX219）
        int cap_width = 1920;   // 采集尺寸（须为 Argus 支持的模式）
        int cap_height = 1080;
        int cap_fps = 30;
        int enc_width = 1280;   // 编码尺寸（nvvidconv 缩放目标）
        int enc_height = 720;
        int bitrate_kbps = 2500;
        int gop = 30;           // IDR 间隔（帧数）
    };

    GstH264Source() = default;
    ~GstH264Source() { Stop(); }
    GstH264Source(const GstH264Source&) = delete;
    GstH264Source& operator=(const GstH264Source&) = delete;

    void SetOnAu(OnAuCb cb) { m_on_au = std::move(cb); }

    // 构建并启动管线；失败返回 false（打印原因）
    bool Start(const Config& cfg);

    // 停止管线并回收线程（幂等）
    void Stop();

    // 管线是否在运行（总线报错/EOS 后会置 false）
    bool IsRunning() const { return m_running.load(); }

private:
    bool BuildPipeline(const Config& cfg);
    void PullLoop();
    void BusLoop();
    void Teardown();

    GstElement* m_pipeline = nullptr;
    GstElement* m_sink = nullptr;      // appsink
    GstBus* m_bus = nullptr;

    OnAuCb m_on_au;
    std::thread m_pull_thread;
    std::thread m_bus_thread;
    std::atomic<bool> m_running{false};
};