// Revit 读取器的导入（R5）：楼板、天花板与屋顶（迹线屋顶与拉伸屋顶）。
// 移植自 BIMCraft crates/rvt/src/tests/floors.rs 与 roofs.rs 的测试（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE。
// 要样例文件（BIMCRAFT_RVT_SAMPLES）的测试没有移植
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
using ImportFixture::FloorProject;
using ImportFixture::Get;
using ImportFixture::Imported;
using ImportFixture::LevelNamed;
using ImportFixture::Line;
using ImportFixture::Near;
using ImportFixture::Of;
using ImportFixture::Rectangle;
using ImportFixture::Said;
using ImportFixture::Says;
using ImportFixture::Skipped;
using ProjectFixture::ArcBytes;
using ProjectFixture::Block;
using ProjectFixture::LayersBytes;
using ProjectFixture::LineBytes;
using ProjectFixture::NameBytes;
using ProjectFixture::Project;
using ProjectFixture::Rec;
using ProjectFixture::TypeRec;
using RvtTest::Check;
using RvtTest::Contains;

namespace
{
    constexpr int64_t Floors   = ImportFixture::Floors;
    constexpr int64_t Ceilings = ImportFixture::Ceilings;
    constexpr int64_t Roofs    = ImportFixture::Roofs;
    constexpr int64_t Sketch   = ImportFixture::Sketch;

    std::vector<const Bim::Floor*> FloorsOf(const Imported& i)
    {
        std::vector<const Bim::Floor*> out;
        for (const auto& [_, f] : ImportFixture::All<Bim::Floor>(i))
            out.push_back(f);
        return out;
    }

    std::vector<const Bim::Ceiling*> CeilingsOf(const Imported& i)
    {
        std::vector<const Bim::Ceiling*> out;
        for (const auto& [_, c] : ImportFixture::All<Bim::Ceiling>(i))
            out.push_back(c);
        return out;
    }

    std::vector<const Bim::Roof*> RoofsOf(const Imported& i)
    {
        std::vector<const Bim::Roof*> out;
        for (const auto& [_, r] : ImportFixture::All<Bim::Roof>(i))
            out.push_back(r);
        return out;
    }

    Rec* Find(Project& p, uint32_t id)
    {
        for (Rec& r : p.recs)
            if (r.id == id)
                return &r;
        return nullptr;
    }

    Block* FindData(Project& p, uint32_t id)
    {
        for (Block& b : p.data)
            if (b.id == id)
                return &b;
        return nullptr;
    }

    // 标高 1 上顶面在标高处的楼板，20 × 10 英尺，带一个 4 × 4 英尺的洞
    Project FloorWithHole(uint32_t release, bool data)
    {
        Project p = FloorProject(release);
        p.recs.push_back(Rec(100, Floors).At({ 0, 0, -0.5, 20, 10, 0 }, { 10, 510, 100 }));
        Rectangle(p, 100, 200, { 0, 0, 20, 10 }, 0, data);
        Rectangle(p, 100, 210, { 5, 3, 9, 7 }, 0, data);
        return p;
    }

    // ── 楼板与天花板（floors.rs）──────────────────────────────

    void ASketchLineNamesItsFloorWhereverTheIdSits()
    {
        // 真文件在第二个列表的 id 里说它属于谁：第一个、中间、最后，有时后面还有它碰到的别的。
        // Snowdon 样例的楼板曾因为只看最后一格而略去
        const std::vector<std::vector<uint64_t>> slots = { { 100 }, { 30, 100 }, { 30, 100, 999 }, { 100, 30 }, { 999, 100, 30 } };
        for (const auto& s : slots)
        {
            Project p = FloorWithHole(2024, true);
            for (Rec& r : p.recs)
                if (r.category == Sketch)
                    r.refs2 = s;
            const Imported i = ImportFixture::Import(p);
            Check(FloorsOf(i).size() == 1, "草图线在第二个列表的哪一格说楼板都行，" + std::to_string(s.size()) + " 格" + Skipped(i));
        }
    }

    void AFloorComesInWithItsBoundaryHoleTypeLevelAndOffset()
    {
        // 由草图线数据里的曲线（2024 起），或只由它们的包围盒（2023 及以前的文件就是这样）
        for (const auto& [release, data] : { std::pair{ 2024u, true }, std::pair{ 2026u, true }, std::pair{ 2024u, false }, std::pair{ 2023u, false }, std::pair{ 2019u, true } })
        {
            const std::string label = Of(release) + (data ? "，有数据" : "，只有包围盒");
            const Imported i = ImportFixture::Import(FloorWithHole(release, data));
            const auto fs = FloorsOf(i);
            if (fs.size() != 1)
            {
                Check(false, "一块楼板进来，" + label + Skipped(i));
                continue;
            }
            const Bim::Floor& f = *fs[0];
            Check(f.level == LevelNamed(i, "Level 1") && f.offset == 0.0 && f.typeName == "Slab" && Get(i.model.types.floor, "Slab").thickness == 152.4,
                  "楼板的标高、偏移与类型，" + label);
            const double outer = Bim::Geom::Area(f.boundary);
            Check(Near(outer, 6096.0 * 3048.0, 1.0) && f.boundary.size() == 4, "楼板的边界，" + label);
            Check(f.openings.size() == 1 && Near(std::abs(Bim::Geom::Area(f.openings[0])), 1219.2 * 1219.2, 1.0), "楼板的洞，" + label);
            Check(!f.structural && f.slope == 0.0 && !f.arrow, "楼板是平的、不承重，" + label);
            Check(i.skipped.empty(), "楼板没有说明，" + label + Skipped(i));
        }
    }

