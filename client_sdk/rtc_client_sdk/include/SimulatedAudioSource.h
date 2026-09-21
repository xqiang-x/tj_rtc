#pragma once

#include <vector>
#include <cstdint>
#include <cmath>

namespace audio {

// 模拟音频源 - 生成 PCM 16-bit 正弦波数据
// 用于测试，不需要真实的麦克风采集
class SimulatedAudioSource {
public:
    SimulatedAudioSource(int sampleRate = 48000, int channels = 2)
        : m_sampleRate(sampleRate)
        , m_channels(channels)
        , m_phase(0.0)
    {
        // 每帧 20ms 音频
        m_framesPerPacket = sampleRate / 50;  // 48000/50 = 960 frames
        m_bytesPerFrame = channels * 2;  // 16-bit = 2 bytes
        m_bytesPerPacket = m_framesPerPacket * m_bytesPerFrame;
    }
    
    // 生成一帧音频数据（PCM 16-bit）
    // 返回包含正弦波的音频包
    std::vector<uint8_t> GeneratePacket() {
        std::vector<uint8_t> packet(m_bytesPerPacket);
        auto* samples = reinterpret_cast<int16_t*>(packet.data());
        
        // 生成正弦波（440Hz A4 音符）
        double frequency = 440.0;
        double phaseIncrement = (2.0 * M_PI * frequency) / m_sampleRate;
        
        for (size_t i = 0; i < m_framesPerPacket; ++i) {
            // 生成 -16000 到 +16000 之间的正弦波（避免削波）
            int16_t sample = static_cast<int16_t>(16000.0 * sin(m_phase));
            
            // 双声道：左右声道相同
            for (int ch = 0; ch < m_channels; ++ch) {
                samples[i * m_channels + ch] = sample;
            }
            
            m_phase += phaseIncrement;
            if (m_phase >= 2.0 * M_PI) {
                m_phase -= 2.0 * M_PI;
            }
        }
        
        return packet;
    }
    
    int GetSampleRate() const { return m_sampleRate; }
    int GetChannels() const { return m_channels; }
    int GetFramesPerPacket() const { return m_framesPerPacket; }
    int GetBytesPerPacket() const { return m_bytesPerPacket; }
    
private:
    int m_sampleRate;
    int m_channels;
    int m_framesPerPacket;
    int m_bytesPerFrame;
    int m_bytesPerPacket;
    double m_phase;
};

} // namespace audio
