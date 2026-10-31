// 移植自 BIMCraft crates/rvt/src/curves.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_beam_axes.rs (the bounded
// line and arc records in an element's data, RE-49, RE-75).
// Changed: rewritten for BIMCraft; only the record readers are kept (not the beam solver or the scans over the file), every
// read is bounds-checked, and the readers take the bytes of one element's data; then rewritten in C++ for MiniBIM.
//
// 元素数据里的曲线：墙的定位线或圆弧。
//
// 有界直线是 `04 00 08 01`、起点与终点参数（f64，英尺）、原点（三个 f64）与单位方向（三个 f64）：线从 `原点 + 起点 · 方向`
// 到 `原点 + 终点 · 方向`。圆弧是同一个标签，前面是 u64 的 1（直线的是 0）：起始角与终止角（弧度）、圆弧的 X 轴与 Y 轴
// （互相垂直的单位向量）、半径（英尺）与圆心。它从起始角处的 `圆心 + 半径 · (cos a · X + sin a · Y)` 走到终止角。
// 墙的第一条曲线记录是它的定位线，不论定位线设置是什么都是它的中心线
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace MiniBIM::Rvt::Curves
{
    using Vec3 = std::array<double, 3>;

    // 有界直线
    struct BoundedLine
    {
        double startParameter = 0;
        double endParameter   = 0;
        Vec3   origin{};
        Vec3   direction{};

        Vec3 Point(double parameter) const { return { origin[0] + parameter * direction[0], origin[1] + parameter * direction[1], origin[2] + parameter * direction[2] }; }
        // 线的第一个端点（模型英尺）
        Vec3 Start() const { return Point(startParameter); }
        // 线的第二个端点（模型英尺）
        Vec3 End() const { return Point(endParameter); }
        bool operator==(const BoundedLine&) const = default;
    };

    // 圆弧
    struct BoundedArc
    {
        double startAngle = 0;
        double endAngle   = 0;
        Vec3   xAxis{};
        Vec3   yAxis{};
        double radius = 0;
        Vec3   centre{};

        // 圆弧上 angle 处的点（模型英尺）
        Vec3 Point(double angle) const;
        // 从上面看角度是否逆时针增加：两轴的叉积朝上
        bool CounterClockwise() const { return xAxis[0] * yAxis[1] - xAxis[1] * yAxis[0] > 0.0; }
        bool operator==(const BoundedArc&) const = default;
    };

    // 标签从 at 开始的有界直线：值都有限、方向是单位向量、起点参数小于终点参数
    std::optional<BoundedLine> LineAt(std::span<const uint8_t> buf, size_t at);
    // data 里第一条有界直线。数据的第一条曲线记录是圆弧时为空：它的轴会被读成一条线
    std::optional<BoundedLine> FirstLine(std::span<const uint8_t> data);
    // 标签从 at 开始的圆弧：它前面的 u64 是 1、值都有限、两轴是互相垂直的单位向量、半径为正、起始角小于终止角
    std::optional<BoundedArc> ArcAt(std::span<const uint8_t> buf, size_t at);
    // 不论前面的字是什么都把 at 处的记录读成圆弧：草图线的圆弧没有标出来
    std::optional<BoundedArc> ArcValuesAt(std::span<const uint8_t> buf, size_t at);
    // 墙的数据的第一条曲线记录是圆弧时的那个圆弧
    std::optional<BoundedArc> FirstArc(std::span<const uint8_t> data);
}
