// 模型的三维场景（src/View/ModelScene）：示例房屋经 SMLib 建成实体、离散成三角形与边线
#include "Commands/Commands.h"
#include "Commands/Sample.h"
#include "Commands/Session.h"
#include "Solid/SMLib/SMLibKernel.h"
#include "View/ModelScene.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace MiniBIM;

namespace
{
    int g_failed = 0;
    void Check(bool ok, const std::string& label)
    {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
        g_failed += ok ? 0 : 1;
    }

    bool Near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

    constexpr double kPi = 3.14159265358979323846;

    Bim::Session Sample()
    {
        Bim::Session s;
        std::vector<Bim::Json> out;
        std::string error;
        if (!s.Script(Bim::SampleScript, out, error))
            std::printf("  示例：%s\n", error.c_str());
        return s;
    }

    void TestNoKernel()
    {
        const Bim::Session s;
        ModelScene scene;
        std::string error;
        Check(!scene.Build(s.GetModel(), nullptr, error) && !error.empty() && scene.Empty(), "三维场景：没有实体内核时什么都不建，说明原因");
    }

    void TestSampleHouse()
    {
        const Bim::Session s = Sample();
        auto kernel = Solid::CreateSMLibKernel();
        ModelScene scene;
        std::string error;
        const bool built = scene.Build(s.GetModel(), kernel.get(), error);
        if (!built)
            std::printf("  %s\n", error.c_str());
        const ModelScene::Counts& c = scene.GetCounts();
        Check(built && c.walls == 7 && c.floors == 1 && c.doors == 4 && c.windows == 5 && c.ceilings == 4 && c.roofs == 1 && c.components == 11,
              "三维场景：示例房屋的墙、楼板、门、窗、天花板、屋顶、家具都建出来");
        Check(!scene.GetTriangles().empty() && scene.GetTriangles().size() % 3 == 0 && !scene.GetEdges().empty() && scene.GetEdges().size() % 2 == 0,
              "三维场景：有三角形和边线");
        // 外墙 300 毫米厚、中心线在 10 × 7 米的轴线上；楼板 200 毫米厚，顶面在标高上。屋顶伸出外墙面 500，
        // 在 Level 2（3000）上 30° 四坡：檐口比标高低 500 tan 30°，8.3 米宽的短边中间是屋脊，上面是 300 厚的板（铅垂 300 / cos 30°）
        const AABB& b = scene.GetBounds();
        const double tan30 = std::tan(kPi / 6.0), ridge = 3000.0 + 3650.0 * tan30 + 300.0 / std::cos(kPi / 6.0);
        Check(Near(b.Min.x, -650.0, 1.0) && Near(b.Min.y, -650.0, 1.0) && Near(b.Max.x, 10650.0, 1.0) && Near(b.Max.y, 7650.0, 1.0) &&
                  Near(b.Min.z, -200.0, 1.0) && Near(b.Max.z, ridge, 1.0),
              "三维场景：范围是屋顶的檐口、楼板底到屋脊");
        // 拾取：从上往下先碰到屋顶；从屋檐下面朝上碰到的是天花板（屋顶下面）以外的东西也行，这里看从下往上先碰到楼板
        const Bim::Model& m = s.GetModel();
        const auto top = scene.Pick({ 2000.0, 2000.0, 20000.0 }, { 0.0, 0.0, -1.0 });
        const auto bottom = scene.Pick({ 2000.0, 2000.0, -5000.0 }, { 0.0, 0.0, 1.0 });
        const auto miss = scene.Pick({ 50000.0, 50000.0, 20000.0 }, { 0.0, 0.0, -1.0 });
        Check(top && m.elements.Get(*top) && std::string(m.elements.Get(*top)->Category()) == "roof" && bottom &&
                  std::string(m.elements.Get(*bottom)->Category()) == "floor" && !miss,
              "三维场景：拾取射线最先碰到的元素");
    }

