// H264Decoder.cpp — VideoToolbox H.264 硬件解码实现

#include "H264Decoder.h"

#include <iostream>
#include <cstring>

// ── 推流端定义的载荷帧类型标识（与 Publisher.cpp 保持一致） ────────────
static constexpr uint8_t kVideoParams = 0x01;  // SPS+PPS
static constexpr uint8_t kVideoIDR    = 0x02;  // IDR 关键帧
static constexpr uint8_t kVideoP      = 0x03;  // P/B 帧

// ─────────────────────────────────────────────────────────────────────────────
// 工具：从 Annex-B 码流中提取各 NAL 单元的起止位置
// 返回 vector<{nalStart, nalEnd}>，均为相对于 data 的字节偏移
// nalStart 指向 NAL 首字节（nal_unit_type 字节），已跳过起始码
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::pair<size_t, size_t>>
FindNalUnits(const uint8_t* data, size_t len)
{
    // 收集所有起始码位置（指向 00...01 的 00 的位置）以及对应的 NAL 起始位置
    std::vector<size_t> scPos;   // 起始码的起始字节位置
    std::vector<size_t> nalPos;  // NAL 数据起始位置（起始码之后）

    size_t i = 0;
    while (i < len) {
        bool sc4 = (i + 4 <= len &&
                    data[i] == 0 && data[i+1] == 0 &&
                    data[i+2] == 0 && data[i+3] == 1);
        bool sc3 = !sc4 && (i + 3 <= len &&
                    data[i] == 0 && data[i+1] == 0 && data[i+2] == 1);
        if (sc4) { scPos.push_back(i); nalPos.push_back(i + 4); i += 4; }
        else if (sc3) { scPos.push_back(i); nalPos.push_back(i + 3); i += 3; }
        else { ++i; }
    }

    std::vector<std::pair<size_t, size_t>> result;
    for (size_t k = 0; k < nalPos.size(); ++k) {
        size_t start = nalPos[k];
        size_t end   = (k + 1 < scPos.size()) ? scPos[k + 1] : len;
        if (end > start) result.push_back({start, end});
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
H264Decoder::H264Decoder()  = default;

H264Decoder::~H264Decoder() {
    DestroySession();
    if (m_fmtDesc) { CFRelease(m_fmtDesc); m_fmtDesc = nullptr; }
}

// ─────────────────────────────────────────────────────────────────────────────
void H264Decoder::FeedPayload(const uint8_t* data, size_t len)
{
    if (!data || len < 2) return;

    uint8_t frameType = data[0];
    const uint8_t* annexB = data + 1;
    size_t  annexBLen     = len - 1;

    switch (frameType) {
    case kVideoParams: HandleParamSet(annexB, annexBLen); break;
    case kVideoIDR:    HandleVideoFrame(annexB, annexBLen); break;
    case kVideoP:      HandleVideoFrame(annexB, annexBLen); break;
    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 解析 SPS/PPS，重建格式描述和解码会话
// ─────────────────────────────────────────────────────────────────────────────
void H264Decoder::HandleParamSet(const uint8_t* annexB, size_t len)
{
    auto nals = FindNalUnits(annexB, len);

    std::vector<uint8_t> newSps, newPps;
    for (auto& [s, e] : nals) {
        uint8_t nalType = annexB[s] & 0x1F;
        if (nalType == 7 && newSps.empty()) {  // SPS
            newSps.assign(annexB + s, annexB + e);
        } else if (nalType == 8 && newPps.empty()) {  // PPS
            newPps.assign(annexB + s, annexB + e);
        }
    }

    // 推流端（sim_pusher b_repeat_headers + NaluSplitter）把 SPS、PPS 拆成
    // 独立 NAL 帧推送，单帧内通常只有其一；跨帧缓存补齐后再建会话。
    if (!newSps.empty()) m_sps = std::move(newSps);
    if (!newPps.empty()) m_pps = std::move(newPps);

    if (m_sps.empty() || m_pps.empty()) {
        std::cerr << "[H264] 未找到完整的 SPS/PPS" << std::endl;
        return;
    }
    // SPS/PPS 未变化时不重建会话（首次建立后通过 kFormatDescSeq 判断）
    if (m_fmtDesc && m_sps == m_lastSps && m_pps == m_lastPps) return;

    m_lastSps = m_sps;
    m_lastPps = m_pps;

    // 重建 CMVideoFormatDescription
    if (m_fmtDesc) { CFRelease(m_fmtDesc); m_fmtDesc = nullptr; }

    const uint8_t* paramSets[2] = { m_sps.data(), m_pps.data() };
    size_t         paramSizes[2] = { m_sps.size(),  m_pps.size() };

    OSStatus err = CMVideoFormatDescriptionCreateFromH264ParameterSets(
        kCFAllocatorDefault, 2, paramSets, paramSizes,
        4,          // NAL length field size (AVCC 使用 4 字节)
        &m_fmtDesc);

    if (err != noErr) {
        std::cerr << "[H264] CMVideoFormatDescriptionCreateFromH264ParameterSets 失败: "
                  << err << std::endl;
        return;
    }

    CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(m_fmtDesc);
    m_width  = dim.width;
    m_height = dim.height;
    std::cout << "[H264] 视频尺寸: " << m_width << "x" << m_height << std::endl;

    CreateSession();
}

// ─────────────────────────────────────────────────────────────────────────────
// 解码一帧（IDR 或 P/B）。传输链路以完整访问单元（AU）为帧：IDR 帧内可能
// 内嵌 SPS/PPS（关键帧 = [SPS][PPS][IDR]），先提取参数集、变化时重建会话，
// 再只把 slice 喂给解码器（参数集由 format description 提供）。
// ─────────────────────────────────────────────────────────────────────────────
void H264Decoder::HandleVideoFrame(const uint8_t* annexB, size_t len)
{
    // 帧内是否携带参数集（SPS/PPS）
    bool hasParams = false;
    {
        auto nals = FindNalUnits(annexB, len);
        for (auto& [s, e] : nals) {
            uint8_t nalType = annexB[s] & 0x1F;
            if (nalType == 7 || nalType == 8) { hasParams = true; break; }
        }
    }
    // 提取并应用参数集（内部去重：未变化不重建会话）
    if (hasParams) HandleParamSet(annexB, len);
    // P 帧仅有 slice 时，若会话尚未建立（如中途订阅），等待下一 IDR
    if (!m_session) return;

    CMSampleBufferRef sample = nullptr;
    if (!MakeSampleBuffer(annexB, len, sample)) return;

    VTDecodeInfoFlags infoFlags = 0;
    // flags = 0 → 同步解码，回调在本调用栈内触发
    OSStatus err = VTDecompressionSessionDecodeFrame(
        m_session, sample, 0, nullptr, &infoFlags);

    CFRelease(sample);

    if (err != noErr && err != kVTVideoDecoderBadDataErr) {
        std::cerr << "[H264] VTDecompressionSessionDecodeFrame 失败: " << err << std::endl;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 将 Annex-B 转换为 AVCC 并封装为 CMSampleBuffer
// ─────────────────────────────────────────────────────────────────────────────
bool H264Decoder::MakeSampleBuffer(const uint8_t* annexB, size_t len, CMSampleBufferRef& outSample)
{
    auto nals = FindNalUnits(annexB, len);
    if (nals.empty()) return false;

    // 拼接 AVCC 格式：每个 NAL 前加 4 字节大端长度
    // 帧内嵌的 SPS/PPS（参数集）跳过——由 format description 提供
    std::vector<uint8_t> avcc;
    avcc.reserve(len + nals.size() * 4);

    for (auto& [s, e] : nals) {
        uint8_t nalType = annexB[s] & 0x1F;
        if (nalType == 7 || nalType == 8) continue;
        size_t  nalSize = e - s;
        uint8_t hdr[4] = {
            (uint8_t)(nalSize >> 24), (uint8_t)(nalSize >> 16),
            (uint8_t)(nalSize >>  8), (uint8_t)(nalSize)
        };
        avcc.insert(avcc.end(), hdr, hdr + 4);
        avcc.insert(avcc.end(), annexB + s, annexB + e);
    }
    if (avcc.empty()) return false;

    // ── CMBlockBuffer ──────────────────────────────────────────────────────
    // 将 avcc 数据拷贝到 malloc 内存，由 CoreMedia 在 CMBlockBuffer 释放时 free()
    uint8_t* rawBuf = (uint8_t*)malloc(avcc.size());
    if (!rawBuf) return false;
    memcpy(rawBuf, avcc.data(), avcc.size());

    CMBlockBufferRef blockBuf = nullptr;
    OSStatus err = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        rawBuf,          // memoryBlock（已拷贝）
        avcc.size(),     // blockLength
        kCFAllocatorMalloc,  // blockAllocator → 释放时调用 free()
        nullptr,         // customBlockSource
        0,               // offsetToData
        avcc.size(),     // dataLength
        0,               // flags
        &blockBuf);
    if (err != noErr) {
        free(rawBuf);
        std::cerr << "[H264] CMBlockBufferCreateWithMemoryBlock 失败: " << err << std::endl;
        return false;
    }

    // ── CMSampleBuffer ─────────────────────────────────────────────────────
    CMTime pts = CMTimeMake(m_pts++, 90000);  // 90kHz 时间基
    CMSampleTimingInfo timing = { kCMTimeInvalid, pts, kCMTimeInvalid };
    size_t sampleSize = avcc.size();

    err = CMSampleBufferCreateReady(
        kCFAllocatorDefault,
        blockBuf, m_fmtDesc,
        1, 1, &timing,
        1, &sampleSize,
        &outSample);

    CFRelease(blockBuf);

    if (err != noErr) {
        std::cerr << "[H264] CMSampleBufferCreateReady 失败: " << err << std::endl;
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 创建 VTDecompressionSession，输出像素格式为 NV12 Full Range
// ─────────────────────────────────────────────────────────────────────────────
bool H264Decoder::CreateSession()
{
    DestroySession();

    if (!m_fmtDesc) return false;

    // 输出像素格式：NV12 Full Range（SDL_PIXELFORMAT_NV12 兼容）
    const OSType pixFmt = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    CFNumberRef pixNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixFmt);

    const void* keys[]   = { kCVPixelBufferPixelFormatTypeKey };
    const void* values[] = { pixNum };
    CFDictionaryRef destAttrs = CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(pixNum);

    VTDecompressionOutputCallbackRecord cb = { DecompressionCallback, this };

    OSStatus err = VTDecompressionSessionCreate(
        kCFAllocatorDefault,
        m_fmtDesc,
        nullptr,    // videoDecoderSpecification
        destAttrs,
        &cb,
        &m_session);

    CFRelease(destAttrs);

    if (err != noErr) {
        std::cerr << "[H264] VTDecompressionSessionCreate 失败: " << err << std::endl;
        return false;
    }

    std::cout << "[H264] 解码会话已创建（VideoToolbox）" << std::endl;
    return true;
}

void H264Decoder::DestroySession()
{
    if (m_session) {
        VTDecompressionSessionInvalidate(m_session);
        CFRelease(m_session);
        m_session = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// VTDecompressionSession 输出回调（在 FeedPayload 调用栈内同步调用）
// ─────────────────────────────────────────────────────────────────────────────
void H264Decoder::DecompressionCallback(
    void*             refCon,
    void*             /*frameRefCon*/,
    OSStatus          status,
    VTDecodeInfoFlags /*infoFlags*/,
    CVImageBufferRef  imageBuffer,
    CMTime            /*pts*/,
    CMTime            /*dur*/)
{
    if (status != noErr || !imageBuffer) return;

    auto* self = reinterpret_cast<H264Decoder*>(refCon);
    if (self->m_callback) {
        self->m_callback(imageBuffer, self->m_width, self->m_height);
    }
}
