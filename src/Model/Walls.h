// 移植自 BIMCraft crates/core/src/walls.rs、walls/index.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Model.h"
#include <map>
#include <optional>
#include <tuple>
#include <vector>

// 墙的几何。定位线从起点到终点，直线或圆弧；墙面和各层的分界线按类型与定位线横在它两侧，外侧在左。
// 墙相接（端对端任意角度，或一面墙的端头落在另一面墙上）时墙面在角上相交：每个端头的封口（Cap）。
// 封口与墙面组成平面图填充、三维与剖面剖切、IFC 拉伸用的轮廓。墙顶可以贴着上面的屋顶或楼板，墙底可以立在下面的：
// 取它们平面中最低的那个
namespace MiniBIM::Bim::Walls
{
    // 墙的定位线，从起点到终点：直线或圆弧
    class Axis
    {
    public:
        P2 start;
        P2 end;

        static Axis Of(const Wall& w) { return Axis(w.start, w.end, w.arc); }
        // 从 start 到 end，转过 arc 度
        Axis(P2 start, P2 end, double arc);

        std::optional<P2>     Centre() const;
        std::optional<double> Radius() const;
        double Length() const;
        // 沿它 s 毫米处的走向
        P2     Tangent(double s) const;
        // 沿它 s 毫米、向左 off 毫米的点
        P2     At(double s, double off) const;
        // 离 p 最近的点沿它多远（0 到长度），以及 p 在它左边多远
        std::pair<double, double> Project(P2 p) const;
        // p 到它左边 off 毫米那条线的距离
        double Distance(P2 p, double off) const;
        // 从 s0 到 s1 切成段的位置：两端，圆弧上再加足够多的点，使弦离圆弧不超过 1 毫米、转角不超过 5°
        std::vector<double> Stations(double s0, double s1) const;
        // 沿它左边 off 毫米的线从 s0 到 s1 的点（见 Stations）
        std::vector<P2>     Points(double s0, double s1, double off) const;

        bool operator==(const Axis&) const = default;

    private:
        struct Arc
        {
            P2     centre;
            double radius = 0.0;
            double a0     = 0.0;     // 起点相对圆心的角度
            double sweep  = 0.0;     // 弧度，逆时针为正
            bool operator==(const Arc&) const = default;
        };
        std::optional<Arc> m_arc;
    };

    // 从 a 经 p 到 b 的圆弧转过多少度（逆时针为正）：p 在 a→b 直线上时为 0，越向右凸出越大
    double SweepThrough(P2 a, P2 p, P2 b);

    // 墙的两个面和各层分界线在定位线左边多远，从外侧面到内侧面
    std::vector<double> Boundaries(const Model& m, const Wall& w);
    // 墙的外侧面与内侧面在定位线左边多远
    std::pair<double, double> Faces(const Model& m, const Wall& w);
    // 墙的中心线在定位线左边多远
    double Centre(const Model& m, const Wall& w);
    // 两面墙在高度上重叠，所以相接
    bool   Stacked(const Model& m, const Wall& a, const Wall& b);

    // 只读的加速索引（一份模型快照用）：按高度分段找可能相接的墙，按宿主找门窗。分段只缩小候选，
    // 相接与否仍按原来的规则逐个判断
    class Index
    {
    public:
        explicit Index(const Model& m);
        std::vector<Id> Neighbours(Id id) const;
        const std::vector<Id>& Openings(Guid host) const;

    private:
        std::map<Id, std::pair<double, double>> m_spans;
        std::map<int64_t, std::vector<Id>>      m_bands;
        std::vector<Id>                         m_broad;
        std::map<Guid, std::vector<Id>>         m_hosts;
    };

    // 封闭墙 id 起点（atEnd 时为终点）的角点：从离开节点方向的顺时针一侧墙面绕到另一侧。端头在这里的墙以任意角度相接，
    // 墙面延伸到相交（离节点不超过最厚的墙厚，超过就平头）；端头落在另一面墙上的停在那面墙的墙面上。
    // 三面以上的墙相接时，节点本身填在它们之间
    std::vector<P2> Cap(const Model& m, Id id, bool atEnd, const Index* index = nullptr);
    // 墙 id 的中心线（圆弧为弦）：与别的墙相接的端头延伸（或缩回）到最近的那条中心线，所以不论墙立在哪条线上，房间都在中心线处封闭
    std::vector<P2> CentreLine(const Model& m, Id id);
    // 墙 id 的平面轮廓：起点封口、外侧面、终点封口、内侧面
    std::vector<P2> Outline(const Model& m, Id id, const Index* index = nullptr);
    // 墙 id 的两个面和各层分界线，外侧在前，各从起点封口到终点封口
    std::vector<std::vector<P2>> LayerLines(const Model& m, Id id, const Index* index = nullptr);

    // 多边形分成凸块，各逆时针：凸的就是它自己，否则是它的三角形
    std::vector<Ring> ConvexPieces(const Ring& ring);
    // ring 被凸的逆时针 piece 切开：在里面的部分（如果有）与外面的各部分
    std::pair<std::optional<Ring>, std::vector<Ring>> Split(Ring ring, const Ring& piece);
    // rings 在 hole 以外剩下的：凸块，逆时针
    std::vector<Ring> Minus(const std::vector<Ring>& rings, const Ring& hole);

    // 屋顶或楼板 id 在 q 上方的底面高度（如果它在 q 上方）
    std::optional<double> Underside(const Model& m, Id id, P2 q);
    // 墙在中心线上 s 毫米处的顶部
    double TopAt(const Model& m, const Wall& w, double s);
    // 墙沿墙面与中心线最低的底部与最高的顶部，贴着的东西折弯处采样（屋脊可能落在采样点之间）
    std::pair<double, double> Heights(const Model& m, const Wall& w);

    // 墙实体的一部分：平面上的一个环，从底部平面到顶部平面；曲墙的还有圆心和中心线半径
    struct Prism
    {
        Ring  ring;
        Plane bottom;
        Plane top;
        std::optional<std::pair<P2, double>> curve;
        bool operator==(const Prism&) const = default;
    };

    // 墙 id 分成棱柱：轮廓在门窗起止处横向切开（openings 时），圆弧切成短直段，每段从底部到顶部、扣掉上方的门窗洞，
    // 再在贴着的东西换平面处切开
    std::vector<Prism> Prisms(const Model& m, Id id, bool openings, const Index* index = nullptr);
}
