// Revit 读取器的记录层（R4）：类表、分区里的记录、元素清单与标高。
// 移植自 BIMCraft crates/rvt/src/tests/metadata.rs（类表）、records.rs 与 inventory.rs 的测试（f3bbb33），Apache-2.0 许可（不是 MIT），
// 已改写为 C++，见 src/Rvt/NOTICE。要样例文件（BIMCRAFT_RVT_SAMPLES）的测试没有移植
#include "Check.h"
#include "Fixture.h"
#include "ProjectFixture.h"

#include "Rvt/Bytes.h"
#include "Rvt/Container.h"
#include "Rvt/ElemTable.h"
#include "Rvt/Inventory.h"
#include "Rvt/Records.h"
#include "Rvt/Schema.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace Rvt = MiniBIM::Rvt;
namespace RB  = MiniBIM::Rvt::Bytes;
using Fixture::Bytes;
using ProjectFixture::Lev;
using ProjectFixture::Project;
using ProjectFixture::Rec;
using ProjectFixture::TypeRec;
using RvtTest::Check;
using RvtTest::Contains;

namespace
{
    constexpr int64_t Walls   = -2'000'011;
    constexpr int64_t Windows = -2'000'014;
    constexpr int64_t Doors   = -2'000'023;

    const Bytes kBasic{ 'b', 0, 'f', 0, 'i', 0 };

    std::string Of(uint32_t release) { return "Revit " + std::to_string(release); }

    // 合成的文件，打不开时记一项失败
    Rvt::Revit Open(const Bytes& file, const Rvt::Limits& limits = {})
    {
        Rvt::Revit r;
        Rvt::Error e;
        if (!Rvt::Revit::FromBytes(file, limits, r, e))
            Check(false, "打开合成的项目：" + e.message);
        return r;
    }

    // 文件的清单；读不了时为空，why 是原因
    std::optional<Rvt::Inventory> InventoryOf(const Bytes& file, std::string* why = nullptr, const Rvt::Limits& limits = {})
    {
        Rvt::Revit r = Open(file, limits);
        Rvt::Inventory inv;
        Rvt::Error e;
        if (!Rvt::ReadInventory(r, inv, e))
        {
            if (why)
                *why = e.message;
            return std::nullopt;
        }
        return inv;
    }

    Rvt::Inventory MustInventory(const Bytes& file)
    {
        std::string why;
        auto inv = InventoryOf(file, &why);
        if (!inv)
        {
            Check(false, "读清单：" + why);
            return {};
        }
        return *inv;
    }

    // 要了所有类别的一遍
    Rvt::Surveyed SurveyOf(const Bytes& file)
    {
        Rvt::Revit r = Open(file);
        Rvt::Surveyed s;
        Rvt::Error e;
        if (!Rvt::Survey(r, [](int64_t) { return true; }, s, e))
            Check(false, "读记录：" + e.message);
        return s;
    }

    using Counts = std::vector<std::tuple<int64_t, size_t, size_t>>;
    Counts CountsOf(const Rvt::Inventory& inv)
    {
        Counts out;
        for (const Rvt::Count& c : inv.Counts())
            out.emplace_back(c.category, c.elements, c.placed);
        return out;
    }
    Counts CountsOf(const Project& p) { return CountsOf(MustInventory(p.File())); }

    std::vector<uint32_t> Parse(const Bytes& table, std::string* why = nullptr)
    {
        std::vector<uint32_t> ids;
        Rvt::Error e;
        if (!Rvt::ParseElemTable(table, ids, e) && why)
            *why = e.message;
        return ids;
    }

    size_t Find(const Bytes& hay, const Bytes& needle) { return RB::Find(hay, needle); }

    void Set(Bytes& b, size_t at, const Bytes& v) { std::copy(v.begin(), v.end(), b.begin() + static_cast<std::ptrdiff_t>(at)); }

    Bytes Le(uint64_t v, size_t width) { return ProjectFixture::Le(v, width); }

    // ---------- 类表 ----------