    void AFloorsTopIsItsOffsetFromTheLevelItNames()
    {
        Project p = FloorWithHole(2024, true);
        // 顶面在标高 1 下 2 英尺，在它说的标高上；没说标高的，在它顶面以下的标高上
        p.recs[0].bbox = { 0, 0, -2.5, 20, 10, -2 };
        for (Rec& r : p.recs)
            if (r.category == Sketch)
                r.bbox[2] = r.bbox[5] = -2.0;
        const Imported i = ImportFixture::Import(p);
        const auto fs = FloorsOf(i);
        Check(!fs.empty() && fs[0]->level == LevelNamed(i, "Level 1") && fs[0]->offset == -609.6, "楼板顶面在它说的标高下");
        Project q = FloorWithHole(2024, true);
        q.recs[0].refs = { 510, 100 };
        q.recs[0].bbox = { 0, 0, 9.5, 20, 10, 10 };
        for (Rec& r : q.recs)
            if (r.category == Sketch)
                r.bbox[2] = r.bbox[5] = 10.0;
        const Imported j = ImportFixture::Import(q);
        const auto gs = FloorsOf(j);
        Check(!gs.empty() && gs[0]->level == LevelNamed(j, "Level 2") && gs[0]->offset == 0.0, "没说标高的楼板在它顶面以下的标高上" + Skipped(j));
    }

    void ACurvedEdgeComesInAsChords()
    {
        // 10 英尺的方形，右上角被圆心 (6, 6)、半径 4 英尺的四分之一圆切去：一条圆弧边
        Project p = FloorProject(2024);
        p.recs.push_back(Rec(100, Floors).At({ 0, 0, -0.5, 10, 10, 0 }, { 10, 510, 100 }));
        Line(p, 100, 200, { 0, 0 }, { 10, 0 }, 0, true);
        Line(p, 100, 201, { 10, 0 }, { 10, 6 }, 0, true);
        Line(p, 100, 203, { 6, 10 }, { 0, 10 }, 0, true);
        Line(p, 100, 204, { 0, 10 }, { 0, 0 }, 0, true);
        Rec r = Rec(202, Sketch).At({ 6, 6, 0, 10, 10, 0 }, { 1 });
        r.refs2 = { 30, 100 };
        p.recs.push_back(r);
        p.data.push_back(Block{ 202, ArcBytes({ 0.0, std::numbers::pi / 2.0 }, { 1, 0, 0 }, { 0, 1, 0 }, 4.0, { 6, 6, 0 }) });
        const Imported i = ImportFixture::Import(p);
        const auto fs = FloorsOf(i);
        // 方形减去圆弧外的角：16 − 4π 平方英尺
        const double exact = (100.0 - (16.0 - 4.0 * std::numbers::pi)) * 304.8 * 304.8;
        Check(fs.size() == 1 && std::abs(std::abs(Bim::Geom::Area(fs[0]->boundary)) - exact) / exact < 1e-4 && fs[0]->boundary.size() > 12,
              "草图的弧边以半毫米内的弦进来" + Skipped(i));
    }

    void ADiagonalSketchLineKnownOnlyByItsBox()
    {
        // 三角形：斜边的包围盒是整个 10 × 10 英尺，只有一对开着的顶点是它的角
        Project p = FloorProject(2023);
        p.recs.push_back(Rec(100, Floors).At({ 0, 0, -0.5, 10, 10, 0 }, { 10, 510, 100 }));
        Line(p, 100, 200, { 0, 0 }, { 10, 0 }, 0, false);
        Line(p, 100, 201, { 10, 0 }, { 10, 10 }, 0, false);
        Line(p, 100, 202, { 10, 10 }, { 0, 0 }, 0, false);
        const Imported i = ImportFixture::Import(p);
        const auto fs = FloorsOf(i);
        Check(fs.size() == 1 && Near(std::abs(Bim::Geom::Area(fs[0]->boundary)), 50.0 * 304.8 * 304.8, 1.0), "只有包围盒的斜线放在唯一合得上的一对顶点" + Skipped(i));
    }

    void AFloorTheReaderCannotDescribe()
    {
        Project p = FloorWithHole(2024, true);
        // 带坡度或有高差：包围盒比类型厚
        p.recs.push_back(Rec(101, Floors).At({ 0, 0, -1.5, 20, 10, 0 }, { 10, 510, 101 }));
        Rectangle(p, 101, 300, { 0, 0, 20, 10 }, 0, true);
        // 不闭合的草图
        p.recs.push_back(Rec(102, Floors).At({ 0, 0, -0.5, 20, 10, 0 }, { 10, 510, 102 }));
        Rectangle(p, 102, 310, { 0, 0, 20, 10 }, 0, true);
        std::erase_if(p.recs, [](const Rec& r) { return r.id == 313; });
        // 没有草图、没有类型、类型没有层
        p.recs.push_back(Rec(103, Floors).At({ 0, 0, -0.5, 20, 10, 0 }, { 10, 510, 103 }));
        p.recs.push_back(Rec(104, Floors).At({ 0, 0, -0.5, 20, 10, 0 }, { 10, 104 }));
        Rectangle(p, 104, 320, { 0, 0, 20, 10 }, 0, true);
        p.types.push_back(TypeRec(511, Floors));
        p.data.push_back(Block{ 511, NameBytes("Bare") });
        p.recs.push_back(Rec(105, Floors).At({ 0, 0, -0.5, 20, 10, 0 }, { 10, 511, 105 }));
        Rectangle(p, 105, 330, { 0, 0, 20, 10 }, 0, true);
        const Imported i = ImportFixture::Import(p);
        // 读不全的楼板在文件说得够的地方仍然进来：放平、带坡度或高差的、与自己的包围盒一样厚；完全读不了的略去并说明原因
        Check(FloorsOf(i).size() == 4, "读不全的楼板里四块进来" + Skipped(i));
        Check(Contains(Said(i, "sloped or stepped"), "101") && Contains(Said(i, "comes in level"), "101"), "带坡度的楼板放平进来" + Skipped(i));
        Check(Contains(Said(i, "do not close"), "102") && Contains(Said(i, "joined by a straight line"), "102"), "不闭合的草图以直线连上" + Skipped(i));
        Check(Contains(Said(i, "no sketch lines"), "103"), "没有草图的楼板略去");
        Check(Contains(Said(i, "does not name one floor type"), "104"), "没说楼板类型的略去");
        Check(Contains(Said(i, "no layers"), "105") && Contains(Said(i, "as thick as its own box"), "105"), "没有层的类型与包围盒一样厚" + Skipped(i));
    }

