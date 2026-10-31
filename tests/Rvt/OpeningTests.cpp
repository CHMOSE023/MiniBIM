// Revit 读取器的导入（R5）：门窗、房间与柱。
// 移植自 BIMCraft crates/rvt/src/tests/openings.rs、rooms.rs 与 columns.rs 的测试（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，
// 见 src/Rvt/NOTICE。要样例文件（BIMCRAFT_RVT_SAMPLES）的测试没有移植
#include "Check.h"
#include "Fixture.h"
#include "ImportFixture.h"
#include "ProjectFixture.h"

#include "Model/Geom.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Bim = MiniBIM::Bim;
using Fixture::Bytes;
using ImportFixture::Base;
using ImportFixture::Get;
using ImportFixture::Imported;
using ImportFixture::LevelNamed;
using ImportFixture::Near;
using ImportFixture::Of;
using ImportFixture::Said;
using ImportFixture::Says;
using ImportFixture::Skipped;
using ImportFixture::Straight;
using ImportFixture::WallOf;
using ProjectFixture::ArcBytes;
using ProjectFixture::Block;
using ProjectFixture::Lev;
using ProjectFixture::LineBytes;
using ProjectFixture::NameBytes;
using ProjectFixture::NameEntry;
using ProjectFixture::OrientationBytes;
using ProjectFixture::Project;
using ProjectFixture::Rec;
using ProjectFixture::TransformBytes;
using ProjectFixture::TypeRec;
using RvtTest::Check;
using RvtTest::Contains;

namespace
{
    constexpr int64_t Walls      = ImportFixture::Walls;
    constexpr int64_t Doors      = ImportFixture::Doors;
    constexpr int64_t Windows    = ImportFixture::Windows;
    constexpr int64_t Rooms      = ImportFixture::Rooms;
    constexpr int64_t Columns    = ImportFixture::Columns;
    constexpr int64_t Structural = ImportFixture::Structural;

    Rec* Find(Project& p, uint32_t id)
    {
        for (Rec& r : p.recs)
            if (r.id == id)
                return &r;
        return nullptr;
    }

    Rec Unplaced(uint32_t id, int64_t category)
    {
        Rec r(id, category);
        r.placed = false;
        return r;
    }

    // ── 门窗（openings.rs）──────────────────────────────────

    struct Hole
    {
        Bim::Id             id = 0;
        bool                door = false;
        const Bim::Opening* o = nullptr;
    };

    std::vector<Hole> OpeningsOf(const Imported& i)
    {
        std::vector<Hole> out;
        for (const auto& [id, e] : i.model.elements.Items())
            if (const Bim::Door* d = e->As<Bim::Door>())
                out.push_back({ id, true, d });
            else if (const Bim::Window* w = e->As<Bim::Window>())
                out.push_back({ id, false, w });
        return out;
    }

    const Hole* FirstOf(const std::vector<Hole>& holes, bool door)
    {
        for (const Hole& h : holes)
            if (h.door == door)
                return &h;
        return nullptr;
    }

    double WidthOf(const Imported& i, const Hole& h) { return Get(h.door ? i.model.types.door : i.model.types.window, h.o->typeName).width; }

    // Straight 的墙（id 100，沿 y = 0 向东从 0 到 20 英尺，10 英尺高），门的族符号 600 与窗的 601，各有名字
    Project OpeningProject(uint32_t release)
    {
        Project p = Base(release);
        Straight(p, 100, false);
        p.recs.push_back(Unplaced(600, Doors));
        p.recs.push_back(Unplaced(601, Windows));
        p.names = { NameEntry{ 600, "Single-Flush", Doors }, NameEntry{ 601, "Fixed", Windows } };
        return p;
    }

    // 门（3 英尺宽、7 英尺高、从地面起），中心在墙上 at 英尺处，除了它的符号还列出 refs
    void Door(Project& p, uint32_t id, double at, std::vector<uint64_t> refs)
    {
        refs.insert(refs.end(), { 600, id });
        p.recs.push_back(Rec(id, Doors).At({ at - 1.5, -0.4, 0, at + 1.5, 1.2, 7 }, refs));
    }

    void ADoorAndAWindowComeInSetInTheirWall()
    {
        for (const uint32_t release : { 2019u, 2023u, 2024u, 2026u })
        {
            Project p = OpeningProject(release);
            Door(p, 200, 5.0, { 100 });
            p.recs.push_back(Rec(201, Windows).At({ 11, -0.4, 3, 15, 0.4, 7 }, { 100, 601, 201 }));
            const Imported i = ImportFixture::Import(p);
            const auto ops = OpeningsOf(i);
            const Hole* d = FirstOf(ops, true);
            const Hole* w = FirstOf(ops, false);
            if (ops.size() != 2 || !d || !w)
            {
                Check(false, "门与窗都进来，" + Of(release) + Skipped(i));
                continue;
            }
            Check(d->o->offset == 1524.0 && d->o->sill == 0.0 && d->o->typeName == "Single-Flush", "门的位置与类型，" + Of(release));
            const auto& dt = Get(i.model.types.door, "Single-Flush");
            Check(dt.width == 914.4 && dt.height == 2133.6, "门类型是包围盒的大小，" + Of(release));
            Check(w->o->offset == 3962.4 && w->o->sill == 914.4 && w->o->typeName == "Fixed", "窗的位置、窗台与类型，" + Of(release));
            const auto& wt = Get(i.model.types.window, "Fixed");
            Check(wt.width == 1219.2 && wt.height == 1219.2, "窗类型是包围盒的大小，" + Of(release));
            Check(w->o->host == d->o->host && !d->o->flipFacing && !d->o->flipHand && !d->o->family, "门窗在同一面墙里，不翻转、不带族，" + Of(release));
            Check(i.skipped.empty(), "门窗没有说明，" + Of(release) + Skipped(i));
        }
    }