    void TestOpeningsAreCut()
    {
        // 一面墙，开一扇窗：窗洞里没有墙的三角形，只有玻璃
        Bim::Session s;
        Bim::Json out;
        std::string error;
        s.Execute("wall", Bim::Json::parse(R"({"points": [[0, 0], [4000, 0]]})"), out, error);
        auto kernel = Solid::CreateSMLibKernel();
        ModelScene plain;
        plain.Build(s.GetModel(), kernel.get(), error);
        s.Execute("window", Bim::Json::parse(R"({"at": [2000, 0]})"), out, error);
        ModelScene cut;
        const bool built = cut.Build(s.GetModel(), kernel.get(), error);
        // 墙的中心面（y = 0）在窗洞中间的一点：被玻璃挡住，但没有墙的三角形穿过窗洞前面（y = -100 的墙面）
        auto wallFaceThere = [](const ModelScene& scene)
        {
            const auto& t = scene.GetTriangles();
            for (size_t i = 0; i + 2 < t.size(); i += 3)
            {
                const auto& a = t[i].pos;
                const auto& b = t[i + 1].pos;
                const auto& c = t[i + 2].pos;
                if (std::abs(a.y + 100.0f) > 0.5f || std::abs(b.y + 100.0f) > 0.5f || std::abs(c.y + 100.0f) > 0.5f)
                    continue;
                // 点 (2000, 1650) 在这个三角形里（x、z 平面）
                const double px = 2000.0, pz = 1650.0;
                auto side = [&](const auto& p, const auto& q) { return (q.x - p.x) * (pz - p.z) - (q.z - p.z) * (px - p.x); };
                const double d1 = side(a, b), d2 = side(b, c), d3 = side(c, a);
                const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
                if (!(neg && pos))
                    return true;
            }
            return false;
        };
        Check(built && cut.GetCounts().windows == 1 && wallFaceThere(plain) && !wallFaceThere(cut), "三维场景：墙在窗洞处扣掉");
    }

    void TestColumns()
    {
        Bim::Session s;
        Bim::Json out;
        std::string error;
        const bool placed = s.Execute("column", Bim::Json::parse(R"({"at": [0, 0]})"), out, error) &&
                            s.Execute("structural_column", Bim::Json::parse(R"({"at": [5000, 0], "type": "Concrete Round 400", "height": 2000})"), out, error);
        auto kernel = Solid::CreateSMLibKernel();
        ModelScene scene;
        const bool built = placed && scene.Build(s.GetModel(), kernel.get(), error);
        if (!built)
            std::printf("  %s\n", error.c_str());
        const AABB& b = scene.GetBounds();
        Check(built && scene.GetCounts().columns == 2 && Near(b.Min.x, -200.0, 1.0) && Near(b.Max.x, 5200.0, 1.0) && Near(b.Min.z, 0.0, 1.0) &&
                  Near(b.Max.z, 3000.0, 1.0),
              "三维场景：柱从底拉伸到顶，圆柱是圆的");
        const auto hit = scene.Pick({ 5000.0, 0.0, 20000.0 }, { 0.0, 0.0, -1.0 });
        const Bim::Element* e = hit ? s.GetModel().elements.Get(*hit) : nullptr;
        Check(e && std::string(e->Category()) == "structuralcolumn", "三维场景：从上往下拾取到结构柱");
    }