    void ASketchOfSeparateLoopsMakesAFloorForEach()
    {
        Project p = FloorWithHole(2024, true);
        Rectangle(p, 100, 220, { 30, 0, 40, 10 }, 0, true);
        p.recs[0].bbox = { 0, 0, -0.5, 40, 10, 0 };
        const Imported i = ImportFixture::Import(p);
        Check(FloorsOf(i).size() == 2 && Says(i, { "separate loops", "100" }), "几个分开的环，每个一块楼板" + Skipped(i));
        // 与另一个环相交的环围不出读取器能跟着走的区域：楼板以它包围盒的轮廓进来，并说明
        Project q = FloorWithHole(2024, true);
        Rectangle(q, 100, 220, { 8, 5, 12, 8 }, 0, true);
        const Imported j = ImportFixture::Import(q);
        Check(FloorsOf(j).size() == 1 && Says(j, { "footprint of its own box", "100" }), "相交的环以包围盒的轮廓进来" + Skipped(j));
    }

    void ACeilingIsItsUndersidesLevelAndOffset()
    {
        Project p = FloorProject(2024);
        p.recs.push_back(Rec(100, Ceilings).At({ 0, 0, 8, 20, 10, 8.25 }, { 10, 520, 100 }));
        Rectangle(p, 100, 200, { 0, 0, 20, 10 }, 8.25, true);
        const Imported i = ImportFixture::Import(p);
        const auto cs = CeilingsOf(i);
        Check(cs.size() == 1 && cs[0]->level == LevelNamed(i, "Level 1") && cs[0]->offset == 2438.4 && cs[0]->typeName == "Tiles" &&
                  Get(i.model.types.ceiling, "Tiles").thickness == 76.2 && Near(Bim::Geom::Area(cs[0]->boundary), 6096.0 * 3048.0, 1.0),
              "天花板是它底面的标高与偏移" + Skipped(i));
    }

    void AMutationSweepOverFloors()
    {
        for (const uint32_t release : { 2019u, 2024u })
        {
            Project p = FloorWithHole(release, release >= 2024);
            p.recs.push_back(Rec(101, Ceilings).At({ 0, 0, 8, 20, 10, 8.25 }, { 10, 520, 101 }));
            Rectangle(p, 101, 300, { 0, 0, 20, 10 }, 8.25, release >= 2024);
            p.members = 3;
            const Imported clean = ImportFixture::Import(p);
            Check(FloorsOf(clean).size() == 1 && CeilingsOf(clean).size() == 1, "没受损的楼板与天花板都进来，" + Of(release) + Skipped(clean));
            ImportFixture::Sweep(p, 0x1357'9bdf'2468'ace0ull ^ release, ImportFixture::SweepRounds, "受损的楼板不崩溃、读出的过检查，" + Of(release),
                                 [](const Imported& i) { return FloorsOf(i).size() <= 3 && CeilingsOf(i).size() <= 3; });
        }
    }

    void FloorAndCeilingTypesOfOneName()
    {
        Project p = FloorProject(2024);
        // 511 与 521 同 510 与 520；512 与 522 是另一种材质
        for (const auto& [tid, category, name, thick, material] : { std::tuple{ 511u, Floors, "Slab", 0.5, 701u }, std::tuple{ 512u, Floors, "Slab", 0.5, 700u },
                                                                   std::tuple{ 521u, Ceilings, "Tiles", 0.25, 700u }, std::tuple{ 522u, Ceilings, "Tiles", 0.25, 701u } })
        {
            p.types.push_back(TypeRec(tid, category));
            const uint32_t function = category == Floors ? 1 : 4;
            p.data.push_back(Block{ tid, Fixture::Concat({ NameBytes(name), LayersBytes({ { thick, material, function } }, 2024) }) });
        }
        uint32_t k = 0;
        for (const auto& [category, tid] : { std::pair{ Floors, 510u }, std::pair{ Floors, 511u }, std::pair{ Floors, 512u }, std::pair{ Ceilings, 520u },
                                             std::pair{ Ceilings, 521u }, std::pair{ Ceilings, 522u } })
        {
            const uint32_t id = 100 + k;
            const double x = 30.0 * k;
            const bool floor = category == Floors;
            p.recs.push_back(Rec(id, category).At({ x, 0, floor ? -0.5 : 8.0, x + 20, 10, floor ? 0.0 : 8.25 }, { 10, tid, id }));
            Rectangle(p, id, 200 + 10 * k, { x, 0, x + 20, 10 }, floor ? 0.0 : 8.25, true);
            ++k;
        }
        const Imported i = ImportFixture::Import(p);
        std::vector<std::string> floors, ceilings;
        for (const Bim::Floor* f : FloorsOf(i))
            floors.push_back(f->typeName);
        for (const Bim::Ceiling* c : CeilingsOf(i))
            ceilings.push_back(c->typeName);
        Check(floors == std::vector<std::string>{ "Slab", "Slab", "Slab (2)" }, "同名的楼板类型层一样时是一种" + Skipped(i));
        Check(ceilings == std::vector<std::string>{ "Tiles", "Tiles", "Tiles (2)" }, "同名的天花板类型层一样时是一种" + Skipped(i));
        Check(i.skipped.empty(), "一个环的草图不用说明" + Skipped(i));
    }

