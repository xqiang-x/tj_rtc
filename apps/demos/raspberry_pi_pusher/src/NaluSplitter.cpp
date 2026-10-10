// SPDX-License-Identifier: BSD-2-Clause
// NaluSplitter.cpp

#include "NaluSplitter.h"

#include <vector>

namespace {
constexpr uint8_t kNalTypeMask = 0x1F;
constexpr uint8_t kNalSlice = 1;
constexpr uint8_t kNalIdrSlice = 5;
constexpr uint8_t kNalSps = 7;
constexpr uint8_t kNalPps = 8;
}  // namespace

std::ptrdiff_t NaluSplitter::findStartCode(const uint8_t *data, std::size_t size,
                                           std::size_t from, int &sc_len) {
    if (size < 3) return -1;
    for (std::size_t i = from; i + 2 < size; ++i) {
        if (data[i] != 0x00 || data[i + 1] != 0x00) continue;
        if (data[i + 2] == 0x01) {
            sc_len = 3;
            return static_cast<std::ptrdiff_t>(i);
        }
        if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
            sc_len = 4;
            return static_cast<std::ptrdiff_t>(i);
        }
    }
    return -1;
}

void NaluSplitter::Process(const uint8_t *data, std::size_t size) {
    if (!on_nalu_ || !data || size < 4) return;

    // 1. 找出所有 NAL 单元 [起始码后指针, 长度, type]
    struct Nalu {
        const uint8_t *p;
        std::size_t len;
        uint8_t type;
    };
    std::vector<Nalu> nals;
    nals.reserve(8);

    std::size_t pos = 0;
    int sc_len = 0;
    auto first = findStartCode(data, size, pos, sc_len);
    if (first < 0) return;
    pos = static_cast<std::size_t>(first) + sc_len;

    while (pos < size) {
        int next_sc = 0;
        auto next = findStartCode(data, size, pos, next_sc);
        std::size_t nal_end = (next < 0) ? size : static_cast<std::size_t>(next);
        if (nal_end > pos) {
            uint8_t header = data[pos];
            nals.push_back({data + pos, nal_end - pos, static_cast<uint8_t>(header & kNalTypeMask)});
        }
        if (next < 0) break;
        pos = nal_end + next_sc;
    }

    // 2. 归类输出：
    //    - 参数集（SPS/PPS）与紧随其后的 slice 合并为同一个访问单元整体输出，
    //      即关键帧 = [SPS][PPS][IDR slice] 一个 frame（IDR 帧带参数集）
    //    - 连续的 slice NAL 属于同一帧（多 slice 编码时后续 slice 用
    //      3 字节起始码分隔），合并为一个访问单元整体输出
    //    - SEI/AUD 等非 slice NAL 作为分组边界丢弃
    auto emit_params = [this](const std::vector<Nalu> &group) {
        std::vector<uint8_t> buf;
        for (const auto &n : group) {
            buf.push_back(0x00);
            buf.push_back(0x00);
            buf.push_back(0x00);
            buf.push_back(0x01);
            buf.insert(buf.end(), n.p, n.p + n.len);
        }
        on_nalu_(Kind::kParams, buf.data(), buf.size());
    };
    auto emit_slices = [this](const std::vector<Nalu> &group,
                              const std::vector<Nalu> &params) {
        bool has_idr = false;
        std::vector<uint8_t> buf;
        // 参数集拼接在 slice 之前，组成一个完整的访问单元
        for (const auto &n : params) {
            buf.push_back(0x00);
            buf.push_back(0x00);
            buf.push_back(0x00);
            buf.push_back(0x01);
            buf.insert(buf.end(), n.p, n.p + n.len);
        }
        for (const auto &n : group) {
            if (n.type == kNalIdrSlice) has_idr = true;
            buf.push_back(0x00);
            buf.push_back(0x00);
            buf.push_back(0x00);
            buf.push_back(0x01);
            buf.insert(buf.end(), n.p, n.p + n.len);
        }
        on_nalu_(has_idr ? Kind::kIdr : Kind::kSlice, buf.data(), buf.size());
    };

    std::vector<Nalu> param_group;
    std::vector<Nalu> slice_group;
    auto flush_slices = [&]() {
        if (!slice_group.empty()) {
            // 参数集与 slice 同属一帧时合并输出；纯参数集帧仅在无 slice 跟随
            // （边界残留）时单独输出
            if (!param_group.empty()) {
                emit_slices(slice_group, param_group);
                param_group.clear();
            } else {
                emit_slices(slice_group, {});
            }
            slice_group.clear();
        } else if (!param_group.empty()) {
            emit_params(param_group);
            param_group.clear();
        }
    };
    auto flush_params = [&]() {
        if (!param_group.empty()) {
            emit_params(param_group);
            param_group.clear();
        }
    };

    for (const auto &n : nals) {
        if (n.type == kNalSps || n.type == kNalPps) {
            // 上一帧 slice 先输出（此前可能积累），参数集等待与本帧 slice 合并
            flush_slices();
            param_group.push_back(n);
        } else if (n.type >= 1 && n.type <= 5) {
            slice_group.push_back(n);
        } else {
            // SEI/AUD/Filler 等：分组边界，不单独推流
            flush_slices();
            flush_params();
        }
    }
    flush_slices();
    flush_params();
}
