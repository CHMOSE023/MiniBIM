// 移植自 BIMCraft crates/rvt/src/curves.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_beam_axes.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Curves.h).
#include "Rvt/Curves.h"
#include "Rvt/Bytes.h"
#include "Rvt/Records.h"

#include <cmath>

namespace MiniBIM::Rvt::Curves
{
    namespace
    {
        using Buf = std::span<const uint8_t>;

        // 有界直线或圆弧开头的字节
        constexpr uint8_t Tag[4] = { 0x04, 0x00, 0x08, 0x01 };

        // 曲线记录的值：标签之后 N 个，都有限且不超过 Records::MaxFeet
        template <size_t N>
        std::optional<std::array<double, N>> Values(Buf buf, size_t at)
        {
            if (!Bytes::Matches(buf, at, Tag))
                return std::nullopt;
            std::array<double, N> out{};
            for (size_t i = 0; i < N; ++i)
            {
                const auto x = Bytes::F64At(buf, at + 4 + 8 * i);
                if (!x || !std::isfinite(*x) || std::abs(*x) > Records::MaxFeet)
                    return std::nullopt;
                out[i] = *x;
            }
            return out;
        }

        bool Unit(const Vec3& v) { return std::abs(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] - 1.0) <= 1e-9; }
    }

    Vec3 BoundedArc::Point(double angle) const
    {
        const double s = std::sin(angle), c = std::cos(angle);
        Vec3 p{};
        for (size_t a = 0; a < 3; ++a)
            p[a] = centre[a] + radius * (c * xAxis[a] + s * yAxis[a]);
        return p;
    }

    std::optional<BoundedLine> LineAt(Buf buf, size_t at)
    {
        const auto v = Values<8>(buf, at);
        if (!v)
            return std::nullopt;
        const auto& [startParameter, endParameter, ox, oy, oz, dx, dy, dz] = *v;
        if (!Unit({ dx, dy, dz }) || !(startParameter < endParameter))
            return std::nullopt;
        return BoundedLine{ startParameter, endParameter, { ox, oy, oz }, { dx, dy, dz } };
    }

    std::optional<BoundedLine> FirstLine(Buf data)
    {
        const size_t first = Bytes::Find(data, Tag);
        if (first == Bytes::npos || ArcAt(data, first))
            return std::nullopt;
        for (size_t at = first; at != Bytes::npos; at = Bytes::Find(data, Tag, at + sizeof Tag))
            if (const auto line = LineAt(data, at))
                return line;
        return std::nullopt;
    }

    std::optional<BoundedArc> ArcAt(Buf buf, size_t at)
    {
        if (at < 8 || Bytes::U64At(buf, at - 8) != uint64_t(1))
            return std::nullopt;
        return ArcValuesAt(buf, at);
    }

    std::optional<BoundedArc> ArcValuesAt(Buf buf, size_t at)
    {
        const auto v = Values<12>(buf, at);
        if (!v)
            return std::nullopt;
        const auto& [startAngle, endAngle, xx, xy, xz, yx, yy, yz, radius, cx, cy, cz] = *v;
        const Vec3 x{ xx, xy, xz }, y{ yx, yy, yz };
        if (!Unit(x) || !Unit(y) || std::abs(xx * yx + xy * yy + xz * yz) > 1e-9 || !(radius > 0.0) || !(startAngle < endAngle))
            return std::nullopt;
        return BoundedArc{ startAngle, endAngle, x, y, radius, { cx, cy, cz } };
    }

    std::optional<BoundedArc> FirstArc(Buf data)
    {
        const size_t first = Bytes::Find(data, Tag);
        return first == Bytes::npos ? std::nullopt : ArcAt(data, first);
    }
}