    // 移植自 BIMCraft structure_tests.rs 的 the_3d_view_has_a_solid_for_a_beam_a_round_one_and_the_columns_and_footings（f3bbb33）
    void TestStructure()
    {
        Bim::Session s;
        Bim::Json out;
        std::string error;
        const auto run = [&](const char* id, const char* p)
        {
            if (!s.Execute(id, Bim::Json::parse(p), out, error))
                std::printf("  %s：%s\n", id, error.c_str());
            return out;
        };
        const Bim::Id beam = run("beam", R"({"points": [[0, 0], [6000, 0]], "offset": 2800})")["ids"][0].get<Bim::Id>();
        const Bim::Id tube = run("beam", R"({"points": [[0, 2000], [6000, 2000]], "type": "Steel Tube 150", "justify": "centre"})")["ids"][0].get<Bim::Id>();
        run("structural_column", R"({"at": [0, 4000]})");
        const Bim::Id footing = run("footing", R"({"at": [3000, 4000]})")["id"].get<Bim::Id>();
        auto kernel = Solid::CreateSMLibKernel();
        ModelScene scene;
        const bool built = scene.Build(s.GetModel(), kernel.get(), error);
        if (!built)
            std::printf("  %s\n", error.c_str());
        Check(built && scene.GetCounts().columns == 1 && scene.GetCounts().runs == 3, "三维场景：梁、圆梁与基础都建出来");
        const auto down = [&](double x, double y) { return scene.Pick({ x, y, 20000.0 }, { 0.0, 0.0, -1.0 }); };
        const auto up   = [&](double x, double y) { return scene.Pick({ x, y, -20000.0 }, { 0.0, 0.0, 1.0 }); };
        Check(down(3000, 0) == beam && up(3000, 0) == beam && down(3000, 2000) == tube && down(3000, 4000) == footing, "三维场景：拾取到梁、圆梁与基础");
        // 梁 2300 到 2800；基础 -400 到 0；圆梁中心线在标高上，伸到 -75
        const AABB& b = scene.GetBounds();
        Check(Near(b.Min.z, -400.0, 1.0) && Near(b.Max.z, 3000.0, 1.0) && Near(b.Min.x, -150.0, 1.0) && Near(b.Max.x, 6000.0, 1.0),
              "三维场景：结构的范围");
    }
    // 移植自 BIMCraft mep_tests.rs 的 the_3d_view_has_a_solid_for_a_run（f3bbb33）：每类 run 一个实体，圆的是圆的
    void TestMep()
    {
        const std::pair<const char*, bool> cases[] = { { "pipe", true }, { "conduit", true }, { "cable_tray", false }, { "wire", true },
                                                       { "down_conductor", true }, { "ground_conductor", true }, { "cabling", true } };
        auto kernel = Solid::CreateSMLibKernel();
        for (const auto& [command, round] : cases)
        {
            Bim::Session s;
            Bim::Json out;
            std::string error;
            if (!s.Execute(command, Bim::Json::parse(R"({"points": [[0, 0], [6000, 0]], "offset": 2400})"), out, error))
                std::printf("  %s：%s\n", command, error.c_str());
            const Bim::Id id = out.is_object() ? out["ids"][0].get<Bim::Id>() : Bim::Id(0);
            ModelScene scene;
            const bool built = scene.Build(s.GetModel(), kernel.get(), error);
            const Bim::Json e = Bim::ElementJson(s.GetModel(), id);
            const double depth = e.value("depth", 0.0);
            const AABB& b = scene.GetBounds();
            const auto [first, last] = scene.RangeOf(id);
            // 方的是一个盒子（12 个三角形），圆的围着一圈
            const size_t tris = (last - first) / 3;
            Check(built && scene.GetCounts().runs == 1 && Near(b.Min.z, 2400.0 - depth / 2.0, 0.5) && Near(b.Max.z, 2400.0 + depth / 2.0, 0.5) &&
                      (round ? tris > 12 : tris == 12),
                  std::string("三维场景：") + command + " 的实体，中心在 2400，" + (round ? "圆的" : "方的"));
        }
    }
    // 体量：拉伸的是 12 个三角形的盒子；放样的是两个轮廓之间的三角形，矩形的拟台每个侧面是平的，也是 12 个；从底到顶
    void TestMassing()
    {
        auto kernel = Solid::CreateSMLibKernel();
        Bim::Session s;
        Bim::Json plain, lofted;
        std::string error;
        if (!s.Execute("mass", Bim::Json::parse(R"({"boundary": [[0, 0], [10000, 0], [10000, 6000], [0, 6000]], "height": 4000, "base_offset": 500})"), plain, error) ||
            !s.Execute("mass", Bim::Json::parse(R"({"boundary": [[20000, 0], [30000, 0], [30000, 6000], [20000, 6000]], "height": 6000, "top": [[22000, 2000], [28000, 2000], [28000, 4000], [22000, 4000]]})"),
                       lofted, error))
            std::printf("  mass：%s\n", error.c_str());
        ModelScene scene;
        const bool built = scene.Build(s.GetModel(), kernel.get(), error);
        const auto box  = scene.RangeOf(plain.value("id", Bim::Id(0)));
        const auto loft = scene.RangeOf(lofted.value("id", Bim::Id(0)));
        const AABB& b = scene.GetBounds();
        Check(built && scene.GetCounts().masses == 2 && (box.second - box.first) / 3 == 12 && (loft.second - loft.first) / 3 == 12 && Near(b.Min.z, 0.0, 0.5) &&
                  Near(b.Max.z, 6000.0, 0.5),
              "三维场景：体量，拉伸的与放样的");
    }

}

