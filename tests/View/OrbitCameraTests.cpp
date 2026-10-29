// 三维视图的相机（src/View/OrbitCamera）：屏幕方向、框住模型、滚轮缩放时光标下的点不动
#include "View/OrbitCamera.h"
#include "Core/Math/MathUtils.hpp"
#include <cmath>
#include <cstdio>
#include <string>

using namespace MiniBIM;

namespace
{
    int g_failed = 0;
    void Check(bool ok, const std::string& label)
    {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
        g_failed += ok ? 0 : 1;
    }

    constexpr double kPi = 3.14159265358979323846;

    // 世界点 → 视口像素（原点左上、y 向下）
    void ToPixel(const OrbitCamera& cam, const Math::Point3& p, double& x, double& y, double& z)
    {
        const Math::Mat4 m = cam.GetViewProj();
        const double cx = p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8]  + m.m[12];
        const double cy = p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9]  + m.m[13];
        z               = p.x * m.m[2] + p.y * m.m[6] + p.z * m.m[10] + m.m[14];
        x = (cx + 1.0) * 0.5 * cam.GetViewportWidth();
        y = (1.0 - cy) * 0.5 * cam.GetViewportHeight();
    }

    bool Near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

    void TestFrontView()
    {
        // 方位角 -90°：相机在 -Y 一侧向 +Y 看（前视），+X 在屏幕右边，+Z 在上面
        OrbitCamera cam;
        cam.SetViewportSize(800, 600);
        cam.SetDirection(-kPi / 2.0, 0.0);
        const Math::Vec3 r = cam.GetRight(), u = cam.GetUp(), f = cam.GetForward();
        Check(Near(r.x, 1.0, 1e-9) && Near(u.z, 1.0, 1e-9) && Near(f.y, 1.0, 1e-9), "前视：右 = +X，上 = +Z，视线 = +Y");

        double x0, y0, z0, x1, y1, z1;
        ToPixel(cam, { 0, 0, 0 }, x0, y0, z0);
        ToPixel(cam, { 1000, 0, 1000 }, x1, y1, z1);
        Check(x1 > x0 && y1 < y0, "+X 在右、+Z 在上（屏幕 y 向下）");
        ToPixel(cam, { 0, 1000, 0 }, x1, y1, z1);
        Check(z1 > z0 && z0 >= 0.0 && z1 <= 1.0, "远处的点深度更大，都在 [0, 1] 里");
    }

    void TestFitAndZoom()
    {
        OrbitCamera cam;
        cam.SetViewportSize(1000, 500);
        const AABB box({ 0, 0, 0 }, { 9000, 6000, 3000 });
        cam.HomeView(box);
        bool inside = true;
        for (int i = 0; i < 8; ++i)
        {
            double x, y, z;
            ToPixel(cam, { (i & 1) ? 9000.0 : 0.0, (i & 2) ? 6000.0 : 0.0, (i & 4) ? 3000.0 : 0.0 }, x, y, z);
            inside = inside && x > 0 && x < 1000 && y > 0 && y < 500 && z > 0 && z < 1;
        }
        Check(inside, "HomeView 后包围盒的八个角都在视口里");

        // 滚轮缩放：光标下的世界点（视线平面上）缩放前后在同一个像素
        const Math::Point3 p = cam.GetTarget() + cam.GetRight() * 1200.0 + cam.GetUp() * 500.0;
        double xa, ya, za, xb, yb, zb;
        ToPixel(cam, p, xa, ya, za);
        cam.ZoomAt(xa, ya, 0.5);
        ToPixel(cam, p, xb, yb, zb);
        Check(Near(xa, xb, 1e-6) && Near(ya, yb, 1e-6), "缩放时光标下的点不动");

        // 平移：画面跟着指针走
        cam.Pan(10.0, 0.0);
        double xc, yc, zc;
        ToPixel(cam, p, xc, yc, zc);
        Check(Near(xc, xb + 10.0, 1e-6) && Near(yc, yb, 1e-6), "平移 10 像素，点也右移 10 像素");
    }

    void TestOrbitClamp()
    {
        OrbitCamera cam;
        cam.SetDirection(0.0, 0.0);
        cam.Orbit(0.0, 1.0e6);
        Check(cam.GetElevation() < kPi / 2.0, "仰角到不了正上方");
        Check(std::isfinite(cam.GetRight().x) && cam.GetRight().Length() > 0.99, "仰角最大时右方向仍然有效");
    }
}

int main()
{
    TestFrontView();
    TestFitAndZoom();
    TestOrbitClamp();
    std::printf("%s：%d 项失败\n", g_failed ? "FAILED" : "OK", g_failed);
    return g_failed ? 1 : 0;
}
