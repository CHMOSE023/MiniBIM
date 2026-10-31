// 移植自 BIMCraft crates/rvt/src/inventory.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_element_records.rs,
// src/partition_element_records_2023.rs and src/partition_level_records.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Inventory.h).
#include "Rvt/Inventory.h"
#include "Rvt/Bytes.h"
#include "Rvt/Container.h"
#include "Rvt/ElemTable.h"
#include "Rvt/Info.h"
#include "Rvt/Names.h"
#include "Rvt/Schema.h"
#include "Rvt/Types.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace MiniBIM::Rvt
{
    namespace
    {
        using Buf = std::span<const uint8_t>;
        using Bytes::U16At;
        using Bytes::U32At;
        using Bytes::U64At;

        // Revit 2015 到 2018 写在标高上的种类
        constexpr uint32_t LevelKind2015 = 0xffff'ef6f;
        // 标高的名字块
        constexpr size_t Run64            = 56;
        constexpr size_t Run32            = 28;
        constexpr size_t MaxNameChars     = 128;
        constexpr size_t ElevationSearch  = 2048;
        constexpr size_t ElevationAfterMarker = 55;
        constexpr size_t ConfirmLeadIn    = 24;
        constexpr double MaxElevationFeet = 1.0e7;
        // 在标高名字之后找"建筑楼层"设置时看多远，以及框住它的：一个未设的 id、`00 01`、一个数与标高自己的 id
        // （2024 起 u64，之前 u32），然后是设置
        constexpr size_t StoryWindow = 0x200;
        // 标高的名字从块的 u64 ElementId 起在哪开始，以及以 id 为键的对象开头的 `01 00 00 00`
        constexpr size_t  NameAfterOwner  = 0x47;
        constexpr uint8_t ObjectPrefix[4] = { 1, 0, 0, 0 };

        // 对象样式里的材质可能给层用的类别：墙、楼板、屋顶与天花板
        const std::vector<int64_t> CategoryMaterialCategories = { -2'000'011, -2'000'032, -2'000'035, -2'000'038 };

        // 按 id 排
        constexpr std::pair<int64_t, const char*> CategoryNames[] = {
            { -2'009'030, "Structural Connections" },
            { -2'009'000, "Structural Rebar" },
            { -2'008'132, "Conduits" },
            { -2'008'130, "Cable Trays" },
            { -2'008'128, "Conduit Fittings" },
            { -2'008'126, "Cable Tray Fittings" },
            { -2'008'099, "Sprinklers" },
            { -2'008'087, "Lighting Devices" },
            { -2'008'049, "Pipe Fittings" },
            { -2'008'044, "Pipes" },
            { -2'008'020, "Flex Ducts" },
            { -2'008'013, "Air Terminals" },
            { -2'008'010, "Duct Fittings" },
            { -2'008'000, "Ducts" },
            { -2'006'114, "Design Options" },
            { -2'006'112, "Design Option Sets" },
            { -2'003'400, "Mass" },
            { -2'003'200, "Areas" },
            { -2'001'392, "Floors: Slab Edges" },
            { -2'001'370, "Entourage" },
            { -2'001'360, "Planting" },
            { -2'001'354, "Structural Stiffeners" },
            { -2'001'350, "Specialty Equipment" },
            { -2'001'336, "Structural Trusses" },
            { -2'001'330, "Structural Columns" },
            { -2'001'327, "Structural Beam Systems" },
            { -2'001'320, "Structural Framing" },
            { -2'001'300, "Structural Foundations" },
            { -2'001'263, "Site: Pads" },
            { -2'001'180, "Parking" },
            { -2'001'160, "Plumbing Fixtures" },
            { -2'001'140, "Mechanical Equipment" },
            { -2'001'120, "Lighting Fixtures" },
            { -2'001'100, "Furniture Systems" },
            { -2'001'060, "Electrical Fixtures" },
            { -2'001'040, "Electrical Equipment" },
            { -2'001'000, "Casework" },
            { -2'000'997, "Walls: Rectangular Straight Wall Opening" },
            { -2'000'996, "Shaft Openings" },
            { -2'000'920, "Stairs: Landings" },
            { -2'000'919, "Stairs: Runs" },
            { -2'000'267, "Assemblies" },
            { -2'000'240, "Levels" },
            { -2'000'182, "Walls: Reveals" },
            { -2'000'181, "Walls: Wall Sweeps" },
            { -2'000'171, "Curtain Wall Mullions" },
            { -2'000'170, "Curtain Panels" },
            { -2'000'160, "Rooms" },
            { -2'000'151, "Generic Models" },
            { -2'000'126, "Railings" },
            { -2'000'123, "Stairs: Supports" },
            { -2'000'120, "Stairs" },
            { -2'000'112, "Phases" },
            { -2'000'100, "Columns" },
            { -2'000'080, "Furniture" },
            { -2'000'051, "Lines" },
            { -2'000'045, "Lines: <Sketch>" },
            { -2'000'038, "Ceilings" },
            { -2'000'035, "Roofs" },
            { -2'000'032, "Floors" },
            { -2'000'023, "Doors" },
            { -2'000'014, "Windows" },
            { -2'000'011, "Walls" },
        };

        struct Block
        {
            uint32_t            id = 0;
            std::string         name;
            double              elevationFeet = 0;
            std::optional<bool> buildingStory;
        };

        // 扫一个分区找到的
        struct Found
        {
            size_t                                                records = 0;
            std::map<std::pair<int64_t, uint32_t>, std::pair<uint16_t, bool>> elements;
            std::map<uint32_t, bool>                              levelRecords;
            std::vector<Block>                                    blocks;
            std::set<uint32_t>                                    owned;   // 声明过的、有在别的元素里的标高的块的 id

            void AddElement(int64_t category, uint32_t id, uint16_t classTag, bool placed)
            {
                ++records;
                auto& e = elements.try_emplace({ category, id }, classTag, false).first->second;
                e.second = e.second || placed;
            }

            void AddLevel(uint32_t id, bool placed)
            {
                bool& held = levelRecords.try_emplace(id, false).first->second;
                held = held || placed;
            }
        };

        std::array<uint8_t, 8> Le64(int64_t v)
        {
            std::array<uint8_t, 8> b{};
            for (size_t i = 0; i < 8; ++i)
                b[i] = static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i));
            return b;
        }

        bool IsPlacedLevel(uint32_t kind, uint32_t release) { return kind == Records::KindPlaced || (release < 2019 && kind == LevelKind2015); }

        // 2024 起分区里的标高记录：靠类别找，标记的后六个字节在 +0x50
        void Levels64(Buf buf, const Tags& tags, uint32_t release, const std::vector<uint32_t>& ids, Found& found)
        {
            const auto marker = tags.Marker();
            const auto spans  = Records::Chain(buf, marker);
            const Buf tail(marker.data() + 2, 6);
            Bytes::ForEach(buf, Le64(OstLevels), [&](size_t hit)
            {
                if (hit < Records::CategoryAt)
                    return;
                const size_t f = hit - Records::CategoryAt;
                if (f + Records::MarkerAt + 8 > buf.size() || U16At(buf, f + 0x10) != uint16_t(0) || !Bytes::Matches(buf, f + Records::MarkerAt, tail))
                    return;
                const uint64_t raw = U64At(buf, f).value_or(0);
                std::optional<uint32_t> id;
                if (Records::NoId(raw))
                {
                    if (const auto* s = Records::Enclosing(spans, f); s && s->id <= 0xffff'ffffull)
                        id = static_cast<uint32_t>(s->id);
                }
                else
                    id = static_cast<uint32_t>(raw);
                if (!id || !Records::Declared(ids, *id))
                    return;
                const uint64_t container = U64At(buf, f + Records::ContainerAt).value_or(0);
                const uint32_t kind      = static_cast<uint32_t>(U64At(buf, f + Records::KindAt).value_or(0));
                found.AddLevel(*id, container == Records::NoContainer && IsPlacedLevel(kind, release));
            });
        }

        // 2023 及以前分区里的标高记录：id 在类别前 14 字节，标记在它后面 38 字节
        void Levels32(Buf buf, const Tags& tags, uint32_t release, const std::vector<uint32_t>& ids, Found& found)
        {
            const auto marker = tags.Marker();
            const Buf tail(marker.data() + 2, 6);
            Bytes::ForEach(buf, Le64(OstLevels), [&](size_t hit)
            {
                if (hit < 14)
                    return;
                const size_t idAt = hit - 14;
                const auto id = U32At(buf, idAt);
                if (!id || !Records::Declared(ids, *id))
                    return;
                if (U16At(buf, idAt + Records::HeaderTagAfterId) != tags.header || !Bytes::Matches(buf, hit + 38, tail))
                    return;
                const auto container = U32At(buf, hit + 16);
                const uint32_t kind  = U32At(buf, hit + 24).value_or(0);
                found.AddLevel(*id, container == 0xffff'ffffu && IsPlacedLevel(kind, release));
            });
        }

        // 标高的"建筑楼层"设置：名字之后第一个未设的 id 与 `00 01`、一个数（多数标高是 0）与标高自己的 id 之后的那个字节，
        // 1 是建筑楼层，0 不是。找不到框或那个字节两者都不是时为空
        std::optional<bool> BuildingStoryAfter(Buf buf, size_t nameEnd, bool wide)
        {
            std::vector<uint8_t> anchor(wide ? 8 : 4, 0xff);
            anchor.insert(anchor.end(), { 0, 1 });
            if (nameEnd > buf.size())
                return std::nullopt;
            const Buf window = buf.subspan(nameEnd, std::min(StoryWindow, buf.size() - nameEnd));
            const size_t found = Bytes::Find(window, anchor);
            if (found == Bytes::npos)
                return std::nullopt;
            const size_t idAt = nameEnd + found + anchor.size() + 8;
            size_t setting = idAt + 4;
            if (wide)
            {
                const auto id = U64At(buf, idAt);
                if (!id || *id > 0xffff'ffffull)
                    return std::nullopt;
                setting = idAt + 8;
            }
            if (setting >= buf.size())
                return std::nullopt;
            if (buf[setting] == 0)
                return false;
            if (buf[setting] == 1)
                return true;
            return std::nullopt;
        }

        std::optional<Block> BlockAt(Buf buf, const Tags& tags, const std::vector<uint32_t>& ids, size_t start, size_t idLen, size_t runLen)
        {
            if (start < idLen)
                return std::nullopt;
            const size_t ownerAt = start - idLen;
            std::optional<uint64_t> owner;
            if (idLen == 4)
            {
                if (const auto v = U32At(buf, ownerAt))
                    owner = *v;
            }
            else
                owner = U64At(buf, ownerAt);
            if (!owner || *owner > 0xffff'ffffull || !Records::Declared(ids, static_cast<uint32_t>(*owner)))
                return std::nullopt;
            const uint32_t id = static_cast<uint32_t>(*owner);
            // 那串 ff 到这里结束，后面跟三个字节的填充（第一个是 0 或 1）
            const size_t after = start + runLen;
            if (after > buf.size() || buf.size() - after < 3)
                return std::nullopt;
            if (buf[after] > 1 || buf[after + 1] != 0 || buf[after + 2] != 0)
                return std::nullopt;
            const auto chars = U32At(buf, after + 3);
            if (!chars || *chars == 0 || *chars > MaxNameChars)
                return std::nullopt;
            const size_t value = after + 7, end = value + 2 * static_cast<size_t>(*chars);
            const auto name = Bytes::CodePointsAt(buf, value, *chars);
            if (!name || std::any_of(name->begin(), name->end(), [](char32_t c) { return Bytes::IsControl(c); }))
                return std::nullopt;
            const auto marker = tags.ElevationMarker();
            const Buf window  = buf.subspan(end, std::min(ElevationSearch, buf.size() - end));
            const size_t found = Bytes::Find(window, marker);
            if (found == Bytes::npos)
                return std::nullopt;
            const size_t at = end + found + ElevationAfterMarker;
            const auto elevation = Bytes::F64At(buf, at);
            if (!elevation || !std::isfinite(*elevation) || std::abs(*elevation) > MaxElevationFeet || at < ConfirmLeadIn)
                return std::nullopt;
            const Buf copy  = buf.subspan(at - ConfirmLeadIn, ConfirmLeadIn + 8);
            const Buf later = buf.subspan(at + 8, std::min(ElevationSearch, buf.size() - (at + 8)));
            if (Bytes::Find(later, copy) == Bytes::npos)
                return std::nullopt;
            return Block{ id, Bytes::Utf8(*name), *elevation, BuildingStoryAfter(buf, end, idLen == 8) };
        }

        // 名字与高程的块：所有者的 id、一串 ff、三个字节的填充、名字的长度与它的 UTF-16，然后是高程标记，再往后
        // ElevationAfterMarker 字节是高程（英尺），它与前面 24 个字节一起再出现一次
        void Blocks(Buf buf, const Tags& tags, const std::vector<uint32_t>& ids, bool wideIds, Found& found)
        {
            const size_t idLen = wideIds ? 8 : 4, runLen = wideIds ? Run64 : Run32;
            const std::vector<uint8_t> run(runLen, 0xff);
            size_t index = 0;
            for (size_t at = Bytes::Find(buf, run, index); at != Bytes::npos; at = Bytes::Find(buf, run, index))
            {
                size_t start = at;
                while (start > index && buf[start - 1] == 0xff)
                    --start;
                index = start;
                while (index < buf.size() && buf[index] == 0xff)
                    ++index;
                if (auto block = BlockAt(buf, tags, ids, start, idLen, runLen))
                    found.blocks.push_back(std::move(*block));
            }
        }

        // 在别的元素里的标高：以 id 为键的对象（`01 00 00 00` 与 u64），项目标高的块那串 56 个 ff 的地方，它的块在八个 ff
        // 之后是所有者的 id
        void Owned64(Buf buf, const std::vector<uint32_t>& ids, Found& found)
        {
            static constexpr uint8_t ones[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
            Bytes::ForEach(buf, ObjectPrefix, [&](size_t at)
            {
                const size_t idAt = at + sizeof ObjectPrefix;
                const auto raw = U64At(buf, idAt);
                if (!raw || *raw > 0xffff'ffffull || !Records::Declared(ids, static_cast<uint32_t>(*raw)))
                    return;
                const size_t chars = U32At(buf, idAt + NameAfterOwner - 4).value_or(0);
                const auto owner   = U64At(buf, idAt + 16);
                if (chars >= 1 && chars <= MaxNameChars && Bytes::Matches(buf, idAt + 8, ones) && owner && *owner != 0 && *owner < 0xffff'ffffull)
                    found.owned.insert(static_cast<uint32_t>(*raw));
            });
        }

        // Partitions/NN 的 NN（Rust 的 str::parse::<u32>，不是数时为 0）
        uint32_t PartitionNumber(const std::string& name)
        {
            std::string_view n(name);
            n.remove_prefix(n.rfind('/') + 1);
            if (!n.empty() && n[0] == '+')
                n.remove_prefix(1);
            if (n.empty())
                return 0;
            uint64_t v = 0;
            for (const char c : n)
            {
                if (c < '0' || c > '9')
                    return 0;
                v = v * 10 + static_cast<uint64_t>(c - '0');
                if (v > 0xffff'ffffull)
                    return 0;
            }
            return static_cast<uint32_t>(v);
        }

        std::vector<std::string> Partitions(const Revit& r)
        {
            std::vector<std::string> out;
            for (std::string& name : r.StreamNames())
                if (name.starts_with("Partitions/"))
                    out.push_back(std::move(name));
            return out;
        }
    }

    std::vector<Count> Inventory::Counts() const
    {
        std::map<int64_t, std::pair<size_t, size_t>> by;
        for (const Element& e : elements)
        {
            auto& c = by[e.category];
            ++c.first;
            c.second += e.placed ? 1 : 0;
        }
        std::vector<Count> out;
        for (const auto& [category, c] : by)
            out.push_back({ category, c.first, c.second });
        return out;
    }

    const char* CategoryName(int64_t id)
    {
        const auto it = std::lower_bound(std::begin(CategoryNames), std::end(CategoryNames), id, [](const auto& known, int64_t v) { return known.first < v; });
        return it != std::end(CategoryNames) && it->first == id ? it->second : nullptr;
    }

    bool ReadInventory(Revit& r, Inventory& out, Error& error)
    {
        Surveyed s;
        if (!Survey(r, [](int64_t) { return false; }, s, error))
            return false;
        out = std::move(s.inventory);
        return true;
    }

    bool Survey(Revit& r, const std::function<bool(int64_t)>& keep, Surveyed& out, Error& error)
    {
        BasicFileInfo info;
        if (!ReadBasicFileInfo(r, info, error))
            return false;
        const uint32_t release = info.release;
        if (release < FirstRelease || release > LastRelease)
        {
            error = Error{ ErrorKind::Format, "Revit " + std::to_string(release) + ": this reader knows the element records of releases " + std::to_string(FirstRelease) + " to "
                                                  + std::to_string(LastRelease) + " and does not guess at others" };
            return false;
        }
        Schema schema;
        Tags tags;
        std::vector<uint32_t> ids;
        if (!ReadSchema(r, schema, error) || !Tags::Of(schema, tags, error) || !ReadElementIds(r, ids, error))
            return false;
        const std::vector<std::string> names = Partitions(r);
        if (names.empty())
        {
            error = Error{ ErrorKind::Format, "the file has no Partitions stream, so it holds no elements" };
            return false;
        }
        const bool wide = release >= 2024;
        std::map<std::pair<int64_t, uint32_t>, std::pair<uint16_t, bool>> elements;
        std::map<uint32_t, bool> levelRecords;
        std::map<uint32_t, std::pair<uint32_t, Block>> named;
        std::set<uint32_t> owned;
        size_t records = 0;
        std::vector<Record> kept;
        std::vector<TypeRecord> types;
        for (const std::string& name : names)
        {
            // 一次一个分区：解开、读完、丢掉
            const uint32_t number = PartitionNumber(name);
            Gzip::Inflated inflated;
            if (!r.InflateAll(name, inflated, error))
                return false;
            const Buf buf = inflated.bytes;
            Found found;
            const auto seen = [&](Record rec)
            {
                found.AddElement(rec.category, rec.id, rec.classTag, rec.placed);
                if (keep(rec.category))
                    kept.push_back(std::move(rec));
            };
            const auto type = [&](const TypeRecord& t) { types.push_back(t); };
            if (wide)
            {
                Records::Records64(buf, tags, ids, number, seen);
                Records::Types64(buf, tags, ids, number, type);
                Levels64(buf, tags, release, ids, found);
                Owned64(buf, ids, found);
            }
            else
            {
                Records::Records32(buf, tags, ids, number, seen);
                Records::Types32(buf, tags, ids, number, type);
                Levels32(buf, tags, release, ids, found);
            }
            Blocks(buf, tags, ids, wide, found);
            records += found.records;
            owned.insert(found.owned.begin(), found.owned.end());
            for (const auto& [key, value] : found.elements)
            {
                auto& e = elements.try_emplace(key, value.first, false).first->second;
                e.second = e.second || value.second;
            }
            for (const auto& [id, placed] : found.levelRecords)
            {
                bool& held = levelRecords.try_emplace(id, false).first->second;
                held = held || placed;
            }
            // 前一次保存后改过的标高在后面的分区里再写一份：最新的块才算
            for (Block& block : found.blocks)
            {
                const auto it = named.find(block.id);
                if (it != named.end() && it->second.first > number)
                    continue;
                const uint32_t id = block.id;
                named.insert_or_assign(id, std::pair{ number, std::move(block) });
            }
        }
        // 在别的元素里的标高不论块怎么读都不是项目的标高
        std::erase_if(owned, [&](uint32_t id) { return !levelRecords.contains(id); });
        Inventory inventory;
        size_t placedLevels = 0;
        for (const auto& [id, placed] : levelRecords)
        {
            if (!placed || owned.contains(id))
                continue;
            ++placedLevels;
            if (const auto it = named.find(id); it != named.end())
            {
                const Block& b = it->second.second;
                inventory.levels.push_back({ b.id, b.name, b.elevationFeet, b.buildingStory });
            }
        }
        std::stable_sort(inventory.levels.begin(), inventory.levels.end(), [](const Level& a, const Level& b)
        {
            if (Bytes::TotalLess(a.elevationFeet, b.elevationFeet))
                return true;
            if (Bytes::TotalLess(b.elevationFeet, a.elevationFeet))
                return false;
            return a.id < b.id;
        });
        inventory.unreadLevels = placedLevels - inventory.levels.size();
        for (const auto& [id, placed] : levelRecords)
        {
            auto& e = elements.try_emplace({ OstLevels, id }, uint16_t(0), false).first->second;
            e.second = e.second || placed;
        }
        for (const auto& [key, value] : elements)
            inventory.elements.push_back({ key.first, key.second, value.first, value.second });
        inventory.declared    = ids.size();
        inventory.records     = records;
        inventory.ownedLevels.assign(owned.begin(), owned.end());
        out.release   = release;
        out.tags      = tags;
        out.ids       = std::move(ids);
        out.inventory = std::move(inventory);
        out.records   = std::move(kept);
        out.types     = std::move(types);
        return true;
    }

    bool Gather(Revit& r, uint32_t release, const Tags& tags, const std::vector<uint32_t>& ids, const Wants& wants, Gathered& out, Error& error)
    {
        const bool wide = release >= 2024;
        // 到 2023 版，对象没给名字的材质在自己的数据里给自己起名字
        std::set<uint32_t> wanted = wants.data;
        if (!wide)
            wanted.insert(wants.materials.begin(), wants.materials.end());
        Data::Elements data;
        std::map<uint32_t, std::optional<std::string>> materials;
        std::map<int64_t, std::set<uint64_t>> styled;
        Names::Latest<std::pair<std::string, std::optional<int64_t>>> named;
        std::map<uint32_t, std::vector<Data::Node>> nodes;
        Names::Latest<std::pair<std::string, std::string>> roomNames;
        for (const std::string& name : Partitions(r))
        {
            const uint32_t number = PartitionNumber(name);
            Gzip::Inflated inflated;
            if (!r.InflateAll(name, inflated, error))
                return false;
            const Buf buf = inflated.bytes;
            Data::ElementData(buf, tags, wide, wanted, number, data);
            Data::MaterialNames(buf, tags, wide, ids, materials);
            Data::CategoryMaterials(buf, tags, release, CategoryMaterialCategories, styled);
            Data::ColumnNodes(buf, wants.columns, nodes);
            Data::RoomEntries(buf, wide, wants.rooms, number, roomNames);
            std::vector<Names::Entry> entries;
            if (wide)
                Names::Entries64(buf, ids, entries);
            else
                Names::Entries32(buf, ids, entries);
            for (Names::Entry& e : entries)
                named.Offer(e.id, number, { std::move(e.name), e.category });
        }
        if (!wide)
        {
            const auto header = tags.DataHeader();
            for (const uint32_t id : wants.materials)
            {
                if (const auto it = materials.find(id); it != materials.end() && it->second)
                    continue;
                const auto copies = data.find(id);
                if (copies == data.end())
                    continue;
                const auto found = Data::Newest<std::string>(copies->second, [&](std::span<const uint8_t> b) { return Types::FirstName(b, header, release); });
                if (found)
                    materials.insert_or_assign(id, found->first);
            }
        }
        // 两个材质会同名的名字至少有一个读错了
        std::map<std::string, size_t> uses;
        for (const auto& [id, name] : materials)
            if (name)
                ++uses[*name];
        Gathered g;
        for (const auto& [id, name] : materials)
            if (name && uses[*name] == 1)
                g.materials.emplace(id, *name);
        // 类别的材质：只写了一个值，且是有名字的材质
        for (const auto& [category, values] : styled)
        {
            if (values.size() != 1)
                continue;
            const uint64_t v = *values.begin();
            if (v == 0 || v >= 0xffff'ffffull || !g.materials.contains(static_cast<uint32_t>(v)))
                continue;
            g.category.emplace(category, static_cast<uint32_t>(v));
        }
        g.data          = std::move(data);
        g.names         = named.Values();
        g.columnBottoms = Data::ColumnBottoms(nodes, wants.columns);
        g.rooms         = roomNames.Values();
        out = std::move(g);
        return true;
    }
}
