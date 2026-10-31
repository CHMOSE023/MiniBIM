// 移植自 BIMCraft crates/rvt/src/project_fixture.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
#include "ProjectFixture.h"

#include "Rvt/Bytes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ProjectFixture
{
    namespace
    {
        constexpr uint32_t KindPlaced = 0xffff'ef7f;
        constexpr uint32_t KindSymbol = 0xffff'8000;
        constexpr int64_t  Walls      = -2'000'011;
        constexpr int64_t  Levels     = -2'000'240;

        void Add(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }
        void Fill(Bytes& out, uint8_t byte, size_t n) { out.insert(out.end(), n, byte); }
        void U16(Bytes& out, uint16_t v) { Add(out, Le(v, 2)); }
        void U32(Bytes& out, uint32_t v) { Add(out, Le(v, 4)); }
        void U64(Bytes& out, uint64_t v) { Add(out, Le(v, 8)); }
        void I64(Bytes& out, int64_t v) { Add(out, Le(static_cast<uint64_t>(v), 8)); }
        void I32(Bytes& out, int32_t v) { Add(out, Le(static_cast<uint32_t>(v), 4)); }
        void Put32(Bytes& out, size_t at, uint32_t v)
        {
            const Bytes b = Le(v, 4);
            std::copy(b.begin(), b.end(), out.begin() + static_cast<std::ptrdiff_t>(at));
        }

        Bytes Marker()
        {
            Bytes m(8, 0xff);
            m[0] = static_cast<uint8_t>(Outline);
            m[1] = 0;
            m[6] = static_cast<uint8_t>(Parents);
            m[7] = 0;
            return m;
        }

        Bytes MarkerTail()
        {
            const Bytes m = Marker();
            return Bytes(m.begin() + 2, m.end());
        }

        // 包围盒：六个 f64
        Bytes BoxBytes(const Box& b)
        {
            Bytes out;
            for (const double v : b)
                Add(out, F64(v));
            return out;
        }

        // 两个引用列表：计数与各格（u64，到 2023 版 u32）；第二个只跟在第一个后面
        Bytes Lists(const Rec& r, bool wide)
        {
            Bytes f;
            const auto list = [&](const std::vector<uint64_t>& l)
            {
                U32(f, static_cast<uint32_t>(l.size()));
                for (const uint64_t v : l)
                {
                    if (wide)
                        U64(f, v);
                    else
                        U32(f, static_cast<uint32_t>(v));
                }
            };
            list(r.refs);
            if (!r.refs.empty() || !wide)
                list(r.refs2);
            return f;
        }

        // 2024 起框架从 +0x10 到包围盒结束的字段，以及引用列表
        Bytes Frame64(uint64_t rawId, const Rec& r)
        {
            Bytes f;
            U64(f, rawId);
            U32(f, 0x8c);   // 记录的大小
            U16(f, static_cast<uint16_t>(Parents - 12));
            U16(f, 0);
            U16(f, 0);             // +0x10
            I64(f, r.category);    // +0x12
            Fill(f, 0xff, 16);     // +0x1a
            Fill(f, 0xff, 8);      // +0x2a 设计选项
            U64(f, ~0ull);         // +0x32 容器
            Fill(f, 0xff, 8);      // +0x3a
            U32(f, r.placed ? KindPlaced : KindSymbol);   // +0x42
            U32(f, 0);
            U16(f, r.classTag);    // +0x4a
            Fill(f, 0xff, 4);
            Add(f, Marker());      // +0x50
            Add(f, BoxBytes(r.bbox));   // +0x58
            Add(f, Lists(r, true));     // +0x88 引用列表
            return f;
        }

        // 链记录的尾巴：八个 0、标志 0 与重复的大小
        void Trailer(Bytes& f)
        {
            const uint32_t size = static_cast<uint32_t>(f.size());
            Put32(f, 8, size);
            Fill(f, 0, 8);
            U32(f, 0);
            U32(f, size);
        }

        // 一条链记录：一个框架（有 body 时框架后面再跟着别的），与重复大小的尾巴
        Bytes Record64(uint64_t id, const Rec& r, const Bytes& body)
        {
            Bytes f = Frame64(id, r);
            Add(f, body);
            Trailer(f);
            return f;
        }

        Bytes Level64(const Lev& l)
        {
            Rec r(l.id, Levels);
            r.placed = l.placed;
            Bytes f = Frame64(l.id, r);
            f.resize(0x50);
            Add(f, MarkerTail());   // 标记的六个字节，不带 Outline 的标签
            U16(f, 2);
            Fill(f, 0, 8);
            Trailer(f);
            return f;
        }

        // 到 2023 版的元素记录，从标记往回排
        Bytes Record32(const Rec& r)
        {
            Bytes f;
            U32(f, r.id);   // m-52
            U32(f, 112);
            U16(f, Header);   // m-44
            Fill(f, 0, 4);
            I64(f, r.category);   // m-38
            Fill(f, 0xff, 8);
            U32(f, 0xffff'ffffu);   // m-22 容器
            Fill(f, 0xff, 4);
            U32(f, r.placed ? KindPlaced : KindSymbol);   // m-14
            U32(f, 0);
            U16(f, r.classTag);   // m-6
            Fill(f, 0xff, 4);
            Add(f, Marker());   // m
            Add(f, BoxBytes(r.bbox));   // m+8
            Add(f, Lists(r, false));
            return f;
        }

        Bytes Level32(const Lev& l)
        {
            Bytes f;
            U32(f, l.id);   // id 在类别前 14 字节
            U32(f, 66);
            U16(f, Header);
            Fill(f, 0, 4);
            I64(f, Levels);   // 类别
            Fill(f, 0, 8);
            U32(f, 0xffff'ffffu);   // +16 容器
            Fill(f, 0xff, 4);
            U32(f, l.placed ? KindPlaced : KindSymbol);   // +24
            Fill(f, 0, 10);
            Add(f, MarkerTail());   // +38
            Fill(f, 0, 8);
            return f;
        }

        // 给标高起名字、给出高程的块（高程两次，第二次带着它前面的 24 个字节）
        Bytes NameBlock(const Lev& l, bool wide)
        {
            Bytes b(7, 0x11);
            if (wide && l.owner)
            {
                // 在别的元素里的标高的块：它的 id，然后在项目标高写一串 56 个 ff 的地方是八个 ff 与所有者的 id
                Add(b, { 1, 0, 0, 0 });
                U64(b, l.id);
                Fill(b, 0xff, 8);
                U64(b, *l.owner);
                Fill(b, 0xff, 40);
            }
            else if (wide)
            {
                U64(b, l.id);
                Fill(b, 0xff, 56);
            }
            else
            {
                U32(b, l.id);
                Fill(b, 0xff, 28);
            }
            Fill(b, 0, 3);
            U32(b, Units(l.name));
            Add(b, Utf16(l.name));
            if (l.story)
            {
                // 一个未设的 id、`00 01`、一个数、标高的 id 与设置
                Fill(b, 0xff, wide ? 8 : 4);
                Add(b, { 0, 1 });
                Add(b, F64(0.0));
                if (wide)
                    U64(b, l.id);
                else
                    U32(b, l.id);
                b.push_back(*l.story ? 1 : 0);
            }
            Fill(b, 0x22, 10);
            Add(b, { 5, 0, 0, 0 });
            U16(b, Plane);
            Bytes filler;
            for (uint8_t i = 0; i < 49; ++i)
                filler.push_back(static_cast<uint8_t>(0x30 + i));
            Add(b, filler);
            const Bytes feet = F64(l.feet);
            Add(b, feet);
            Fill(b, 0x55, 100);
            if (l.confirmed)
            {
                b.insert(b.end(), filler.begin() + 25, filler.end());
                Add(b, feet);
            }
            // 足够远，下一个块的设置不在这个块的搜索范围内
            Fill(b, 0x66, 0x300);
            return b;
        }

        // 2024 起没有包围盒的记录的框架，到它的格子列表（里面有类型自己）为止
        Bytes TypeFrame64(uint64_t rawId, const TypeRec& t)
        {
            Rec r(t.id, t.category);
            Bytes f = Frame64(rawId, r);
            f.resize(0x50);
            Put32(f, 0x42, t.kind);
            Add(f, MarkerTail());   // +0x50
            U32(f, 2);              // +0x56 格子列表：另一个元素的 id 与它自己的
            U64(f, 7);
            U64(f, t.id);
            return f;
        }

        // 2024 起没有包围盒的记录：标高那样的框架，带类型的种类与有它自己 id 的格子列表
        Bytes Type64(const TypeRec& t)
        {
            if (t.inside)
            {
                // 一条类型 id 的链记录，身体里是类型的框架，没有自己的 id
                Rec host(t.id, t.category);
                host.placed = false;
                Bytes body(10, 0x33);
                Add(body, TypeFrame64(0, t));
                return Record64(t.id, host, body);
            }
            Bytes f = TypeFrame64(t.id, t);
            Trailer(f);
            return f;
        }

        // 到 2023 版没有包围盒的记录：标高的框架
        Bytes Type32(const TypeRec& t)
        {
            Bytes f = Level32(Lev(t.id, "", 0.0));
            const Bytes category = Le(static_cast<uint64_t>(t.category), 8);
            std::copy(category.begin(), category.end(), f.begin() + 14);   // 类别跟在 id、大小、头的标签与四个字节之后
            Put32(f, 14 + 24, t.kind);
            return f;
        }

        // 元素的数据：`ff ff ff ff`、CellList 的标签、`01 00 00 00`、id（到 2023 版 u32 再四个字节）与内容
        Bytes DataBlock(const Block& b, bool wide)
        {
            Bytes f(5, 0x77);
            Add(f, { 0xff, 0xff, 0xff, 0xff });
            U16(f, CellList);
            Add(f, { 1, 0, 0, 0 });
            if (wide)
                U64(f, b.id);
            else
            {
                U32(f, b.id);
                Fill(f, 0x11, 4);
            }
            Add(f, b.payload);
            return f;
        }

        // 对象样式里类别的材质：2024 起（也只有 2024）是类别的 id、一个未设的 id、`01 00 00 00`、另一个未设的 id 与材质；
        // 之前是 i32 的 id、四个 ff、字 1、1、PatternHelper 的框、八个字节、`i32 -3000010` 与材质
        Bytes CategoryMaterial(int64_t category, uint32_t material, uint32_t release)
        {
            Bytes f(3, 0x14);
            if (release >= 2024)
            {
                I64(f, category);
                Fill(f, 0xff, 8);
                Add(f, { 1, 0, 0, 0 });
                Fill(f, 0xff, 8);
                U64(f, material);
            }
            else
            {
                I32(f, static_cast<int32_t>(category));
                Fill(f, 0xff, 4);
                U32(f, 1);
                U32(f, 1);
                Fill(f, 0xff, 4);
                U16(f, Pattern);
                Fill(f, 0x15, 8);
                I32(f, -3'000'010);
                U32(f, material);
            }
            Fill(f, 0x16, 3);
            return f;
        }

        // 材质对象：`01 00 00 00`、它的 id、固定距离处的类标签与它的名字（放在 style 说的地方）
        Bytes MaterialObject(const Mat& m, bool wide, NameStyle style)
        {
            Bytes f(3, 0x55);
            Add(f, { 1, 0, 0, 0 });
            const Bytes units = Utf16(m.name);
            const Bytes count = Le(Units(m.name), 4);
            Bytes name;
            switch (style)
            {
            case NameStyle::Own:
                Fill(name, 0, 8);
                Add(name, count);
                Add(name, units);
                Fill(name, 0x66, 40);
                break;
            case NameStyle::PatternFramed:
                Fill(name, 0x66, 16);
                Fill(name, 0xff, 4);
                U16(name, Pattern);
                Add(name, count);
                Add(name, units);
                Fill(name, 0x66, 40);
                break;
            case NameStyle::ParamEntry:
                Fill(name, 0x66, 16);
                if (wide)
                    I64(name, -1'001'203);
                else
                    I32(name, -1'001'203);
                Fill(name, 0, 4);
                Add(name, count);
                Add(name, units);
                Fill(name, 0x66, 40);
                break;
            case NameStyle::Closed:
                Fill(name, 0x66, 16);
                Add(name, count);
                Add(name, units);
                Fill(name, 0, 8);
                U64(name, m.id);
                Fill(name, 0xff, 8);
                Fill(name, 0x66, 40);
                break;
            }
            if (wide)
            {
                U64(f, m.id);
                Fill(f, 0x31, 0x47 - 8);
            }
            else
            {
                U32(f, m.id);
                Fill(f, 0x31, 0x27 - 4 - 7);
                Add(f, { 0, 0, 0, 0xff, 0xff, 0xff, 0xff });
            }
            U16(f, Material);
            Add(f, name);
            return f;
        }

        // 名字条目：2024 起是 id、带计数的名字与类别；之前是 `01 00 00 00`、id 与带计数的名字
        Bytes NameEntryBytes(const NameEntry& n, bool wide)
        {
            Bytes f(3, 0x42);
            if (wide)
            {
                U64(f, n.id);
                U32(f, Units(n.name));
                Add(f, Utf16(n.name));
                I64(f, n.category);
            }
            else
            {
                Add(f, { 1, 0, 0, 0 });
                U32(f, n.id);
                U32(f, Units(n.name));
                Add(f, Utf16(n.name));
            }
            Fill(f, 0x24, 3);
            return f;
        }

        // 房间的对象：`01 00 00 00`、它的 id、一些字节，然后是编号与名称两条参数
        Bytes RoomObject(uint32_t id, const std::string& number, const std::string& name, bool wide)
        {
            Bytes f(3, 0x62);
            Add(f, { 1, 0, 0, 0 });
            Bytes numberTag, nameTag;
            if (wide)
            {
                U64(f, id);
                I64(numberTag, -1'006'901);
                I64(nameTag, -1'006'900);
            }
            else
            {
                U32(f, id);
                I32(numberTag, -1'006'901);
                I32(nameTag, -1'006'900);
            }
            Fill(f, 0x71, 40);
            const auto text = [](const std::string& s)
            {
                Bytes t = Le(Units(s), 4);
                Add(t, Utf16(s));
                return t;
            };
            Add(f, numberTag);
            Add(f, text(number));
            Add(f, nameTag);
            Add(f, text(name));
            Fill(f, 0x72, 20);
            return f;
        }
    }

    Bytes Le(uint64_t v, size_t width)
    {
        Bytes out(width);
        for (size_t i = 0; i < width; ++i)
            out[i] = static_cast<uint8_t>(v >> (8 * i));
        return out;
    }

    Bytes F64(double v)
    {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, 8);
        return Le(bits, 8);
    }

    Bytes Utf16(const std::string& utf8) { return MiniBIM::Rvt::Bytes::Wide(utf8); }

    uint32_t Units(const std::string& utf8) { return static_cast<uint32_t>(Utf16(utf8).size() / 2); }

    Bytes Project::PartitionBytes() const
    {
        Bytes out, blocks;
        if (Wide())
        {
            std::vector<uint32_t> hosts;
            for (const Rec& r : recs)
                if (r.inside)
                    hosts.push_back(*r.inside);
            for (const Rec& r : recs)
                if (!r.inside)
                    Add(out, Record64(r.id, r, {}));
            for (const uint32_t host : hosts)
            {
                Bytes body;
                for (const Rec& r : recs)
                    if (r.inside == host)
                    {
                        Fill(body, 0x33, 10);
                        Add(body, Frame64(0, r));
                    }
                Add(out, Record64(host, Rec(host, Walls), body));
            }
            for (const Lev& l : levels)
                Add(out, Level64(l));
            for (const TypeRec& t : types)
                Add(out, Type64(t));
        }
        else
        {
            for (const Rec& r : recs)
            {
                Add(out, Record32(r));
                Fill(out, 0x44, 5);
            }
            for (const Lev& l : levels)
            {
                Add(out, Level32(l));
                Fill(out, 0x44, 3);
            }
            for (const TypeRec& t : types)
            {
                Add(out, Type32(t));
                Fill(out, 0x44, 3);
            }
        }
        for (const Block& b : data)
            Add(blocks, DataBlock(b, Wide()));
        for (const Mat& m : mats)
            Add(blocks, MaterialObject(m, Wide(), nameStyle));
        for (const auto& [category, material] : categoryMaterials)
            Add(blocks, CategoryMaterial(category, material, release));
        for (const NameEntry& n : names)
            Add(blocks, NameEntryBytes(n, Wide()));
        for (const auto& [id, number, name] : roomNames)
            Add(blocks, RoomObject(id, number, name, Wide()));
        for (const auto& [id, first, second] : nodes)
        {
            Fill(blocks, 0x13, 5);
            Add(blocks, BoxBytes(first));
            Add(blocks, BoxBytes(second));
            U64(blocks, id);
            U64(blocks, 3);
            U64(blocks, ~0ull);
        }
        for (const Lev& l : levels)
            if (l.named)
                Add(blocks, NameBlock(l, Wide()));
        Add(out, blocks);
        return out;
    }

    Bytes Project::ElementTable() const
    {
        std::vector<uint32_t> ids;
        for (const Rec& r : recs)
            if (r.declared)
                ids.push_back(r.id);
        for (const Rec& r : recs)
            if (r.inside)
                ids.push_back(*r.inside);
        for (const Lev& l : levels)
            ids.push_back(l.id);
        for (const TypeRec& t : types)
            ids.push_back(t.id);
        for (const Mat& m : mats)
            ids.push_back(m.id);
        for (const NameEntry& n : names)
            ids.push_back(n.id);
        for (const auto& room : roomNames)
            ids.push_back(std::get<0>(room));
        ids.insert(ids.end(), extraIds.begin(), extraIds.end());
        // 只有一条记录的表两种记录大小都放得下；真的表有几千个 id
        for (uint32_t id = 900'001; id < 900'006; ++id)
            ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        Bytes out{ 0xaa, 0x05 };
        U32(out, static_cast<uint32_t>(ids.size()));
        for (const uint32_t id : ids)
        {
            if (Wide())
            {
                U64(out, id);
                Fill(out, 0, 12);
                U64(out, id);
                U64(out, ~0ull);
                Fill(out, 0, 4);
            }
            else
            {
                U32(out, id);
                U32(out, id);
                Fill(out, 0, 16);
                U32(out, 0xffff'ffffu);
            }
        }
        Fill(out, 0, 19);
        return out;
    }

    Bytes Project::SchemaBytes()
    {
        return Fixture::Schema({
            { "Outline", { "m_min" } },
            { "ElementParents", { "m_ids" } },
            { "ElementHeader", {} },
            { "Plane", { "m_origin" } },
            { "CellList", { "m_cells" } },
            { "VerticalRegionsStructure", { "m_layers" } },
            { "Material", { "m_name" } },
            { "PhysicalParamSet", { "m_set" } },
            { "PatternHelper", { "m_pattern" } },
            { "ProfileRoof", { "m_sketch" } },
            { "Roof", { "m_extrusion" } },
        });
    }

    Bytes Project::File() const { return FileFrom(PartitionBytes()); }

    Bytes Project::FileFrom(const Bytes& all) const
    {
        const size_t n = std::max<size_t>(members, 1);
        const auto cut = [&](size_t k) { return all.size() * k / n; };
        std::vector<Bytes> chunks;
        for (size_t k = 0; k < n; ++k)
            chunks.emplace_back(all.begin() + static_cast<std::ptrdiff_t>(cut(k)), all.begin() + static_cast<std::ptrdiff_t>(cut(k + 1)));
        Fixture::Basic basic;
        basic.release  = release;
        basic.username = savedBy;
        return Fixture::Container({
            { "BasicFileInfo", Fixture::BasicFileInfo(basic) },
            { "Formats/Latest", Fixture::Gzip(SchemaBytes()) },
            { "Global/ElemTable", Fixture::Global(ElementTable()) },
            { "Partitions/21", Fixture::Partition(chunks) },
        });
    }

    Bytes LineBytes(double start, double end, std::array<double, 3> origin, std::array<double, 3> direction)
    {
        Bytes f(8, 0);   // 直线的标记字是 0
        Add(f, { 0x04, 0x00, 0x08, 0x01 });
        for (const double v : { start, end, origin[0], origin[1], origin[2], direction[0], direction[1], direction[2] })
            Add(f, F64(v));
        return f;
    }

    Bytes ArcBytes(std::array<double, 2> angles, std::array<double, 3> x, std::array<double, 3> y, double radius, std::array<double, 3> centre)
    {
        Bytes f = Le(1, 8);
        Add(f, { 0x04, 0x00, 0x08, 0x01 });
        for (const double v : { angles[0], angles[1], x[0], x[1], x[2], y[0], y[1], y[2], radius, centre[0], centre[1], centre[2] })
            Add(f, F64(v));
        return f;
    }

    Bytes OrientationBytes(uint32_t locationLine, uint32_t word, bool flip, bool wide)
    {
        Bytes f{ 0xff, 0xff, 0xff, 0xff, 1, 0, 0, 0 };
        U32(f, locationLine);
        if (wide)
            U32(f, word);
        Add(f, { static_cast<uint8_t>(flip ? 1 : 0), 0, 0, 0 });
        return f;
    }

    Bytes NameBytes(const std::string& name)
    {
        Bytes f{ 0xff, 0xff, 0xff, 0xff };
        U16(f, NameTag);
        U32(f, Units(name));
        Add(f, Utf16(name));
        return f;
    }

    Bytes LayersBytes(const std::vector<std::tuple<double, std::optional<uint32_t>, uint32_t>>& layers, uint32_t release)
    {
        Bytes f{ 0xff, 0xff, 0xff, 0xff };
        U16(f, Regions);
        U32(f, static_cast<uint32_t>(layers.size()));
        for (const auto& [width, material, function] : layers)
        {
            Bytes r = F64(width);
            if (release >= 2024 && release <= 2027)
            {
                U64(r, material ? uint64_t(*material) : ~0ull);
                U64(r, ~0ull);
                U32(r, function);
                Fill(r, 0, 9);
                if (release >= 2026)
                    Fill(r, 0, 4);
            }
            else
            {
                U32(r, function);
                Fill(r, 0, 4);
                U32(r, material.value_or(0xffff'ffffu));
                U32(r, 0xffff'ffffu);
                Fill(r, 0, 5);
            }
            Add(f, r);
        }
        return f;
    }

    Bytes TransformBytes(double angle, std::array<double, 3> origin)
    {
        const double s = std::sin(angle), c = std::cos(angle);
        Bytes f;
        for (const double v : { c, -s, 0.0, s, c, 0.0, 0.0, 0.0, 1.0, origin[0], origin[1], origin[2] })
            Add(f, F64(v));
        return f;
    }
}
