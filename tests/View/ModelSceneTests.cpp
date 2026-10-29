// 模型的三维场景（src/View/ModelScene）：示例房屋经 SMLib 建成实体、离散成三角形与边线
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
        Check(built && c.walls == 7 && c.floors == 1 && c.doors == 4 && c.windows == 5 && c.ceilings == 4 && c.roofs == 1,
              "三维场景：示例房屋的墙、楼板、门、窗、天花板、屋顶都建出来");
        Check(!scene.GetTriangles().empty() && scene.GetTriangles().size() % 3 == 0 && !scene.GetEdges().empty() && scene.GetEdges().size() % 2 == 0,
              "三维场景：有三角形和边线");
        // 外墙 300 毫米厚、中心线在 10 × 7 米的轴线上；楼板 200 毫米厚，顶面在标高上。屋顶伸出外墙面 500，
        // 在 Level 2（3000）上 30° 四坡：檐口比标高低 500 tan 30°，8.3 米宽的短边中间是屋脊，上面是 300 厚的板（铅垂 300 / cos 30°）
        const AABB& b = scene.GetBounds();
        const double tan30 = std::tan(kPi / 6.0), ridge = 3000.0 + 3650.0 * tan30 + 300.0 / std::cos(kPi / 6.0);
        Check(Near(b.Min.x, -650.0, 1.0) && Near(b.Min.y, -650.0, 1.0) && Near(b.Max.x, 10650.0, 1.0) && Near(b.Max.y, 7650.0, 1.0) &&
                  Near(b.Min.z, -200.0, 1.0) && Near(b.Max.z, ridge, 1.0),
              "三维场景：范围是屋顶的檐口、楼板底到屋脊");
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
}

int main()
{
    TestNoKernel();
    TestSampleHouse();
    TestOpeningsAreCut();
    std::printf("\n%s：%d 项失败\n", g_failed == 0 ? "全部通过" : "有失败", g_failed);
    return g_failed == 0 ? 0 : 1;
}