    void AFloorIsAsThickAsItsTypeWithinAMillimetre()
    {
        Project p = FloorProject(2024);
        // 比 152.4 毫米厚 0.88 毫米：楼板；厚 1.13 毫米：带坡度或有高差的
        p.recs.push_back(Rec(100, Floors).At({ 0, 0, -0.5029, 20, 10, 0 }, { 10, 510, 100 }));
        Rectangle(p, 100, 200, { 0, 0, 20, 10 }, 0, true);
        p.recs.push_back(Rec(101, Floors).At({ 0, 0, -0.5037, 20, 10, 0 }, { 10, 510, 101 }));
        Rectangle(p, 101, 210, { 0, 0, 20, 10 }, 0, true);
        // 只有膜的类型
        p.types.push_back(TypeRec(511, Floors));
        p.data.push_back(Block{ 511, Fixture::Concat({ NameBytes("Film"), LayersBytes({ { 0.0, std::nullopt, 100 } }, 2024) }) });
        p.recs.push_back(Rec(102, Floors).At({ 0, 0, -0.5, 20, 10, 0 }, { 10, 511, 102 }));
        Rectangle(p, 102, 220, { 0, 0, 20, 10 }, 0, true);
        const Imported i = ImportFixture::Import(p);
        Check(FloorsOf(i).size() == 3, "三块楼板都进来" + Skipped(i));
        const std::string sloped = Said(i, "sloped or stepped");
        Check(Contains(sloped, "101") && !Contains(sloped, "100"), "一毫米内算一样厚" + Skipped(i));
        Check(Contains(Said(i, "no layers"), "102"), "只有膜的类型读作它的包围盒" + Skipped(i));
        Check(std::any_of(i.model.types.floor.begin(), i.model.types.floor.end(), [](const auto& t) { return t.second.thickness == 152.4; }), "有 152.4 毫米的楼板类型");
    }

    // ── 屋顶（roofs.rs）──────────────────────────────────────

    // 屋顶类型（id 530，0.25 英尺），屋顶类别的
    Project WithRoofType(uint32_t release)
    {
        Project p = FloorProject(release);
        p.types.push_back(TypeRec(530, Roofs));
        p.data.push_back(Block{ 530, Fixture::Concat({ NameBytes("Warm"), LayersBytes({ { 0.25, 700, 4 } }, release) }) });
        return p;
    }

    void Roof(Project& p, uint32_t id, uint16_t cls, const ProjectFixture::Box& bbox)
    {
        Rec r(id, Roofs);
        r.classTag = cls;
        p.recs.push_back(r.At(bbox, { 10, 530, id }));
    }

    void AFlatRoofByFootprintComesIn()
    {
        for (const auto& [release, data] : { std::pair{ 2024u, true }, std::pair{ 2023u, false } })
        {
            Project p = WithRoofType(release);
            Roof(p, 100, ProjectFixture::ProfileRoof, { 0, 0, 12, 20, 10, 12.25 });
            Rectangle(p, 100, 200, { 0, 0, 20, 10 }, 3, data);
            Rectangle(p, 100, 210, { 5, 3, 9, 7 }, 3, data);
            const Imported i = ImportFixture::Import(p);
            const auto rs = RoofsOf(i);
            if (rs.size() != 1)
            {
                Check(false, "一个平屋顶进来，" + Of(release) + Skipped(i));
                continue;
            }
            const Bim::Roof& r = *rs[0];
            // 它只说了标高 1：底面在 12 英尺，比标高高 3,657.6 毫米
            Check(r.level == LevelNamed(i, "Level 1") && r.offset == 3657.6 && r.slope == 0.0 && r.typeName == "Warm", "平屋顶的标高、底面与类型，" + Of(release));
            Check(r.courtyards.size() == 1 && Near(Bim::Geom::Area(r.boundary), 6096.0 * 3048.0, 1.0), "平屋顶的迹线与天井，" + Of(release));
            Check(Get(i.model.types.roof, "Warm").thickness == 76.2, "屋顶类型的厚度，" + Of(release));
        }
    }