    void AnOpeningIsMeasuredFromTheStartOfAWall()
    {
        // 墙掉过头来（外侧在右），朝北立着：偏移从它的定位线起点量
        Project p = Base(2024);
        p.recs.push_back(Rec(100, Walls).At({ -0.4, 0, 0, 0.4, 20, 10 }, { 10, 11, 500, 100 }));
        p.data.push_back(Block{ 100, Fixture::Concat({ LineBytes(0, 20, { 0, 0, 0 }, { 0, 1, 0 }), OrientationBytes(0, 1, true, true) }) });
        p.recs.push_back(Unplaced(600, Doors));
        p.recs.push_back(Rec(200, Doors).At({ -0.4, 3.5, 0, 1.2, 6.5, 7 }, { 100, 600, 200 }));
        const Imported i = ImportFixture::Import(p);
        const auto ws = ImportFixture::All<Bim::Wall>(i);
        Check(ws.size() == 1 && ws[0].second->start.y == 6096.0 && ws[0].second->end.y == 0.0, "墙朝南走：外侧在右");
        const auto ops = OpeningsOf(i);
        // 离定位线起点（北端）5 英尺
        Check(ops.size() == 1 && ops[0].o->offset == 6096.0 - 1524.0, "门从墙的起点量，不论墙朝哪走" + Skipped(i));
    }

    void TheHostIsTheNearestWallBelowThenAbove()
    {
        const auto wallAt = [](Project& p, uint32_t id, double y, uint64_t typeId)
        {
            p.recs.push_back(Rec(id, Walls).At({ 0, y - 0.4, 0, 20, y + 0.4, 10 }, { 10, 11, typeId, id }));
            p.data.push_back(Block{ id, Fixture::Concat({ LineBytes(0, 20, { 0, y, 0 }, { 1, 0, 0 }), OrientationBytes(0, 1, false, true) }) });
        };
        Project p = OpeningProject(2024);
        wallAt(p, 80, 20, 500);
        wallAt(p, 300, 30, 500);
        // 幕墙：它的类型没有层，它有的名字就是它进来时的名字
        p.types.push_back(TypeRec(502, Walls));
        p.data.push_back(Block{ 502, NameBytes("Storefront") });
        wallAt(p, 90, 40, 502);
        // 门 200 说了墙 80 与 100，都在它下面：下面最近的是 100。门 250 只说了在它上面的墙 300
        Door(p, 200, 5.0, { 80, 100 });
        p.recs.push_back(Rec(250, Doors).At({ 3.5, 29.6, 0, 6.5, 31.2, 7 }, { 300, 600, 250 }));
        // 门 95 说了幕墙 90（在它下面）与墙 100（在它上面）：下面最近的是幕墙，它以包围盒给的平面进来，所以是宿主。门 410 只说了它
        Door(p, 95, 9.0, { 90, 100 });
        p.recs.push_back(Rec(410, Doors).At({ 3.5, 40, 0, 6.5, 41.2, 7 }, { 90, 600, 410 }));
        const auto made = ImportFixture::ImportWithOrigins(p);
        const Imported& i = made.first;
        const auto& from = made.second;
        const auto host = [&](uint32_t door) -> std::string
        {
            for (const Hole& h : OpeningsOf(i))
            {
                const auto was = from.find(h.id);
                if (was == from.end() || was->second != "Door " + std::to_string(door))
                    continue;
                const auto id = i.model.elements.IdOf(h.o->host);
                const auto it = id ? from.find(*id) : from.end();
                return it != from.end() ? it->second : std::string("?");
            }
            return "-";
        };
        Check(host(200) == "Wall 100", "宿主是 id 下面最近的墙：" + host(200) + Skipped(i));
        Check(host(250) == "Wall 300", "下面没有时是上面最近的：" + host(250));
        Check(host(95) == "Wall 90", "幕墙也是墙：" + host(95));
        Check(host(410) == "Wall 90", "只说了幕墙的门：" + host(410) + Skipped(i));
    }

    // 斜着的墙：从 (0, 5) 英尺起沿 direction 15 英尺长，10 英尺高
    void AngledWall(Project& p, uint32_t id, std::array<double, 3> direction)
    {
        const double ex = 15.0 * direction[0], ey = 5.0 + 15.0 * direction[1];
        p.recs.push_back(Rec(id, Walls).At({ std::fmin(0.0, ex), std::fmin(5.0, ey), 0, std::fmax(0.0, ex), std::fmax(5.0, ey), 10 }, { 10, 11, 500, id }));
        p.data.push_back(Block{ id, Fixture::Concat({ LineBytes(0, 15, { 0, 5, 0 }, direction), OrientationBytes(0, 1, false, true) }) });
    }

    void AnOpeningInAWallAtAnAngleHasItsWidthSolvedFromItsBox()
    {
        // 3-4-5 的墙朝 (0.8, 0.6)。4 英尺宽、1 英尺深、中心在墙上 6 英尺处的窗在 (4.8, 8.6)，
        // 围着它的包围盒 x 方向 4·0.8 + 1·0.6 = 3.8 英尺、y 方向 4·0.6 + 1·0.8 = 3.2 英尺
        Project p = OpeningProject(2024);
        AngledWall(p, 150, { 0.8, 0.6, 0 });
        p.recs.push_back(Rec(210, Windows).At({ 4.8 - 1.9, 8.6 - 1.6, 3, 4.8 + 1.9, 8.6 + 1.6, 7 }, { 150, 601, 210 }));
        const Imported i = ImportFixture::Import(p);
        const auto ops = OpeningsOf(i);
        Check(ops.size() == 1 && !ops[0].door && Near(WidthOf(i, ops[0]), 4 * 304.8, 0.01) && Near(ops[0].o->offset, 6 * 304.8, 0.01),
              "斜墙里的窗由包围盒解出宽度" + Skipped(i));
        // 同一面墙朝罗盘另一边转，从同一个起点朝 (-0.6, 0.8)：包围盒往另一边一样宽
        Project q = OpeningProject(2024);
        AngledWall(q, 150, { -0.6, 0.8, 0 });
        // 沿它 5 英尺：中心在 (-3, 9)，3 英尺宽、0.5 英尺深的门的范围是 3·0.6 + 0.5·0.8 = 2.2 与 3·0.8 + 0.5·0.6 = 2.7
        q.recs.push_back(Rec(211, Doors).At({ -3 - 1.1, 9 - 1.35, 0, -3 + 1.1, 9 + 1.35, 7 }, { 150, 600, 211 }));
        const Imported j = ImportFixture::Import(q);
        const auto qs = OpeningsOf(j);
        Check(qs.size() == 1 && qs[0].door && Near(WidthOf(j, qs[0]), 3 * 304.8, 0.01) && Near(qs[0].o->offset, 5 * 304.8, 0.01), "朝另一边转的斜墙里的门" + Skipped(j));
        // 45° 时两个范围说的一样，宽度丢了：门窗略去并说明
        const double half = std::numbers::sqrt2 / 2.0;
        Project r = OpeningProject(2024);
        AngledWall(r, 150, { half, half, 0 });
        r.recs.push_back(Rec(212, Windows).At({ 2, 7, 3, 5, 10, 7 }, { 150, 601, 212 }));
        const Imported k = ImportFixture::Import(r);
        Check(OpeningsOf(k).empty() && Says(k, { "runs at an angle", "212" }), "45° 的墙里的窗略去" + Skipped(k));
    }

