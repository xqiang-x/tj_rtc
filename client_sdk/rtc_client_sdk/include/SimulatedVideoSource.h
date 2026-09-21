#pragma once

#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace video {

// 模拟视频源 - 生成伪 H.264 NAL 单元
// 关键帧 (IDR) 约 50~80KB，普通帧 (P-frame) 约 5~15KB
// 用于音视频混合测试，不需要真实编码器
class SimulatedVideoSource {
public:
    struct Config {
        int fps = 25;
        int gop_size = 50;          // 每 50 帧一个关键帧 (2s @25fps)
        int idr_min_bytes = 50000;  // 关键帧最小 ~50KB
        int idr_max_bytes = 80000;  // 关键帧最大 ~80KB
        int p_min_bytes = 5000;     // P 帧最小 ~5KB
        int p_max_bytes = 15000;    // P 帧最大 ~15KB
    };

    enum class FrameKind {
        PARAMS,   // SPS + PPS
        IDR,      // 关键帧
        P_FRAME,  // 普通帧
    };

    struct GeneratedFrame {
        std::vector<uint8_t> data;
        FrameKind kind;
        bool isParamSet;
        bool isKeyFrame;
        int frame_index;
    };

    SimulatedVideoSource() = default;
    explicit SimulatedVideoSource(const Config& cfg) : m_cfg(cfg) {}

    // 生成下一帧（按 GOP 顺序循环：PARAMS → IDR → P → P → ... → IDR → ...）
    GeneratedFrame GenerateFrame() {
        GeneratedFrame frame;
        frame.frame_index = m_frameIndex++;

        int posInGop = frame.frame_index % m_cfg.gop_size;

        if (posInGop == 0) {
            // GOP 起始：SPS + PPS
            frame.data = buildParams();
            frame.kind = FrameKind::PARAMS;
            frame.isParamSet = true;
            frame.isKeyFrame = false;
        } else if (posInGop == 1) {
            // IDR 关键帧
            frame.data = buildNalUnit(0x65, randomInRange(m_cfg.idr_min_bytes, m_cfg.idr_max_bytes));
            frame.kind = FrameKind::IDR;
            frame.isParamSet = false;
            frame.isKeyFrame = true;
        } else {
            // P 帧
            frame.data = buildNalUnit(0x41, randomInRange(m_cfg.p_min_bytes, m_cfg.p_max_bytes));
            frame.kind = FrameKind::P_FRAME;
            frame.isParamSet = false;
            frame.isKeyFrame = false;
        }

        return frame;
    }

    int GetFps() const { return m_cfg.fps; }
    int GetGopSize() const { return m_cfg.gop_size; }
    int GetFrameIndex() const { return m_frameIndex; }

private:
    Config m_cfg;
    int m_frameIndex = 0;

    int randomInRange(int lo, int hi) const {
        if (lo >= hi) return lo;
        return lo + (std::rand() % (hi - lo + 1));
    }

    // 构造一个 NAL 单元：Annex-B start code + NAL header + 填充数据
    static std::vector<uint8_t> buildNalUnit(uint8_t nalHeader, int totalBytes) {
        std::vector<uint8_t> buf(totalBytes);
        // Annex-B start code
        buf[0] = 0x00;
        buf[1] = 0x00;
        buf[2] = 0x00;
        buf[3] = 0x01;
        // NAL header
        buf[4] = nalHeader;
        // 填充：用递增 pattern 方便调试时肉眼区分帧
        for (int i = 5; i < totalBytes; ++i) {
            buf[i] = static_cast<uint8_t>(i & 0xFF);
        }
        return buf;
    }

    // 构造 SPS + PPS（两个 NAL 单元拼在一起）
    static std::vector<uint8_t> buildParams() {
        std::vector<uint8_t> buf;
        buf.reserve(64);

        // SPS: nal_type=7
        buf.insert(buf.end(), {0x00, 0x00, 0x00, 0x01, 0x67});
        // 模拟 SPS 内容（20 字节）
        for (int i = 0; i < 20; ++i) {
            buf.push_back(static_cast<uint8_t>(0xA0 + i));
        }

        // PPS: nal_type=8
        buf.insert(buf.end(), {0x00, 0x00, 0x00, 0x01, 0x68});
        // 模拟 PPS 内容（8 字节）
        for (int i = 0; i < 8; ++i) {
            buf.push_back(static_cast<uint8_t>(0xB0 + i));
        }

        return buf;
    }
};

} // namespace video
