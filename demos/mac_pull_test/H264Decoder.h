#pragma once
// H264Decoder.h — macOS VideoToolbox H.264 硬件解码器
// 输入：SFU 拉流端的 payload（[1字节帧类型][Annex-B H.264]）
// 输出：通过回调返回解码后的 CVImageBufferRef（NV12 格式）

#include <cstdint>
#include <functional>
#include <vector>

#include <VideoToolbox/VideoToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

class H264Decoder {
public:
    // 解码回调：在 FeedPayload 调用栈内同步触发（网络线程）
    // imageBuffer 有效期仅在回调内，如需保留必须 CFRetain
    using FrameCallback =
        std::function<void(CVImageBufferRef imageBuffer, int width, int height)>;

    H264Decoder();
    ~H264Decoder();

    // 不可拷贝
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // 设置解码完成回调
    void SetFrameCallback(FrameCallback cb) { m_callback = std::move(cb); }

    // 喂入来自 pull::Subscriber::SetOnFrame 的 payload
    //   data[0] = 0x01 → SPS+PPS（kVideoParams）
    //   data[0] = 0x02 → IDR 关键帧
    //   data[0] = 0x03 → P/B 帧
    //   data[1..] = Annex-B H.264 码流
    void FeedPayload(const uint8_t* data, size_t len);

    int GetWidth()  const { return m_width;  }
    int GetHeight() const { return m_height; }
    bool IsReady()  const { return m_session != nullptr; }

private:
    // 解析并存储 SPS/PPS，重建 CMVideoFormatDescription 和解码会话
    void HandleParamSet(const uint8_t* annexB, size_t len);

    // 解码一帧（IDR 或 P/B）
    void HandleVideoFrame(const uint8_t* annexB, size_t len);

    // 创建 VTDecompressionSession（需要 m_fmtDesc 已就绪）
    bool CreateSession();
    void DestroySession();

    // 将 Annex-B 转换为 AVCC（4字节大端长度前缀），打包进 CMSampleBuffer
    bool MakeSampleBuffer(const uint8_t* annexB, size_t len, CMSampleBufferRef& outSample);

    // VTDecompressionSession 输出回调（静态）
    static void DecompressionCallback(
        void*                refCon,
        void*                frameRefCon,
        OSStatus             status,
        VTDecodeInfoFlags    infoFlags,
        CVImageBufferRef     imageBuffer,
        CMTime               presentationTimeStamp,
        CMTime               presentationDuration);

    CMVideoFormatDescriptionRef m_fmtDesc = nullptr;
    VTDecompressionSessionRef   m_session = nullptr;
    FrameCallback               m_callback;

    std::vector<uint8_t> m_sps;
    std::vector<uint8_t> m_pps;
    std::vector<uint8_t> m_lastSps;  // 已建会话使用的 SPS（用于判断是否变化）
    std::vector<uint8_t> m_lastPps;

    int     m_width  = 0;
    int     m_height = 0;
    int64_t m_pts    = 0;  // 单调递增，单位 90kHz
};
