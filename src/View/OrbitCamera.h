#pragma once
#include "Core/GeomKernel/AABB.hpp"
#include "Core/Math/Mat4.hpp"
#include "Core/Math/Point3.hpp"
#include "Core/Math/Vec3.hpp"

namespace MiniBIM
{
    // 三维视图的相机：正交投影，绕目标点环绕。世界坐标 Z 向上，单位毫米（同 BIMCraft 的模型）。
    // 屏幕坐标为视口内的物理像素，原点在左上角、y 向下（同 MiniGUI::ViewportPointerEvent::pixel）。
    // 矩阵按 Math::Mat4 的行向量约定（P' = P · M），深度 [0, 1]（D3D）
    class OrbitCamera
    {
    public:
        void SetViewportSize(int pixelWidth, int pixelHeight);
        int  GetViewportWidth() const  { return m_width; }
        int  GetViewportHeight() const { return m_height; }

        // 方位角：视线水平分量从目标指向相机的方向，自 +X 逆时针（弧度）；仰角：相机在目标之上为正（弧度）
        void   SetDirection(double azimuth, double elevation);
        double GetAzimuth() const   { return m_azimuth; }
        double GetElevation() const { return m_elevation; }

        // 西南等轴测（Revit 默认三维视图的方向），并框住 bounds
        void HomeView(const AABB& bounds);
        // 方向不变，平移、缩放到刚好框住 bounds（四周留一点边）；bounds 为空时不动
        void ZoomToFit(const AABB& bounds);

        // 交互（像素增量）：环绕、平移，滚轮缩放时光标下的点不动（factor < 1 放大）
        void Orbit(double dxPixels, double dyPixels);
        void Pan(double dxPixels, double dyPixels);
        void ZoomAt(double pixelX, double pixelY, double factor);

        const Math::Point3& GetTarget() const { return m_target; }
        double              GetViewHeight() const { return m_viewHeight; }     // 视口竖直方向看得到的世界长度
        Math::Vec3          GetForward() const;     // 视线方向（从相机看向目标），单位长度
        Math::Vec3          GetRight() const;       // 屏幕向右
        Math::Vec3          GetUp() const;          // 屏幕向上

        Math::Mat4 GetViewProj() const;

    private:
        Math::Point3 PixelToPlane(double pixelX, double pixelY) const;     // 过目标、垂直于视线的平面上的点
        double       WorldPerPixel() const;

        Math::Point3 m_target;
        double       m_azimuth    = 0.0;
        double       m_elevation  = 0.0;
        double       m_viewHeight = 10000.0;
        double       m_depth      = 1.0e6;      // 目标前后各这么深的范围都画得出来（正交投影的近、远平面）
        int          m_width      = 1;
        int          m_height     = 1;
    };
}
