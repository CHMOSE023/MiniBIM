// 移植自 BIMCraft crates/core/src/snap.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Model.h"
#include <optional>
#include <span>

// 捕捉：在光标附近点下去的点落在哪里。落在平面上那里有的东西（画出来的东西的端点、中点、圆心，两条线的交点，
// 从上一个点作的垂足），落在过上一个点、按角度的引导线上（从正东量，也从前一段量），落在墙的中心线上，否则取最近的 10 毫米。
// 标尺的读数与刻度也在这里，各个前端读得一样。
//
// 移植的是平面上已有元素的部分：墙、轴网、构件、楼板、天花板、屋顶。详图线（曲线的象限点、切点）、点云、CAD 导入、柱、
// 楼梯、体量、草图模式的 in_sketch，以及视图的图形覆盖（隐藏的元素不捕捉）随那些模块移植时再加
namespace MiniBIM::Bim::Snap
{
    // 点捕捉到了什么
    enum class Kind
    {
        Endpoint,
        Midpoint,
        Intersection,
        Centre,
        Perpendicular,
        Nearest,        // 墙中心线上最近的点
        Angle,          // 角度引导线上的点
        Free,           // 没有可捕捉的：最近的 10 毫米
    };
    // 叫什么（英文原文，显示前 Tr；Free 为空）
    const char* Name(Kind k);

    // 开着的捕捉
    struct Snaps
    {
        bool endpoint      = true;
        bool midpoint      = true;
        bool intersection  = true;
        bool centre        = true;
        bool perpendicular = true;
        bool nearest       = true;
        // 方向捕捉的角度间隔（度），从正东和从前一段量，1 到 180；别的值为不捕捉角度
        double angle = 15.0;

        // 什么都不捕捉，也没有角度
        static Snaps None();
        bool operator==(const Snaps&) const = default;
    };

    // 捕捉离光标多远还算数，毫米：到点，到线
    struct Reach
    {
        double point = 0.0;
        double line  = 0.0;
    };

    // 点捕捉到的引导线：过 from，角度 angle
    struct Guide
    {
        P2     from;
        double angle = 0.0;             // 从正东逆时针的度数，[0, 360)
        std::optional<double> corner;   // 上一个点之前还有一段时，这条线与它的夹角（度）
        bool operator==(const Guide&) const = default;
    };

    struct Found
    {
        P2   at;
        Kind kind = Kind::Free;
        std::optional<Guide> guide;
        bool operator==(const Found&) const = default;
    };

    // 在 level 的平面上 p 处点下的点落在哪里：平面上那里有的东西，从 from 的最后一点（这一点跟着的前几点）作的垂足，
    // 过那一点的角度引导线，墙的中心线，否则最近的 10 毫米。square 让从最后一点起的这一步保持水平或竖直（Shift）
    // extraLines、extraPoints：草图模式里还没进模型的草图线（直线：端点、中点、可相交；圆弧只给端点与中点）
    Found At(const Model& m, Guid level, P2 p, Reach reach, std::span<const P2> from, bool square, const Snaps& o,
             std::span<const Segment> extraLines = {}, std::span<const std::pair<P2, Kind>> extraPoints = {});

    // last 处从回到 prev 的线到通往 next 的线的夹角，度：一直往前是 180°，直角转弯 90°；任一段没有长度时没有
    std::optional<double> Corner(P2 prev, P2 last, P2 next);

    // 从 a 量到 b 的标尺：长度、两个分量、角度（从正东逆时针，[0, 360)）
    struct Ruler
    {
        double length = 0.0;
        double dx     = 0.0;
        double dy     = 0.0;
        double angle  = 0.0;
    };
    Ruler Measure(P2 a, P2 b);
    // 每毫米 pxPerMm 像素时标尺刻度的间距，毫米：10 的幂的 1、2、5 倍，至少相隔 10 像素
    double Graduation(double pxPerMm);

    // 方向从 from 的最后一点每隔 inc 度捕捉的角度：从正东，以及从最后一点之前那一段（测试用）
    std::vector<double> Rays(std::span<const P2> from, double inc);
}
