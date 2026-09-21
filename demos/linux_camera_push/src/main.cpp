// linux_camera_push - 本机摄像头 → H.264(FFmpeg 库: libavdevice/libx264) → SFU 推流
//
// 用法:
//   ./linux_camera_push [server_ip] [port] [stream_id] [user_id]
//                       [width] [height] [fps] [bitrate_kbps] [udp]
//   默认: 127.0.0.1 9200 room1 publisher1 640 480 30 1500 tcp
//   摄像头设备默认 /dev/video0，可用环境变量 CAM_DEV 覆盖

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "Publisher.h"
#include "NaluSplitter.h"

static std::atomic_bool g_stop{false};
static std::atomic_bool g_forceKey{false};  // PLI → 下一帧强制 IDR

static void SignalHandler(int sig) {
    std::cout << "\n[Main] 收到信号 " << sig << "，正在停止..." << std::endl;
    g_stop.store(true);
}

// v4l2 采集 + libx264 编码，Annex-B 码流经 splitter 回调输出
class CameraEncoder {
public:
    ~CameraEncoder() { Close(); }

    bool Open(int width, int height, int fps, int bitrateKbps) {
        avdevice_register_all();

        std::string cam = "/dev/video0";
        const char* env = std::getenv("CAM_DEV");
        if (env && *env) cam = env;

        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "input_format", "yuyv422", 0);
        av_dict_set(&opts, "video_size", (std::to_string(width) + "x" + std::to_string(height)).c_str(), 0);
        av_dict_set(&opts, "framerate", std::to_string(fps).c_str(), 0);

        AVInputFormat* ifmt = av_find_input_format("v4l2");
        int ret = avformat_open_input(&m_fmtCtx, cam.c_str(), ifmt, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            LogErr("打开摄像头失败", ret);
            return false;
        }
        if ((ret = avformat_find_stream_info(m_fmtCtx, nullptr)) < 0) {
            LogErr("读取摄像头流信息失败", ret);
            return false;
        }

        ret = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (ret < 0) {
            std::cerr << "[Cam] 未找到视频流" << std::endl;
            return false;
        }
        m_streamIdx = ret;
        AVCodecParameters* inPar = m_fmtCtx->streams[m_streamIdx]->codecpar;

        m_sws = sws_getContext(inPar->width, inPar->height, (AVPixelFormat)inPar->format,
                               width, height, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) {
            std::cerr << "[Cam] 初始化 swscale 失败" << std::endl;
            return false;
        }