    void AnOpeningInAnAngledOrCurvedWallOrWithNoWallIsLeftOut()
    {
        Project p = OpeningProject(2024);
        // 斜墙（3-4-5），里面的门的包围盒不是转到墙方向的矩形
        AngledWall(p, 150, { 0.8, 0.6, 0 });
        p.recs.push_back(Rec(210, Doors).At({ 4, 8, 0, 7, 10, 7 }, { 150, 600, 210 }));
        // 没说墙的门，与它的墙没有定位线只有包围盒的门（族实例）：那面墙沿它的包围盒进来
        p.recs.push_back(Rec(220, Doors).At({ 4, -0.4, 0, 7, 0.4, 7 }, { 600, 220 }));
        p.recs.push_back(Rec(130, Walls).At({ 0, 50, 0, 20, 51, 10 }, { 10, 11, 500, 130 }));
        p.recs.push_back(Rec(230, Doors).At({ 4, 50, 0, 7, 51, 7 }, { 130, 600, 230 }));
        const Imported i = ImportFixture::Import(p);
        Check(Contains(Said(i, "runs at an angle"), "210"), "包围盒对不上斜墙的门略去" + Skipped(i));
        Check(Contains(Said(i, "names no wall"), "220"), "没说墙的门略去");
        Check(OpeningsOf(i).size() == 1, "由包围盒读的墙里的门进来" + Skipped(i));
        // 弧墙
        Project q = OpeningProject(2024);
        q.recs[0].bbox = { 0, -10.4, 0, 10.4, 10.4, 10 };
        q.data[1].payload = Fixture::Concat({ ArcBytes({ 0.0, 1.5 }, { 1, 0, 0 }, { 0, 1, 0 }, 10.0, { 0, 0, 0 }), OrientationBytes(0, 1, false, true) });
        Door(q, 200, 5.0, { 100 });
        const Imported j = ImportFixture::Import(q);
        Check(OpeningsOf(j).empty() && Says(j, { "curved" }), "弧墙里的门略去" + Skipped(j));
    }

    // 门窗在墙上占的范围，从小到大
    std::vector<std::pair<double, double>> Spans(const Imported& i)
    {
        std::vector<std::pair<double, double>> v;
        for (const Hole& h : OpeningsOf(i))
        {
            const double w = WidthOf(i, h);
            v.emplace_back(h.o->offset - w / 2.0, h.o->offset + w / 2.0);
        }
        std::sort(v.begin(), v.end());
        return v;
    }

    void NeighboursThatOverlapALittleAreNarrowedToFit()
    {
        Project p = OpeningProject(2024);
        // 两个 4 英尺的窗重叠 0.2 英尺（1219.2 里的 61 毫米：百分之几）
        for (const auto& [id, at] : { std::pair{ 201u, 5.0 }, std::pair{ 202u, 8.8 } })
            p.recs.push_back(Rec(id, Windows).At({ at - 2, -0.4, 3, at + 2, 0.4, 7 }, { 100, 601, id }));
        const Imported i = ImportFixture::Import(p);
        const auto spans = Spans(i);
        Check(spans.size() == 2, "两个窗都进来" + Skipped(i));
        if (spans.size() == 2)
        {
            Check(std::abs(spans[0].second - spans[1].first) < 1e-6, "它们相接");
            Check(std::abs(spans[0].first - 3 * 304.8) < 1e-6 && std::abs(spans[1].second - 10.8 * 304.8) < 1e-6, "外边不动");
        }
        Check(Says(i, { "2 openings", "narrowed to fit" }), "收窄说明了" + Skipped(i));
        // 重叠 2 英尺不是百分之几：去掉一个，并说明
        Project q = OpeningProject(2024);
        for (const auto& [id, at] : { std::pair{ 201u, 5.0 }, std::pair{ 202u, 7.0 } })
            q.recs.push_back(Rec(id, Windows).At({ at - 2, -0.4, 3, at + 2, 0.4, 7 }, { 100, 601, id }));
        const Imported j = ImportFixture::Import(q);
        Check(OpeningsOf(j).size() == 1 && Says(j, { "overlap in wall" }), "重叠很多的去掉一个" + Skipped(j));
    }

    void AnOpeningWhoseBoxOverrunsItsWallComesInAsFarAsTheWallGoes()
    {
        Project p = OpeningProject(2024);
        Door(p, 200, 5.0, { 100 });
        Door(p, 201, -0.5, { 100 });   // 一半在墙的起点之前
        p.recs.push_back(Rec(202, Windows).At({ 10, -0.4, 9, 12, 0.4, 12 }, { 100, 601, 202 }));   // 高过墙顶
        const Imported i = ImportFixture::Import(p);
        // Revit 画它们时门窗框比墙的包围盒多出一点：各自到墙为止进来，并说明
        Check(OpeningsOf(i).size() == 3, "超出墙的门窗也进来" + Skipped(i));
        const std::string said = Said(i, "overruns its wall");
        Check(Contains(said, "2 openings") && Contains(said, "ids 201, 202"), "超出墙的说明了" + Skipped(i));
        const auto ops = OpeningsOf(i);
        const Hole* window = FirstOf(ops, false);
        Check(window && Get(i.model.types.window, window->o->typeName).height <= 3048.0, "窗不比它的墙高");
    }

    void WhatTheReaderCutsDownToFitIsToldAsACount()
    {
        // 两个门窗的包围盒以同样的方式超出墙：一条说明，两个 id
        Project p = OpeningProject(2024);
        Door(p, 200, 5.0, { 100 });
        Door(p, 201, -0.5, { 100 });
        p.recs.push_back(Rec(203, Windows).At({ 10, -0.4, 9, 12, 0.4, 12 }, { 100, 601, 203 }));
        const Imported i = ImportFixture::Import(p);
        std::vector<std::string> said;
        for (const std::string& s : i.skipped)
            if (Contains(s, "overruns its wall"))
                said.push_back(s);
        Check(said.size() == 1 && ImportFixture::StartsWith(said[0], "2 openings: ") && ImportFixture::EndsWith(said[0], "(ids 201, 203)"),
              "截短的按类计数并列出 id" + Skipped(i));
        Check(OpeningsOf(i).size() == 3, "三个都进来");
    }