    void ARoofThatRisesComesInLevelAndOneNotByFootprintAsItsBox()
    {
        Project p = WithRoofType(2024);
        // 比类型厚：升起来的
        Roof(p, 100, ProjectFixture::ProfileRoof, { 0, 0, 12, 20, 10, 15 });
        Rectangle(p, 100, 200, { 0, 0, 20, 10 }, 3, true);
        // 平的，但是别的类（拉伸的）
        Roof(p, 101, 100, { 0, 0, 12, 20, 10, 12.25 });
        Rectangle(p, 101, 210, { 0, 0, 20, 10 }, 3, true);
        const Imported i = ImportFixture::Import(p);
        // 升起的屋顶放平进来，别的类的以它包围盒的轮廓进来：各自说明哪里没读
        Check(RoofsOf(i).size() == 2, "两个屋顶都进来" + Skipped(i));
        Check(Contains(Said(i, "slopes are not read"), "100"), "升起的屋顶放平并说明" + Skipped(i));
        Check(Contains(Said(i, "roof by face"), "101"), "别的类的屋顶说明了" + Skipped(i));
    }

    void AWallWhoseRecordNamesARoofHasItsTopCutByIt()
    {
        // 房间上的平屋顶与两面墙：一面在它约束到的标高里说了屋顶（Revit 这样列出墙顶附着的屋顶），另一面没说
        Project p = WithRoofType(2024);
        Roof(p, 100, ProjectFixture::ProfileRoof, { 0, 0, 9, 20, 10, 9.25 });
        Rectangle(p, 100, 200, { 0, 0, 20, 10 }, 9, true);
        for (const auto& [id, y, named] : { std::tuple{ 110u, 1.0, true }, std::tuple{ 111u, 5.0, false } })
        {
            std::vector<uint64_t> refs{ 10, 530, id };
            if (named)
                refs.push_back(100);
            p.recs.push_back(Rec(id, ImportFixture::Walls).At({ 0, y - 0.4, 0, 20, y + 0.4, 10 }, refs));
            p.data.push_back(Block{ id, LineBytes(0, 20, { 0, y, 0 }, { 1, 0, 0 }) });
        }
        const Imported i = ImportFixture::Import(p);
        const auto under = [&](double y) -> const Bim::Wall*
        {
            for (const auto& [_, w] : ImportFixture::All<Bim::Wall>(i))
                if (std::abs(w->start.y - y) < 1.0)
                    return w;
            return nullptr;
        };
        const Bim::Wall* attached = under(304.8);
        Check(attached && attached->attachTop, "墙顶跟着它说的屋顶" + Skipped(i));
        Check(attached && Near(i.model.TopZ(*attached), 9.0 * 304.8, 1.0), "墙顶在 9 英尺被屋顶切");
        const Bim::Wall* other = under(1524.0);
        Check(other && !other->attachTop, "没说屋顶的墙不附着");
        Check(Says(i, { "follows the roof it names", "110" }), "附着说明了" + Skipped(i));
    }

    void AWallNamingARoofThatOnlyComesInLevelKeepsItsOwnBox()
    {
        // 升起的屋顶（3 英尺高、类型 0.25 英尺厚）在包围盒底部放平进来，别的类的以包围盒的轮廓进来。
        // 檐口不是山墙上的屋顶：墙会在檐口被切掉，所以它保留自己的包围盒
        for (const bool byFace : { false, true })
        {
            Project p = WithRoofType(2024);
            Roof(p, 100, byFace ? uint16_t(100) : ProjectFixture::ProfileRoof, { 0, 0, 9, 20, 10, 12 });
            Rectangle(p, 100, 200, { 0, 0, 20, 10 }, 9, true);
            p.recs.push_back(Rec(110, ImportFixture::Walls).At({ 0, 0.6, 0, 20, 1.4, 10 }, { 10, 530, 110, 100 }));
            p.data.push_back(Block{ 110, LineBytes(0, 20, { 0, 1, 0 }, { 1, 0, 0 }) });
            const Imported i = ImportFixture::Import(p);
            const std::string label = std::string("按面 ") + (byFace ? "是" : "否");
            Check(!RoofsOf(i).empty(), "屋顶进来，" + label + Skipped(i));
            const auto ws = ImportFixture::All<Bim::Wall>(i);
            Check(!ws.empty() && !ws[0].second->attachTop, "说了只放平进来的屋顶的墙保留自己的包围盒，" + label + Skipped(i));
        }
    }

    void AMutationSweepOverRoofs()
    {
        for (const uint32_t release : { 2019u, 2024u })
        {
            Project p = WithRoofType(release);
            Roof(p, 100, ProjectFixture::ProfileRoof, { 0, 0, 12, 20, 10, 12.25 });
            Rectangle(p, 100, 200, { 0, 0, 20, 10 }, 3, release >= 2024);
            Rectangle(p, 100, 210, { 5, 3, 9, 7 }, 3, release >= 2024);
            Roof(p, 101, 100, { 0, 0, 12, 20, 10, 15 });
            p.members = 3;
            ImportFixture::Sweep(p, 0x0f1e'2d3c'4b5a'6978ull ^ release, ImportFixture::SweepRounds, "受损的屋顶不崩溃、读出的过检查，" + Of(release),
                                 [](const Imported& i) { return RoofsOf(i).size() <= 2; });
        }
    }

