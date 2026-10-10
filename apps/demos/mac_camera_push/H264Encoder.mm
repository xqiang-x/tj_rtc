// H264Encoder.mm
// VideoToolbox H.264 硬编码实现（Objective-C++）

#import "H264Encoder.h"

#import <VideoToolbox/VideoToolbox.h>
#import <Foundation/Foundation.h>

#include <vector>
#include <iostream>
#include <cstring>

namespace h264 {

// ─────────────────────────────────────────────────────────────
// VTCompressionSession 输出回调
// ─────────────────────────────────────────────────────────────
void H264Encoder::OutputCallback(
    void*             outputCallbackRefCon,
    void*             /*sourceFrameRefCon*/,
    OSStatus          status,
    VTEncodeInfoFlags /*infoFlags*/,
    CMSampleBufferRef sampleBuffer)
{
    if (status != noErr || !sampleBuffer) return;
    if (!CMSampleBufferDataIsReady(sampleBuffer)) return;

    H264Encoder* self = static_cast<H264Encoder*>(outputCallbackRefCon);
    if (!self || !self->m_callback) return;

    // ── 判断是否为关键帧 ──────────────────────────────────────
    bool isKeyFrame = false;
    CFArrayRef attachments =
        CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict =
            (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFBooleanRef notSync =
            (CFBooleanRef)CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync);
        isKeyFrame = (notSync == nullptr || !CFBooleanGetValue(notSync));
    }

    // ── 关键帧：提取 SPS + PPS 参数集，与本帧 IDR 合并为一个 packet ──
    // 传输粒度 = 完整视频帧（访问单元）：关键帧 = [SPS][PPS][IDR] 一帧
    std::vector<uint8_t> paramBuf;
    if (isKeyFrame) {
        CMFormatDescriptionRef fmtDesc =
            CMSampleBufferGetFormatDescription(sampleBuffer);
        if (fmtDesc) {
            size_t paramCount = 0;
            // 获取参数集总数
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                fmtDesc, 0, nullptr, nullptr, &paramCount, nullptr);

            if (paramCount > 0) {
                const uint8_t startCode[] = {0x00, 0x00, 0x00, 0x01};

                for (size_t i = 0; i < paramCount; ++i) {
                    const uint8_t* paramData = nullptr;
                    size_t         paramLen  = 0;
                    OSStatus s = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                        fmtDesc, i, &paramData, &paramLen, nullptr, nullptr);
                    if (s == noErr && paramData && paramLen > 0) {
                        paramBuf.insert(paramBuf.end(), startCode, startCode + 4);
                        paramBuf.insert(paramBuf.end(), paramData, paramData + paramLen);
                    }
                }
            }
        }
    }

    // ── 提取 AVCC 格式的 NAL 单元，转为 Annex-B ──────────────
    CMBlockBufferRef blockBuf = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!blockBuf) return;

    size_t totalLen = 0;
    char*  dataPtr  = nullptr;
    OSStatus s = CMBlockBufferGetDataPointer(
        blockBuf, 0, nullptr, &totalLen, &dataPtr);
    if (s != noErr || !dataPtr || totalLen == 0) return;

    // AVCC 每个 NAL 单元：[4字节大端长度][NAL 数据]
    // 转换为 Annex-B：[0x00 0x00 0x00 0x01][NAL 数据]
    std::vector<uint8_t> annexB;
    annexB.reserve(totalLen + 8);
    const uint8_t startCode[] = {0x00, 0x00, 0x00, 0x01};

    size_t offset = 0;
    while (offset + 4 <= totalLen) {
        uint32_t naluLen =
            (static_cast<uint32_t>((uint8_t)dataPtr[offset])     << 24) |
            (static_cast<uint32_t>((uint8_t)dataPtr[offset + 1]) << 16) |
            (static_cast<uint32_t>((uint8_t)dataPtr[offset + 2]) <<  8) |
            (static_cast<uint32_t>((uint8_t)dataPtr[offset + 3]));
        offset += 4;

        if (offset + naluLen > totalLen) break;

        annexB.insert(annexB.end(), startCode, startCode + 4);
        annexB.insert(annexB.end(),
                      (const uint8_t*)dataPtr + offset,
                      (const uint8_t*)dataPtr + offset + naluLen);
        offset += naluLen;
    }

    if (!annexB.empty()) {
        // 关键帧：参数集（若有）拼接在 IDR slice 之前，组成完整访问单元
        // 局部 vector 持有数据；m_callback 为同步回调（内部拷贝），随后销毁安全
        std::vector<uint8_t> fullFrame;
        fullFrame.reserve(paramBuf.size() + annexB.size());
        fullFrame.insert(fullFrame.end(), paramBuf.begin(), paramBuf.end());
        fullFrame.insert(fullFrame.end(), annexB.begin(), annexB.end());

        EncodedPacket pkt;
        pkt.data       = fullFrame.data();
        pkt.size       = fullFrame.size();
        pkt.isKeyFrame = isKeyFrame;
        pkt.isParamSet = false;
        self->m_callback(pkt);
    }
}

