#include "View/OrbitCamera.h"
#include "Core/Math/MathUtils.hpp"
#include <algorithm>
#include <cmath>

namespace MiniBIM
{
    namespace
    {
        constexpr double kPi           = 3.14159265358979323846;
        constexpr double kMaxElevation = 89.0 * kPi / 180.0;   // 不到正上方：正俯视时屏幕"向上"的方向不确定
        constexpr double kOrbitPerPixel = 0.008;                // 拖动一个像素转多少弧度
    }

    void OrbitCamera::SetViewportSize(int pixelWidth, int pixelHeight)
    {
        m_width  = std::max(1, pixelWidth);
        m_height = std::max(1, pixelHeight);
    }

    void OrbitCamera::SetDirection(double azimuth, double elevation)
    {
        m_azimuth   = azimuth;
        m_elevation = std::clamp(elevation, -kMaxElevation, kMaxElevation);
    }

    Math::Vec3 OrbitCamera::GetForward() const
    {
        // 从目标指向相机的反方向
        const double c = std::cos(m_elevation);
        return Math::Vec3{ -c * std::cos(m_azimuth), -c * std::sin(m_azimuth), -std::sin(m_elevation) };
    }

    Math::Vec3 OrbitCamera::GetRight() const
    {
        return Math::Cross(GetForward(), Math::Vec3{ 0.0, 0.0, 1.0 }).Normalized();
    }

    Math::Vec3 OrbitCamera::GetUp() const
    {
        return Math::Cross(GetRight(), GetForward());
    }

    double OrbitCamera::WorldPerPixel() const
    {
        return m_viewHeight / static_cast<double>(m_height);
    }

    void OrbitCamera::HomeView(const AABB& bounds)
    {
        // 西南等轴测：相机在 (-1, -1, 1) 方向
        SetDirection(225.0 * kPi / 180.0, std::atan(1.0 / std::sqrt(2.0)));
        ZoomToFit(bounds);
    }

    void OrbitCamera::ZoomToFit(const AABB& bounds)
    {
        if (!(bounds.Min.x <= bounds.Max.x && bounds.Min.y <= bounds.Max.y && bounds.Min.z <= bounds.Max.z))
            return;
        m_target = bounds.Center();

        // 八个角投影到屏幕的范围
        const Math::Vec3 right = GetRight(), up = GetUp();
        double halfW = 0.0, halfH = 0.0;
        for (int i = 0; i < 8; ++i)
        {
            const Math::Point3 corner{ (i & 1) ? bounds.Max.x : bounds.Min.x,
                                       (i & 2) ? bounds.Max.y : bounds.Min.y,
                                       (i & 4) ? bounds.Max.z : bounds.Min.z };
            const Math::Vec3 d = corner - m_target;
            halfW = std::max(halfW, std::abs(Math::Dot(d, right)));
            halfH = std::max(halfH, std::abs(Math::Dot(d, up)));
        }
        const double aspect = static_cast<double>(m_width) / static_cast<double>(m_height);
        m_viewHeight = std::max(2.0 * std::max(halfH, halfW / aspect) * 1.15, 1.0);

        const Math::Vec3 diagonal = bounds.Max - bounds.Min;
        m_depth = std::max(1.0e4, diagonal.Length() * 4.0);
    }

    void OrbitCamera::Orbit(double dxPixels, double dyPixels)
    {
        SetDirection(m_azimuth - dxPixels * kOrbitPerPixel, m_elevation + dyPixels * kOrbitPerPixel);
    }

    void OrbitCamera::Pan(double dxPixels, double dyPixels)
    {
        // 画面跟着指针走：指针向右，目标向左
        const double s = WorldPerPixel();
        m_target = m_target - GetRight() * (dxPixels * s) + GetUp() * (dyPixels * s);
    }

    Math::Point3 OrbitCamera::PixelToPlane(double pixelX, double pixelY) const
    {
        const double s = WorldPerPixel();
        const double x = (pixelX - m_width * 0.5) * s;
        const double y = (m_height * 0.5 - pixelY) * s;
        return m_target + GetRight() * x + GetUp() * y;
    }

    void OrbitCamera::ZoomAt(double pixelX, double pixelY, double factor)
    {
        factor = std::clamp(factor, 0.05, 20.0);
        const double newHeight = std::clamp(m_viewHeight * factor, 1.0, 1.0e9);
        factor = newHeight / m_viewHeight;
        const Math::Point3 anchor = PixelToPlane(pixelX, pixelY);
        m_target     = anchor + (m_target - anchor) * factor;
        m_viewHeight = newHeight;
    }

    Math::Mat4 OrbitCamera::GetViewProj() const
    {
        // 视图矩阵：世界点 → (·右, ·上, ·视线)，以目标为原点；行向量约定下三个基向量放在前三列
        const Math::Vec3 r = GetRight(), u = GetUp(), f = GetForward();
        const Math::Vec3 t{ m_target.x, m_target.y, m_target.z };
        Math::Mat4 view = Math::Mat4::Identity();
        view.m[0] = r.x;  view.m[1] = u.x;  view.m[2]  = f.x;
        view.m[4] = r.y;  view.m[5] = u.y;  view.m[6]  = f.y;
        view.m[8] = r.z;  view.m[9] = u.z;  view.m[10] = f.z;
        view.m[12] = -Math::Dot(t, r);
        view.m[13] = -Math::Dot(t, u);
        view.m[14] = -Math::Dot(t, f);

        const double aspect = static_cast<double>(m_width) / static_cast<double>(m_height);
        return view * Math::Mat4::OrthoLH(m_viewHeight * aspect, m_viewHeight, -m_depth, m_depth);
    }
}