    void RoofTypesOfOneName()
    {
        Project p = WithRoofType(2024);
        for (const auto& [tid, material] : { std::pair{ 531u, 700u }, std::pair{ 532u, 701u } })
        {
            p.types.push_back(TypeRec(tid, Roofs));
            p.data.push_back(Block{ tid, Fixture::Concat({ NameBytes("Warm"), LayersBytes({ { 0.25, material, 4 } }, 2024) }) });
        }
        uint32_t k = 0;
        for (const uint64_t tid : { 530u, 531u, 532u })
        {
            const uint32_t id = 100 + k;
            const double x = 30.0 * k;
            Rec r(id, Roofs);
            r.classTag = ProjectFixture::ProfileRoof;
            p.recs.push_back(r.At({ x, 0, 12, x + 20, 10, 12.25 }, { 10, tid, id }));
            Rectangle(p, id, 200 + 10 * k, { x, 0, x + 20, 10 }, 3, true);
            ++k;
        }
        const Imported i = ImportFixture::Import(p);
        std::vector<std::string> names;
        for (const Bim::Roof* r : RoofsOf(i))
            names.push_back(r->typeName);
        Check(names == std::vector<std::string>{ "Warm", "Warm", "Warm (2)" }, "同名的屋顶类型层一样时是一种" + Skipped(i));
        Check(i.skipped.empty(), "一个环的草图不用说明" + Skipped(i));
    }

    using YZ = std::pair<double, double>;

    // 拉伸屋顶轮廓的一条草图线，在竖直平面 x = plane 里从 (y, z) 到 (y, z)（英尺），曲线在它的数据里
    void ProfileLine(Project& p, uint32_t owner, uint32_t id, double plane, YZ a, YZ b)
    {
        const double dy = b.first - a.first, dz = b.second - a.second;
        const double len = std::hypot(dy, dz);
        Rec r = Rec(id, Sketch).At({ plane, std::min(a.first, b.first), std::min(a.second, b.second), plane, std::max(a.first, b.first), std::max(a.second, b.second) }, { 1 });
        r.refs2 = { 30, owner };
        p.recs.push_back(r);
        p.data.push_back(Block{ id, LineBytes(0, len, { plane, a.first, a.second }, { 0, dy / len, dz / len }) });
    }

    // 过 p 沿 d 的线与过 q 沿 e 的线交在哪
    YZ Meet(YZ p, YZ d, YZ q, YZ e)
    {
        const double k = ((q.first - p.first) * e.second - (q.second - p.second) * e.first) / (d.first * e.second - d.second * e.first);
        return { p.first + k * d.first, p.second + k * d.second };
    }

    // 轮廓 a、ridge、b（沿 y、向上 z：英尺）的底面，屋顶垂直于各坡面 thickness 厚时就是这样，与读取器反着算：
    // 每个坡面的线垂直于自己往下移，两条相交，檐口竖直切
    std::array<YZ, 3> Underside(YZ a, YZ ridge, YZ b, double thickness)
    {
        const auto shifted = [&](YZ p, YZ q)
        {
            const double dy = q.first - p.first, dz = q.second - p.second;
            const double l = std::hypot(dy, dz);
            // 垂直于坡面向下：朝 +y 时是 (dz, -dy)
            const double ny = dz / l * thickness, nz = -dy / l * thickness;
            return std::pair{ YZ{ p.first + ny, p.second + nz }, YZ{ dy, dz } };
        };
        const auto [pa, da] = shifted(a, ridge);
        const auto [pb, db] = shifted(ridge, b);
        const YZ top = Meet(pa, da, pb, db);
        const auto at = [](YZ p, YZ d, double y) { return YZ{ y, p.second + d.second / d.first * (y - p.first) }; };
        return { at(pa, da, a.first), top, at(pb, db, b.first) };
    }

    double Lowest(const std::array<YZ, 3>& under)
    {
        double low = std::numeric_limits<double>::infinity();
        for (const YZ& u : under)
            low = std::fmin(low, u.second);
        return low;
    }

    // "Warm"（0.25 英尺厚）的拉伸屋顶：顶面轮廓在平面 x = 10 里，一个檐口在 y = 0 处 12 英尺高，屋脊在 y = 10 处 20 英尺高，
    // 另一个檐口在 y = 20 处 eaveB 英尺高，从 x = 0 扫到 x = 30。包围盒是它的底面与顶面围成的
    Project Gable(uint32_t release, double eaveB)
    {
        Project p = WithRoofType(release);
        const double low = Lowest(Underside({ 0, 12 }, { 10, 20 }, { 20, eaveB }, 0.25));
        Rec r(100, Roofs);
        r.classTag = ProjectFixture::ExtrusionRoof;
        p.recs.push_back(r.At({ 0, 0, low, 30, 20, 20 }, { 10, 530, 100 }));
        ProfileLine(p, 100, 200, 10, { 0, 12 }, { 10, 20 });
        ProfileLine(p, 100, 201, 10, { 20, eaveB }, { 10, 20 });
        return p;
    }

