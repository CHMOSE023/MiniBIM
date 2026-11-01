// 移植自 BIMCraft crates/rvt/src/sketch.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/element_record_plan_profiles.rs
// (a sketched element's boundary from its sketch lines, RE-25, RE-50, RE-96) and src/partition_beam_axes.rs (a sketch line's
// curve, RE-96).
// Changed: rewritten for BIMCraft; one tracer serves lines of known ends and lines known by their boxes, loops are
// bounded in number and size, and the pieces and voids are found by containment alone; then rewritten in C++ for MiniBIM.
//
// 草图元素（楼板、天花板、屋顶）的边界：它的草图线连成的环。
//
// 楼板是一个草图：边界曲线围成一个外环与任意个内环。2024 起的项目里每条曲线是 OST_SketchLines 类别里自己的一条记录，
// 带着包围盒，草图的所有者在它第二个引用列表的 id 里；曲线本身（直线或圆弧）是它数据里的第一条曲线记录，两种读法都从那里读出，
// 由包围盒说是哪一种。2024 以前只读包围盒，平行于坐标轴的线段就是它的包围盒：斜线留下两种可能的端点，由环本身在恰好只有
// 一对空着的端点合适时定下来。
//
// 每个端点必须恰好由两条曲线共用，否则整个草图不要：这里不猜顶点
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "Rvt/Curves.h"

namespace MiniBIM::Rvt::Sketch
{
    // 读一个草图最多用几条曲线
    inline constexpr size_t MaxEdges = 4096;

    struct Pt
    {
        double x = 0;
        double y = 0;
        bool operator==(const Pt&) const = default;
    };

    // 一条边界曲线：两个端点与中间的点，从 a 起按顺序
    struct Edge
    {
        Pt              a;
        Pt              b;
        std::vector<Pt> mid;
        bool operator==(const Edge&) const = default;
    };

    // 一块区域：外环（逆时针）与其中的洞（顺时针）
    struct Piece
    {
        std::vector<Pt>              outer;
        std::vector<std::vector<Pt>> holes;
        bool operator==(const Piece&) const = default;
    };

    // 草图线的第一条曲线记录，能读成直线就读成直线、能读成圆弧就读成圆弧：记录不说它是哪种，由包围盒决定
    std::optional<std::pair<std::optional<Curves::BoundedLine>, std::optional<Curves::BoundedArc>>> SketchCurve(std::span<const uint8_t> data);
    // 包围盒为 bbox 的草图线的曲线：直线与圆弧两种读法里合包围盒的那个；都不在包围盒里时为空
    std::optional<Edge> EdgeOf(std::span<const uint8_t> data, const std::array<double, 6>& bbox);

    // 曲线围成的区域；不是毫无选择地闭合时为空
    std::optional<std::vector<Piece>> FromEdges(const std::vector<Edge>& edges);
    // 去掉什么也没围住的曲线（如结构样例里浮在楼板轮廓内的草图线）之后围成的区域，以及去掉的每条的下标；一个环也不闭合时为空
    std::optional<std::pair<std::vector<Piece>, std::vector<size_t>>> FromEdgesOpen(const std::vector<Edge>& edges);
    // 没接上的端点用直线连上之后围成的区域；空着的端点多于 8 个（那不是草图而是损坏），或连上之后仍什么也不围时为空
    std::optional<std::vector<Piece>> FromEdgesClosed(const std::vector<Edge>& edges);
    // 相差不到 0.005 英尺的端点当作同一个顶点之后围成的区域；不是毫无选择地闭合时为空
    std::optional<std::vector<Piece>> FromEdgesSnapped(const std::vector<Edge>& edges);
    // 只知道曲线的平面包围盒 `[最小 x, 最小 y, 最大 x, 最大 y]` 时围成的区域
    std::optional<std::vector<Piece>> FromBoxes(const std::vector<std::array<double, 4>>& boxes);
}