namespace
{
    // 剖面：沿 y = 3500 自西向东剖开示例房屋（看向北）
    void TestSectionCut()
    {
        const Bim::Session s = Sample();
        auto kernel = Solid::CreateSMLibKernel();
        ModelScene scene;
        std::string error;
        scene.Build(s.GetModel(), kernel.get(), error);
        const auto all = [](Bim::Id) { return true; };
        const ModelScene::SectionCut c = scene.Cut({ -2000.0, 3500.0 }, { 12000.0, 3500.0 }, std::nullopt, std::nullopt, all);
        // 西墙中心线在 x = 0、300 厚：离起点 1850 到 2150，从楼板顶（0）起
        bool west = false;
        for (const auto& [id, rings] : c.cuts)
            for (const Bim::Ring& r : rings)
            {
                double lo = 1e300, hi = -1e300, bottom = 1e300, top = -1e300;
                for (const Bim::P2& q : r)
                {
                    lo = std::min(lo, q.x);
                    hi = std::max(hi, q.x);
                    bottom = std::min(bottom, q.y);
                    top = std::max(top, q.y);
                }
                if (Near(lo, 1850.0, 1.0) && Near(hi, 2150.0, 1.0) && top - bottom > 2500.0)
                    west = west || std::string(s.GetModel().elements.Get(id)->Category()) == "wall";
            }
        Check(west, "剖面：剖到西墙，300 厚的一块");
        Check(c.any && !c.faces.empty() && c.faces.size() % 3 == 0 && !c.edges.empty() && c.edges.size() % 2 == 0, "剖面：线后看到的面与边");
        bool behind = true;
        for (const Vertex_P3_C4& p : c.faces)
            behind = behind && p.pos.y >= 3500.0 - 1e-3;
        Check(behind, "剖面：看到的都在线后（北边）");
        // 深度 10 毫米时几乎看不到什么；裁剪了的只留起点到终点、底到顶
        const ModelScene::SectionCut shallow = scene.Cut({ -2000.0, 3500.0 }, { 12000.0, 3500.0 }, 10.0, std::nullopt, all);
        Check(shallow.faces.size() < c.faces.size(), "剖面：深度以内的");
        const ModelScene::SectionCut cropped = scene.Cut({ -2000.0, 3500.0 }, { 12000.0, 3500.0 }, std::nullopt, std::array<double, 2>{ 0.0, 1000.0 }, all);
        bool inside = cropped.any;
        for (const Vertex_P3_C4& p : cropped.faces)
            inside = inside && p.pos.z >= -1e-3 && p.pos.z <= 1000.0 + 1e-3;
        Check(inside && cropped.hi <= 1000.0 + 1e-3, "剖面：裁剪了的只留底与顶之间");
        // 隐藏的元素不算
        const ModelScene::SectionCut none = scene.Cut({ -2000.0, 3500.0 }, { 12000.0, 3500.0 }, std::nullopt, std::nullopt, [](Bim::Id) { return false; });
        Check(!none.any && none.cuts.empty() && none.faces.empty(), "剖面：都隐藏时什么也没有");
    }
}

int main()
{
    TestNoKernel();
    TestSampleHouse();
    TestOpeningsAreCut();
    TestColumns();
    TestStructure();
    TestMep();
    TestMassing();
    TestSectionCut();
    std::printf("\n%s：%d 项失败\n", g_failed == 0 ? "全部通过" : "有失败", g_failed);
    return g_failed == 0 ? 0 : 1;
}
