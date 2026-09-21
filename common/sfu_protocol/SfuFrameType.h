#pragma once

#include <cstdint>

namespace sfu {

// SFU 帧类型枚举
// 用于标识音视频帧的类型
enum class FrameType : uint8_t {
    // 视频帧类型
    VIDEO_PARAMS  = 0x01,  // 视频参数帧（SPS/PPS）
    VIDEO_IDR     = 0x02,  // 视频 IDR 帧（关键帧）
    VIDEO_P       = 0x03,  // 视频 P 帧（普通帧）
    
    // 音频帧类型
    AUDIO_PCM     = 0x10,  // PCM 音频帧（16-bit）
    
    // 保留类型
    UNKNOWN       = 0x00,
};

// 判断是否为视频帧
inline bool IsVideoFrame(FrameType type) {
    return type == FrameType::VIDEO_PARAMS || 
           type == FrameType::VIDEO_IDR || 
           type == FrameType::VIDEO_P;
}

// 判断是否为音频帧
inline bool IsAudioFrame(FrameType type) {
    return type == FrameType::AUDIO_PCM;
}

// 获取帧类型名称（用于日志）
inline const char* FrameTypeName(FrameType type) {
    switch (type) {
        case FrameType::VIDEO_PARAMS: return "VIDEO_PARAMS";
        case FrameType::VIDEO_IDR:    return "VIDEO_IDR";
        case FrameType::VIDEO_P:      return "VIDEO_P";
        case FrameType::AUDIO_PCM:    return "AUDIO_PCM";
        default:                      return "UNKNOWN";
    }
}

} // namespace sfu