    void SchemaTests()
    {
        {
            const Fixture::Class base{ "Element", { "m_id", "m_name" } };
            const Bytes bytes = Fixture::Schema({ { "Outline", { "m_min", "m_max", "m_z" } }, { "Wall", {}, &base }, { "ElementParents", { "m_a" } } });
            const Rvt::Schema schema = Rvt::Schema::Parse(bytes);
            Check(!schema.stopped && schema.parsedBytes == bytes.size(), "类表读到末尾");
            std::vector<std::tuple<uint16_t, std::string, std::optional<uint16_t>, uint32_t>> got;
            for (const auto& c : schema.classes)
                got.emplace_back(c.tag, c.name, c.base, c.fieldCount);
            // Wall 是 13，它内联的基类 Element 是 14，所以 ElementParents 是 15；按标签排
            const decltype(got) want{ { 12, "Outline", std::nullopt, 3 }, { 13, "Wall", 14, 0 }, { 14, "Element", std::nullopt, 2 }, { 15, "ElementParents", std::nullopt, 1 } };
            Check(got == want, "类表给每个类标签、基类与字段数");
            Check(schema.Tag("Wall") == uint16_t(13), "按名字找标签");
            const auto* parents = schema.ByTag(15);
            Check(parents && parents->name == "ElementParents", "按标签找类");
            Check(!schema.ByTag(11) && !schema.ByTag(16) && !schema.Tag("Nope"), "没有的标签与名字");
        }
        {
            const Bytes good = Fixture::Schema({ { "One", { "a" } }, { "Two", {} } });
            Bytes bad = good;
            const size_t at = RB::FindLast(good, Fixture::Of("Two"));
            bad[at - 2] = 0xf0;   // Two 的名字长度不像话了
            const Rvt::Schema schema = Rvt::Schema::Parse(bad);
            Check(schema.classes.size() == 1 && schema.stopped && Contains(*schema.stopped, "name length"), "不合语法的类表停在那里，留下前面的类");
            Check(Rvt::Schema::Parse({}).classes.empty(), "空的类表");
            const Bytes junk{ 1, 2, 3, 4 };
            Check(Rvt::Schema::Parse(junk).stopped.has_value(), "不是类表的字节");
            // 内联基类的引用重复的不是给它的标签时不可信
            const Fixture::Class element{ "Element", {} };
            Bytes mismatched = Fixture::Schema({ { "Wall", {}, &element } });
            const size_t wall = Find(mismatched, Fixture::Of("Wall")) + 4;
            Set(mismatched, wall, Le(0x8000u | 99, 2));
            const Rvt::Schema m = Rvt::Schema::Parse(mismatched);
            Check(m.classes.empty() && m.stopped && Contains(*m.stopped, "inline tag"), "内联标签对不上");
            // 内联基类可以嵌套；没完没了地嵌套的文件停下而不是栈溢出
            Bytes nested;
            for (uint16_t tag = 12; tag < 400; ++tag)
            {
                if (tag == 12)
                    nested.insert(nested.end(), { 0, 0 });
                nested.insert(nested.end(), { 1, 0, 'A' });
                const Bytes reference = Le(0x8000u | (tag + 1u), 2);
                nested.insert(nested.end(), reference.begin(), reference.end());
                nested.insert(nested.end(), { 0, 0 });
            }
            const Rvt::Schema n = Rvt::Schema::Parse(nested);
            Check(n.stopped && Contains(*n.stopped, "nest"), "嵌套太深的类表停下");
        }
        {
            const Bytes bytes  = Fixture::Schema({ { "Level", { "m_elevation" } } });
            const Bytes stream = Fixture::Paged(Fixture::Gzip(bytes));
            Rvt::Revit r = Open(Fixture::Container({ { "BasicFileInfo", kBasic }, { "Formats/Latest", stream } }));
            Rvt::Schema schema;
            Rvt::Error e;
            Check(Rvt::ReadSchema(r, schema, e) && schema.Tag("Level") == uint16_t(12), "从容器的 Formats/Latest 读类表");
            Rvt::Revit none = Open(Fixture::Container({ { "BasicFileInfo", kBasic } }));
            Check(!Rvt::ReadSchema(none, schema, e) && Contains(e.message, "Formats/Latest"), "没有类表的文件报错说出流名");
        }
    }

    // ---------- 记录 ----------