    void TypesAreNamedByTheirNameEntriesAndToldApartBySize()
    {
        for (const uint32_t release : { 2019u, 2023u, 2024u })
        {
            Project p = OpeningProject(release);
            Door(p, 200, 5.0, { 100 });
            // 同一类型、更宽的门：MiniBIM 的类型只有一种大小
            p.recs.push_back(Rec(201, Doors).At({ 9, -0.4, 0, 13, 1.2, 7 }, { 100, 600, 201 }));
            // 它的符号没有名字条目：自己的名字
            p.recs.push_back(Unplaced(602, Doors));
            p.recs.push_back(Rec(202, Doors).At({ 15, -0.4, 0, 18, 1.2, 7 }, { 100, 602, 202 }));
            const Imported i = ImportFixture::Import(p);
            // 新项目自己有门类型；导入不动它们，同 IFC 导入
            std::vector<std::string> names;
            for (const auto& [name, _] : i.model.types.door)
                if (ImportFixture::StartsWith(name, "Imported") || ImportFixture::StartsWith(name, "Single-Flush"))
                    names.push_back(name);
            Check(names == std::vector<std::string>{ "Imported door", "Single-Flush", "Single-Flush (2)" }, "门类型按名字条目命名、按大小分开，" + Of(release) + Skipped(i));
            Check(Get(i.model.types.door, "Single-Flush (2)").width == 1219.2, "更宽的那种，" + Of(release));
        }
        // 别的类别的名字条目不给门的类型命名（2024 起保留类别）
        Project p = OpeningProject(2024);
        p.names[0].category = Windows;
        Door(p, 200, 5.0, { 100 });
        Check(ImportFixture::Import(p).model.types.door.contains("Imported door"), "窗的名字条目不给门命名");
    }

    void AMutationSweepOverOpenings()
    {
        for (const uint32_t release : { 2019u, 2024u })
        {
            Project p = OpeningProject(release);
            Door(p, 200, 5.0, { 100 });
            p.recs.push_back(Rec(201, Windows).At({ 11, -0.4, 3, 15, 0.4, 7 }, { 100, 601, 201 }));
            p.members = 3;
            ImportFixture::Sweep(p, 0x2468'ace0'1357'9bdfull ^ release, ImportFixture::SweepRounds, "受损的门窗不崩溃、读出的过检查，" + Of(release),
                                 [](const Imported& i) { return OpeningsOf(i).size() <= 2; });
        }
    }

    void AnOpeningIsMeasuredAlongAWallThatStartsAwayFromTheOrigin()
    {
        Project p = OpeningProject(2024);
        // 从 (5, 3) 向东到 (25, 3)，从 (40, 3) 向北到 (40, 23)
        WallOf(p, 110, { 5, 2.6, 0, 25, 3.4, 10 }, { 10, 11, 500, 110 }, LineBytes(0, 20, { 5, 3, 0 }, { 1, 0, 0 }));
        WallOf(p, 111, { 39.6, 3, 0, 40.4, 23, 10 }, { 10, 11, 500, 111 }, LineBytes(0, 20, { 40, 3, 0 }, { 0, 1, 0 }));
        p.recs.push_back(Rec(210, Doors).At({ 10, 2.6, 0, 13, 3.4, 7 }, { 110, 600, 210 }));
        p.recs.push_back(Rec(211, Windows).At({ 39.6, 8, 3, 40.4, 12, 7 }, { 111, 601, 211 }));
        const Imported i = ImportFixture::Import(p);
        const auto ops = OpeningsOf(i);
        const Hole* d = FirstOf(ops, true);
        const Hole* w = FirstOf(ops, false);
        Check(d && w && d->o->offset == 6.5 * 304.8 && w->o->offset == 7.0 * 304.8, "沿不从原点起的墙量，两个方向都对" + Skipped(i));
    }

    void NeighboursThatOverlapByMoreThanATenthOfTheNarrowerAreNotFitted()
    {
        const auto narrowed = [](const Imported& i) { return Says(i, { "narrowed" }); };
        // 从 10 英尺起的 4 英尺窗与从 13.7 起的 2 英尺窗：重叠 0.3 英尺（91 毫米），多于窄的十分之一（61 毫米）、少于宽的十分之一（122 毫米）。
        // MiniBIM 不接受重叠的门窗，所以去掉一个，不收窄
        Project p = OpeningProject(2024);
        p.recs.push_back(Rec(200, Windows).At({ 10, -0.4, 3, 14, 0.4, 7 }, { 100, 601, 200 }));
        p.recs.push_back(Rec(201, Windows).At({ 13.7, -0.4, 3, 15.7, 0.4, 7 }, { 100, 601, 201 }));
        const Imported i = ImportFixture::Import(p);
        Check(OpeningsOf(i).size() == 1 && !narrowed(i), "重叠多于窄的十分之一时不收窄，宽的在前" + Skipped(i));
        // 窄的在前：从 10 英尺起的 2 英尺窗与从 11.7 起的 4 英尺窗，同样重叠 0.3 英尺
        Project r = OpeningProject(2024);
        r.recs.push_back(Rec(200, Windows).At({ 10, -0.4, 3, 12, 0.4, 7 }, { 100, 601, 200 }));
        r.recs.push_back(Rec(201, Windows).At({ 11.7, -0.4, 3, 15.7, 0.4, 7 }, { 100, 601, 201 }));
        const Imported j = ImportFixture::Import(r);
        Check(OpeningsOf(j).size() == 1 && !narrowed(j), "重叠多于窄的十分之一时不收窄，窄的在前" + Skipped(j));
        // 窄的十分之一的一半收窄：各让一半
        Project q = OpeningProject(2024);
        q.recs.push_back(Rec(200, Windows).At({ 10, -0.4, 3, 14, 0.4, 7 }, { 100, 601, 200 }));
        q.recs.push_back(Rec(201, Windows).At({ 13.97, -0.4, 3, 15.97, 0.4, 7 }, { 100, 601, 201 }));
        const Imported k = ImportFixture::Import(q);
        Check(OpeningsOf(k).size() == 2 && Says(k, { "narrowed", "200", "201" }), "重叠少时各让一半" + Skipped(k));
    }

