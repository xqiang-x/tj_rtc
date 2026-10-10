// SPDX-License-Identifier: BSD-2-Clause
// GstH264Source.cpp

#include "GstH264Source.h"

#include <cstdio>

namespace {

GstElement* MakeElement(const char* factory, const char* alias) {
    GstElement* e = gst_element_factory_make(factory, alias);
    if (e == nullptr) {
        std::fprintf(stderr,
                     "[gst] 缺少插件 %s（用 gst-inspect-1.0 %s 检查 JetPack 安装）\n",
                     factory, factory);
    }
    return e;
}

GstElement* MakeCapsFilter(const char* caps_str) {
    GstElement* cf = gst_element_factory_make("capsfilter", nullptr);
    if (cf == nullptr) return nullptr;
    GstCaps* caps = gst_caps_from_string(caps_str);
    if (caps == nullptr) {
        gst_object_unref(cf);
        return nullptr;
    }
    g_object_set(G_OBJECT(cf), "caps", caps, nullptr);
    gst_caps_unref(caps);
    return cf;
}

}  // namespace

bool GstH264Source::BuildPipeline(const Config& cfg) {
    m_pipeline = gst_pipeline_new("jetson_h264_push");

    char argus_caps[160];
    std::snprintf(argus_caps, sizeof(argus_caps),
                  "video/x-raw(memory:NVMM),width=%d,height=%d,framerate=%d/1,format=NV12",
                  cfg.cap_width, cfg.cap_height, cfg.cap_fps);
    char enc_caps[160];
    std::snprintf(enc_caps, sizeof(enc_caps),
                  "video/x-raw(memory:NVMM),width=%d,height=%d,format=I420",
                  cfg.enc_width, cfg.enc_height);

    GstElement* src = MakeElement("nvarguscamerasrc", "src");
    GstElement* argus_filter = MakeCapsFilter(argus_caps);
    GstElement* conv = MakeElement("nvvidconv", "conv");
    GstElement* enc_filter = MakeCapsFilter(enc_caps);
    GstElement* enc = MakeElement("nvv4l2h264enc", "enc");
    GstElement* parse = MakeElement("h264parse", "parse");
    GstElement* au_filter =
        MakeCapsFilter("video/x-h264,stream-format=byte-stream,alignment=au");
    m_sink = MakeElement("appsink", "sink");

    if (src == nullptr || argus_filter == nullptr || conv == nullptr ||
        enc_filter == nullptr || enc == nullptr || parse == nullptr ||
        au_filter == nullptr || m_sink == nullptr) {
        Teardown();
        return false;
    }

    g_object_set(G_OBJECT(src), "sensor-id", cfg.sensor_id, nullptr);
    g_object_set(G_OBJECT(enc),
                 "bitrate", static_cast<guint>(cfg.bitrate_kbps * 1000),
                 "insert-sps-pps", TRUE,  // 每个 IDR 前插入 SPS/PPS，新订阅者秒开
                 "iframeinterval", static_cast<guint>(cfg.gop),
                 "idrinterval", static_cast<guint>(cfg.gop),
                 "insert-vui", TRUE,      // 携带时序信息，解码端友好
                 "preset-level", 1,       // Fast 档，720p30 画质/负载均衡
                 nullptr);
    // 拉取线程消费，缓冲区满直接丢旧帧（网络慢时不背压相机）
    g_object_set(G_OBJECT(m_sink),
                 "max-buffers", 4,
                 "drop", TRUE,
                 "sync", FALSE,
                 nullptr);

    gst_bin_add_many(GST_BIN(m_pipeline), src, argus_filter, conv, enc_filter,
                     enc, parse, au_filter, m_sink, nullptr);

    if (!gst_element_link_many(src, argus_filter, conv, enc_filter, enc, parse,
                               au_filter, m_sink, nullptr)) {
        std::fprintf(stderr, "[gst] 管线元素连接失败\n");
        Teardown();
        return false;
    }
    return true;
}

bool GstH264Source::Start(const Config& cfg) {
    if (m_running.load()) return true;
    if (!BuildPipeline(cfg)) return false;

    m_bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));

    m_running.store(true);
    m_pull_thread = std::thread(&GstH264Source::PullLoop, this);
    m_bus_thread = std::thread(&GstH264Source::BusLoop, this);

    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        std::fprintf(stderr, "[gst] 管线进入 PLAYING 失败\n");
        Stop();
        return false;
    }
    return true;
}

void GstH264Source::PullLoop() {
    while (m_running.load()) {
        GstSample* sample =
            gst_app_sink_try_pull_sample(GST_APP_SINK(m_sink), 200 * GST_MSECOND);
        if (sample == nullptr) continue;  // 超时/停止/EOS
        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (buf != nullptr && gst_buffer_map(buf, &map, GST_MAP_READ)) {
            if (m_on_au && map.size > 0) {
                m_on_au(map.data, map.size);
            }
            gst_buffer_unmap(buf, &map);
        }
        gst_sample_unref(sample);
    }
}

void GstH264Source::BusLoop() {
    while (m_running.load()) {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            m_bus, 200 * GST_MSECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (msg == nullptr) continue;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError* err = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            std::fprintf(stderr, "[gst] 管线错误: %s (%s)\n",
                         err != nullptr ? err->message : "?", dbg != nullptr ? dbg : "");
            if (err != nullptr) g_error_free(err);
            g_free(dbg);
        } else {
            std::fprintf(stderr, "[gst] 管线 EOS\n");
        }
        gst_message_unref(msg);
        m_running.store(false);  // 主循环据此退出
    }
}

void GstH264Source::Stop() {
    if (m_pipeline == nullptr) return;
    m_running.store(false);
    gst_element_set_state(m_pipeline, GST_STATE_NULL);  // 解除 pull 阻塞
    if (m_pull_thread.joinable()) m_pull_thread.join();
    if (m_bus_thread.joinable()) m_bus_thread.join();
    Teardown();
}

void GstH264Source::Teardown() {
    if (m_bus != nullptr) {
        gst_object_unref(m_bus);
        m_bus = nullptr;
    }
    if (m_pipeline != nullptr) {
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_sink = nullptr;
}