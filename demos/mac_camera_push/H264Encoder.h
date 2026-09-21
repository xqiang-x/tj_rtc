#pragma once

#include <functional>
#include <cstdint>
#include <cstddef>

#ifdef __APPLE__
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#endif

namespace h264 {

// 编码后的 NAL 单元包（Annex-B 格式，以 0x00 0x00 0x00 0x01 起始）
struct EncodedPacket {
    const uint8_t* data = nullptr;
    size_t         size = 0;

    // true  → IDR 帧（关键帧）
    // false → 普通 P/B 帧
    bool isKeyFrame  = false;

    // true  → SPS+PPS 参数集（在 IDR 帧之前单独发送）
    bool isParamSet  = false;
};

// 编码输出回调
// 注意：回调在 VideoToolbox 内部线程触发，需要线程安全处理
using PacketCallback = std::function<void(const EncodedPacket& pkt)>;

// H.264 编码器（基于 Apple VideoToolbox VTCompressionSession）
class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();

    // 禁止拷贝
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // 初始化编码器
    // width/height  : 视频分辨率（必须与输入 PixelBuffer 一致）
    // fps           : 帧率（用于计算 duration 和关键帧间隔）
    // bitrateKbps   : 目标码率（Kbps）
    bool Init(int width, int height, int fps, int bitrateKbps = 2000);

    // 释放编码器资源
    void Release();

    // 注册输出回调（必须在 Init 之前或之后，EncodeFrame 之前调用）
    void SetPacketCallback(PacketCallback cb);

    // 编码一帧视频
    // pixelBuffer : kCVPixelFormatType_420YpCbCr8BiPlanarFullRange 格式
    // pts         : 显示时间戳
    void EncodeFrame(CVPixelBufferRef pixelBuffer, CMTime pts);

    // 强制插入关键帧（下一帧将为 IDR）
    void ForceKeyFrame();

    bool IsInitialized() const { return m_initialized; }

private:
    void* m_session     = nullptr;   // VTCompressionSessionRef
    PacketCallback m_callback;
    int  m_fps          = 30;
    bool m_initialized  = false;
    bool m_forceKeyNext = false;

    // VideoToolbox 输出回调（静态函数，转发到 m_callback）
    static void OutputCallback(
        void* outputCallbackRefCon,
        void* sourceFrameRefCon,
        OSStatus status,
        VTEncodeInfoFlags infoFlags,
        CMSampleBufferRef sampleBuffer);
};

} // namespace h264