// ─────────────────────────────────────────────────────────────
// H264Encoder
// ─────────────────────────────────────────────────────────────
H264Encoder::H264Encoder() = default;

H264Encoder::~H264Encoder() {
    Release();
}

bool H264Encoder::Init(int width, int height, int fps, int bitrateKbps) {
    if (m_initialized) Release();

    m_fps = fps;

    VTCompressionSessionRef session = nullptr;
    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        width, height,
        kCMVideoCodecType_H264,
        nullptr,    // encoderSpecification（nil = 自动选择硬件/软件编码器）
        nullptr,    // sourceImageBufferAttributes
        nullptr,    // compressedDataAllocator
        OutputCallback,
        this,
        &session);

    if (status != noErr || !session) {
        std::cerr << "[H264] VTCompressionSessionCreate 失败, status=" << status << std::endl;
        return false;
    }

    // ── 编码配置 ──────────────────────────────────────────────

    // 实时模式
    VTSessionSetProperty(session,
        kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

    // 禁止 B 帧（降低延迟）
    VTSessionSetProperty(session,
        kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    // 编码 Profile：High Level Auto（兼容性好，画质优）
    VTSessionSetProperty(session,
        kVTCompressionPropertyKey_ProfileLevel,
        kVTProfileLevel_H264_High_AutoLevel);

    // 平均码率（bps）
    int bitrateBps = bitrateKbps * 1000;
    CFNumberRef bitrateRef = CFNumberCreate(nullptr, kCFNumberIntType, &bitrateBps);
    VTSessionSetProperty(session,
        kVTCompressionPropertyKey_AverageBitRate, bitrateRef);
    CFRelease(bitrateRef);

    // 目标帧率
    CFNumberRef fpsRef = CFNumberCreate(nullptr, kCFNumberIntType, &fps);
    VTSessionSetProperty(session,
        kVTCompressionPropertyKey_ExpectedFrameRate, fpsRef);
    CFRelease(fpsRef);

    // 关键帧最大间隔（2 秒插一个 IDR）
    int kfInterval = fps * 2;
    CFNumberRef kfRef = CFNumberCreate(nullptr, kCFNumberIntType, &kfInterval);
    VTSessionSetProperty(session,
        kVTCompressionPropertyKey_MaxKeyFrameInterval, kfRef);
    CFRelease(kfRef);

    // 准备开始编码
    VTCompressionSessionPrepareToEncodeFrames(session);

    m_session     = session;
    m_initialized = true;

    std::cout << "[H264] 编码器初始化成功: "
              << width << "x" << height
              << " @" << fps << "fps "
              << bitrateKbps << "kbps" << std::endl;
    return true;
}

void H264Encoder::Release() {
    if (m_session) {
        VTCompressionSessionRef session =
            static_cast<VTCompressionSessionRef>(m_session);
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        m_session = nullptr;
    }
    m_initialized  = false;
    m_forceKeyNext = false;
}

void H264Encoder::SetPacketCallback(PacketCallback cb) {
    m_callback = std::move(cb);
}

void H264Encoder::ForceKeyFrame() {
    m_forceKeyNext = true;
}

void H264Encoder::EncodeFrame(CVPixelBufferRef pixelBuffer, CMTime pts) {
    if (!m_initialized || !m_session || !pixelBuffer) return;

    VTCompressionSessionRef session =
        static_cast<VTCompressionSessionRef>(m_session);

    CMTime duration = CMTimeMake(1, m_fps);

    // 构建帧属性字典（可选：强制关键帧）
    CFMutableDictionaryRef frameProps = nullptr;
    if (m_forceKeyNext) {
        frameProps = CFDictionaryCreateMutable(
            nullptr, 1,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(frameProps,
            kVTEncodeFrameOptionKey_ForceKeyFrame,
            kCFBooleanTrue);
        m_forceKeyNext = false;
    }

    OSStatus status = VTCompressionSessionEncodeFrame(
        session,
        pixelBuffer,
        pts,
        duration,
        frameProps,   // frameProperties
        nullptr,      // sourceFrameRefCon
        nullptr);     // infoFlagsOut

    if (frameProps) CFRelease(frameProps);

    if (status != noErr) {
        std::cerr << "[H264] EncodeFrame 失败, status=" << status << std::endl;
    }
}

} // namespace h264
