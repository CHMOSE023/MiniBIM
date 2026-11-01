// 移植自 BIMCraft crates/rvt/src/elem_table.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/elem_table.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/ElemTable.h).
#include "Rvt/ElemTable.h"
#include "Rvt/Bytes.h"
#include "Rvt/Container.h"

#include <algorithm>
#include <optional>
#include <string>

namespace MiniBIM::Rvt
{
    namespace
    {
        constexpr size_t Origin = 6;
        // （记录大小，ElementId 的位置），长的在前
        constexpr std::pair<size_t, size_t> Layouts[2] = { { 40, 20 }, { 28, 4 } };
    }

    bool ParseElemTable(std::span<const uint8_t> data, std::vector<uint32_t>& out, Error& error)
    {
        const auto bad = [&](const std::string& why)
        {
            error = Error{ ErrorKind::Format, "Global/ElemTable " + why };
            return false;
        };
        const auto count32 = Bytes::U32At(data, 2);
        if (!count32)
            return bad("is too short to hold its record count");
        const size_t count = *count32;
        if (count == 0)
            return bad("lists no elements");
        std::optional<std::pair<size_t, std::pair<size_t, size_t>>> best;   // （尾巴，布局）
        for (const auto& [size, idAt] : Layouts)
        {
            if (count > (data.size() - std::min(data.size(), Origin)) / size)
                continue;
            const size_t end = Origin + count * size;
            bool rises = true;
            std::optional<uint32_t> previous;
            for (size_t k = 0; k < count && rises; ++k)
            {
                const auto id = Bytes::U32At(data, Origin + k * size + idAt);
                rises = id && (!previous || *id > *previous);
                previous = id;
            }
            if (rises && (!best || data.size() - end < best->first))
                best = { data.size() - end, { size, idAt } };
        }
        if (!best)
            return bad("has no record layout (28 or 40 bytes) in which its " + std::to_string(count) + " records fit with ElementIds that rise");
        const auto [size, idAt] = best->second;
        out.clear();
        out.reserve(count);
        for (size_t k = 0; k < count; ++k)
            if (const auto id = Bytes::U32At(data, Origin + k * size + idAt))
                out.push_back(*id);
        return true;
    }

    bool ReadElementIds(Revit& r, std::vector<uint32_t>& out, Error& error)
    {
        Gzip::Inflated got;
        return r.Inflate("Global/ElemTable", got, error) && ParseElemTable(got.bytes, out, error);
    }
}
