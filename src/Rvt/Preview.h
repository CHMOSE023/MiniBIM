// 移植自 BIMCraft crates/rvt/src/preview.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/reader.rs (`preview_png`: the
// stream `RevitPreview4.0` is a wrapper of about 300 bytes and then a PNG, found by its signature).
// Changed: rewritten for BIMCraft (the PNG walked chunk by chunk, each chunk's size capped and its CRC-32 checked, from IHDR
// to IEND, its dimensions bounded, what is returned ending at IEND); then rewritten in C++ for MiniBIM.
//
// Revit 存在 RevitPreview4.0 里的预览图：约 300 字节的包装，然后是 PNG。交出去之前先查：签名、第一块是尺寸合理的 IHDR、
// 每块大小在上限内且 CRC-32 对、最后是 IEND
#pragma once

#include "Rvt/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace MiniBIM::Rvt
{
    class Revit;

    // 预览图最大多少字节
    inline constexpr size_t MaxPng = 4 << 20;

    struct Preview
    {
        uint32_t             width  = 0;
        uint32_t             height = 0;
        std::vector<uint8_t> png;   // 从签名到 IEND 结束

        // 在流的字节里找出 PNG 并检查
        static bool Find(std::span<const uint8_t> stream, Preview& out, Error& error);
    };

    // 文件有预览图时读出；2023 到 2026 版的样例项目都没有
    bool ReadPreview(Revit& r, std::optional<Preview>& out, Error& error);
}