        // 采集参数（每个 packet 的原始帧数据直接转换）
        m_inWidth = inPar->width;
        m_inHeight = inPar->height;
        m_inPixFmt = (AVPixelFormat)inPar->format;

        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            std::cerr << "[Cam] 找不到 H.264 编码器" << std::endl;
            return false;
        }
        m_encCtx = avcodec_alloc_context3(codec);
        m_encCtx->width = width;
        m_encCtx->height = height;
        m_encCtx->time_base = {1, fps};
        m_encCtx->framerate = {fps, 1};
        m_encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        m_encCtx->bit_rate = (int64_t)bitrateKbps * 1000;
        m_encCtx->gop_size = fps;            // 1 秒一个关键帧
        m_encCtx->max_b_frames = 0;
        m_encCtx->thread_count = 4;
        m_encCtx->thread_type = FF_THREAD_FRAME;  // 禁 slice 线程：每帧单 NALU，降低切分/传输开销
        av_opt_set(m_encCtx->priv_data, "preset", "veryfast", 0);
        av_opt_set(m_encCtx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(m_encCtx->priv_data, "profile", "baseline", 0);
        // 每个 IDR 前带 SPS/PPS，新订阅者秒开；force-idr 让强制 I 帧成为 IDR（PLI 快恢复）
        av_opt_set(m_encCtx->priv_data, "x264-params", "repeat-headers=1:force-idr=1", 0);
        // 不设 GLOBAL_HEADER：编码器输出 Annex-B（带起始码），与 Android 端一致

        if ((ret = avcodec_open2(m_encCtx, codec, nullptr)) < 0) {
            LogErr("打开编码器失败", ret);
            return false;
        }

        m_frame = av_frame_alloc();
        m_frame->format = AV_PIX_FMT_YUV420P;
        m_frame->width = width;
        m_frame->height = height;
        if ((ret = av_frame_get_buffer(m_frame, 0)) < 0) {
            LogErr("分配编码帧缓冲失败", ret);
            return false;
        }
        m_pkt = av_packet_alloc();

        std::cout << "[Cam] 已打开 " << cam << " 输入 " << inPar->width << "x" << inPar->height
                  << " 编码 " << codec->name << " " << width << "x" << height
                  << " @" << fps << "fps " << bitrateKbps << "kbps" << std::endl;
        return true;
    }

    // 采集一帧并编码；成功产出码流时调用 onPacket（可能多次，也可能 0 次）
    // 返回 false 表示采集/编码出错或设备结束
    bool ReadAndEncode(const std::function<void(const uint8_t*, size_t)>& onPacket) {
        AVPacket* inPkt = av_packet_alloc();
        int ret = av_read_frame(m_fmtCtx, inPkt);
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN)) LogErr("采集失败", ret);
            av_packet_free(&inPkt);
            return ret == AVERROR(EAGAIN);
        }
        bool ok = (inPkt->stream_index == m_streamIdx);
        if (ok) {
            // v4l2 rawvideo：packet 数据即一帧原始图像，直接做像素格式转换
            uint8_t* srcData[4] = {nullptr};
            int srcLinesize[4] = {0};
            av_image_fill_arrays(srcData, srcLinesize,
                                 inPkt->data, m_inPixFmt, m_inWidth, m_inHeight, 1);
            if (av_frame_make_writable(m_frame) < 0 ||
                sws_scale(m_sws, srcData, srcLinesize, 0, m_inHeight,
                          m_frame->data, m_frame->linesize) <= 0) {
                std::cerr << "[Cam] 像素格式转换失败" << std::endl;
                ok = false;
            }
        }
        av_packet_free(&inPkt);
        if (!ok) return true;
        return EncodeOne(onPacket);
    }

    void Close() {
        if (m_pkt) av_packet_free(&m_pkt);
        if (m_frame) av_frame_free(&m_frame);
        if (m_encCtx) avcodec_free_context(&m_encCtx);
        if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
        if (m_fmtCtx) avformat_close_input(&m_fmtCtx);
    }

private:
    bool EncodeOne(const std::function<void(const uint8_t*, size_t)>& onPacket) {
        m_frame->pts = m_nextPts++;
        m_frame->pict_type = g_forceKey.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        int ret = avcodec_send_frame(m_encCtx, m_frame);
        if (ret < 0) {
            LogErr("送编码失败", ret);
            return false;
        }
        while (true) {
            ret = avcodec_receive_packet(m_encCtx, m_pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                LogErr("取编码输出失败", ret);
                return false;
            }
            onPacket(m_pkt->data, m_pkt->size);
            av_packet_unref(m_pkt);
        }
        return true;
    }

    static void LogErr(const char* what, int err) {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(err, buf, sizeof(buf));
        std::cerr << "[Cam] " << what << ": " << buf << std::endl;
    }

    AVFormatContext* m_fmtCtx = nullptr;
    AVCodecContext* m_encCtx = nullptr;
    SwsContext* m_sws = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_pkt = nullptr;
    int m_streamIdx = -1;
    int m_inWidth = 0;
    int m_inHeight = 0;
    AVPixelFormat m_inPixFmt = AV_PIX_FMT_NONE;
    int64_t m_nextPts = 0;
};

