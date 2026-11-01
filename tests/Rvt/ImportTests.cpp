// Revit 读取器的导入（R5）：读成模型的标高、说明的写法、元素在文件里是什么，以及墙。
// 移植自 BIMCraft crates/rvt/src/tests/import.rs 与 walls.rs 的测试（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE。
// 主窗口接上的读取器的两条移植自 apps/bimcraft/src/main.rs 的测试（MIT），要 apps/Common 的 RevitReport（不碰界面控件）
// 要样例文件（BIMCRAFT_RVT_SAMPLES）的测试没有移植
#include "Check.h"
#include "Fixture.h"
#include "ImportFixture.h"
#include "ProjectFixture.h"

#include "GUI/RevitReport.h"
#include "Rvt/Import.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace Rvt = MiniBIM::Rvt;
namespace Bim = MiniBIM::Bim;
using Fixture::Bytes;
using ImportFixture::Base;
using ImportFixture::Get;
using ImportFixture::Imported;
using ImportFixture::LevelList;
using ImportFixture::LevelNamed;
using ImportFixture::Of;
using ImportFixture::Said;
using ImportFixture::Says;
using ImportFixture::Skipped;
using ImportFixture::Straight;
using ImportFixture::WallOf;
using ProjectFixture::ArcBytes;
using ProjectFixture::Block;
using ProjectFixture::LayersBytes;
using ProjectFixture::Lev;
using ProjectFixture::LineBytes;
using ProjectFixture::Mat;
using ProjectFixture::NameBytes;
using ProjectFixture::OrientationBytes;
using ProjectFixture::Project;
using ProjectFixture::Rec;
using ProjectFixture::TypeRec;
using RvtTest::Check;
using RvtTest::Contains;

namespace
{
    constexpr int64_t Walls = ImportFixture::Walls;

    std::vector<std::pair<Bim::Id, const Bim::Wall*>> WallsOf(const Imported& i) { return ImportFixture::All<Bim::Wall>(i); }

    // ── 标高与说明（import.rs）─────────────────────────────────

    void LevelsComeInLowestFirst()
    {
        for (const uint32_t release : { 2019u, 2023u, 2024u, 2026u })
        {
            Project p(release);
            p.levels = { Lev(12, "Roof", 19.68503937007874), Lev(10, "Level 1", 0.0), Lev(11, "Sótão ☃", -3.0) };
            const Imported i = ImportFixture::Import(p);
            const std::vector<std::pair<std::string, double>> want{ { "Sótão ☃", -914.4 }, { "Level 1", 0.0 }, { "Roof", 6000.0 } };
            Check(LevelList(i) == want, "标高从低到高、以毫米进来，" + Of(release));
            Check(i.schema == Of(release) && ImportFixture::Checks(i) && !i.moved && i.skipped.empty(), "模型过检查、没有挪动、没有说明，" + Of(release) + Skipped(i));
        }
    }

    void LevelsThatShareANameAreToldApart()
    {
        Project p(2024);
        p.levels = { Lev(10, "Level", 0.0), Lev(11, "Level", 10.0), Lev(12, "Level", 20.0) };
        std::vector<std::string> names;
        for (const auto& [name, _] : LevelList(ImportFixture::Import(p)))
            names.push_back(name);
        Check(names == std::vector<std::string>{ "Level", "Level (2)", "Level (3)" }, "同名的标高分开命名");
        const Imported bare = ImportFixture::Import(Project(2024));
        Check(LevelList(bare) == std::vector<std::pair<std::string, double>>{ { "Level 1", 0.0 } } && Says(bare, { "no levels" }),
              "没有标高的文件得到一个，并说明" + Skipped(bare));
    }

    void WhatTheInventoryCannotMakeALevelOfIsListed()
    {
        Project p(2024);
        Lev inside(11, "Inside a family", 5.0);
        inside.owner = 4321;
        Lev nameless(12, "Nameless", 10.0);
        nameless.named = false;
        p.levels = { Lev(10, "Level 1", 0.0), inside, nameless, Lev(13, "Far away", 1.0e7) };
        const Imported i = ImportFixture::Import(p);
        Check(LevelList(i) == std::vector<std::pair<std::string, double>>{ { "Level 1", 0.0 } }, "只有能读的标高进来");
        Check(Says(i, { "inside other elements" }) && Says(i, { "ids 11" }), "在别的元素里的标高说明了" + Skipped(i));
        Check(Says(i, { "1 level" }) && Says(i, { "no name" }), "没有名字的标高说明了" + Skipped(i));
        Check(Says(i, { "Far away" }), "超出模型范围的标高以规则说明" + Skipped(i));
        Check(ImportFixture::Checks(i), "模型过检查");
        // 每条都是计数的说明：`N kinds left out, why`，桌面版加起来放进状态栏
        const auto counted = [&](const std::string& kind, const std::string& what)
        { return std::any_of(i.skipped.begin(), i.skipped.end(), [&](const std::string& s) { return ImportFixture::StartsWith(s, "1 " + kind + " left out, ") && Contains(s, what); }); };
        Check(counted("level", "inside other elements") && counted("level", "no name") && counted("level", "Far away"), "三条都是计数的说明" + Skipped(i));
    }