    void ARoofByExtrusionComesInAsItsProfileSweptAcross()
    {
        // 坡度 0.8 与 0.6（另一组两边都是 0.8）：两个底面相交处不在屋脊下面
        for (const auto& [release, eaveB] : { std::pair{ 2024u, 14.0 }, std::pair{ 2026u, 14.0 }, std::pair{ 2024u, 12.0 } })
        {
            const std::string label = Of(release) + "，檐口 " + std::to_string(eaveB);
            const Imported i = ImportFixture::Import(Gable(release, eaveB));
            const auto rs = RoofsOf(i);
            if (rs.size() != 1 || !rs[0]->extrusion)
            {
                Check(false, "一个拉伸屋顶进来，" + label + Skipped(i));
                continue;
            }
            const Bim::Roof& r = *rs[0];
            const Bim::Extrusion& x = *r.extrusion;
            const double ft = 304.8;
            const auto under = Underside({ 0, 12 }, { 10, 20 }, { 20, eaveB }, 0.25);
            const double low = Lowest(under);
            Check(x.line == std::array<Bim::P2, 2>{ Bim::P2(3048, 0), Bim::P2(3048, 6096) }, "沿 y 在平面 x = 10 英尺里，" + label);
            // 往它的左边，即西：x = 30 英尺在平面右边 20 英尺，x = 0 在左边 10 英尺
            Check(x.from == -6096.0 && x.to == 3048.0, "扫过的范围，" + label);
            bool profile = x.profile.size() == 3;
            for (size_t k = 0; profile && k < 3; ++k)
                profile = Near(x.profile[k].x, under[k].first * ft, 0.01) && Near(x.profile[k].y, (under[k].second - low) * ft, 0.01);
            Check(profile, "轮廓是垂直于各坡面那么厚的底面，" + label);
            Check(Near(r.offset, low * ft, 0.01), "底面的最低点是包围盒的底，" + label);
            Check(r.level == LevelNamed(i, "Level 1") && r.typeName == "Warm" && r.boundary.empty() && r.courtyards.empty(), "拉伸屋顶的标高与类型，" + label);
            Check(ImportFixture::Checks(i) && i.skipped.empty(), "拉伸屋顶过检查、没有说明，" + label + Skipped(i));
        }
    }