    void OnlyAFamilySymbolARecordNamesGivesItsType()
    {
        Project p = OpeningProject(2024);
        // 777 有门类别的名字条目，不是符号
        p.extraIds.push_back(777);
        p.names.push_back(NameEntry{ 777, "Not a symbol", Doors });
        Door(p, 200, 3.0, { 100, 777 });
        Door(p, 201, 8.0, { 100 });
        // 同一个符号，2 英尺宽：自己一种类型
        p.recs.push_back(Rec(202, Doors).At({ 13, -0.4, 0, 15, 1.2, 7 }, { 100, 600, 202 }));
        const Imported i = ImportFixture::Import(p);
        std::vector<std::string> names;
        for (const Hole& h : OpeningsOf(i))
            names.push_back(h.o->typeName);
        std::sort(names.begin(), names.end());
        Check(names == std::vector<std::string>{ "Single-Flush", "Single-Flush", "Single-Flush (2)" }, "只有记录说的族符号给类型，一种大小一种类型" + Skipped(i));
    }

    // ── 房间（rooms.rs）──────────────────────────────────────

    std::vector<const Bim::Room*> RoomsOf(const Imported& i)
    {
        std::vector<const Bim::Room*> out;
        for (const auto& [_, r] : ImportFixture::All<Bim::Room>(i))
            out.push_back(r);
        return out;
    }

    // 围着矩形 r（英尺）的四面墙，id 从 first 起，从标高 1 到标高 2，第一面向东
    void Enclose(Project& p, uint32_t first, std::array<double, 4> r)
    {
        const bool word = !(p.release >= 2015 && p.release <= 2020);
        const auto [x0, y0, x1, y1] = r;
        const std::tuple<uint32_t, std::array<double, 2>, std::array<double, 2>, double> walls[] = {
            { first, { x0, y0 }, { 1, 0 }, x1 - x0 },
            { first + 1, { x1, y0 }, { 0, 1 }, y1 - y0 },
            { first + 2, { x1, y1 }, { -1, 0 }, x1 - x0 },
            { first + 3, { x0, y1 }, { 0, -1 }, y1 - y0 },
        };
        for (const auto& [id, o, d, len] : walls)
        {
            const double xa = std::fmin(o[0], o[0] + d[0] * len) - 0.4, xb = std::fmax(o[0], o[0] + d[0] * len) + 0.4;
            const double ya = std::fmin(o[1], o[1] + d[1] * len) - 0.4, yb = std::fmax(o[1], o[1] + d[1] * len) + 0.4;
            p.recs.push_back(Rec(id, Walls).At({ xa, ya, 0, xb, yb, 10 }, { 10, 11, 500, id }));
            p.data.push_back(Block{ id, Fixture::Concat({ LineBytes(0, len, { o[0], o[1], 0 }, { d[0], d[1], 0 }), OrientationBytes(0, 1, false, word) }) });
        }
    }

    // 四面墙围成的 20 × 10 英尺，从标高 1 到标高 2
    Project Walled(uint32_t release)
    {
        Project p = Base(release);
        Enclose(p, 100, { 0, 0, 20, 10 });
        return p;
    }

    void Room(Project& p, uint32_t id, const std::string& number, const std::string& name, const ProjectFixture::Box& bbox)
    {
        p.recs.push_back(Rec(id, Rooms).At(bbox, { 10, 11, id }));
        p.roomNames.emplace_back(id, number, name);
    }

    void ARoomComesInWithItsNumberNameLevelAndAPoint()
    {
        for (const uint32_t release : { 2019u, 2023u, 2024u, 2026u })
        {
            Project p = Walled(release);
            Room(p, 300, "101", "Kitchen ☃", { 0.2, 0.2, 0, 19.8, 9.8, 8 });
            const Imported i = ImportFixture::Import(p);
            const auto rs = RoomsOf(i);
            if (rs.size() != 1)
            {
                Check(false, "一个房间进来，" + Of(release) + Skipped(i));
                continue;
            }
            Check(rs[0]->number == "101" && rs[0]->name == "Kitchen ☃" && rs[0]->level == LevelNamed(i, "Level 1"), "房间的编号、名称与标高，" + Of(release));
            const auto region = i.model.RoomRegion(*rs[0]);
            Check(region && Near(region->Area(), 6096.0 * 3048.0, 1.0), "房间的点在墙里面，" + Of(release));
        }
    }

    void ARoomsPointIsMovedOffAWallAndARoomWithNoNameIsCalledRoom()
    {
        // 房间中间、沿房间中线的隔墙：中点在墙上，旁边的格点不在。隔墙把围合分成两个房间，所以房间是它的点所在的那半，
        // 它有那半的包围盒
        Project p = Walled(2024);
        p.recs.push_back(Rec(104, Walls).At({ 9.6, 0, 0, 10.4, 10, 10 }, { 10, 11, 500, 104 }));
        p.data.push_back(Block{ 104, Fixture::Concat({ LineBytes(0, 10, { 10, 0, 0 }, { 0, 1, 0 }), OrientationBytes(0, 1, false, true) }) });
        p.recs.push_back(Rec(300, Rooms).At({ 0.2, 0.2, 0, 9.8, 9.8, 8 }, { 10, 11, 300 }));
        const Imported i = ImportFixture::Import(p);
        const auto rs = RoomsOf(i);
        Check(rs.size() == 1 && rs[0]->name == "Room" && rs[0]->number.empty() && rs[0]->point.x > 0.0 && rs[0]->point.x < 3048.0,
              "房间的点挪开墙，没有名字的叫 Room" + Skipped(i));
        // 整个围合的包围盒与点找到的那半对不上：房间略去
        Project q = p;
        std::erase_if(q.recs, [](const Rec& r) { return r.id == 300; });
        Room(q, 301, "1", "Hall", { 0.2, 0.2, 0, 19.8, 9.8, 8 });
        q.recs.push_back(Rec(104, Walls).At({ 9.6, 0, 0, 10.4, 10, 10 }, { 10, 11, 500, 104 }));
        const Imported j = ImportFixture::Import(q);
        Check(RoomsOf(j).empty() && Says(j, { "do not enclose its box", "301" }), "包围盒对不上的房间略去" + Skipped(j));
    }

    void RoomsWithoutWallsOrALevelAreLeftOut()
    {
        Project p = Base(2024);
        Room(p, 300, "1", "Open", { 0, 0, 0, 10, 10, 8 });
        const Imported i = ImportFixture::Import(p);
        Check(RoomsOf(i).empty() && Says(i, { "room", "300" }), "没有墙的房间略去并说明" + Skipped(i));
    }

