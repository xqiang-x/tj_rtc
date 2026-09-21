// SPDX-License-Identifier: BSD-2-Clause
// NaluSplitter.h - H.264 Annex-B NAL 拆分/聚合器
//
// 作用：把编码器输出的一段 H.264 Annex-B 字节流（应为完整的一帧=
// 一个访问单元 AU），按 NAL 单元拆开、聚合归类后回调：
//   - VIDEO_IDR：含 IDR slice 的访问单元，参数集（SPS/PPS）与其合并
//                 为一段，即关键帧 = [SPS][PPS][IDR] 一个 frame
//   - VIDEO_P  ：普通 slice 访问单元（多 slice 编码时同帧 slice 合并）
//   - VIDEO_PARAMS：纯参数集帧（无 slice 跟随的边界残留，极少出现）
//
// 调用方应保证一次 Process 传入完整一帧（一次编码输出的全部 NAL）；
// 回调数据均自带 0x00000001 起始码（Annex-B）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

class NaluSplitter {
public:
    enum class Kind {
        kParams,  // 纯参数集帧（SPS+PPS，无 slice 跟随）
        kIdr,     // IDR 访问单元（可含 SPS/PPS + 多 slice）
        kSlice,   // 普通 slice 访问单元（多 slice 合并）
    };

    using OnNaluCb = std::function<void(Kind kind, const uint8_t *data,
                                        std::size_t size)>;

    void SetOnNalu(OnNaluCb cb) { on_nalu_ = std::move(cb); }

    // 解析一段 H.264 Annex-B 流（应含完整一帧的全部 NAL：可含
    // SPS/PPS 和/或一个或多个 slice）
    // 数据应包含起始码 0x000001 / 0x00000001。
    void Process(const uint8_t *data, std::size_t size);

private:
    OnNaluCb on_nalu_;

    // 找下一个起始码偏移，返回 -1 表示没有
    // 出参 sc_len 写起始码长度（3 或 4）
    static std::ptrdiff_t findStartCode(const uint8_t *data, std::size_t size,
                                        std::size_t from, int &sc_len);
};
