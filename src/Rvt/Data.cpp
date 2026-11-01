// 移植自 BIMCraft crates/rvt/src/data.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_names.rs,
// src/partition_beam_axes.rs and src/partition_materials.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Data.h).
#include "Rvt/Data.h"
#include "Rvt/Bytes.h"
#include "Rvt/Records.h"

#include <cmath>

namespace MiniBIM::Rvt::Data
{
    namespace
    {
        using Buf = std::span<const uint8_t>;
        using Bytes::U32At;
        using Bytes::U64At;

        constexpr uint8_t One[4] = { 1, 0, 0, 0 };

        std::array<uint8_t, 8> Le64(int64_t v)
        {
            std::array<uint8_t, 8> b{};
            for (size_t i = 0; i < 8; ++i)
                b[i] = static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i));
            return b;
        }

        std::array<uint8_t, 4> Le32(int32_t v)
        {
            std::array<uint8_t, 4> b{};
            for (size_t i = 0; i < 4; ++i)
                b[i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
            return b;
        }

        // `ff ff ff ff` 与一个类标签
        std::array<uint8_t, 6> Framed(uint16_t tag) { return { 0xff, 0xff, 0xff, 0xff, static_cast<uint8_t>(tag), static_cast<uint8_t>(tag >> 8) }; }

        std::optional<uint32_t> SmallId(std::optional<uint64_t> v)
        {
            if (!v || *v > 0xffff'ffffull)
                return std::nullopt;
            return static_cast<uint32_t>(*v);
        }

        // 2024 起对象样式里类别的 id 之后、材质之前的
        constexpr uint8_t CategoryFrame64[20] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        // `i32 -3000010`：32 位形式里紧挨在材质之前
        constexpr uint8_t CategoryMark32[4] = { 0x36, 0x39, 0xd2, 0xff };

        // 柱的 u64 id 之后在几何节点里的：`u64 3 · u64 -1`
        constexpr uint8_t NodeAfterId[16] = { 3, 0, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255 };
        constexpr double NodeTolerance = 0.01;
        constexpr double PlanTolerance = 1e-6;

        std::optional<Box> NodeBox(Buf buf, size_t at)
        {
            Box out{};
            for (size_t i = 0; i < 6; ++i)
            {
                const auto v = Bytes::F64At(buf, at + i * 8);
                if (!v || !std::isfinite(*v) || std::abs(*v) > Records::MaxFeet)
                    return std::nullopt;
                out[i] = *v;
            }
            if (out[3] > out[0] && out[4] > out[1] && out[5] > out[2])
                return out;
            return std::nullopt;
        }

        bool PlanEqual(const Box& a, const Box& b, double tolerance)
        {
            for (size_t i : { 0, 1, 3, 4 })
                if (!(std::abs(a[i] - b[i]) < tolerance))
                    return false;
            return true;
        }

        constexpr int64_t RoomNumber = -1'006'901;
        constexpr int64_t RoomName   = -1'006'900;
        constexpr size_t  RoomWindow = 0x400;

        // at 处计数不超过 limit 的 UTF-16 文字，以及它结束的位置
        std::optional<std::pair<std::string, size_t>> Counted(Buf buf, size_t at, size_t limit)
        {
            const auto n = U32At(buf, at);
            if (!n || *n > limit)
                return std::nullopt;
            const auto text = Bytes::CodePointsAt(buf, at + 4, *n);
            if (!text || std::any_of(text->begin(), text->end(), [](char32_t c) { return Bytes::IsControl(c); }))
                return std::nullopt;
            return std::pair{ Bytes::Utf8(*text), at + 4 + 2 * static_cast<size_t>(*n) };
        }

        constexpr size_t  NameWindow    = 0x1000;
        constexpr int64_t NameParameter = -1'001'203;
        constexpr size_t  TagAfterId64  = 0x47;
        constexpr size_t  TagAfterId32  = 0x27;

        // 码元都在 0x20 到 0xfffd 之间、1 到 256 个码元的名字
        std::optional<std::u32string> Utf16Name(Buf buf, size_t at, uint32_t n)
        {
            if (n < 1 || n > 256)
                return std::nullopt;
            for (size_t i = 0; i < n; ++i)
            {
                const auto u = Bytes::U16At(buf, at + 2 * i);
                if (!u || *u < 0x20 || *u >= 0xfffe)
                    return std::nullopt;
            }
            return Bytes::CodePointsAt(buf, at, n);
        }

        bool IsBuiltinParameter(int64_t id) { return id >= -2'000'000 && id <= -1'000'000; }

        // object 里 from 处两到四个为 0 的 u32 之后的名字；后面跟的是内置参数 id（参数条目）时不算
        std::optional<std::u32string> OwnName(Buf object, size_t from)
        {
            size_t at = from, zeros = 0;
            while (zeros < 4 && U32At(object, at) == uint32_t(0))
            {
                at += 4;
                ++zeros;
            }
            if (zeros < 2)
                return std::nullopt;
            const auto n = U32At(object, at);
            if (!n)
                return std::nullopt;
            const auto param   = U64At(object, at + 4);
            const auto param32 = U32At(object, at + 4);
            if ((param && IsBuiltinParameter(static_cast<int64_t>(*param))) || (param32 && IsBuiltinParameter(static_cast<int32_t>(*param32))))
                return std::nullopt;
            auto name = Utf16Name(object, at + 4, *n);
            // 私用区与特殊区的码元不会是名字
            if (name && std::any_of(name->begin(), name->end(), [](char32_t c) { return (c >= 0xe000 && c <= 0xf8ff) || (c >= 0xfff0 && c <= 0xffff); }))
                return std::nullopt;
            return name;
        }

        // 恰好在 `ff ff ff ff` 与标签之前结束的那一个带计数的字符串
        std::optional<std::u32string> Terminated(Buf object, size_t from, uint16_t tag)
        {
            if (from > object.size())
                return std::nullopt;
            const auto terminator = Framed(tag);
            const size_t end = Bytes::Find(object, terminator, from);
            if (end == Bytes::npos)
                return std::nullopt;
            std::optional<std::u32string> found;
            for (uint32_t n = 1; n <= 256; ++n)
            {
                const size_t back = 4 + 2 * static_cast<size_t>(n);
                if (end < back || end - back < from)
                    continue;
                const size_t at = end - back;
                if (U32At(object, at) != n)
                    continue;
                if (auto name = Utf16Name(object, at + 4, n))
                {
                    if (found)
                        return std::nullopt;
                    found = std::move(name);
                }
            }
            return found;
        }

        // 最后的办法：对象里第一个以八个 0 字节、一个 u64 ElementId 与八个 ff 收尾的字符串。自己的名字字段是一串共享参数条目的材质，
        // 名字在这些条目之后
        std::optional<std::u32string> ClosedName(Buf object, size_t from)
        {
            static constexpr uint8_t zeros[8] = {}, ones[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
            const size_t last = object.size() < 4 ? 0 : object.size() - 4;
            for (size_t at = from; at < last; ++at)
            {
                const auto n = U32At(object, at);
                if (!n)
                    continue;
                auto name = Utf16Name(object, at + 4, *n);
                if (!name)
                    continue;
                const size_t after = at + 4 + 2 * static_cast<size_t>(*n);
                const auto id = U64At(object, after + 8);
                if (id && Bytes::Matches(object, after, zeros) && Bytes::Matches(object, after + 16, ones) && *id > 0 && *id <= 0xffff'ffffull)
                    return name;
            }
            return std::nullopt;
        }

        // 材质的名字：object 是从它的 id 起的字节，类标签在 tagAt
        std::optional<std::u32string> MaterialName(Buf object, size_t tagAt, const Tags& tags, bool wide)
        {
            const size_t from = tagAt + 2;
            if (auto name = OwnName(object, from))
                return name;
            if (tags.physicalParamSet)
                if (auto name = Terminated(object, from, *tags.physicalParamSet))
                    return name;
            if (tags.patternHelper && wide)
            {
                const auto frame = Framed(*tags.patternHelper);
                std::optional<std::u32string> framed;
                for (size_t hit = Bytes::Find(object, frame); hit != Bytes::npos && !framed; hit = Bytes::Find(object, frame, hit + frame.size()))
                    if (const auto n = U32At(object, hit + frame.size()))
                        framed = Utf16Name(object, hit + frame.size() + 4, *n);
                if (framed)
                    return framed;
            }
            for (const size_t entryLen : { size_t(8), size_t(4) })
            {
                std::vector<uint8_t> entry;
                if (entryLen == 8)
                {
                    const auto b = Le64(NameParameter);
                    entry.assign(b.begin(), b.end());
                }
                else
                {
                    const auto b = Le32(static_cast<int32_t>(NameParameter));
                    entry.assign(b.begin(), b.end());
                }
                entry.insert(entry.end(), { 0, 0, 0, 0 });
                std::optional<std::u32string> found;
                for (size_t hit = Bytes::Find(object, entry); hit != Bytes::npos && !found; hit = Bytes::Find(object, entry, hit + entry.size()))
                    if (const auto n = U32At(object, hit + entry.size()))
                        found = Utf16Name(object, hit + entry.size() + 4, *n);
                if (found)
                    return found;
            }
            return wide ? ClosedName(object, from) : std::nullopt;
        }
    }

    void ElementData(Buf buf, const Tags& tags, bool wide, const std::set<uint32_t>& wanted, uint32_t partition, Elements& out)
    {
        if (wanted.empty())
            return;
        const auto header = tags.DataHeader();
        std::vector<size_t> hits;
        Bytes::ForEach(buf, header, [&](size_t at) { hits.push_back(at); });
        for (size_t index = 0; index < hits.size(); ++index)
        {
            const size_t hit = hits[index], at = hit + header.size();
            const auto id = wide ? SmallId(U64At(buf, at)) : U32At(buf, at);
            if (!id || !wanted.contains(*id))
                continue;
            const size_t next = index + 1 < hits.size() ? hits[index + 1] : buf.size();
            const size_t end  = std::min({ next, hit + Window, buf.size() });
            if (at + 8 <= end)
                out[*id].push_back({ partition, std::vector<uint8_t>(buf.begin() + static_cast<std::ptrdiff_t>(at + 8), buf.begin() + static_cast<std::ptrdiff_t>(end)) });
        }
    }

    void CategoryMaterials(Buf buf, const Tags& tags, uint32_t release, const std::vector<int64_t>& categories, std::map<int64_t, std::set<uint64_t>>& out)
    {
        for (const int64_t category : categories)
        {
            if (release >= 2024)
            {
                // 在 2024 上量的；Revit 2025 的文件里没有这样的条目
                if (release != 2024)
                    continue;
                const auto id = Le64(category);
                std::vector<uint8_t> pattern(id.begin(), id.end());
                pattern.insert(pattern.end(), std::begin(CategoryFrame64), std::end(CategoryFrame64));
                Bytes::ForEach(buf, pattern, [&](size_t at)
                {
                    if (const auto material = U64At(buf, at + pattern.size()))
                        out[category].insert(*material);
                });
            }
            else if (tags.patternHelper && category >= INT32_MIN && category <= INT32_MAX)
            {
                const auto frame = Framed(*tags.patternHelper);
                const auto id    = Le32(static_cast<int32_t>(category));
                const uint8_t pattern[8] = { id[0], id[1], id[2], id[3], 0xff, 0xff, 0xff, 0xff };
                Bytes::ForEach(buf, pattern, [&](size_t at)
                {
                    const auto word = U32At(buf, at + 8);
                    if (!word || (*word != 1 && *word != 2) || U32At(buf, at + 12) != uint32_t(1) || !Bytes::Matches(buf, at + 16, frame)
                        || !Bytes::Matches(buf, at + 30, CategoryMark32))
                        return;
                    if (const auto material = U32At(buf, at + 34))
                        out[category].insert(*material);
                });
            }
        }
    }

    void ColumnNodes(Buf buf, const std::map<uint32_t, Box>& targets, std::map<uint32_t, std::vector<Node>>& out)
    {
        if (targets.empty())
            return;
        Bytes::ForEach(buf, NodeAfterId, [&](size_t at)
        {
            if (at < 8 || at - 8 < 96)
                return;
            const size_t idAt = at - 8;
            const auto id = SmallId(U64At(buf, idAt));
            if (!id || !targets.contains(*id))
                return;
            const auto first = NodeBox(buf, idAt - 96), second = NodeBox(buf, idAt - 48);
            if (first && second)
                out[*id].emplace_back(*first, *second);
        });
    }

    std::map<uint32_t, double> ColumnBottoms(const std::map<uint32_t, std::vector<Node>>& nodes, const std::map<uint32_t, Box>& targets)
    {
        std::map<uint32_t, double> out;
        for (const auto& [id, list] : nodes)
        {
            const auto record = targets.find(id);
            if (record == targets.end())
                continue;
            std::vector<double> bottoms;
            for (const auto& [first, second] : list)
                if (PlanEqual(first, second, 1e-9) && PlanEqual(second, record->second, PlanTolerance) && std::abs(second[5] - record->second[5]) < NodeTolerance
                    && std::abs(second[2] - record->second[2]) > NodeTolerance)
                    bottoms.push_back(second[2]);
            if (bottoms.empty())
                continue;
            const double first = bottoms.front();
            if (std::all_of(bottoms.begin(), bottoms.end(), [&](double b) { return std::abs(b - first) < NodeTolerance; }))
                out.emplace(id, first);
        }
        return out;
    }

    void RoomEntries(Buf buf, bool wide, const std::set<uint32_t>& rooms, uint32_t partition, Names::Latest<std::pair<std::string, std::string>>& out)
    {
        if (rooms.empty())
            return;
        std::vector<uint8_t> number, name;
        if (wide)
        {
            const auto a = Le64(RoomNumber), b = Le64(RoomName);
            number.assign(a.begin(), a.end());
            name.assign(b.begin(), b.end());
        }
        else
        {
            const auto a = Le32(static_cast<int32_t>(RoomNumber)), b = Le32(static_cast<int32_t>(RoomName));
            number.assign(a.begin(), a.end());
            name.assign(b.begin(), b.end());
        }
        const size_t idLen = wide ? 8 : 4;
        Bytes::ForEach(buf, One, [&](size_t hit)
        {
            const auto id = wide ? SmallId(U64At(buf, hit + 4)) : U32At(buf, hit + 4);
            if (!id || !rooms.contains(*id))
                return;
            const size_t start = hit + 4 + idLen;
            if (start > buf.size())
                return;
            const Buf window = buf.subspan(start, std::min(RoomWindow, buf.size() - start));
            for (size_t at = Bytes::Find(window, number); at != Bytes::npos; at = Bytes::Find(window, number, at + number.size()))
            {
                const auto n = Counted(buf, start + at + number.size(), 64);
                if (!n || !Bytes::Matches(buf, n->second, name))
                    continue;
                auto m = Counted(buf, n->second + name.size(), 256);
                if (!m)
                    continue;
                out.Offer(*id, partition, { n->first, std::move(m->first) });
                break;
            }
        });
    }

    void MaterialNames(Buf buf, const Tags& tags, bool wide, const std::vector<uint32_t>& ids, std::map<uint32_t, std::optional<std::string>>& out)
    {
        if (!tags.material)
            return;
        const uint16_t cls = *tags.material;
        const uint8_t classBytes[2] = { static_cast<uint8_t>(cls), static_cast<uint8_t>(cls >> 8) };
        const auto note = [&](uint32_t id, const std::optional<std::u32string>& found)
        {
            if (!found)
                return;
            const std::string name = Bytes::Utf8(*found);
            const auto it = out.find(id);
            if (it == out.end())
                out.emplace(id, name);
            else if (it->second != name)
                it->second.reset();
        };
        const auto object = [&](size_t idAt) { return buf.subspan(idAt, std::min(NameWindow, buf.size() - idAt)); };
        if (wide)
        {
            Bytes::ForEach(buf, One, [&](size_t at)
            {
                const size_t idAt = at + 4;
                const auto id = SmallId(U64At(buf, idAt));
                if (!id || !Records::Declared(ids, *id) || !Bytes::Matches(buf, idAt + TagAfterId64, classBytes))
                    return;
                note(*id, MaterialName(object(idAt), TagAfterId64, tags, true));
            });
        }
        else
        {
            // `01 00 00 00`、id、……、`00 00 00 ff ff ff ff` 与类标签
            static constexpr uint8_t prefix[7] = { 0, 0, 0, 0xff, 0xff, 0xff, 0xff };
            Bytes::ForEach(buf, classBytes, [&](size_t tagAt)
            {
                if (tagAt < TagAfterId32 + 4 || tagAt < sizeof prefix)
                    return;
                const size_t idAt = tagAt - TagAfterId32;
                if (!Bytes::Matches(buf, idAt - 4, One) || !Bytes::Matches(buf, tagAt - sizeof prefix, prefix))
                    return;
                const auto id = U32At(buf, idAt);
                if (!id || !Records::Declared(ids, *id))
                    return;
                note(*id, MaterialName(object(idAt), TagAfterId32, tags, false));
            });
        }
    }
}