    void TheNewestPartitionsNumberAndNameWin()
    {
        Project older = Walled(2024);
        Room(older, 300, "101", "Old", { 0.2, 0.2, 0, 19.8, 9.8, 8 });
        Project newer = older;
        newer.roomNames = { { 300, "102", "" } };
        Fixture::Basic b;
        b.release = 2024;
        const auto i = ImportFixture::TryImport(Fixture::Container({ { "BasicFileInfo", Fixture::BasicFileInfo(b) },
                                                                      { "Formats/Latest", Fixture::Gzip(Project::SchemaBytes()) },
                                                                      { "Global/ElemTable", Fixture::Global(newer.ElementTable()) },
                                                                      { "Partitions/21", Fixture::Partition({ older.PartitionBytes() }) },
                                                                      { "Partitions/22", Fixture::Partition({ newer.PartitionBytes() }) } }));
        const auto rs = i ? RoomsOf(*i) : std::vector<const Bim::Room*>{};
        Check(rs.size() == 1 && rs[0]->number == "102" && rs[0]->name == "Room", "新分区说 102、没有名字");
    }

    void AMutationSweepOverRooms()
    {
        for (const uint32_t release : { 2019u, 2024u })
        {
            Project p = Walled(release);
            Room(p, 300, "101", "Kitchen", { 0.2, 0.2, 0, 19.8, 9.8, 8 });
            Room(p, 301, "102", "Hall", { 0.2, 0.2, 0, 9.8, 9.8, 8 });
            p.members = 3;
            ImportFixture::Sweep(p, 0x2468'ace0'1357'9bdfull ^ release, ImportFixture::SweepRounds, "受损的房间不崩溃、读出的过检查，" + Of(release),
                                 [](const Imported& i) { return RoomsOf(i).size() <= 2; });
        }
    }

    void AValueFarBeyondRevitsLimitsIsDamage()
    {
        // 围合的一面墙从一个古戈尔英尺外起：读取器不接受的长度，于是墙没有定位线，沿它自己的包围盒进来，房间仍然闭合。
        // 一路上什么也不能崩溃：房间的点在建出的墙里找
        Project p = Walled(2024);
        std::erase_if(p.data, [](const Block& b) { return b.id == 100; });
        p.data.push_back(Block{ 100, Fixture::Concat({ LineBytes(0, 1e300, { 0, 0, 0 }, { 1, 0, 0 }), OrientationBytes(0, 1, false, true) }) });
        Room(p, 300, "101", "Kitchen", { 0.2, 0.2, 0, 19.8, 9.8, 8 });
        const Imported i = ImportFixture::Import(p);
        Check(ImportFixture::All<Bim::Wall>(i).size() == 4, "那面墙沿包围盒进来" + Skipped(i));
        Check(Says(i, { "no location line", "100" }), "没有定位线说明了" + Skipped(i));
        Check(RoomsOf(i).size() == 1, "四面墙围着房间" + Skipped(i));
    }

    void ARoomWhoseMiddleIsInsideACoreHasItsPointOutsideIt()
    {
        // 20 × 10 英尺房间正中一个 4 × 4 英尺的核心筒：包围盒的中点在核心筒里，它的区域不是房间的，
        // 所以点是包围盒上 9 × 9 格点里在核心筒中线以外、离中点最近的那个（面积在那里量）
        Project p = Walled(2024);
        Enclose(p, 110, { 8, 3, 12, 7 });
        Room(p, 300, "101", "Hall", { 0.2, 0.2, 0, 19.8, 9.8, 8 });
        const Imported i = ImportFixture::Import(p);
        const auto rs = RoomsOf(i);
        if (rs.size() != 1)
        {
            Check(false, "绕着核心筒的房间进来" + Skipped(i));
            return;
        }
        const auto region = i.model.RoomRegion(*rs[0]);
        Check(region && Near(region->Area(), 6096.0 * 3048.0 - 1219.2 * 1219.2, 1.0), "房间绕着核心筒");
        // 格点在包围盒的九分之一处，从九分之一的一半起；在核心筒外离中点最近的，取第一个
        const double bx0 = 0.2 * 304.8, bx1 = 19.8 * 304.8, by0 = 0.2 * 304.8, by1 = 9.8 * 304.8;
        double near = std::numeric_limits<double>::max();
        for (int k = 0; k < 81; ++k)
        {
            const double x = bx0 + (bx1 - bx0) * ((k % 9) + 0.5) / 9.0, y = by0 + (by1 - by0) * ((k / 9) + 0.5) / 9.0;
            if (x >= 8 * 304.8 && x < 12 * 304.8 && y >= 3 * 304.8 && y < 7 * 304.8)
                continue;
            near = std::fmin(near, std::hypot(x - 10 * 304.8, y - 5 * 304.8));
        }
        const double got = std::hypot(rs[0]->point.x - 10 * 304.8, rs[0]->point.y - 5 * 304.8);
        Check(std::abs(got - near) < 1e-6, "点是核心筒外最近的格点：" + std::to_string(got) + " 对 " + std::to_string(near));
    }

    void ARoomWhoseOnlyFreeGridPointsAreInTheLastRowOrColumn()
    {
        // 20 × 30 英尺的房间里一个几乎一样大的核心筒：房间包围盒上的 9 × 9 格点只有最后一行（核心筒的上边在它下面）
        // 或最后一列在核心筒外。核心筒的区域离包围盒一边超过 600 毫米，所以房间不是核心筒的
        const std::pair<std::array<double, 4>, std::pair<double, double>> cases[] = { { { 1, 1, 19, 25.5 }, { 10, 28.2 } }, { { 1, 1, 17.5, 29 }, { 18.7, 15 } } };
        for (const auto& [core, freeAt] : cases)
        {
            Project p = Base(2024);
            Enclose(p, 100, { 0, 0, 20, 30 });
            Enclose(p, 110, core);
            Room(p, 300, "1", "Hall", { 0.2, 0.2, 0, 19.8, 29.8, 8 });
            const Imported i = ImportFixture::Import(p);
            const auto rs = RoomsOf(i);
            const std::string label = "核心筒到 (" + std::to_string(core[2]) + ", " + std::to_string(core[3]) + ")";
            Check(rs.size() == 1 && Near(rs[0]->point.x / 304.8, freeAt.first, 0.05) && Near(rs[0]->point.y / 304.8, freeAt.second, 0.05),
                  "只有最后一行或一列的格点空着也找得到，" + label + Skipped(i));
        }
    }