    void RecordTests()
    {
        for (const uint32_t release : { 2019u, 2023u, 2024u, 2026u })
        {
            Project p(release);
            Rec second = Rec(101, Walls).At({ 0, 0, 0, 0, 0, 0 }, { 1 });
            second.refs2 = { 30, 31 };
            p.recs = { Rec(100, Walls).At({ 1, 2, 3, 4, 5, 6 }, { 7, 8, 9, 100, 11 }), second };
            p.recs[1].bbox = { 0, 0, 0, 1, 2, 3 };
            const auto s = SurveyOf(p.File());
            const auto r = std::find_if(s.records.begin(), s.records.end(), [](const Rvt::Record& x) { return x.id == 100; });
            const bool found = r != s.records.end();
            Check(found && r->bbox == std::array<double, 6>{ 1, 2, 3, 4, 5, 6 } && r->refs == std::vector<uint64_t>{ 7, 8, 9, 100, 11 } && r->refs2.empty() && r->placed && r->category == Walls,
                  "记录带着包围盒与两个引用列表，" + Of(release));
            Check(found && r->Extent() == std::array<double, 3>{ 3, 3, 3 }, "包围盒的长度，" + Of(release));
            Check(found && r->Preceding() == uint32_t(9), "记录自己的 id 前面那一格：门窗的宿主，" + Of(release));
            const auto q = std::find_if(s.records.begin(), s.records.end(), [](const Rvt::Record& x) { return x.id == 101; });
            Check(q != s.records.end() && q->refs == std::vector<uint64_t>{ 1 } && q->refs2 == std::vector<uint64_t>{ 30, 31 } && !q->Preceding(), "第二个列表，" + Of(release));
        }
        {
            const auto r = [](std::vector<uint64_t> refs)
            {
                Rvt::Record x;
                x.partition = 1;
                x.id        = 100;
                x.category  = Walls;
                x.placed    = true;
                x.refs      = std::move(refs);
                return x;
            };
            Check(r({ 5, 100 }).Preceding() == uint32_t(5), "前面一格");
            Check(r({ 5, 100, 6, 100 }).Preceding() == uint32_t(6), "最后一次出现");
            Check(!r({ 100, 5 }).Preceding(), "排在第一：前面没有");
            Check(!r({ 0, 100 }).Preceding() && !r({ ~0ull, 100 }).Preceding() && !r({ 1, 2 }).Preceding() && !r({}).Preceding(), "前面的不是宿主");
        }
        {
            // 2024 起一格也没有的列表不是列表（后面也没有第二个），超过 1024 格或越出数据的也不是；到 2023 版空列表就是空列表
            Project p(2024);
            p.recs = { Rec(100, Walls).At({ 0, 0, 0, 0, 0, 0 }, { 7, 100 }), Rec(101, Walls).At({ 0, 0, 0, 0, 0, 0 }, { 8, 101 }) };
            p.recs[1].refs2 = { 40 };
            const Bytes bytes = p.PartitionBytes();
            const size_t first = Find(bytes, Fixture::Concat({ Le(2, 4), Le(7, 8), Le(100, 8) }));
            for (const auto& [what, count] : { std::pair{ "零", 0u }, std::pair{ "超过上限", 1025u }, std::pair{ "越出数据", 0xffff'ffffu } })
            {
                Bytes bad = bytes;
                Set(bad, first, Le(count, 4));
                const auto s = SurveyOf(p.FileFrom(bad));
                const auto r = std::find_if(s.records.begin(), s.records.end(), [](const Rvt::Record& x) { return x.id == 100; });
                Check(r != s.records.end() && r->refs.empty() && r->refs2.empty(), std::string("对不上的引用列表不是列表：") + what);
                Check(std::any_of(s.records.begin(), s.records.end(), [](const Rvt::Record& x) { return x.id == 101 && x.refs == std::vector<uint64_t>{ 8, 101 } && x.refs2 == std::vector<uint64_t>{ 40 }; }),
                      std::string("下一条记录不受影响：") + what);
            }
        }
        for (const uint32_t release : { 2023u, 2024u })
        {
            Project p(release);
            p.levels = { Lev(10, "Level 1", 0.0) };
            p.recs   = { Rec(100, Walls) };
            TypeRec material(700, -2'000'700);
            material.kind = ProjectFixture::KindMaterial;
            p.types = { TypeRec(500, Walls), material };
            const auto s = SurveyOf(p.File());
            Check(s.records.size() == 1, "类型不是带包围盒的放置的元素，" + Of(release));
            // （到 2023 版标高也是没有包围盒的记录，种类是放置的；它不是类型）
            std::vector<std::tuple<uint32_t, int64_t, bool>> got;
            bool levelsAreNoTypes = true;
            for (const auto& t : s.types)
            {
                if (t.category == Rvt::OstLevels)
                    levelsAreNoTypes = levelsAreNoTypes && !t.IsType();
                else
                    got.emplace_back(t.id, t.category, t.IsType());
            }
            Check(levelsAreNoTypes, "标高的记录不是类型，" + Of(release));
            Check(got == decltype(got){ { 500, Walls, true }, { 700, -2'000'700, false } }, "类型与材质有没有包围盒的、写着自己的记录，" + Of(release));
            const auto t = std::find_if(s.types.begin(), s.types.end(), [](const Rvt::TypeRecord& x) { return x.id == 500; });
            Check(t != s.types.end() && t->kind == ProjectFixture::KindType, "类型的种类，" + Of(release));
        }
        {
            // 2024 起格子列表必须有记录自己的 id；计数为 0 或越界的不是记录
            Project p(2024);
            p.types = { TypeRec(500, Walls) };
            const Bytes bytes = p.PartitionBytes();
            const size_t list = Find(bytes, Fixture::Concat({ Le(2, 4), Le(7, 8), Le(500, 8) }));
            for (const auto& [what, edit] : { std::pair{ "另一个 id", 24 }, std::pair{ "计数为 0", 0 }, std::pair{ "计数超过上限", 1 } })
            {
                Bytes bad = bytes;
                if (edit == 24)
                    Set(bad, list + 12, Le(501, 8));
                else
                    Set(bad, list, Le(edit == 0 ? 0 : 5000, 4));
                Check(SurveyOf(p.FileFrom(bad)).types.empty(), std::string("格子列表对不上的不是类型的记录：") + what);
            }
        }
        {
            // 多数类型在项目载入的族的文档里，在另一条记录的身体里（第二序言）
            Project p(2024);
            TypeRec inside(500, Walls);
            inside.inside = true;
            p.types = { inside, TypeRec(501, Walls) };
            const auto s = SurveyOf(p.File());
            std::vector<std::pair<uint32_t, int64_t>> got;
            for (const auto& t : s.types)
                got.emplace_back(t.id, t.category);
            std::sort(got.begin(), got.end());
            Check(got == decltype(got){ { 500, Walls }, { 501, Walls } }, "没有自己 id 的类型记录取它所在记录的 id");
            // 记录里的框架的格子列表必须写着那条记录的 id
            const Bytes bytes = p.PartitionBytes();
            const size_t list = Find(bytes, Fixture::Concat({ Le(2, 4), Le(7, 8), Le(500, 8) }));
            Bytes bad = bytes;
            Set(bad, list + 12, Le(999, 8));
            const auto t = SurveyOf(p.FileFrom(bad));
            Check(t.types.size() == 1 && t.types[0].id == 501, "格子列表没写所在记录的 id 时不算");
        }
    }

    // ---------- 元素清单与标高 ----------

    // 墙、门（一个是类型的族符号）、标高，2024 起还有墙自己记录里的一扇窗
    Project Sample(uint32_t release)
    {
        Project p(release);
        Rec symbol(201, Doors);
        symbol.placed = false;
        p.recs = { Rec(100, Walls), Rec(101, Walls), Rec(200, Doors), symbol };
        if (release >= 2024)
        {
            Rec window(300, Windows);
            window.inside = 105;
            p.recs.push_back(window);
        }
        p.levels = { Lev(12, "Sótão ☃", -3.0), Lev(10, "Level 1", 0.0), Lev(11, "Level 2", 9.84251968503937) };
        return p;
    }

    // 三个标高：一个楼层、一个不是楼层的、一个块里没有设置的
    Project Storeys(uint32_t release)
    {
        Project p(release);
        Lev one(10, "Level 1", 0.0), footing(11, "Top of Footing", -3.0);
        one.story     = true;
        footing.story = false;
        p.levels = { one, footing, Lev(12, "Level 2", 10.0) };
        return p;
    }

    std::vector<std::optional<bool>> Stories(const Rvt::Inventory& inv)
    {
        std::vector<std::optional<bool>> out;
        for (const auto& l : inv.levels)
            out.push_back(l.buildingStory);
        return out;
    }

    void InventoryTests()
    {
        for (const uint32_t release : { 2015u, 2019u, 2023u, 2024u, 2026u, 2027u })
        {
            const Project p = Sample(release);
            const Rvt::Inventory inv = MustInventory(p.File());
            const size_t walls = release >= 2024 ? 3 : 2;
            Counts want{ { Rvt::OstLevels, 3, 3 }, { Doors, 2, 1 }, { Walls, walls, walls } };
            if (release >= 2024)
                want.emplace_back(Windows, 1, 1);
            std::sort(want.begin(), want.end());
            Check(CountsOf(inv) == want, "清单按类别数元素，" + Of(release));
            std::vector<std::pair<std::string, double>> names;
            for (const auto& l : inv.levels)
                names.emplace_back(l.name, l.elevationFeet);
            Check(names == decltype(names){ { "Sótão ☃", -3.0 }, { "Level 1", 0.0 }, { "Level 2", 9.84251968503937 } }, "标高按高程排，" + Of(release));
            Check(!inv.levels.empty() && std::abs(inv.levels[0].ElevationMm() + 914.4) < 1e-9, "1 英尺是 304.8 毫米");
            Check(inv.unreadLevels == 0, "没有读不出的标高，" + Of(release));
            Check(inv.declared == Parse(p.ElementTable()).size(), "声明的 id 个数，" + Of(release));
            // 记录的类标签留着，第 2 部分可以问类表它是什么
            const auto wall = std::find_if(inv.elements.begin(), inv.elements.end(), [](const Rvt::Element& e) { return e.id == 100; });
            Check(wall != inv.elements.end() && wall->classTag == 100 && wall->placed, "墙的类标签，" + Of(release));
        }
        for (const uint32_t release : { 2023u, 2025u })
        {
            Project p = Sample(release);
            const Counts whole = CountsOf(p);
            for (const size_t members : { 2u, 3u, 7u })
            {
                p.members = members;
                Check(CountsOf(p) == whole, "跨 gzip 段的记录整条找到：" + std::to_string(members) + " 段，" + Of(release));
            }
        }
        for (const uint32_t release : { 2023u, 2024u })
        {
            Project p(release);
            Rec undeclared(101, Walls);
            undeclared.declared = false;   // 元素表没声明的 id
            p.recs = { Rec(100, Walls), undeclared, Rec(102, -5), Rec(103, -3'000'000) };   // 后两个的类别在 Revit 的范围外
            Check(CountsOf(p) == Counts{ { Walls, 1, 1 } }, "对不上的记录不要，" + Of(release));
            // 不是有限值的包围盒，或里外颠倒的
            Bytes bytes = p.PartitionBytes();
            const size_t marker = Find(bytes, { 12, 0, 0xff, 0xff, 0xff, 0xff, 13, 0 });
            Set(bytes, marker + 8, ProjectFixture::F64(std::numeric_limits<double>::quiet_NaN()));
            Check(MustInventory(p.FileFrom(bytes)).elements.empty(), "NaN 的包围盒，" + Of(release));
            Set(bytes, marker + 8, ProjectFixture::F64(5.0));   // 最小 x 5 大过最大 x 1
            Check(MustInventory(p.FileFrom(bytes)).elements.empty(), "里外颠倒的包围盒，" + Of(release));
        }
        for (const uint32_t release : { 2023u, 2024u })
        {
            Project p(release);
            Rec a(100, Walls), c(101, Walls);
            a.placed = c.placed = false;
            p.recs = { a, Rec(100, Walls), c };
            const auto inv = MustInventory(p.File());
            Check(inv.records == 3 && inv.Counts() == std::vector<Rvt::Count>{ { Walls, 2, 1 } }, "几条记录里的元素是一个元素，有一条是放置的就算放置的，" + Of(release));
        }
        {
            Project p(2024);
            // 第二个是另一个类别的族符号框架，归错了就会多出一个元素
            Rec window(300, Windows), door(301, Doors);
            window.inside = door.inside = 105;
            door.placed = false;
            p.recs = { window, door };
            // 只有放置的框架归到它的宿主（宿主自己是一面墙）：族符号框架没有 id
            const auto inv = MustInventory(p.File());
            std::vector<std::pair<int64_t, uint32_t>> got;
            for (const auto& e : inv.elements)
                got.emplace_back(e.category, e.id);
            Check(got == decltype(got){ { Windows, 105 }, { Walls, 105 } }, "没有自己 id 的框架只在放置时取宿主的 id");
        }
        {
            Project p(2024);
            Lev unnamed(11, "x", 3.0), symbol(12, "A type", 6.0);
            unnamed.named = false;
            symbol.placed = false;
            p.levels = { Lev(10, "Level 1", 0.0), unnamed, symbol };
            for (const uint32_t release : { 2023u, 2024u })
            {
                p.release = release;
                const auto inv = MustInventory(p.File());
                Check(inv.levels.size() == 1 && inv.unreadLevels == 1 && inv.Counts() == std::vector<Rvt::Count>{ { Rvt::OstLevels, 3, 2 } },
                      "没有名字块的标高算读不出，族符号不是标高，" + Of(release));
            }
        }
        for (const uint32_t release : { 2023u, 2024u })
        {
            Project p(release);
            p.levels = { Lev(10, "Level 1", 0.0) };
            const auto ok = MustInventory(p.File());
            Check(ok.levels.size() == 1 && ok.unreadLevels == 0, "名字块读得出，" + Of(release));
            // 高程必须出现两次
            p.levels[0].confirmed = false;
            const auto inv = MustInventory(p.File());
            Check(inv.levels.empty() && inv.unreadLevels == 1, "高程没有确认，" + Of(release));
            // 那串 ff 之后的三个字节是 0 或 1，然后是 0 与 0
            p.levels[0].confirmed = true;
            Bytes bytes = p.PartitionBytes();
            const size_t run = release >= 2024 ? 56 : 28;
            const size_t at  = Find(bytes, Bytes(run, 0xff)) + run;
            for (const auto& [pad, readable] : { std::pair{ uint8_t(1), true }, std::pair{ uint8_t(2), false } })
            {
                bytes[at] = pad;
                Check(MustInventory(p.FileFrom(bytes)).levels.size() == (readable ? 1u : 0u), "填充 " + std::to_string(pad) + "，" + Of(release));
            }
        }
        {
            const auto named = [](const std::string& name, double feet)
            {
                Project p(2024);
                p.levels = { Lev(10, name, feet) };
                return p;
            };
            const Project old = named("Old name", 0.0), now = named("New name", 1.0);
            Fixture::Basic b;
            b.release = 2024;
            const Bytes basic  = Fixture::BasicFileInfo(b);
            const Bytes schema = Fixture::Gzip(Project::SchemaBytes());
            const Bytes table  = Fixture::Global(now.ElementTable());
            const auto levelIn = [&](const Project& at21, const Project& at22)
            {
                const auto inv = MustInventory(Fixture::Container({ { "BasicFileInfo", basic },
                                                                    { "Formats/Latest", schema },
                                                                    { "Global/ElemTable", table },
                                                                    { "Partitions/21", Fixture::Partition({ at21.PartitionBytes() }) },
                                                                    { "Partitions/22", Fixture::Partition({ at22.PartitionBytes() }) } }));
                return inv.levels.size() == 1 ? std::pair{ inv.levels[0].name, inv.levels[0].elevationFeet } : std::pair{ std::string(), -1.0 };
            };
            // 编号大的分区存着现在的那份，不论名字排在前面的是哪个
            Check(levelIn(old, now) == std::pair{ std::string("New name"), 1.0 } && levelIn(now, old) == std::pair{ std::string("Old name"), 0.0 },
                  "在后面的分区里重写的标高取最新的名字");
        }
        for (const uint32_t release : { 2019u, 2023u, 2024u, 2026u })
        {
            const auto inv = MustInventory(Storeys(release).File());
            std::vector<std::pair<std::string, std::optional<bool>>> seen;
            for (const auto& l : inv.levels)
                seen.emplace_back(l.name, l.buildingStory);
            Check(seen == decltype(seen){ { "Top of Footing", false }, { "Level 1", true }, { "Level 2", std::nullopt } }, "两种记录形式里都读得出建筑楼层设置，" + Of(release));
        }
        for (const uint32_t release : { 2023u, 2024u })
        {
            const Project p = Storeys(release);
            Bytes bytes = p.PartitionBytes();
            // 设置跟在一个未设的 id、`00 01`、一个数与标高的 id 之后；第一个是 Level 1 的。改成 2
            const size_t width = release >= 2024 ? 8 : 4;
            Bytes anchor(width, 0xff);
            anchor.insert(anchor.end(), { 0, 1 });
            const size_t at = Find(bytes, anchor) + anchor.size() + 8 + width;
            Check(bytes[at] == 1, "Level 1 是楼层，" + Of(release));
            bytes[at] = 2;
            Check(Stories(MustInventory(p.FileFrom(bytes))) == std::vector<std::optional<bool>>{ false, std::nullopt, std::nullopt }, "不是 0 或 1 的设置不算，" + Of(release));
        }
        {
            Project p = Storeys(2024);
            Lev owned(13, "Inside a family", 5.0);
            owned.owner = 4321;
            p.levels.push_back(owned);
            const auto inv = MustInventory(p.File());
            std::vector<std::string> names;
            for (const auto& l : inv.levels)
                names.push_back(l.name);
            Check(names == std::vector<std::string>{ "Top of Footing", "Level 1", "Level 2" } && inv.ownedLevels == std::vector<uint32_t>{ 13 }, "在别的元素里的标高不是项目的标高");
            Check(inv.unreadLevels == 0, "在别的元素里的标高也不算读不出");
            // 它仍是项目标高类别的元素
            const auto counts = inv.Counts();
            const auto levels = std::find_if(counts.begin(), counts.end(), [](const Rvt::Count& c) { return c.category == Rvt::OstLevels; });
            Check(levels != counts.end() && levels->elements == 4, "它仍算标高类别的元素");
            // 块根本不在的标高是读不出，不是在别的元素里
            Project q = Storeys(2024);
            Lev nameless(14, "Nameless", 5.0);
            nameless.named = false;
            q.levels.push_back(nameless);
            const auto other = MustInventory(q.File());
            Check(other.ownedLevels.empty() && other.unreadLevels == 1, "没有块的标高是读不出");
            // 到 2023 版不找在别的元素里的标高
            Check(MustInventory(Storeys(2023).File()).ownedLevels.empty(), "2023 不找在别的元素里的标高");
        }
        {
            Project p(2024);
            Lev inside(13, "Inside", 5.0);
            inside.owner = 4321;
            p.levels = { inside };
            const Bytes bytes = p.PartitionBytes();
            const size_t at = Find(bytes, { 1, 0, 0, 0, 13, 0, 0, 0 }) + 4;
            // 所有者的 id 为 0、八个 ff 少了一个、所有者的 id 超过 u32、名字长度为 0
            const std::tuple<size_t, size_t, uint8_t, const char*> edits[] = {
                { 16, 24, 0, "所有者为 0" }, { 8, 9, 0, "哨兵里有缺口" }, { 20, 21, 1, "所有者超过 u32" }, { 0x43, 0x47, 0, "没有名字" } };
            for (const auto& [from, to, value, says] : edits)
            {
                Bytes bad = bytes;
                std::fill(bad.begin() + static_cast<std::ptrdiff_t>(at + from), bad.begin() + static_cast<std::ptrdiff_t>(at + to), value);
                Check(MustInventory(p.FileFrom(bad)).ownedLevels.empty(), std::string("不是 ElementId 或没填的所有者格不算：") + says);
            }
            Check(MustInventory(p.File()).ownedLevels == std::vector<uint32_t>{ 13 }, "没改过的块是在别的元素里的");
        }
        {
            const Project p = Sample(2024);
            const Bytes schema = Fixture::Gzip(Project::SchemaBytes());
            const Bytes table  = Fixture::Global(p.ElementTable());
            const Bytes part   = Fixture::Partition({ p.PartitionBytes() });
            const auto why = [&](uint32_t release, const Bytes& s, const Bytes& t, const Bytes* partition)
            {
                Fixture::Basic b;
                b.release = release;
                Fixture::Streams all{ { "BasicFileInfo", Fixture::BasicFileInfo(b) }, { "Formats/Latest", s }, { "Global/ElemTable", t } };
                if (partition)
                    all.emplace_back("Partitions/21", *partition);
                std::string message;
                return InventoryOf(Fixture::Container(all), &message) ? std::string("（读出来了）") : message;
            };
            Check(Contains(why(2030, schema, table, &part), "does not guess") && Contains(why(2010, schema, table, &part), "does not guess"), "不认得的版本不猜");
            Check(Contains(why(2024, schema, table, nullptr), "no Partitions"), "没有分区");
            const Bytes noPlane = Fixture::Gzip(Fixture::Schema({ { "Outline", {} }, { "ElementParents", {} }, { "ElementHeader", {} } }));
            Check(Contains(why(2024, noPlane, table, &part), "Plane"), "类表里没有 Plane");
            Check(Contains(why(2024, schema, Fixture::Global(Fixture::Of("garbage")), &part), "ElemTable"), "元素表坏了");
        }
        for (const uint32_t release : { 2023u, 2024u })
        {
            Project p = Sample(release);
            p.extraIds = { 7, 5000, 5001 };
            const auto ids = Parse(p.ElementTable());
            bool rising = true;
            for (size_t i = 1; i < ids.size(); ++i)
                rising = rising && ids[i - 1] < ids[i];
            Check(rising && std::binary_search(ids.begin(), ids.end(), 5001u) && std::binary_search(ids.begin(), ids.end(), 7u), "元素表的 id 升序，" + Of(release));
            Rvt::Revit r = Open(p.File());
            std::vector<uint32_t> read;
            Rvt::Error e;
            Check(Rvt::ReadElementIds(r, read, e) && read == ids, "从文件读元素表，" + Of(release));
        }
        {
            const Bytes table = Sample(2024).ElementTable();
            Bytes falling = table, huge = table, zero = table;
            Set(falling, 6 + 20, Le(0xffff'ffffu, 4));   // 第一个 id 大过第二个
            Set(huge, 2, Le(0xffff'ffffu, 4));           // 表放不下的计数
            Set(zero, 2, Le(0, 4));
            const std::pair<Bytes, const char*> cases[] = {
                { falling, "rise" }, { huge, "rise" }, { zero, "no elements" }, { Bytes(table.begin(), table.begin() + 4), "too short" }, { Bytes(), "too short" } };
            for (const auto& [bad, says] : cases)
            {
                std::string why;
                Parse(bad, &why);
                Check(Contains(why, says), std::string("对不上的元素表报错：") + says);
            }
        }
        {
            const auto name = [](int64_t id) { const char* n = Rvt::CategoryName(id); return n ? std::string(n) : std::string("（无）"); };
            Check(name(Walls) == "Walls" && name(Rvt::OstLevels) == "Levels" && name(-2'001'320) == "Structural Framing", "认得的类别有名字");
            Check(!Rvt::CategoryName(-2'009'664) && !Rvt::CategoryName(0), "不认得的类别没有名字");
            bool all = true;
            for (const int64_t id : { -2'009'030, -2'000'011, -2'001'000, -2'008'000, -2'000'240 })
                all = all && Rvt::CategoryName(id);
            Check(all, "类别名字表排好序，能二分查找");
        }
        for (const uint32_t release : { 2023u, 2024u })
        {
            // 损坏之后不会崩溃，也不会凭空多出元素
            Project p = Sample(release);
            p.members = 3;
            const Bytes bytes = p.PartitionBytes();
            const Bytes table = p.ElementTable();
            const auto clean = MustInventory(p.File());
            Rvt::Limits limits;
            limits.maxFile     = 1 << 20;
            limits.maxStreams  = 64;
            limits.maxStream   = 1 << 20;
            limits.maxInflated = 1 << 20;
            RvtTest::Xorshift next{ 0x0123'4567'89ab'cdefull ^ release };
            size_t read = 0;
            bool declared = true, bounded = true;
            for (int round = 0; round < 2500; ++round)
            {
                Bytes part = bytes;
                for (uint64_t k = 0, n = 1 + next() % 6; k < n; ++k)
                {
                    const size_t at = static_cast<size_t>(next() % part.size());
                    switch (next() % 4)
                    {
                    case 0: part[at] = 0xff; break;
                    case 1: part[at] = 0; break;
                    case 2: part[at] = part[static_cast<size_t>(next() % part.size())]; break;
                    default: part[at] = static_cast<uint8_t>(next()); break;
                    }
                }
                if (round % 11 == 0)
                    part.resize(static_cast<size_t>(next() % part.size()));
                Rvt::Revit file = Open(p.FileFrom(part), limits);
                if (round % 5 == 0)
                {
                    // 元素表也弄坏
                    Bytes broken = table;
                    broken[static_cast<size_t>(next() % table.size())] = static_cast<uint8_t>(next());
                    Parse(broken);
                }
                Rvt::Inventory inv;
                Rvt::Error e;
                if (Rvt::ReadInventory(file, inv, e))
                {
                    ++read;
                    // 任何损坏都不能让一个元素以没声明的 id 出现
                    std::vector<uint32_t> ids;
                    Rvt::ReadElementIds(file, ids, e);
                    for (const auto& el : inv.elements)
                        declared = declared && std::binary_search(ids.begin(), ids.end(), el.id);
                    bounded = bounded && inv.levels.size() <= clean.levels.size() + 3;
                }
            }
            Check(declared && bounded, "损坏的分区不崩溃，也不凭空多出元素，" + Of(release));
            Check(read > 500, "损坏试验走到了扫描：" + std::to_string(read) + " 次，" + Of(release));
        }
        for (const uint32_t release : { 2023u, 2024u })
        {
            Project p(release);
            p.levels = { Lev(10, "Level 1", 0.0) };
            const Bytes bytes = p.PartitionBytes();
            const auto records = [&](const Bytes& b)
            {
                const auto counts = MustInventory(p.FileFrom(b)).Counts();
                const auto it = std::find_if(counts.begin(), counts.end(), [](const Rvt::Count& c) { return c.category == Rvt::OstLevels; });
                return it == counts.end() ? size_t(0) : it->elements;
            };
            Check(records(bytes) == 1, "标高的记录，" + Of(release));
            const size_t hit = Find(bytes, Le(static_cast<uint64_t>(Rvt::OstLevels), 8));
            // 2024 起标志是类别前 2 字节的 u16，标记在它后面 0x3e；之前头的标签在它前面 6 字节，标记在后面 38
            const size_t damaged[2] = { release >= 2024 ? hit - 2 : hit - 6, release >= 2024 ? hit + 0x3e : hit + 38 };
            for (const size_t at : damaged)
            {
                Bytes bad = bytes;
                bad[at] ^= 0x55;
                Check(records(bad) == 0, "标志、标记或头坏了的标高记录不算，" + Of(release) + "，第 " + std::to_string(at) + " 字节");
            }
        }
        for (const uint32_t release : { 2023u, 2024u })
        {
            const Project p = Storeys(release);
            Bytes bytes = p.PartitionBytes();
            Bytes anchor(release >= 2024 ? 8 : 4, 0xff);
            anchor.insert(anchor.end(), { 0, 1 });
            const size_t at = Find(bytes, anchor);
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), 7, 0x44);
            Check(Stories(MustInventory(p.FileFrom(bytes))) == std::vector<std::optional<bool>>{ false, true, std::nullopt }, "建筑楼层设置可以在名字之后隔几个字节，" + Of(release));
        }
    }
}

namespace RvtTest
{
    void TestSchema() { SchemaTests(); }
    void TestRecords() { RecordTests(); }
    void TestInventory() { InventoryTests(); }
}
