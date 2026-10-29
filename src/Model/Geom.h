// 移植自 BIMCraft crates/core/src/geom.rs、walls.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

// 平面几何，单位毫米：点、环（闭合多边形），以及墙围出的平面区域
namespace MiniBIM::Bim
{
    // 平面上的点或向量（x 向东，y 向北），文件里写作 [x, y]。点与向量同一类型，照 BIMCraft 的 P2
    struct P2
    {
        double x = 0.0;
        double y = 0.0;

        constexpr P2() = default;
        constexpr P2(double xx, double yy) : x(xx), y(yy) {}

        constexpr P2 operator+(const P2& o) const { return { x + o.x, y + o.y }; }
        constexpr P2 operator-(const P2& o) const { return { x - o.x, y - o.y }; }
        constexpr P2 operator*(double k) const { return { x * k, y * k }; }
        constexpr bool operator==(const P2& o) const = default;

        constexpr double Dot(const P2& o) const { return x * o.x + y * o.y; }
        constexpr double Cross(const P2& o) const { return x * o.y - y * o.x; }
        double Length() const { return std::hypot(x, y); }
        double Dist(const P2& o) const { return (*this - o).Length(); }
        // 单位向量（零向量仍为零）
        P2 Unit() const
        {
            const double l = Length();
            return l > 0.0 ? *this * (1.0 / l) : P2{};
        }
        // 逆时针转 90°
        constexpr P2 Perp() const { return { -y, x }; }
        bool IsFinite() const { return std::isfinite(x) && std::isfinite(y); }
    };

    using Ring    = std::vector<P2>;
    using Segment = std::pair<P2, P2>;
    // 空间里的点，毫米：x 向东，y 向北，z 向上
    using P3      = std::array<double, 3>;

    // 平面图上方的一个平面：q 处的高度为 z + g.x · q.x + g.y · q.y（移植自 walls.rs 的 Plane）
    struct Plane
    {
        double z = 0.0;
        P2     g;

        static Plane Flat(double z) { return Plane{ z, P2{} }; }
        double At(P2 q) const { return z + g.Dot(q); }
        // ring 抬到平面上
        std::vector<P3> Lift(const std::vector<P2>& ring) const;
        // 空间里平面多边形所在的平面（Newell 法线）；竖直的多边形没有
        static std::optional<Plane> Of(const std::vector<P3>& face);
        bool operator==(const Plane&) const = default;
    };

    namespace Geom
    {
        // 矩形四条边的夹点（边的中点）：西、东、南、北
        std::array<P2, 4> EdgeGrips(const std::array<P2, 2>& rect);
        // 第 k 条边（顺序同 EdgeGrips）移到 p 的矩形，宽、高至少 100 mm
        std::array<P2, 2> MoveEdge(const std::array<P2, 2>& rect, size_t k, P2 p);

        // 闭合环的各条边：(p0, p1)、(p1, p2) … (pn, p0)
        std::vector<Segment> RingEdges(const Ring& ring);
        // 有向面积（逆时针为正）
        double Area(const Ring& ring);
        double Perimeter(const Ring& ring);
        // 奇偶规则判断点在环内
        bool   Contains(const Ring& ring, P2 p);
        // 过 a、b 的直线与过 c、d 的直线方向相同（夹角不超过千分之一弧度）
        bool   Parallel(P2 a, P2 b, P2 c, P2 d);
        // 环的角点：去掉落在相邻两点连线上的点
        Ring   Corners(const Ring& ring);
        // 逆时针的环向外扩 d：每条边外移，角点取相邻两边的交点
        Ring   Grow(const Ring& ring, double d);
        // 点集的凸包，逆时针（Andrew 单调链）
        Ring   Hull(std::vector<P2> points);
        // 简单多边形剖分成三角形（耳切法），返回各三角形顶点在 ring 里的下标，都是逆时针。
        // 自交的环得到能找到的耳朵；连续重复的点算一个角点
        std::vector<std::array<size_t, 3>> Triangulate(const Ring& ring);
        // 环上 f 不小于 0 的部分（Sutherland-Hodgman）：f 为线性函数时即沿一条直线切开
        Ring   Clip(const Ring& ring, const std::function<double(P2)>& f);
        // 线段 a→b 上离 p 最近的点的参数：a 处为 0，b 处为 1
        double ParamOnSegment(P2 p, P2 a, P2 b);
        double DistToSegment(P2 p, P2 a, P2 b);
        // 过 a、b 的直线与过 c、d 的直线的交点；平行时为空
        std::optional<P2> LineIntersection(P2 a, P2 b, P2 c, P2 d);
        // 线段 a→b 与 c→d 真正相交（交点在两条线段内部）时，交点在 a→b 上的参数 ∈ (0, 1)
        std::optional<double> Crossing(P2 a, P2 b, P2 c, P2 d);
        // 线段构成的平面图的所有面（端点距离小于 tol 的连在一起）：有界的面逆时针（面积为正），
        // 每个连通块的外轮廓顺时针（面积为负）
        std::vector<Ring> Faces(const std::vector<Segment>& segs, double tol);

        // 房间形状的区域：外环（逆时针）与其中独立的岛
        struct Region
        {
            Ring              outer;
            std::vector<Ring> holes;

            double Area() const;
            double Perimeter() const;
            bool operator==(const Region&) const = default;
        };

        // 包围 p 的区域：围住它的最小的环，减去其中独立的块（开敞办公室里的管井）。没有环围住 p 时为空
        std::optional<Region> RegionAt(const std::vector<Segment>& segs, P2 p, double tol);
        // 同 RegionAt，在 Faces 已经算好的面里找：一个平面图要查很多点时只算一次
        std::optional<Region> RegionIn(const std::vector<Ring>& all, P2 p, double tol);
    }
}