    // ── 柱（columns.rs）─────────────────────────────────────

    std::vector<std::pair<bool, const Bim::Column*>> ColumnsOf(const Imported& i)
    {
        std::vector<std::pair<bool, const Bim::Column*>> out;
        for (const auto& [_, e] : i.model.elements.Items())
            if (const Bim::Column* c = e->As<Bim::Column>())
                out.emplace_back(false, c);
            else if (const Bim::StructuralColumn* s = e->As<Bim::StructuralColumn>())
                out.emplace_back(true, s);
        return out;
    }

    // 标高 0 与 10 英尺，名为 Concrete 的柱符号 650
    Project ColumnProject(uint32_t release, int64_t category)
    {
        Project p = Base(release);
        p.recs.push_back(Unplaced(650, category));
        p.names.push_back(NameEntry{ 650, "Concrete", category });
        return p;
    }

    // 平面上 2 × 1 英尺、在 (10, 5) 英尺的柱，从标高 1 到标高 2
    void Column(Project& p, int64_t category, uint32_t id) { p.recs.push_back(Rec(id, category).At({ 9, 4.5, 0, 11, 5.5, 10 }, { 10, 11, 650, id })); }

    void AColumnComesInAtTheMiddleOfItsBox()
    {
        for (const uint32_t release : { 2019u, 2024u })
            for (const auto& [category, structural] : { std::pair{ Columns, false }, std::pair{ Structural, true } })
            {
                const std::string label = Of(release) + (structural ? "，结构柱" : "，建筑柱");
                Project p = ColumnProject(release, category);
                Column(p, category, 100);
                const Imported i = ImportFixture::Import(p);
                const auto cs = ColumnsOf(i);
                if (cs.size() != 1)
                {
                    Check(false, "一根柱进来，" + label + Skipped(i));
                    continue;
                }
                const Bim::Column& c = *cs[0].second;
                Check(cs[0].first == structural && c.at == Bim::P2(3048, 1524) && c.angle == 0.0, "柱在包围盒中间，" + label);
                Check(c.level == LevelNamed(i, "Level 1") && c.topLevel == LevelNamed(i, "Level 2") && c.height == 3048.0 && c.baseOffset == 0.0, "柱的标高与高度，" + label);
                const auto& t = Get(structural ? i.model.types.structuralColumn : i.model.types.column, "Concrete");
                Check(c.typeName == "Concrete" && t.width == 609.6 && t.depth == 304.8 && !t.round, "柱的类型，" + label);
                Check(Says(i, { "1 column", "rectangle of its box", "100" }), "截面没读说明了，" + label + Skipped(i));
            }
    }

    void AColumnThatStandsShortOfItsTopLevelIsAsTallAsItsBox()
    {
        Project p = ColumnProject(2024, Structural);
        p.recs.push_back(Rec(100, Structural).At({ 9, 4.5, 1, 11, 5.5, 9 }, { 10, 11, 650, 100 }));
        const Imported i = ImportFixture::Import(p);
        const auto cs = ColumnsOf(i);
        Check(cs.size() == 1 && !cs[0].second->topLevel && cs[0].second->height == 2438.4 && cs[0].second->baseOffset == 304.8, "没到顶部标高的柱与包围盒一样高");
    }

    void AColumnTurnedOffTheAxesTakesItsSymbolsSize()
    {
        Project p = ColumnProject(2024, Structural);
        // 符号在族自己的轴里的外包框：2 × 1 英尺。实例绕 (10, 5) 转 30 度
        Find(p, 650)->bbox = { -1, -0.5, 0, 1, 0.5, 10 };
        Column(p, Structural, 100);
        Find(p, 100)->bbox = { 8.8, 3.9, 0, 11.2, 6.1, 10 };
        p.data.push_back(Block{ 100, TransformBytes(30.0 * (std::numbers::pi / 180.0), { 10, 5, 0 }) });
        const Imported i = ImportFixture::Import(p);
        const auto cs = ColumnsOf(i);
        Check(cs.size() == 1 && cs[0].second->at == Bim::P2(3048, 1524) && std::abs(cs[0].second->angle - 30.0) < 1e-3, "转过的柱取放置的角度与原点" + Skipped(i));
        const auto& t = Get(i.model.types.structuralColumn, "Concrete");
        Check(t.width == 609.6 && t.depth == 304.8, "转过的柱取符号的大小");
        // 转直角时包围盒就是它：不用外包框
        Project q = ColumnProject(2024, Structural);
        Column(q, Structural, 100);
        q.data.push_back(Block{ 100, TransformBytes(std::numbers::pi / 2.0, { 10, 5, 0 }) });
        const Imported j = ImportFixture::Import(q);
        const auto qs = ColumnsOf(j);
        Check(qs.size() == 1 && qs[0].second->angle == 0.0 && Get(j.model.types.structuralColumn, "Concrete").width == 609.6, "转直角的柱照包围盒" + Skipped(j));
        // 转过、符号的大小不知道：以包围盒进来、转过，并说明
        Project r = ColumnProject(2024, Structural);
        r.names.clear();
        Column(r, Structural, 100);
        r.data.push_back(Block{ 100, TransformBytes(0.5, { 10, 5, 0 }) });
        const Imported k = ImportFixture::Import(r);
        const auto rs = ColumnsOf(k);
        Check(rs.size() == 1 && rs[0].second->angle != 0.0 && Says(k, { "family symbol's size is not known" }), "不知道符号大小的转过的柱说明了" + Skipped(k));
    }

