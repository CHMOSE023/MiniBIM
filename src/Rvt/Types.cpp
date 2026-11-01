// 移植自 BIMCraft crates/rvt/src/types.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_compound_structure.rs,
// src/partition_names.rs and src/partition_type_records.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Types.h).
#include "Rvt/Types.h"
#include "Rvt/Bytes.h"
#include "Rvt/Records.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace MiniBIM::Rvt::Types
{
    namespace
    {
        using Buf = std::span<const uint8_t>;
        using Bytes::U32At;
        using Bytes::U64At;

        // 一个类型最多当作有几层
        constexpr size_t MaxLayers = 32;
        // 类型名字最长几个 UTF-16 码元
        constexpr size_t MaxNameUnits = 256;
        // 在元素的数据里找名字时看多远
        constexpr size_t NameWindow = 0x600;
        // 墙的定位线、一个字与翻转标志前面的字节
        constexpr uint8_t WallFlipAnchor[8] = { 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00 };
        constexpr uint8_t Unset[4] = { 0xff, 0xff, 0xff, 0xff };

        std::optional<uint64_t> IdAt(Buf buf, size_t at, bool wide)
        {
            if (wide)
                return U64At(buf, at);
            const auto id = U32At(buf, at);
            if (!id)
                return std::nullopt;
            return *id == 0xffff'ffffu ? ~0ull : uint64_t(*id);
        }

        // countAt 处个数开头的层；每条记录都是类型会有的才算：宽度小于 10 英尺且为正（只有膜与结构压型板可以是 0）、
        // 材质在 materials 里或按类别、压型板轮廓没有或是声明过的 id、功能已知
        std::optional<std::vector<CompoundLayer>> LayersAt(Buf buf, size_t countAt, const LayerLayout& layout, const std::set<uint32_t>& materials,
                                                           const std::vector<uint32_t>& ids)
        {
            const auto count = U32At(buf, countAt);
            if (!count || *count < 1 || *count > MaxLayers)
                return std::nullopt;
            std::vector<CompoundLayer> layers;
            layers.reserve(*count);
            for (size_t index = 0; index < *count; ++index)
            {
                const size_t at = countAt + 4 + layout.recordLen * index;
                const auto bits     = U64At(buf, at);
                const auto material = IdAt(buf, at + layout.materialAt, layout.wideIds);
                const auto deck     = IdAt(buf, at + layout.deckAt, layout.wideIds);
                const auto function = U32At(buf, at + layout.functionAt);
                if (!bits || !material || !deck || !function)
                    return std::nullopt;
                const double width = std::bit_cast<double>(*bits);
                // 膜没有宽度；上面那层带着轮廓的结构压型板也没有
                const bool widthless = *function == 100 || *function == 200;
                const bool widthOk   = std::isfinite(width) && width < 10.0 && (width >= 1e-3 || (widthless && width == 0.0));
                std::optional<uint32_t> own;
                if (*material != ~0ull)
                {
                    if (*material > 0xffff'ffffull || !materials.contains(static_cast<uint32_t>(*material)))
                        return std::nullopt;
                    own = static_cast<uint32_t>(*material);
                }
                const bool deckOk = *deck == ~0ull || (*deck <= 0xffff'ffffull && Records::Declared(ids, static_cast<uint32_t>(*deck)));
                if (!widthOk || !deckOk || !(*function <= 5 || *function == 100 || *function == 200))
                    return std::nullopt;
                layers.push_back({ width, own, *function });
            }
            return layers;
        }

        // countAt 处的层数是否像类型的层那样被框住：`ff ff ff ff` 与类表里 VerticalRegionsStructure 的标签，
        // 或者类型的名字（u32 k、k 个 UTF-16 码元）与一个为 0 的 u32
        bool Framed(Buf buf, size_t countAt, std::array<uint8_t, 2> tag)
        {
            if (countAt >= 6 && Bytes::Matches(buf, countAt - 6, Unset) && Bytes::Matches(buf, countAt - 2, tag))
                return true;
            if (countAt < 4 || U32At(buf, countAt - 4) != uint32_t(0))
                return false;
            for (size_t k = 1; k <= MaxNameUnits; ++k)
            {
                if (countAt < 4 + 2 * k + 4)
                    break;
                const size_t start = countAt - (4 + 2 * k + 4);
                if (U32At(buf, start) != static_cast<uint32_t>(k))
                    continue;
                bool printable = true;
                for (size_t i = 0; i < k && printable; ++i)
                {
                    const auto unit = Bytes::U16At(buf, start + 4 + 2 * i);
                    printable = unit && *unit >= 0x20;
                }
                if (printable)
                    return true;
            }
            return false;
        }

        // Revit 2015 到 2020 的墙数据里没有 WallOrientation::word
        bool HasWord(uint32_t release) { return !(release >= 2015 && release <= 2020); }
    }

    std::optional<LayerLayout> LayoutOf(uint32_t release)
    {
        // Revit 2015 到 2022 写的是 2023 的记录（rvt-rs RE-180、RE-181）
        if (release >= 2015 && release <= 2023)
            return LayerLayout{ 29, 8, 16, 20, false };
        if (release == 2024 || release == 2025)
            return LayerLayout{ 37, 24, 8, 16, true };
        // 2026 在功能之后加了一个重复它的 u32（RE-124）；2027 写的一样（RE-179）
        if (release == 2026 || release == 2027)
            return LayerLayout{ 41, 24, 8, 16, true };
        return std::nullopt;
    }

    std::optional<std::vector<CompoundLayer>> FindLayers(Buf data, std::array<uint8_t, 2> tag, const LayerLayout& layout, const std::set<uint32_t>& materials,
                                                         const std::vector<uint32_t>& ids)
    {
        data = data.first(std::min(LayerWindow, data.size()));
        const size_t end = data.size() < 4 ? 0 : data.size() - 4;
        for (size_t at = 6; at < end; ++at)
            if (Framed(data, at, tag))
                if (auto layers = LayersAt(data, at, layout, materials, ids))
                    return layers;
        return std::nullopt;
    }

    std::optional<WallOrientation> Orientation(Buf data, uint32_t release)
    {
        data = data.first(std::min(LayerWindow, data.size()));
        const size_t anchor = Bytes::Find(data, WallFlipAnchor);
        if (anchor == Bytes::npos)
            return std::nullopt;
        const size_t at = anchor + sizeof WallFlipAnchor;
        const auto locationLine = U32At(data, at);
        if (!locationLine)
            return std::nullopt;
        uint32_t word = 0;
        size_t flipAt = at + 4;
        if (HasWord(release))
        {
            const auto w = U32At(data, at + 4);
            if (!w)
                return std::nullopt;
            word   = *w;
            flipAt = at + 8;
        }
        if (flipAt > data.size() || data.size() - flipAt < 4)
            return std::nullopt;
        const uint8_t* f = data.data() + flipAt;
        const bool plain = f[1] == 0 && f[2] == 0 && f[3] == 0, second = f[1] == 1 && f[2] == 0xff && f[3] == 0xff;
        if (f[0] > 1 || !(plain || second))
            return std::nullopt;
        if (*locationLine > 5 || word > 2)
            return std::nullopt;
        return WallOrientation{ *locationLine, word, f[0] == 1 };
    }

    std::optional<std::string> FirstName(Buf data, const std::array<uint8_t, 10>& header, uint32_t release)
    {
        const Buf window = data.first(std::min(NameWindow, data.size()));
        for (size_t at = Bytes::Find(window, Unset); at != Bytes::npos; at = Bytes::Find(window, Unset, at + sizeof Unset))
        {
            const auto tag = Bytes::U16At(window, at + 4);
            if (!tag || Bytes::Matches(window, at, header) || *tag == 0xffff)
                continue;
            const auto units = U32At(window, at + 6);
            if (!units || *units < 1 || *units > MaxNameUnits)
                continue;
            const auto name = Bytes::CodePointsAt(window, at + 10, *units);
            if (!name)
                continue;
            // U+FFFF 与 U+FFFE 填在没设的格子里，不会是名字的一部分
            if (Bytes::IsBlank(*name) || std::any_of(name->begin(), name->end(), [](char32_t c) { return Bytes::IsControl(c) || c == 0xfffd || c == 0xfffe || c == 0xffff; }))
                continue;
            if (release < 2023 && name->size() < 2)
                continue;
            return Bytes::Utf8(*name);
        }
        return std::nullopt;
    }

    std::optional<uint32_t> UniqueType(const std::vector<uint64_t>& refs, const std::set<uint32_t>& types)
    {
        std::optional<uint32_t> found;
        for (const uint64_t v : refs)
        {
            if (v > 0xffff'ffffull || !types.contains(static_cast<uint32_t>(v)))
                continue;
            const uint32_t id = static_cast<uint32_t>(v);
            if (!found)
                found = id;
            else if (*found != id)
                return std::nullopt;
        }
        return found;
    }
}
