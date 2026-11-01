// 移植自 BIMCraft crates/core/src/curves.rs（f3bbb33）的多段线与圆，MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Geom.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

// 详图线的曲线：依次经过各点的多段线（每段直线，或按 arcs 像墙的圆弧那样逆时针转那么多度；closed 时最后一点回到第一点），
// 或整圆（从圆心正东起止）。BIMCraft 还有椭圆、样条与拟合样条，读文件时略去，随后面的阶段移植
namespace MiniBIM::Bim
{
    struct Curve
    {
        enum class Kind { Poly, Circle };
        Kind                kind = Kind::Poly;
        std::vector<P2>     points;
        std::vector<double> arcs;       // 空：全是直线；否则每段一个
        bool                closed = false;
        P2                  centre;
        double              radius = 0.0;

        static Curve Line(P2 a, P2 b);
        // 从 a 到 b 逆时针转 sweep 度的圆弧（0 为直线）
        static Curve Arc(P2 a, P2 b, double sweep);
        static Curve CircleOf(P2 centre, double radius);

        // line、arc、polyline、circle（报错与 elements 里的 shape）
        const char* Name() const;
        bool   IsClosed() const;
        P2     Start() const;
        P2     End() const;
        double Length() const;
        // 画出来经过的点：多段线各段的两端，圆弧切成弦（弦离弧不超过 1 毫米）；闭合的最后回到第一点
        std::vector<P2> Points() const;
        // 圆弧与圆的圆心
        std::vector<P2> Centres() const;
        // 第一段是圆弧时它的圆心与半径；圆的圆心与半径
        std::optional<std::pair<P2, double>> Round() const;
        // 每个点经 f 换过（镜像时圆弧反过来转）
        Curve Map(const std::function<P2(P2)>& f, bool mirror) const;
        // 离 p 多远
        double Distance(P2 p) const;
        // 检查：点数、点在范围内、相邻的点不重合、arcs 一段一个、圆弧不到一整圈、不太长也不太短
        bool Check(const std::string& who, std::string& error) const;
        bool operator==(const Curve&) const = default;
    };
}
