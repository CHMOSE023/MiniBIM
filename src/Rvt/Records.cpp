// 移植自 BIMCraft crates/rvt/src/records.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_element_records.rs,
// src/partition_element_records_2023.rs and src/partition_type_records.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Records.h).
#include "Rvt/Records.h"
#include "Rvt/Bytes.h"
#include "Rvt/Schema.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace MiniBIM::Rvt
{
    namespace
    {
        using Bytes::F64At;
        using Bytes::U16At;
        using Bytes::U32At;
        using Bytes::U64At;
        using Buf = std::span<const uint8_t>;

        // BuiltInCategory 落在的范围
        constexpr int64_t CategoryMin = -2'100'000;
        constexpr int64_t CategoryMax = -1'990'000;

        constexpr size_t ClassAt    = 0x4a;
        constexpr size_t BboxAt     = 0x58;
        constexpr size_t FrameLen   = BboxAt + 48;
        constexpr size_t ListAt     = FrameLen;
        // 类型记录的序言常数在 +0x0c，格子列表在 +0x56
        constexpr size_t PrologueAt  = 0x0c;
        constexpr size_t TypeSlotsAt = 0x56;
        // 一个引用列表最多几格
        constexpr size_t MaxReferences = 1024;
        constexpr size_t ChainHeader  = 16;
        constexpr size_t ChainTrailer = 16;
        // 2023 及以前：从标记往回的偏移
        constexpr size_t IdBeforeMarker        = 52;
        constexpr size_t ContainerBeforeMarker = 22;
        constexpr size_t KindBeforeMarker      = 14;
        constexpr size_t ClassBeforeMarker     = 6;

        std::optional<std::array<double, 6>> FiniteBox(Buf buf, size_t at)
        {
            std::array<double, 6> b{};
            for (size_t i = 0; i < 6; ++i)
            {
                const auto x = F64At(buf, at + i * 8);
                if (!x || !std::isfinite(*x) || std::abs(*x) > Records::MaxFeet)
                    return std::nullopt;
                b[i] = *x;
            }
            for (size_t a = 0; a < 3; ++a)
                if (!(b[a + 3] >= b[a]))
                    return std::nullopt;
            return b;
        }

        // at 处带计数的 u64 格子列表（wide 为 false 时 u32）与结束的位置。计数超过上限或列表越出缓冲时为空；计数为 0 只在
        // 32 位形式里读作空列表（2024 起计数为 0 读作没有列表，后面也就没有第二个）
        bool ListAt32or64(Buf buf, size_t at, bool wide, std::vector<uint64_t>& out, size_t& end)
        {
            const auto count32 = U32At(buf, at);
            if (!count32)
                return false;
            const size_t count = *count32;
            if (count > MaxReferences || (wide && count == 0))
                return false;
            const size_t width = wide ? 8 : 4, start = at + 4;
            if (start > buf.size() || (buf.size() - start) / width < count)
                return false;
            out.clear();
            for (size_t i = 0; i < count; ++i)
                out.push_back(wide ? Bytes::U64(buf.data() + start + i * 8) : Bytes::U32(buf.data() + start + i * 4));
            end = start + count * width;
            return true;
        }

        void Lists(Buf buf, size_t at, bool wide, std::vector<uint64_t>& first, std::vector<uint64_t>& second)
        {
            first.clear();
            second.clear();
            size_t end = 0, ignored = 0;
            // 失败的读法不动 out，所以第二个读不出时仍是空的
            if (ListAt32or64(buf, at, wide, first, end))
                ListAt32or64(buf, end, wide, second, ignored);
        }
    }

    bool Records::InBand(int64_t category) { return category >= CategoryMin && category <= CategoryMax; }

    bool Records::Declared(const std::vector<uint32_t>& ids, uint32_t id) { return id != 0 && std::binary_search(ids.begin(), ids.end(), id); }

    bool Tags::Of(const Schema& schema, Tags& out, Error& error)
    {
        Tags t;
        for (const auto& [name, slot] : { std::pair{ "Outline", &t.outline }, std::pair{ "ElementParents", &t.parents }, std::pair{ "ElementHeader", &t.header },
                                           std::pair{ "Plane", &t.plane } })
        {
            const auto tag = schema.Tag(name);
            if (!tag)
            {
                error = Error{ ErrorKind::Format, std::string("Formats/Latest has no class ") + name + ", so the element records cannot be found" };
                return false;
            }
            *slot = *tag;
        }
        t.cellList         = schema.Tag("CellList").value_or(0);
        t.verticalRegions  = schema.Tag("VerticalRegionsStructure");
        t.material         = schema.Tag("Material");
        t.physicalParamSet = schema.Tag("PhysicalParamSet");
        t.patternHelper    = schema.Tag("PatternHelper");
        t.profileRoof      = schema.Tag("ProfileRoof");
        t.roof             = schema.Tag("Roof");
        out = t;
        return true;
    }

    std::array<uint8_t, 8> Tags::Marker() const
    {
        return { static_cast<uint8_t>(outline), static_cast<uint8_t>(outline >> 8), 0xff, 0xff, 0xff, 0xff, static_cast<uint8_t>(parents), static_cast<uint8_t>(parents >> 8) };
    }

    std::array<uint8_t, 6> Tags::ElevationMarker() const { return { 5, 0, 0, 0, static_cast<uint8_t>(plane), static_cast<uint8_t>(plane >> 8) }; }

    std::array<uint8_t, 10> Tags::DataHeader() const { return { 0xff, 0xff, 0xff, 0xff, static_cast<uint8_t>(cellList), static_cast<uint8_t>(cellList >> 8), 1, 0, 0, 0 }; }

    std::optional<uint32_t> Record::Preceding() const
    {
        for (size_t i = refs.size(); i-- > 0;)
            if (refs[i] == id)
            {
                if (i == 0)
                    return std::nullopt;
                const uint64_t v = refs[i - 1];
                if (v > 0xffff'ffffull || v == 0)
                    return std::nullopt;
                return static_cast<uint32_t>(v);
            }
        return std::nullopt;
    }

    std::array<double, 3> Record::Extent() const { return { bbox[3] - bbox[0], bbox[4] - bbox[1], bbox[5] - bbox[2] }; }

    std::vector<Records::Link> Records::Chain(Buf buf, const std::array<uint8_t, 8>& marker)
    {
        const uint16_t constant = static_cast<uint16_t>((marker[6] | marker[7] << 8) - 12);
        std::vector<Link> out;
        size_t at = 0;
        while (const auto id = U64At(buf, at))
        {
            const auto size32 = U32At(buf, at + 8);
            if (!size32)
                break;
            const size_t size = *size32;
            if (size < ChainHeader || U16At(buf, at + 0x0c) != constant)
                break;
            const size_t end = at + size;
            const auto flags = U32At(buf, end + 8);
            const auto echo  = U32At(buf, end + 12);
            if (!flags || (*flags != 0 && *flags != 0x0100'0000) || echo != size32)
                break;
            out.push_back({ at, end, *id });
            at = end + ChainTrailer;
        }
        return out;
    }

    const Records::Link* Records::Enclosing(const std::vector<Link>& chain, size_t at)
    {
        const auto it = std::partition_point(chain.begin(), chain.end(), [&](const Link& s) { return s.start <= at; });
        if (it == chain.begin())
            return nullptr;
        const Link& span = *(it - 1);
        return at < span.end ? &span : nullptr;
    }

    void Records::Records64(Buf buf, const Tags& tags, const std::vector<uint32_t>& ids, uint32_t partition, const KeepRecord& keep)
    {
        const auto marker = tags.Marker();
        const auto spans  = Chain(buf, marker);
        Bytes::ForEach(buf, marker, [&](size_t hit)
        {
            if (hit < MarkerAt)
                return;
            const size_t f = hit - MarkerAt;
            if (f + FrameLen > buf.size() || U16At(buf, f + 0x10) != uint16_t(0))
                return;
            const auto category = U64At(buf, f + CategoryAt);
            if (!category || !InBand(static_cast<int64_t>(*category)))
                return;
            const uint64_t container = U64At(buf, f + ContainerAt).value_or(0);
            const uint32_t kind      = static_cast<uint32_t>(U64At(buf, f + KindAt).value_or(0));
            const bool placed        = container == NoContainer && kind == KindPlaced;
            const uint64_t raw       = U64At(buf, f).value_or(0);
            const bool inside        = NoId(raw);
            uint32_t id = static_cast<uint32_t>(raw);
            if (inside)
            {
                // 第二序言：另一条记录里的放置的实例，用那条的 id
                const Link* host = placed ? Enclosing(spans, f) : nullptr;
                if (!host || host->id > 0xffff'ffffull)
                    return;
                id = static_cast<uint32_t>(host->id);
            }
            const auto bbox = FiniteBox(buf, f + BboxAt);
            if (!bbox || !Declared(ids, id))
                return;
            Record r;
            r.partition = partition;
            r.offset    = f;
            r.id        = id;
            r.category  = static_cast<int64_t>(*category);
            r.classTag  = U16At(buf, f + ClassAt).value_or(0);
            r.placed    = placed;
            r.bbox      = *bbox;
            Lists(buf, f + ListAt, true, r.refs, r.refs2);
            r.inside = inside;
            keep(std::move(r));
        });
    }

    void Records::Records32(Buf buf, const Tags& tags, const std::vector<uint32_t>& ids, uint32_t partition, const KeepRecord& keep)
    {
        const auto marker = tags.Marker();
        Bytes::ForEach(buf, marker, [&](size_t hit)
        {
            if (hit < IdBeforeMarker)
                return;
            const size_t idAt = hit - IdBeforeMarker;
            const auto id = U32At(buf, idAt);
            if (!id || !Declared(ids, *id) || U16At(buf, idAt + HeaderTagAfterId) != tags.header)
                return;
            const auto category = U64At(buf, hit - CategoryBeforeMarker);
            if (!category || !InBand(static_cast<int64_t>(*category)))
                return;
            const auto bbox = FiniteBox(buf, hit + 8);
            if (!bbox)
                return;
            const auto container = U32At(buf, hit - ContainerBeforeMarker);
            const auto kind      = U32At(buf, hit - KindBeforeMarker);
            Record r;
            r.partition = partition;
            r.offset    = idAt;
            r.id        = *id;
            r.category  = static_cast<int64_t>(*category);
            r.classTag  = U16At(buf, hit - ClassBeforeMarker).value_or(0);
            r.placed    = container == 0xffff'ffffu && kind == KindPlaced;
            r.bbox      = *bbox;
            Lists(buf, hit + 8 + 48, false, r.refs, r.refs2);
            keep(std::move(r));
        });
    }

    void Records::Types64(Buf buf, const Tags& tags, const std::vector<uint32_t>& ids, uint32_t partition, const KeepType& keep)
    {
        const auto marker = tags.Marker();
        const auto spans  = Chain(buf, marker);
        const uint32_t constant = static_cast<uint16_t>(tags.parents - 12);
        Bytes::ForEach(buf, Bytes::Span(marker.data() + 2, 6), [&](size_t hit)
        {
            if (hit < MarkerAt)
                return;
            const size_t f = hit - MarkerAt;
            if (U16At(buf, f + 0x10) != uint16_t(0))
                return;
            const auto category = U64At(buf, f + CategoryAt);
            if (!category || !InBand(static_cast<int64_t>(*category)))
                return;
            const uint32_t kind = static_cast<uint32_t>(U64At(buf, f + KindAt).value_or(0));
            const uint64_t raw  = U64At(buf, f).value_or(0);
            std::optional<uint32_t> id;
            if (NoId(raw))
            {
                if (const Link* s = Enclosing(spans, f); s && s->id <= 0xffff'ffffull)
                    id = static_cast<uint32_t>(s->id);
            }
            else if (U32At(buf, f + PrologueAt) == constant)
                id = static_cast<uint32_t>(raw);
            if (!id || !Declared(ids, *id))
                return;
            const size_t count = U32At(buf, f + TypeSlotsAt).value_or(0);
            if (count < 1 || count > MaxReferences)
                return;
            bool named = false;
            for (size_t i = 0; i < count; ++i)
            {
                const auto slot = U64At(buf, f + TypeSlotsAt + 4 + 8 * i);
                if (!slot)
                    return;
                named = named || *slot == *id;
            }
            if (named)
                keep(TypeRecord{ partition, *id, static_cast<int64_t>(*category), kind });
        });
    }

    void Records::Types32(Buf buf, const Tags& tags, const std::vector<uint32_t>& ids, uint32_t partition, const KeepType& keep)
    {
        const auto marker = tags.Marker();
        Bytes::ForEach(buf, Bytes::Span(marker.data() + 2, 6), [&](size_t hit)
        {
            if (hit < CategoryBeforeMarker + 14)
                return;
            const size_t categoryAt = hit - CategoryBeforeMarker, idAt = categoryAt - 14;
            const auto id = U32At(buf, idAt);
            if (!id || !Declared(ids, *id) || U16At(buf, idAt + HeaderTagAfterId) != tags.header)
                return;
            const auto category = U64At(buf, categoryAt);
            if (!category || !InBand(static_cast<int64_t>(*category)))
                return;
            const auto container = U32At(buf, categoryAt + 16);
            const auto kind      = U32At(buf, categoryAt + 24);
            if (!kind)
                return;
            if (container == 0xffff'ffffu)
                keep(TypeRecord{ partition, *id, static_cast<int64_t>(*category), *kind });
        });
    }
}
