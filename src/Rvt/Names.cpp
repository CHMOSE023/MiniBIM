// 移植自 BIMCraft crates/rvt/src/names.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_names.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Names.h).
#include "Rvt/Names.h"
#include "Rvt/Bytes.h"
#include "Rvt/Records.h"

#include <algorithm>

namespace MiniBIM::Rvt::Names
{
    namespace
    {
        constexpr size_t MaxUnits   = 256;
        constexpr size_t MaxUnits32 = 128;

        bool AnyControl(std::u32string_view cps) { return std::any_of(cps.begin(), cps.end(), [](char32_t c) { return Bytes::IsControl(c); }); }

        // 恰好一个 ASCII 以外的字符：32 位数据里只有一个 id 的列表读出来像一个码元的名字（id 的低半）
        bool LoneNonAscii(std::u32string_view cps) { return cps.size() == 1 && cps[0] >= 0x80; }
    }

    void Entries64(std::span<const uint8_t> buf, const std::vector<uint32_t>& ids, std::vector<Entry>& out)
    {
        static constexpr uint8_t top[5] = { 0xff, 0xff, 0xff, 0xff, 0xff };
        Bytes::ForEach(buf, top, [&](size_t hit)
        {
            if (hit < 3)
                return;
            const size_t at = hit - 3;
            const auto category = Bytes::U64At(buf, at);
            if (!category || !Records::InBand(static_cast<int64_t>(*category)))
                return;
            for (size_t n = 1; n <= MaxUnits; ++n)
            {
                if (at < 2 * n + 4)
                    break;
                const size_t start = at - 2 * n, lengthAt = start - 4;
                if (Bytes::U32At(buf, lengthAt) != static_cast<uint32_t>(n))
                    continue;
                if (lengthAt < 8)
                    break;
                const auto raw = Bytes::U64At(buf, lengthAt - 8);
                if (!raw || *raw > 0xffff'ffffull || !Records::Declared(ids, static_cast<uint32_t>(*raw)))
                    continue;
                const auto name = Bytes::CodePointsAt(buf, start, n);
                if (!name || Bytes::IsBlank(*name) || AnyControl(*name))
                    continue;
                out.push_back({ static_cast<uint32_t>(*raw), Bytes::Utf8(*name), static_cast<int64_t>(*category) });
                break;
            }
        });
    }

    void Entries32(std::span<const uint8_t> buf, const std::vector<uint32_t>& ids, std::vector<Entry>& out)
    {
        static constexpr uint8_t one[4] = { 1, 0, 0, 0 };
        Bytes::ForEach(buf, one, [&](size_t at)
        {
            const auto id = Bytes::U32At(buf, at + 4), n = Bytes::U32At(buf, at + 8);
            if (!id || !n || *n < 1 || *n > MaxUnits32 || !Records::Declared(ids, *id))
                return;
            const auto name = Bytes::CodePointsAt(buf, at + 12, *n);
            if (!name)
                return;
            const bool printable = std::all_of(name->begin(), name->end(), [](char32_t c) { return !Bytes::IsControl(c) && !(c >= 0xe000 && c <= 0xf8ff) && c < 0xfff0; });
            if (printable && !LoneNonAscii(*name))
                out.push_back({ *id, Bytes::Utf8(*name), std::nullopt });
        });
    }
}