int main(int argc, char* argv[]) {
    std::string serverIp   = "127.0.0.1";
    int         port       = 9200;
    std::string streamId   = "room1";
    std::string userId     = "publisher1";
    int         width      = 640;
    int         height     = 480;
    int         fps        = 30;
    int         bitrateKbps = 1500;
    bool        useUdp     = false;

    if (argc >= 2) serverIp   = argv[1];
    if (argc >= 3) port       = std::atoi(argv[2]);
    if (argc >= 4) streamId    = argv[3];
    if (argc >= 5) userId     = argv[4];
    if (argc >= 6) width      = std::atoi(argv[5]);
    if (argc >= 7) height     = std::atoi(argv[6]);
    if (argc >= 8) fps        = std::atoi(argv[7]);
    if (argc >= 9) bitrateKbps = std::atoi(argv[8]);
    if (argc >= 10) useUdp    = (std::string(argv[9]) == "udp");

    std::cout << "========================================" << std::endl;
    std::cout << "  Linux Camera Push Demo" << std::endl;
    std::cout << "  Server : " << serverIp << ":" << port << std::endl;
    std::cout << "  Stream : " << streamId << std::endl;
    std::cout << "  User   : " << userId << std::endl;
    std::cout << "  Video  : " << width << "x" << height
              << " @" << fps << "fps  " << bitrateKbps << "kbps" << std::endl;
    std::cout << "  Mode   : " << (useUdp ? "UDP (FEC 分片)" : "TCP") << std::endl;
    std::cout << "========================================" << std::endl;

    signal(SIGINT,  SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGPIPE, SIG_IGN);

    push::Publisher pushClient;
    pushClient.SetOnConnected([](uint32_t sessionId) {
        std::cout << "[Main] 推流连接成功，sessionId=" << sessionId << std::endl;
    });
    pushClient.SetOnDisconnected([]() {
        std::cout << "[Main] 推流连接已断开" << std::endl;
        g_stop.store(true);
    });
    pushClient.SetBandwidth(50'000'000);  // UDP 模式 pacer 带宽 50Mbps
    pushClient.SetOnKeyFrameRequest([]() {
        std::cout << "[Main] 收到关键帧请求(PLI)，下一帧强制 IDR" << std::endl;
        g_forceKey.store(true);
    });

    uint64_t sentFrames = 0;
    uint64_t sentBytes  = 0;
    FILE* dumpFp = nullptr;
    if (const char* dumpPath = std::getenv("PUB_DUMP")) {
        dumpFp = fopen(dumpPath, "wb");
        std::cout << "[Main] 编码输出 dump → " << dumpPath
                  << (dumpFp ? "" : " (打开失败!)") << std::endl;
    }
    NaluSplitter splitter;
    splitter.SetOnNalu([&](NaluSplitter::Kind kind, const uint8_t* data, std::size_t size) {
        bool isParamSet = (kind == NaluSplitter::Kind::kParams);
        bool isKeyFrame = (kind == NaluSplitter::Kind::kIdr);
        // 回调数据自带 00 00 00 01 起始码（Annex-B，Android 解码端需要）
        pushClient.PushVideoFrame(data, size, isParamSet, isKeyFrame);
        if (dumpFp) {
            uint32_t len = static_cast<uint32_t>(size);
            uint8_t type = isParamSet ? 0x01 : (isKeyFrame ? 0x02 : 0x03);
            fwrite(&len, 4, 1, dumpFp);
            fwrite(&type, 1, 1, dumpFp);
            fwrite(data, 1, size, dumpFp);
        }
        ++sentFrames;
        sentBytes += size;
    });

    if (!pushClient.Start(serverIp, port, streamId, userId, useUdp, 0)) {
        std::cerr << "[Main] 推流客户端启动失败" << std::endl;
        return 1;
    }

    std::cout << "[Main] 等待订阅握手..." << std::endl;
    auto waitStart = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        if (pushClient.IsRunning()) break;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - waitStart).count();
        if (elapsed > 10) {
            std::cerr << "[Main] 连接服务器超时" << std::endl;
            pushClient.Stop();
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (g_stop.load()) {
        pushClient.Stop();
        return 0;
    }

    CameraEncoder cam;
    if (!cam.Open(width, height, fps, bitrateKbps)) {
        pushClient.Stop();
        return 1;
    }

    std::cout << "\n[Main] 推流中... 按 Ctrl+C 停止\n" << std::endl;

    // v4l2 不一定按请求帧率出帧，这里按目标 fps 节流
    const auto frameInterval = std::chrono::microseconds(1'000'000 / fps);
    auto nextFrameTime = std::chrono::steady_clock::now();

    while (!g_stop.load()) {
        std::this_thread::sleep_until(nextFrameTime);
        nextFrameTime += frameInterval;
        if (!cam.ReadAndEncode([&](const uint8_t* data, size_t size) {
                splitter.Process(data, size);
            })) {
            std::cerr << "[Main] 采集/编码结束" << std::endl;
            break;
        }
    }

    std::cout << "[Main] 已发送帧数=" << sentFrames
              << " 字节=" << sentBytes << std::endl;

    cam.Close();
    std::cout << "[Main] 正在停止推流..." << std::endl;
    pushClient.Stop();
    if (dumpFp) fclose(dumpFp);
    std::cout << "[Main] 退出" << std::endl;
    return 0;
}
