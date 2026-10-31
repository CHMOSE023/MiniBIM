// 移植自 BIMCraft crates/rvt/src/placement.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_instance_transforms.rs (a
// family instance's placement in its own data, RE-87).
// Changed: rewritten for BIMCraft; the reader takes the bytes of one instance's data and every read is bounds-checked; then
// rewritten in C++ for MiniBIM.
//
// 族实例的放置：它的数据里十二个 f64，一个按行存的 3 × 3 旋转，然后是原点（英尺）。旋转的各列是实例的 X、Y、Z 轴。
// 读的是数据里第一处九个 f64 构成旋转的地方
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace MiniBIM::Rvt::Placement
{
    // 族实例的放置
    struct Transform
    {
        std::array<std::array<double, 3>, 3> axes{};     // 单位 X、Y、Z 轴（模型坐标）
        std::array<double, 3>                origin{};   // 模型英尺

        // X 轴的平面角（弧度，从东起逆时针）；Z 轴不是模型的 Z 轴的实例（侧放的或吊在天花板上的）为空
        std::optional<double> PlanAngle() const;
        bool operator==(const Transform&) const = default;
    };

    // 实例数据里的第一个变换
    std::optional<Transform> TransformOf(std::span<const uint8_t> data);
}
