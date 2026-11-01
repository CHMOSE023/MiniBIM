// 移植自 BIMCraft crates/rvt/src/placement.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_instance_transforms.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Placement.h).
#include "Rvt/Placement.h"
#include "Rvt/Bytes.h"
#include "Rvt/Records.h"

#include <algorithm>
#include <cmath>

namespace MiniBIM::Rvt::Placement
{
    namespace
    {
        using Rows = std::array<std::array<double, 3>, 3>;

        // 在实例的数据里找变换时看多远
        constexpr size_t Window = 0x1000;
        // 九个值要多接近旋转
        constexpr double Tolerance = 1e-9;

        double Dot(const std::array<double, 3>& a, const std::array<double, 3>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

        // at 处的九个值当作旋转（按行）；正交归一且行列式为 1 时才算
        std::optional<Rows> RotationAt(std::span<const uint8_t> data, size_t at)
        {
            Rows rows{};
            for (size_t i = 0; i < 9; ++i)
            {
                const auto v = Bytes::F64At(data, at + 8 * i);
                if (!v || !std::isfinite(*v) || std::abs(*v) > 1.0 + Tolerance)
                    return std::nullopt;
                rows[i / 3][i % 3] = *v;
            }
            for (size_t i = 0; i < 3; ++i)
                for (size_t j = 0; j < 3; ++j)
                    if (std::abs(Dot(rows[i], rows[j]) - (i == j ? 1.0 : 0.0)) > Tolerance)
                        return std::nullopt;
            const auto& [a, b, c] = rows;
            const double det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
            if (std::abs(det - 1.0) > Tolerance)
                return std::nullopt;
            return rows;
        }
    }

    std::optional<double> Transform::PlanAngle() const
    {
        const auto& z = axes[2];
        const bool upright = std::abs(z[0]) <= Tolerance && std::abs(z[1]) <= Tolerance && std::abs(z[2] - 1.0) <= Tolerance;
        if (!upright)
            return std::nullopt;
        return std::atan2(axes[0][1], axes[0][0]);
    }

    std::optional<Transform> TransformOf(std::span<const uint8_t> data)
    {
        const size_t end = std::min(data.size(), Window);
        for (size_t at = 0; at + 96 < end; ++at)
        {
            const auto rows = RotationAt(data, at);
            if (!rows)
                continue;
            const auto ox = Bytes::F64At(data, at + 72), oy = Bytes::F64At(data, at + 80), oz = Bytes::F64At(data, at + 88);
            if (!ox || !oy || !oz)
                continue;
            const std::array<double, 3> origin{ *ox, *oy, *oz };
            if (!std::all_of(origin.begin(), origin.end(), [](double v) { return std::isfinite(v) && std::abs(v) <= Records::MaxFeet; }))
                continue;
            Transform t;
            for (size_t j = 0; j < 3; ++j)
                t.axes[j] = { (*rows)[0][j], (*rows)[1][j], (*rows)[2][j] };
            t.origin = origin;
            return t;
        }
        return std::nullopt;
    }
}