    // 宿主给主窗口接上的读取器（Rvt::ImportPath，项目以文件名命名）读得了 Revit 项目，读不了的说原因
    void TheReaderTheWindowIsGivenNamesTheProjectForItsFile()
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("minibim-desktop-rvt-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path file = dir / u8"Barn House.rvt";
        auto write = [&](const Bytes& bytes)
        {
            std::ofstream f(file, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        };
        const auto u8 = file.u8string();
        const std::string path(u8.begin(), u8.end());
        const MiniBIM::RevitReport::Reader read = &Rvt::ImportPath;
        write(Project(2024).File());
        Bim::Ifc::Imported i;
        std::string e;
        Check(read(path, MiniBIM::RevitReport::ProjectName(path), i, e) && i.model.name == "Barn House" && i.schema == "Revit 2024",
              "读成以文件名命名的项目：" + i.model.name + " " + i.schema + " " + e);
        write(Fixture::Of("not a Revit file"));
        Bim::Ifc::Imported j;
        e.clear();
        Check(!read(path, "Barn House", j, e) && !e.empty(), "不是 Revit 的文件说原因，不崩：" + e);
        std::filesystem::remove_all(dir, ec);
    }

    // 读取器略去的，就是窗口数的"略去了几个"（RevitReport::CountLeftOut）
    void WhatTheReaderLeavesOutIsWhatTheWindowCountsAsLeftOut()
    {
        Project p(2024);
        Lev inside(11, "Inside", 5.0);
        inside.owner = 4321;
        Lev nameless(12, "x", 9.0);
        nameless.named = false;
        p.levels = { Lev(10, "Level 1", 0.0), inside, nameless };
        const Imported i = ImportFixture::Import(p);
        size_t left = 0;
        for (const std::string& n : i.skipped)
            left += MiniBIM::RevitReport::CountLeftOut(n).value_or(0);
        Check(left == 2, "一个在族里、一个没名字：略去两个" + Skipped(i));
    }

    void AFileThatIsNotAProjectIsRefused()
    {
        std::string e;
        Check(!ImportFixture::TryImport(Fixture::Of("not a revit file"), &e) && Contains(e, "Compound File"), "不是复合文档的文件被拒绝：" + e);
        const Bytes family = Fixture::Container({ { "BasicFileInfo", Fixture::BasicFileInfo(Fixture::Basic{}) } });
        e.clear();
        Check(!ImportFixture::TryImport(family, &e) && Contains(e, "Formats/Latest"), "没有类表的文件被拒绝：" + e);
        // 项目里的元素不是标高；没有数据的墙沿它自己的包围盒进来：它是文件里的墙，说明里讲没读到什么
        Project p(2024);
        p.recs   = { Rec(100, Walls) };
        p.levels = { Lev(10, "Level 1", 0.0) };
        Check(!WallsOf(ImportFixture::Import(p)).empty(), "没有数据的墙记录仍是一面墙");
    }

    void ANoteListsTheFirstEightIds()
    {
        const auto said = [](uint32_t count)
        {
            Project p = Base(2024);
            Rec symbol(650, ImportFixture::Columns);
            symbol.placed = false;
            p.recs.push_back(symbol);
            for (uint32_t id = 100; id < 100 + count; ++id)
                p.recs.push_back(Rec(id, ImportFixture::Columns).At({ 0, 0, 0, 1, 1, 10 }, { 10, 11, 650, id }));
            return Said(ImportFixture::Import(p), "columns");
        };
        Check(ImportFixture::EndsWith(said(8), "(ids 100, 101, 102, 103, 104, 105, 106, 107)"), "八个 id 全列出：" + said(8));
        Check(ImportFixture::EndsWith(said(9), "(ids 100, 101, 102, 103, 104, 105, 106, 107, …)"), "多于八个时说还有：" + said(9));
    }

    void UnnamedLevelsAreCountedInTheSingularOrThePlural()
    {
        const auto said = [](uint32_t unnamed)
        {
            Project p = Base(2024);
            for (uint32_t k = 0; k < unnamed; ++k)
            {
                Lev l(20 + k, "x", 30.0);
                l.named = false;
                p.levels.push_back(l);
            }
            return Said(ImportFixture::Import(p), "no name or elevation");
        };
        Check(ImportFixture::StartsWith(said(1), "1 level left out, no name"), "单数：" + said(1));
        Check(ImportFixture::StartsWith(said(2), "2 levels left out, no name"), "复数：" + said(2));
    }

    void WhatAnElementWasInTheFileIsItsKindAndId()
    {
        using Was = std::optional<std::pair<std::string, std::variant<uint32_t, std::string>>>;
        const auto is = [](std::string_view from, const Was& want) { return Rvt::Origin(from) == want; };
        Check(is("Wall 423099", std::pair{ std::string("wall"), std::variant<uint32_t, std::string>(423099u) }), "墙与它的 id");
        Check(is("Structural column 12", std::pair{ std::string("structural column"), std::variant<uint32_t, std::string>(12u) }), "两个词的类");
        Check(is("Level 'Far away'", std::pair{ std::string("level"), std::variant<uint32_t, std::string>(std::string("'Far away'")) }), "标高是它的名字");
        Check(is("Level 'Roof 2'", std::pair{ std::string("level"), std::variant<uint32_t, std::string>(std::string("'Roof 2'")) }), "标高的名字可以以数字结尾");
        Check(is("Wall", std::nullopt) && is("", std::nullopt), "只有类或什么都没有的不是");
    }

    // ── 墙（walls.rs）──────────────────────────────────────────

    void AStraightWallComesIn()
    {
        for (const uint32_t release : { 2019u, 2023u, 2024u, 2026u })
        {
            Project p = Base(release);
            Straight(p, 100, false);
            const Imported i = ImportFixture::Import(p);
            const auto ws = WallsOf(i);
            if (ws.size() != 1)
            {
                Check(false, "一面直墙进来，" + Of(release) + Skipped(i));
                continue;
            }
            const Bim::Wall& w = *ws[0].second;
            Check(w.start == Bim::P2(0, 0) && w.end == Bim::P2(6096, 0), "直墙的定位线，" + Of(release));
            Check(w.level == LevelNamed(i, "Level 1") && w.topLevel == LevelNamed(i, "Level 2"), "直墙的底部与顶部标高，" + Of(release));
            Check(w.height == 3048.0 && w.baseOffset == 0.0 && w.arc == 0.0, "直墙的高度、偏移与弧，" + Of(release));
            const Bim::WallType& t = Get(i.model.types.wall, w.typeName);
            Check(w.typeName == "Brick wall" && t.thickness == 228.6, "直墙的类型与厚度，" + Of(release));
            // 各层按材质：项目建好后库给每种材质自己的 ID 与 GUID
            std::vector<std::tuple<Bim::Function, std::string, double>> layers;
            for (const Bim::Layer& l : t.layers)
                layers.emplace_back(l.function, l.material, l.thickness);
            Check(layers == std::vector<std::tuple<Bim::Function, std::string, double>>{ { Bim::Function::Finish, "Brick", 76.2 }, { Bim::Function::Structure, "Concrete", 152.4 } },
                  "直墙的两层，" + Of(release));
            Check(i.model.materials.contains("Brick") && i.model.materials.contains("Concrete"), "材质进了项目，" + Of(release));
            Check(i.skipped.empty(), "直墙没有说明，" + Of(release) + Skipped(i));
        }
    }

    void AMaterialA32BitFileDoesNotNameIsNamedByItsData()
    {
        // 到 2023 版对象可以不带名字（这里它的名字格是参数条目）；材质的数据里有
        Project p = Base(2023);
        Straight(p, 100, false);
        p.mats[0].name.clear();
        Bytes named = NameBytes("Brick");
        named.insert(named.end(), 4, 0x12);
        p.data.push_back(Block{ 700, named });
        const Imported i = ImportFixture::Import(p);
        const auto& layers = Get(i.model.types.wall, "Brick wall").layers;
        Check(!layers.empty() && layers[0].material == "Brick", "2023 版的材质由数据命名" + Skipped(i));
        // 2024 起名字在对象里，只在数据里的名字不读
        Project q = Base(2024);
        Straight(q, 100, false);
        q.mats[0].name.clear();
        q.data.push_back(Block{ 700, NameBytes("Brick") });
        const Imported j = ImportFixture::Import(q);
        const auto& later = Get(j.model.types.wall, "Brick wall").layers;
        Check(!later.empty() && later[0].material.empty(), "2024 版不读数据里的名字");
    }

    void AWallWhoseExteriorIsToTheRightIsTurnedRound()
    {
        Project p = Base(2024);
        Straight(p, 100, true);
        const Imported i = ImportFixture::Import(p);
        const auto ws = WallsOf(i);
        Check(!ws.empty() && ws[0].second->start.x == 6096.0 && ws[0].second->end.x == 0.0, "外侧在右边的墙掉过头来：MiniBIM 的墙外侧在左边");
        // 数据里没有朝向时墙照画的方向，并说明
        Project q = Base(2024);
        Straight(q, 100, false);
        q.data[1].payload.resize(8 + 4 + 8 * 8);   // 只有定位线，没有后面的朝向
        const Imported j = ImportFixture::Import(q);
        const auto qs = WallsOf(j);
        Check(!qs.empty() && qs[0].second->start.x == 0.0 && qs[0].second->end.x == 6096.0, "没有朝向的墙照画的方向");
        Check(Says(j, { "1 wall:", "which side is the exterior", "ids 100" }), "没有朝向的墙说明了" + Skipped(j));
    }

    void ACurvedWallComesInAsTheArcItTurns()
    {
        const double halfPi = std::numbers::pi / 2.0;
        // 半径 10 英尺、圆心 (0, 0) 的四分之一圆，从 (10, 0) 到 (0, 10)，从上面看逆时针
        const Bytes ccw = ArcBytes({ 0.0, halfPi }, { 1, 0, 0 }, { 0, 1, 0 }, 10.0, { 0, 0, 0 });
        // 同样的四分之一圆，坐标系翻过来（两轴的叉积朝下）：从 (10, 0) 顺时针到 (0, -10)
        const Bytes cw = ArcBytes({ 0.0, halfPi }, { 1, 0, 0 }, { 0, -1, 0 }, 10.0, { 0, 0, 0 });
        const std::tuple<Bytes, bool, std::pair<Bim::P2, Bim::P2>, double> cases[] = {
            { ccw, false, { Bim::P2(3048, 0), Bim::P2(0, 3048) }, 90.0 },
            { cw, false, { Bim::P2(3048, 0), Bim::P2(0, -3048) }, -90.0 },
            { ccw, true, { Bim::P2(0, 3048), Bim::P2(3048, 0) }, -90.0 },
        };
        for (const auto& [arc, flip, ends, turns] : cases)
        {
            Project p = Base(2024);
            p.recs.push_back(Rec(100, Walls).At({ 0, -10.4, 0, 10.4, 10.4, 10 }, { 10, 11, 500, 100 }));
            p.data.push_back(Block{ 100, Fixture::Concat({ arc, OrientationBytes(0, 1, flip, true) }) });
            const Imported i = ImportFixture::Import(p);
            const auto ws = WallsOf(i);
            const std::string label = std::string("弧墙，翻转 ") + (flip ? "是" : "否") + "，转 " + std::to_string(turns);
            if (ws.size() != 1)
            {
                Check(false, label + Skipped(i));
                continue;
            }
            const Bim::Wall& w = *ws[0].second;
            Check(w.start == ends.first && w.end == ends.second && std::abs(w.arc - turns) < 1e-6, label);
        }
    }

    void AWallsLevelsComeFromTheTwoItNamesAndItsBox()
    {
        const auto wall = [](std::vector<uint64_t> refs, const ProjectFixture::Box& bbox)
        {
            Project p = Base(2024);
            p.levels.push_back(Lev(12, "Level 3", 20.0));
            p.recs.push_back(Rec(100, Walls).At(bbox, std::move(refs)));
            p.data.push_back(Block{ 100, Fixture::Concat({ LineBytes(0, 20, { 0, 0, 0 }, { 1, 0, 0 }), OrientationBytes(0, 1, false, true) }) });
            Imported i = ImportFixture::Import(p);
            const auto ws = WallsOf(i);
            std::optional<Bim::Wall> w;
            if (!ws.empty())
                w = *ws[0].second;
            return std::pair{ std::move(i), w };
        };
        const auto withTop = [](double top) { return ProjectFixture::Box{ 0, -0.4, 0, 20, 0.4, top }; };
        // 到说的标高：包围盒在它的高程处结束
        {
            const auto [i, w] = wall({ 12, 10, 500 }, withTop(20.0));
            Check(w && w->level == LevelNamed(i, "Level 1") && w->topLevel == LevelNamed(i, "Level 3") && w->height == 6096.0, "墙到它说的标高");
        }
        // 包围盒没到它约束到的标高：与包围盒一样高，非连接
        {
            const auto [i, w] = wall({ 11, 10, 500 }, withTop(9.0));
            Check(w && w->level == LevelNamed(i, "Level 1") && !w->topLevel && w->height == 2743.2, "没到顶部标高的墙与包围盒一样高");
        }
        // 底部是包围盒底部或以下较高的那个标高，不论列表先说哪个
        {
            const auto [i, w] = wall({ 11, 12, 500 }, { 0, -0.4, 10, 20, 0.4, 19 });
            Check(w && w->level == LevelNamed(i, "Level 2") && w->baseOffset == 0.0, "底部是包围盒底部或以下较高的标高");
        }
        // 从标高之上起的墙：偏移
        {
            const auto [i, w] = wall({ 10, 500 }, { 0, -0.4, 1, 20, 0.4, 9 });
            Check(w && w->level == LevelNamed(i, "Level 1") && w->baseOffset == 304.8 && !w->topLevel && w->height == 2438.4, "从标高之上起的墙有偏移");
        }
        // 说的不是项目标高的不算；一个也没说时，是包围盒底部或以下的标高
        {
            const auto [i, w] = wall({ 999, 500 }, { 0, -0.4, 10.5, 20, 0.4, 19 });
            Check(w && w->level == LevelNamed(i, "Level 2"), "不是标高的引用不算");
        }
    }

    void TheNewestCopyOfAWallIsTheCurrentOne()
    {
        Project older = Base(2024);
        Straight(older, 100, false);
        Project newer = older;
        newer.recs.clear();
        newer.data[1].payload = Fixture::Concat({ LineBytes(0, 10, { 0, 5, 0 }, { 1, 0, 0 }), OrientationBytes(0, 1, false, true) });
        newer.recs.push_back(Rec(100, Walls).At({ 0, 4.6, 0, 10, 5.4, 10 }, { 10, 11, 500, 100 }));
        Fixture::Basic b;
        b.release = 2024;
        const Bytes basic  = Fixture::BasicFileInfo(b);
        const Bytes schema = Fixture::Gzip(Project::SchemaBytes());
        const Bytes table  = Fixture::Global(newer.ElementTable());
        const Bytes a = Fixture::Partition({ older.PartitionBytes() }), c = Fixture::Partition({ newer.PartitionBytes() });
        const auto wallIn = [&](const Bytes& at21, const Bytes& at22)
        {
            const auto i = ImportFixture::TryImport(Fixture::Container({ { "BasicFileInfo", basic },
                                                                          { "Formats/Latest", schema },
                                                                          { "Global/ElemTable", table },
                                                                          { "Partitions/21", at21 },
                                                                          { "Partitions/22", at22 } }));
            const auto ws = i ? WallsOf(*i) : std::vector<std::pair<Bim::Id, const Bim::Wall*>>{};
            return ws.empty() ? std::pair{ -1.0, -1.0 } : std::pair{ ws[0].second->start.y, ws[0].second->end.x };
        };
        // 编号大的分区是新的，不论名字排在前面的是哪个流
        Check(wallIn(a, c) == std::pair{ 1524.0, 3048.0 } && wallIn(c, a) == std::pair{ 0.0, 6096.0 }, "墙的数据与记录取最新的一份");
    }

    void AWallTheReaderCannotDescribeIsLeftOutWithTheReason()
    {
        Project p = Base(2024);
        Straight(p, 100, false);
        // 一点数据也没有
        p.recs.push_back(Rec(101, Walls).At({ 0, 0, 0, 5, 0.3, 10 }, { 10, 11, 500, 101 }));
        // 不水平的线
        p.recs.push_back(Rec(102, Walls).At({ 0, 0, 0, 5, 0.3, 10 }, { 10, 11, 500, 102 }));
        p.data.push_back(Block{ 102, LineBytes(0, 5, { 0, 0, 0 }, { 0.6, 0, 0.8 }) });
        // 没说墙类型的记录，与说了两个的
        p.recs.push_back(Rec(103, Walls).At({ 0, 0, 0, 5, 0.3, 10 }, { 10, 11, 103 }));
        p.types.push_back(TypeRec(501, Walls));
        p.recs.push_back(Rec(104, Walls).At({ 0, 0, 0, 5, 0.3, 10 }, { 10, 11, 500, 501, 104 }));
        for (const uint32_t id : { 103u, 104u })
            p.data.push_back(Block{ id, LineBytes(0, 5, { 0, 0, 0 }, { 1, 0, 0 }) });
        // 没有层的类型
        p.types.push_back(TypeRec(502, Walls));
        p.data.push_back(Block{ 502, NameBytes("Curtain") });
        p.recs.push_back(Rec(105, Walls).At({ 0, 0, 0, 5, 0.3, 10 }, { 10, 11, 502, 105 }));
        p.data.push_back(Block{ 105, LineBytes(0, 5, { 0, 0, 0 }, { 1, 0, 0 }) });
        // 没有体积的符号，与不是放置的
        p.recs.push_back(Rec(106, Walls).At({ 0, 0, 0, 5, 0.3, 0 }, { 10, 11, 500, 106 }));
        Rec unplaced = Rec(107, Walls).At({ 0, 0, 0, 5, 0.3, 10 }, { 10, 11, 500, 107 });
        unplaced.placed = false;
        p.recs.push_back(unplaced);
        const Imported i = ImportFixture::Import(p);
        Check(WallsOf(i).size() == 5, "读不全的墙里五面进来" + Skipped(i));
        // 没有定位线的墙沿它自己的包围盒进来；线不水平的略去
        Check(Contains(Said(i, "no location line"), "101") && Contains(Said(i, "long side of its own box"), "101"), "没有定位线的墙沿包围盒进来" + Skipped(i));
        Check(Contains(Said(i, "not level"), "102"), "线不水平的墙略去");
        // 类型有名字没有层：幕墙或店面，以它自己包围盒给的平面进来
        Check(Contains(Said(i, "its type has no layers"), "103, 104"), "没说一个墙类型的墙说明了" + Skipped(i));
        Check(Contains(Said(i, "comes in as the plane"), "105"), "没有层的类型的墙是包围盒的平面" + Skipped(i));
        Check(std::none_of(i.skipped.begin(), i.skipped.end(), [](const std::string& s) { return Contains(s, "106") || Contains(s, "107"); }),
              "符号不是墙，没放置的记录不是实例");
    }

    void TypesWithOneNameAndDifferentLayersAreToldApart()
    {
        Project p = Base(2024);
        Straight(p, 100, false);
        p.types.push_back(TypeRec(503, Walls));
        p.data.push_back(Block{ 503, Fixture::Concat({ NameBytes("Brick wall"), LayersBytes({ { 0.5, 701, 1 } }, 2024) }) });
        p.recs.push_back(Rec(101, Walls).At({ 0, 4.6, 0, 10, 5.4, 10 }, { 10, 11, 503, 101 }));
        p.data.push_back(Block{ 101, LineBytes(0, 10, { 0, 5, 0 }, { 1, 0, 0 }) });
        // 第一种类型的第三面墙与它共用模型的类型
        p.recs.push_back(Rec(102, Walls).At({ 0, 9.6, 0, 10, 10.4, 10 }, { 10, 11, 500, 102 }));
        p.data.push_back(Block{ 102, LineBytes(0, 10, { 0, 10, 0 }, { 1, 0, 0 }) });
        const Imported i = ImportFixture::Import(p);
        std::vector<std::string> names;
        for (const auto& [_, w] : WallsOf(i))
            names.push_back(w->typeName);
        std::sort(names.begin(), names.end());
        Check(names == std::vector<std::string>{ "Brick wall", "Brick wall", "Brick wall (2)" }, "同名不同层的类型分开");
        Check(Get(i.model.types.wall, "Brick wall (2)").thickness == 152.4, "第二种类型的厚度");
    }

    void WallsWhoseTypesHaveNoLayersComeInAsThePlane()
    {
        Project p = Base(2024);
        Straight(p, 100, false);
        // 有名字没有层的类型（店面），两面墙用它，以及什么也没有的类型
        p.types.push_back(TypeRec(503, Walls));
        p.types.push_back(TypeRec(504, Walls));
        p.data.push_back(Block{ 503, NameBytes("Storefront") });
        for (const auto& [id, ty, y] : { std::tuple{ 101u, 503u, 5.0 }, std::tuple{ 102u, 503u, 8.0 }, std::tuple{ 103u, 504u, 10.0 } })
        {
            p.recs.push_back(Rec(id, Walls).At({ 0, y - 0.4, 0, 10, y + 0.4, 10 }, { 10, 11, ty, id }));
            p.data.push_back(Block{ id, LineBytes(0, 10, { 0, y, 0 }, { 1, 0, 0 }) });
        }
        const Imported i = ImportFixture::Import(p);
        // 没有复合结构的类型是幕墙或店面：它的墙各以自己包围盒给的平面进来，说明讲没读到什么；
        // 数据什么也没说的类型以厚度命名，同楼板的
        Check(WallsOf(i).size() == 4, "没有层的类型的墙也进来" + Skipped(i));
        const std::string said = Said(i, "comes in as the plane");
        Check(Contains(said, "3 walls") && Contains(said, "101") && Contains(said, "102") && Contains(said, "103"), "三面墙都说明了" + Skipped(i));
        Check(Get(i.model.types.wall, "Storefront").layers.size() == 1, "一层，包围盒那么厚");
    }

    void AMutationSweepOverWalls()
    {
        for (const uint32_t release : { 2019u, 2024u })
        {
            Project p = Base(release);
            Straight(p, 100, false);
            Straight(p, 101, true);
            const double halfPi = std::numbers::pi / 2.0;
            p.recs.push_back(Rec(102, Walls).At({ 0, -10.4, 0, 10.4, 10.4, 10 }, { 10, 11, 500, 102 }));
            p.data.push_back(Block{ 102, Fixture::Concat({ ArcBytes({ 0.0, halfPi }, { 1, 0, 0 }, { 0, 1, 0 }, 10.0, { 0, 0, 0 }),
                                                           OrientationBytes(0, 1, false, !(release >= 2015 && release <= 2020)) }) });
            p.members = 3;
            const Imported clean = ImportFixture::Import(p);
            Check(WallsOf(clean).size() == 3, "没受损的三面墙都进来，" + Of(release) + Skipped(clean));
            const auto [ok, failed] = ImportFixture::Sweep(p, 0x9e37'79b9'7f4a'7c15ull ^ release, ImportFixture::SweepRounds, "受损的墙不崩溃、读出的过检查，" + Of(release),
                                                           [](const Imported& i) { return WallsOf(i).size() <= 3; });
            Check(ok * 8 > ImportFixture::SweepRounds * 3 && failed * 8 < ImportFixture::SweepRounds * 5, Of(release) + "：" + std::to_string(ok) + " 份读得出，" + std::to_string(failed) + " 份拒绝");
        }
    }

    void TwoTypesOfOneNameAreOneTypeWhenTheirLayersMatch()
    {
        Project p = Base(2024);
        // 501 的名字与层同 500；502 名字与厚度相同，砖与混凝土对调
        for (const auto& [tid, outer] : { std::pair{ 501u, 700u }, std::pair{ 502u, 701u } })
        {
            p.types.push_back(TypeRec(tid, Walls));
            p.data.push_back(Block{ tid, Fixture::Concat({ NameBytes("Brick wall"), LayersBytes({ { 0.25, outer, 4 }, { 0.5, 701, 1 } }, 2024) }) });
        }
        uint32_t k = 0;
        for (const uint64_t tid : { 500u, 501u, 502u })
        {
            const uint32_t id = 100 + k;
            const double y = 5.0 * k++;
            WallOf(p, id, { 0, y - 0.4, 0, 20, y + 0.4, 10 }, { 10, 11, tid, id }, LineBytes(0, 20, { 0, y, 0 }, { 1, 0, 0 }));
        }
        const Imported i = ImportFixture::Import(p);
        std::vector<std::string> names;
        for (const auto& [_, w] : WallsOf(i))
            names.push_back(w->typeName);
        Check(names == std::vector<std::string>{ "Brick wall", "Brick wall", "Brick wall (2)" }, "同名的类型层一样时是一种，不一样时两种" + Skipped(i));
    }

    void AWallOnAnUpperLevelAndAClimbingLine()
    {
        Project p = Base(2024);
        // 在标高 2（10 英尺高），两个标高都说了：底部是标高 2，包围盒从那里起
        WallOf(p, 100, { 0, -0.4, 10, 20, 0.4, 20 }, { 10, 11, 500, 100 }, LineBytes(0, 20, { 0, 0, 10 }, { 1, 0, 0 }));
        // 12 英尺里爬升 16 英尺的线
        WallOf(p, 101, { 0, -0.4, 0, 12, 0.4, 10 }, { 10, 11, 500, 101 }, LineBytes(0, 20, { 0, 0, 0 }, { 0.6, 0, 0.8 }));
        const Imported i = ImportFixture::Import(p);
        const auto ws = WallsOf(i);
        Check(ws.size() == 1 && ws[0].second->level == LevelNamed(i, "Level 2") && ws[0].second->baseOffset == 0.0 && ws[0].second->height == 3048.0,
              "上层的墙的定位线在那个高度" + Skipped(i));
        Check(Says(i, { "not level", "101" }), "爬升的线不是墙" + Skipped(i));
    }

    void AnArcIsReadByItsAnglesAndAxes()
    {
        const double halfPi = std::numbers::pi / 2.0;
        // 坐标系转过四分之一（x 朝北、y 朝西），逆时针，从 0.5 弧度起：从 r (-sin a, cos a) 到 r (-cos a, -sin a)
        Project p = Base(2024);
        WallOf(p, 100, { -10.4, -10.4, 0, 10.4, 10.4, 10 }, { 10, 11, 500, 100 }, ArcBytes({ 0.5, 0.5 + halfPi }, { 0, 1, 0 }, { -1, 0, 0 }, 10.0, { 0, 0, 0 }));
        // 不在平面里：x 轴竖起来，或只有 y 轴竖起来
        WallOf(p, 101, { -10.4, -10.4, 0, 10.4, 10.4, 10 }, { 10, 11, 500, 101 }, ArcBytes({ 0.0, halfPi }, { 0, 0, 1 }, { 0, 1, 0 }, 10.0, { 0, 0, 0 }));
        WallOf(p, 102, { -10.4, -10.4, 0, 10.4, 10.4, 10 }, { 10, 11, 500, 102 }, ArcBytes({ 0.0, halfPi }, { 1, 0, 0 }, { 0, 0, 1 }, 10.0, { 0, 0, 0 }));
        const Imported i = ImportFixture::Import(p);
        const auto ws = WallsOf(i);
        const double sin = std::sin(0.5), cos = std::cos(0.5);
        const auto mm = [](double v) { return std::round(v * 3048.0 * 1000.0) / 1000.0; };
        Check(ws.size() == 1 && ws[0].second->start == Bim::P2(mm(-sin), mm(cos)) && ws[0].second->end == Bim::P2(mm(-cos), mm(-sin)) &&
                  std::abs(ws[0].second->arc - 90.0) < 1e-6,
              "圆弧按它的角度与轴读，不论从哪起、坐标系怎么转" + Skipped(i));
        Check(Says(i, { "not in plan", "101", "102" }), "不在平面里的圆弧说明了" + Skipped(i));
    }

    void AWallThatNamesNoLevelStandsOnTheOneBelowItsBottom()
    {
        Project p = Base(2024);
        WallOf(p, 100, { 0, -0.4, 0, 20, 0.4, 9 }, { 500, 100 }, LineBytes(0, 20, { 0, 0, 0 }, { 1, 0, 0 }));
        const Imported i = ImportFixture::Import(p);
        const auto ws = WallsOf(i);
        Check(!ws.empty() && ws[0].second->level == LevelNamed(i, "Level 1"), "没说标高的墙立在它底部以下的标高" + Skipped(i));
        // 同一高度的第二个标高：它在哪个上面？
        p.levels.push_back(Lev(12, "Level 1b", 0.0));
        const Imported j = ImportFixture::Import(p);
        Check(WallsOf(j).empty() && Says(j, { "no level", "100" }), "两个标高一样高时不立在任何一个上" + Skipped(j));
    }

    void ATypeWhoseLayersHaveNoWidthIsNoWallType()
    {
        for (const uint32_t release : { 2024u, 2026u })
        {
            Project p = Base(release);
            // 膜没有宽度：只有膜的类型不是墙
            p.types.push_back(TypeRec(501, Walls));
            p.data.push_back(Block{ 501, Fixture::Concat({ NameBytes("Membrane"), LayersBytes({ { 0.0, std::nullopt, 100 } }, release) }) });
            // 上半不为 0 的材质不是 ElementId
            p.types.push_back(TypeRec(502, Walls));
            Bytes layers = LayersBytes({ { 0.25, 700, 4 } }, release);
            layers[22] = 1;
            p.data.push_back(Block{ 502, Fixture::Concat({ NameBytes("Damaged"), layers }) });
            for (const auto& [id, tid] : { std::pair{ 100u, 501u }, std::pair{ 101u, 502u } })
                WallOf(p, id, { 0, -0.4, 0, 20, 0.4, 10 }, { 10, 11, tid, id }, LineBytes(0, 20, { 0, 5.0 * (id - 100), 0 }, { 1, 0, 0 }));
            const Imported i = ImportFixture::Import(p);
            // 膜没有宽度，它的类型不是墙，墙略去；没框住的层列表读作没有列表，类型读作幕墙的，以包围盒给的平面进来并说明
            Check(WallsOf(i).size() == 1, "只有膜的类型的墙略去，" + Of(release) + Skipped(i));
            Check(Says(i, { "no layers", "Membrane", "ids 100" }), "只有膜的类型说明了，" + Of(release) + Skipped(i));
            Check(Says(i, { "comes in as the plane", "101" }), "材质不是 id 的层不是层，" + Of(release) + Skipped(i));
        }
    }

    void AStructuralDeckWithNoWidthIsLeftOutOfTheLayers()
    {
        // Revit 让结构压型板取上面那层的厚度、自己不写：结构样例的一种楼板（金属压型板上的混凝土）就有这样的层，
        // 模型不接受不是膜又没有厚度的层，导入整个文件被拒绝过
        for (const uint32_t release : { 2024u, 2026u })
        {
            Project p = Base(release);
            p.types.push_back(TypeRec(503, Walls));
            p.data.push_back(Block{ 503, Fixture::Concat({ NameBytes("Deck"), LayersBytes({ { 0.5, 701, 1 }, { 0.0, std::nullopt, 200 } }, release) }) });
            WallOf(p, 100, { 0, -0.4, 0, 20, 0.4, 10 }, { 10, 11, 503, 100 }, LineBytes(0, 20, { 0, 0, 0 }, { 1, 0, 0 }));
            const Imported i = ImportFixture::Import(p);
            const auto ws = WallsOf(i);
            const Bim::WallType* t = ws.size() == 1 ? &Get(i.model.types.wall, ws[0].second->typeName) : nullptr;
            Check(t && t->layers.size() == 1 && t->thickness == 152.4, "没有自己宽度的压型板不算一层，" + Of(release) + Skipped(i));
        }
    }

    void ALayerListIsATypesOnlyWhenItIsFramed()
    {
        // `ff ff ff ff` 与别的标签，或对的标签前面没有 `ff ff ff ff`，都框不住什么
        for (const int damage : { 0, 1 })
        {
            Project p = Base(2024);
            Bytes layers = LayersBytes({ { 0.25, 700, 4 }, { 0.5, 701, 1 } }, 2024);
            if (damage == 0)
                layers[4] ^= 0x55;
            else
                std::fill(layers.begin(), layers.begin() + 4, uint8_t(0x11));
            p.data[0].payload = Fixture::Concat({ NameBytes("Brick wall"), layers });
            Straight(p, 100, false);
            const Imported i = ImportFixture::Import(p);
            // 没框住的层列表不是层列表：类型读作幕墙的，它的墙以包围盒给的平面进来，说明这么讲
            Check(WallsOf(i).size() == 1 && Says(i, { "no layers", "comes in as the plane" }), "框不住的层列表不是类型的，第 " + std::to_string(damage) + " 种" + Skipped(i));
        }
    }

    void AMaterialsNameIsReadByEachRule()
    {
        using ProjectFixture::NameStyle;
        const auto layers = [](uint32_t release, NameStyle style)
        {
            Project p = Base(release);
            p.nameStyle = style;
            Straight(p, 100, false);
            const Imported i = ImportFixture::Import(p);
            const auto& t = Get(i.model.types.wall, "Brick wall");
            return t.layers.size() < 2 ? std::pair{ std::string("?"), std::string("?") } : std::pair{ t.layers[0].material, t.layers[1].material };
        };
        const std::pair<std::string, std::string> named{ "Brick", "Concrete" }, none{};
        const std::tuple<uint32_t, NameStyle, std::pair<std::string, std::string>, const char*> cases[] = {
            { 2024, NameStyle::PatternFramed, named, "PatternFramed" }, { 2024, NameStyle::ParamEntry, named, "ParamEntry" },
            { 2024, NameStyle::Closed, named, "Closed" },               { 2023, NameStyle::ParamEntry, named, "ParamEntry" },
            { 2019, NameStyle::ParamEntry, named, "ParamEntry" },       { 2023, NameStyle::PatternFramed, none, "PatternFramed" },
            { 2023, NameStyle::Closed, none, "Closed" },
        };
        for (const auto& [release, style, want, what] : cases)
            Check(layers(release, style) == want, "材质名字的规则 " + std::string(what) + "，" + Of(release));
    }

    void ALayerWithNoMaterialTakesItsCategorys()
    {
        const auto layerMaterial = [](uint32_t release, std::vector<std::pair<int64_t, uint32_t>> written)
        {
            Project p = Base(release);
            p.data[0].payload = Fixture::Concat({ NameBytes("Brick wall"), LayersBytes({ { 0.25, std::nullopt, 4 }, { 0.5, 701, 1 } }, release) });
            p.categoryMaterials = std::move(written);
            Straight(p, 100, false);
            const Imported i = ImportFixture::Import(p);
            const auto& layers = Get(i.model.types.wall, "Brick wall").layers;
            return layers.empty() ? std::string("?") : layers[0].material;
        };
        for (const auto& [release, want] : { std::pair{ 2019u, "Brick" }, std::pair{ 2023u, "Brick" }, std::pair{ 2024u, "Brick" }, std::pair{ 2025u, "" }, std::pair{ 2026u, "" } })
            Check(layerMaterial(release, { { Walls, 700 } }) == want, "没有材质的层取类别的（到 2024 版），" + Of(release));
        // 类别有两个值，或只有别的类别的：没有
        Check(layerMaterial(2024, { { Walls, 700 }, { Walls, 701 } }).empty(), "类别有两个材质时不取");
        Check(layerMaterial(2024, { { ImportFixture::Floors, 700 } }).empty(), "别的类别的材质不取");
        Check(layerMaterial(2023, { { Walls, 700 }, { Walls, 701 } }).empty(), "类别有两个材质时不取，Revit 2023");
    }

    void AMaterialFoundTwiceIsNamedOnlyWhereItsCopiesAgree()
    {
        const auto layer = [](std::optional<Mat> extra)
        {
            Project p = Base(2024);
            if (extra)
                p.mats.push_back(*extra);
            Straight(p, 100, false);
            const Imported i = ImportFixture::Import(p);
            const auto& layers = Get(i.model.types.wall, "Brick wall").layers;
            return layers.empty() ? std::string("?") : layers[0].material;
        };
        Check(layer(std::nullopt) == "Brick", "一份材质");
        Check(layer(Mat{ 700, "Brick" }) == "Brick", "同一个名字两次");
        Check(layer(Mat{ 700, "Clinker" }).empty(), "一种材质两个名字时不命名");
    }
}

namespace RvtTest
{
    void TestImport()
    {
        LevelsComeInLowestFirst();
        LevelsThatShareANameAreToldApart();
        WhatTheInventoryCannotMakeALevelOfIsListed();
        TheReaderTheWindowIsGivenNamesTheProjectForItsFile();
        WhatTheReaderLeavesOutIsWhatTheWindowCountsAsLeftOut();
        AFileThatIsNotAProjectIsRefused();
        ANoteListsTheFirstEightIds();
        UnnamedLevelsAreCountedInTheSingularOrThePlural();
        WhatAnElementWasInTheFileIsItsKindAndId();
    }