    void ABoxsBottomAtTheGroundIsCorrectedFromTheGeometryNode()
    {
        Project p = ColumnProject(2024, Structural);
        Column(p, Structural, 100);
        Find(p, 100)->bbox = { 9, 4.5, 0, 11, 5.5, 20 };
        // 几何节点说它从 2 英尺起，平面与顶部同记录
        p.nodes.emplace_back(100, ProjectFixture::Box{ 9, 4.5, 2, 11, 5.5, 20 }, ProjectFixture::Box{ 9, 4.5, 2, 11, 5.5, 20 });
        p.levels.push_back(Lev(12, "Level 3", 20.0));
        const Imported i = ImportFixture::Import(p);
        const auto cs = ColumnsOf(i);
        Check(cs.size() == 1 && cs[0].second->baseOffset == 609.6 && cs[0].second->height == 5486.4, "包围盒的底由几何节点更正" + Skipped(i));
        // 别的平面的节点、同样的底、两个说法不一的，都不挪
        using Node = std::tuple<uint32_t, ProjectFixture::Box, ProjectFixture::Box>;
        const std::vector<std::vector<Node>> variants = {
            { { 100, { 9, 4.5, 2, 11, 5.5, 20 }, { 9, 4.5, 2, 13, 5.5, 20 } } },
            { { 100, { 9, 4.5, 0, 11, 5.5, 20 }, { 9, 4.5, 0, 11, 5.5, 20 } } },
            { { 100, { 9, 4.5, 2, 11, 5.5, 20 }, { 9, 4.5, 2, 11, 5.5, 20 } }, { 100, { 9, 4.5, 3, 11, 5.5, 20 }, { 9, 4.5, 3, 11, 5.5, 20 } } },
            { { 100, { 9, 4.5, 2, 11, 5.5, 20 }, { 9, 4.5, 2, 11, 5.5, 15 } } },
        };
        int n = 0;
        for (const auto& nodes : variants)
        {
            Project q = p;
            q.nodes = nodes;
            const Imported j = ImportFixture::Import(q);
            const auto qs = ColumnsOf(j);
            Check(!qs.empty() && qs[0].second->baseOffset == 0.0, "对不上的几何节点不挪柱底，第 " + std::to_string(++n) + " 种");
        }
    }

    void AColumnThatNamesNoLevelStandsOnTheOneBelowItsBottom()
    {
        Project p = ColumnProject(2024, Columns);
        p.names.clear();
        p.recs.push_back(Rec(100, Columns).At({ 9, 4.5, 10.5, 11, 5.5, 14 }, { 650, 100 }));
        const Imported i = ImportFixture::Import(p);
        const auto cs = ColumnsOf(i);
        Check(cs.size() == 1 && cs[0].second->level == LevelNamed(i, "Level 2") && cs[0].second->baseOffset == 152.4 && cs[0].second->typeName == "Imported column",
              "没说标高的柱立在它底部以下的标高，没名字的类型有个名字" + Skipped(i));
    }

    void AMutationSweepOverColumns()
    {
        for (const uint32_t release : { 2019u, 2024u })
        {
            Project p = ColumnProject(release, Structural);
            Column(p, Structural, 100);
            Column(p, Structural, 101);
            p.data.push_back(Block{ 101, TransformBytes(0.5, { 10, 5, 0 }) });
            p.nodes.emplace_back(100, ProjectFixture::Box{ 9, 4.5, 2, 11, 5.5, 10 }, ProjectFixture::Box{ 9, 4.5, 2, 11, 5.5, 10 });
            p.members = 3;
            ImportFixture::Sweep(p, 0x0f0f'1e1e'2d2d'3c3cull ^ release, ImportFixture::SweepRounds, "受损的柱不崩溃、读出的过检查，" + Of(release),
                                 [](const Imported& i) { return ColumnsOf(i).size() <= 2; });
        }
    }

    void ColumnTypesOfOneName()
    {
        Project p = ColumnProject(2024, Structural);
        // 同名的第二个符号；第一个的柱 2 × 1 与 3 × 1 英尺，第二个的 2 × 1 英尺
        p.recs.push_back(Unplaced(651, Structural));
        p.names.push_back(NameEntry{ 651, "Concrete", Structural });
        for (const auto& [id, symbol, x1] : { std::tuple{ 100u, 650u, 11.0 }, std::tuple{ 101u, 651u, 41.0 }, std::tuple{ 102u, 650u, 71.5 } })
        {
            const double x0 = x1 - (id == 102 ? 3.0 : 2.0);
            p.recs.push_back(Rec(id, Structural).At({ x0, 4.5, 0, x1, 5.5, 10 }, { 10, 11, symbol, id }));
        }
        const Imported i = ImportFixture::Import(p);
        std::vector<std::string> names;
        for (const auto& [_, c] : ColumnsOf(i))
            names.push_back(c->typeName);
        Check(names == std::vector<std::string>{ "Concrete", "Concrete", "Concrete (2)" }, "同名的柱类型大小一样时是一种" + Skipped(i));
    }
}

namespace RvtTest
{
    void TestOpenings()
    {
        ADoorAndAWindowComeInSetInTheirWall();
        AnOpeningIsMeasuredFromTheStartOfAWall();
        TheHostIsTheNearestWallBelowThenAbove();
        AnOpeningInAWallAtAnAngleHasItsWidthSolvedFromItsBox();
        AnOpeningInAnAngledOrCurvedWallOrWithNoWallIsLeftOut();
        NeighboursThatOverlapALittleAreNarrowedToFit();
        AnOpeningWhoseBoxOverrunsItsWallComesInAsFarAsTheWallGoes();
        WhatTheReaderCutsDownToFitIsToldAsACount();
        TypesAreNamedByTheirNameEntriesAndToldApartBySize();
        AMutationSweepOverOpenings();
        AnOpeningIsMeasuredAlongAWallThatStartsAwayFromTheOrigin();
        NeighboursThatOverlapByMoreThanATenthOfTheNarrowerAreNotFitted();
        OnlyAFamilySymbolARecordNamesGivesItsType();
    }

    void TestRooms()
    {
        ARoomComesInWithItsNumberNameLevelAndAPoint();
        ARoomsPointIsMovedOffAWallAndARoomWithNoNameIsCalledRoom();
        RoomsWithoutWallsOrALevelAreLeftOut();
        TheNewestPartitionsNumberAndNameWin();
        AMutationSweepOverRooms();
        AValueFarBeyondRevitsLimitsIsDamage();
        ARoomWhoseMiddleIsInsideACoreHasItsPointOutsideIt();
        ARoomWhoseOnlyFreeGridPointsAreInTheLastRowOrColumn();
    }

    void TestColumns()
    {
        AColumnComesInAtTheMiddleOfItsBox();
        AColumnThatStandsShortOfItsTopLevelIsAsTallAsItsBox();
        AColumnTurnedOffTheAxesTakesItsSymbolsSize();
        ABoxsBottomAtTheGroundIsCorrectedFromTheGeometryNode();
        AColumnThatNamesNoLevelStandsOnTheOneBelowItsBottom();
        AMutationSweepOverColumns();
        ColumnTypesOfOneName();
    }
}