    void ARoofByExtrusionTheReaderCannotCheckIsLeftOut()
    {
        const auto said = [](const Project& p, const std::string& what)
        {
            const Imported i = ImportFixture::Import(p);
            const std::string s = Said(i, what);
            Check(RoofsOf(i).empty() && !s.empty(), "拉伸屋顶略去并说 " + what + Skipped(i));
            return s;
        };
        // 包围盒比轮廓与类型围成的低一英尺：它的厚度不是读取器以为的
        Project p = Gable(2024, 12);
        Find(p, 100)->bbox[2] -= 1.0;
        Check(ImportFixture::StartsWith(said(p, "box"), "1 roof left out, its box"), "包围盒对不上的拉伸屋顶");
        // 是圆弧的轮廓线
        Project q = Gable(2024, 12);
        FindData(q, 200)->payload = ArcBytes({ 0.0, 1.0 }, { 0, 1, 0 }, { 0, 0, 1 }, 10.0, { 10, 0, 12 });
        said(q, "straight");
        // 轮廓所在的平面不垂直于轴（这里是 45 度）：包围盒说不出它扫过多远
        Project r = Gable(2024, 12);
        for (const auto& [id, a, b] : { std::tuple{ 200u, std::array<double, 3>{ 0, 0, 12 }, std::array<double, 3>{ 7, 7, 20 } },
                                        std::tuple{ 201u, std::array<double, 3>{ 14, 14, 12 }, std::array<double, 3>{ 7, 7, 20 } } })
        {
            const std::array<double, 3> d{ b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            FindData(r, id)->payload = LineBytes(0, len, a, { d[0] / len, d[1] / len, d[2] / len });
        }
        said(r, "axis");
        // 两条线不相接：第二个坡面在第一个结束处上方一英尺结束
        Project s = Gable(2024, 12);
        const double len = std::sqrt(10.0 * 10.0 + 8.0 * 8.0);
        FindData(s, 201)->payload = LineBytes(0, len, { 10, 20, 13 }, { 0, -10.0 / len, 8.0 / len });
        said(s, "chain");
    }

    void AMutationSweepOverARoofByExtrusion()
    {
        for (const uint32_t release : { 2024u, 2026u })
        {
            Project p = Gable(release, 12);
            p.members = 3;
            ImportFixture::Sweep(p, 0x1a2b'3c4d'5e6f'7081ull ^ release, ImportFixture::SweepRounds, "受损的拉伸屋顶不崩溃、读出的过检查，" + Of(release),
                                 [](const Imported& i) { return RoofsOf(i).size() <= 1; });
        }
    }

    void TheLinesOfAProfileMakeTheSameRoofInAnyOrder()
    {
        // 坡度 0.5 的一个坡面分成六段，不论顺序，底面都是顶面往下移 0.25 · √1.25 英尺
        std::vector<std::pair<YZ, YZ>> pieces;
        for (int k = 0; k < 6; ++k)
            pieces.push_back({ { 4.0 * k, 12.0 + 2.0 * k }, { 4.0 * (k + 1), 14.0 + 2.0 * k } });
        const double drop = 0.25 * std::sqrt(1.25);
        const auto build = [&](const std::vector<std::pair<size_t, bool>>& order) -> std::optional<Bim::Extrusion>
        {
            Project p = WithRoofType(2024);
            Rec r(100, Roofs);
            r.classTag = ProjectFixture::ExtrusionRoof;
            p.recs.push_back(r.At({ 0, 0, 12 - drop, 30, 24, 24 }, { 10, 530, 100 }));
            uint32_t n = 0;
            for (const auto& [k, reversed] : order)
            {
                const auto [a, b] = pieces[k];
                ProfileLine(p, 100, 200 + n++, 10, reversed ? b : a, reversed ? a : b);
            }
            const Imported i = ImportFixture::Import(p);
            const auto rs = RoofsOf(i);
            if (rs.size() != 1 || !rs[0]->extrusion)
            {
                Check(false, "轮廓线拼成拉伸屋顶" + Skipped(i));
                return std::nullopt;
            }
            return rs[0]->extrusion;
        };
        const auto ordered = build({ { 0, false }, { 1, false }, { 2, false }, { 3, false }, { 4, false }, { 5, false } });
        Check(ordered && ordered->profile.size() == 7 && Near(ordered->profile[6].x, 24 * 304.8, 0.01) && Near(ordered->profile[6].y, 12 * 304.8, 0.01),
              "按顺序的六段轮廓");
        // 中间的先来，然后在头上（从哪端都行）与尾上（从哪端都行）加，两端再各加一段
        Check(build({ { 2, false }, { 1, true }, { 3, true }, { 0, false }, { 4, false }, { 5, true } }) == ordered, "打乱的轮廓线拼成同一个屋顶");
        Check(build({ { 3, true }, { 4, false }, { 2, false }, { 5, true }, { 1, true }, { 0, true } }) == ordered, "换个打乱法也一样");
    }

    void AProfileOfMoreLinesThanARoofMayHavePointsIsLeftOut()
    {
        Project p = WithRoofType(2024);
        Rec r(100, Roofs);
        r.classTag = ProjectFixture::ExtrusionRoof;
        p.recs.push_back(r.At({ 0, 0, 12 - 0.25 * std::sqrt(1.25), 30, 300, 12 + 300 * 0.5 }, { 10, 530, 100 }));
        for (uint32_t k = 0; k < 300; ++k)
            ProfileLine(p, 100, 1000 + k, 10, { double(k), 12 + 0.5 * k }, { double(k + 1), 12.5 + 0.5 * k });
        const Imported i = ImportFixture::Import(p);
        Check(RoofsOf(i).empty() &&
                  std::any_of(i.skipped.begin(), i.skipped.end(), [](const std::string& s) { return ImportFixture::StartsWith(s, "1 roof left out, its profile has 300 lines"); }),
              "轮廓线比屋顶能有的点多时略去并说明" + Skipped(i));
    }

    void AProfileThatRunsEastToWestIsReadWestToEast()
    {
        // 沿 x、在平面 y = 5 里的双坡：檐口在 x = 0 与 x = 20 处 12 英尺高，屋脊 20 英尺，从 y = 0 扫到 y = 30
        const double drop = 0.25 * std::sqrt(1.0 + 0.8 * 0.8);
        for (const bool reversed : { false, true })
        {
            Project p = WithRoofType(2024);
            Rec roof(100, Roofs);
            roof.classTag = ProjectFixture::ExtrusionRoof;
            p.recs.push_back(roof.At({ 0, 0, 12 - drop, 20, 30, 20 }, { 10, 530, 100 }));
            const std::pair<YZ, YZ> lines[2] = { { { 0, 12 }, { 10, 20 } }, { { 20, 12 }, { 10, 20 } } };
            for (uint32_t n = 0; n < 2; ++n)
            {
                const auto [a, b] = lines[n];
                const double dx = b.first - a.first, dz = b.second - a.second;
                const double len = std::hypot(dx, dz);
                Rec r = Rec(200 + n, Sketch).At({ std::min(a.first, b.first), 5, std::min(a.second, b.second), std::max(a.first, b.first), 5, std::max(a.second, b.second) }, { 1 });
                r.refs2 = { 30, 100 };
                p.recs.push_back(r);
                p.data.push_back(Block{ 200 + n, LineBytes(0, len, { a.first, 5, a.second }, { dx / len, 0, dz / len }) });
            }
            if (reversed)
                std::reverse(p.recs.begin(), p.recs.end());
            const Imported i = ImportFixture::Import(p);
            const auto rs = RoofsOf(i);
            const std::string label = std::string("反过来 ") + (reversed ? "是" : "否");
            if (rs.size() != 1 || !rs[0]->extrusion)
            {
                Check(false, "一个拉伸屋顶进来，" + label + Skipped(i));
                continue;
            }
            const Bim::Extrusion& x = *rs[0]->extrusion;
            Check(x.line == std::array<Bim::P2, 2>{ Bim::P2(0, 1524), Bim::P2(6096, 1524) }, "从西到东，" + label);
            Check(x.from == -1524.0 && x.to == 7620.0, "往它的左边，即北，" + label);
        }
    }
}

namespace RvtTest
{
    void TestFloors()
    {
        ASketchLineNamesItsFloorWhereverTheIdSits();
        AFloorComesInWithItsBoundaryHoleTypeLevelAndOffset();
        AFloorsTopIsItsOffsetFromTheLevelItNames();
        ACurvedEdgeComesInAsChords();
        ADiagonalSketchLineKnownOnlyByItsBox();
        AFloorTheReaderCannotDescribe();
        ASketchOfSeparateLoopsMakesAFloorForEach();
        ACeilingIsItsUndersidesLevelAndOffset();
        AMutationSweepOverFloors();
        FloorAndCeilingTypesOfOneName();
        AFloorIsAsThickAsItsTypeWithinAMillimetre();
    }

    void TestRoofs()
    {
        AFlatRoofByFootprintComesIn();
        ARoofThatRisesComesInLevelAndOneNotByFootprintAsItsBox();
        AWallWhoseRecordNamesARoofHasItsTopCutByIt();
        AWallNamingARoofThatOnlyComesInLevelKeepsItsOwnBox();
        AMutationSweepOverRoofs();
        RoofTypesOfOneName();
        ARoofByExtrusionComesInAsItsProfileSweptAcross();
        ARoofByExtrusionTheReaderCannotCheckIsLeftOut();
        AMutationSweepOverARoofByExtrusion();
        TheLinesOfAProfileMakeTheSameRoofInAnyOrder();
        AProfileOfMoreLinesThanARoofMayHavePointsIsLeftOut();
        AProfileThatRunsEastToWestIsReadWestToEast();
    }
}