    void TestWalls()
    {
        AStraightWallComesIn();
        AMaterialA32BitFileDoesNotNameIsNamedByItsData();
        AWallWhoseExteriorIsToTheRightIsTurnedRound();
        ACurvedWallComesInAsTheArcItTurns();
        AWallsLevelsComeFromTheTwoItNamesAndItsBox();
        TheNewestCopyOfAWallIsTheCurrentOne();
        AWallTheReaderCannotDescribeIsLeftOutWithTheReason();
        TypesWithOneNameAndDifferentLayersAreToldApart();
        WallsWhoseTypesHaveNoLayersComeInAsThePlane();
        AMutationSweepOverWalls();
        TwoTypesOfOneNameAreOneTypeWhenTheirLayersMatch();
        AWallOnAnUpperLevelAndAClimbingLine();
        AnArcIsReadByItsAnglesAndAxes();
        AWallThatNamesNoLevelStandsOnTheOneBelowItsBottom();
        ATypeWhoseLayersHaveNoWidthIsNoWallType();
        AStructuralDeckWithNoWidthIsLeftOutOfTheLayers();
        ALayerListIsATypesOnlyWhenItIsFramed();
        AMaterialsNameIsReadByEachRule();
        ALayerWithNoMaterialTakesItsCategorys();
        AMaterialFoundTwiceIsNamedOnlyWhereItsCopiesAgree();
    }
}
