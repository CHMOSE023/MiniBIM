// 移植自 BIMCraft crates/core/src/runs.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Runs.h"
#include "Model/Model.h"
#include "Model/Text.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace MiniBIM::Bim::Runs
{
    namespace
    {
        P2 Heading(double deg)
        {
            const double a = deg * std::numbers::pi / 180.0;
            return { std::cos(a), std::sin(a) };
        }

        P3 Unit(const P3& v)
        {
            const double l = std::sqrt(V3::Dot(v, v));
            return l > 0.0 ? V3::Mul(v, 1.0 / l) : v;
        }

        std::string Number(double v, int digits)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.*f", digits, v);
            return buf;
        }
    }

    bool MayStand(std::string_view category)
    {
        return category != "beam" && category != "stripfooting";
    }

    const std::vector<IfcRow>& IfcTable()
    {
        static const std::vector<IfcRow> table = {
            { "beam", "IFCBEAM", ".BEAM." },
            { "stripfooting", "IFCFOOTING", ".STRIP_FOOTING." },
            { "pipe", "IFCPIPESEGMENT", ".RIGIDSEGMENT." },
            { "conduit", "IFCCABLECARRIERSEGMENT", ".CONDUITSEGMENT." },
            { "cabletray", "IFCCABLECARRIERSEGMENT", ".CABLETRAYSEGMENT." },
            // 梯架在本程序是桥架，写成桥架：只有别的程序的文件这么写
            { "cabletray", "IFCCABLECARRIERSEGMENT", ".CABLELADDERSEGMENT." },
            { "wire", "IFCCABLESEGMENT", ".CABLESEGMENT." },
            // IFC 没有防雷：引下线与接地线都是导体段（本程序自己的类别分开它们）
            { "downconductor", "IFCCABLESEGMENT", ".CONDUCTORSEGMENT." },
            { "groundconductor", "IFCCABLESEGMENT", ".CONDUCTORSEGMENT." },
            // 数据缆线同导线是缆线段（本程序自己的类别分开它们）
            { "cable", "IFCCABLESEGMENT", ".CABLESEGMENT." },
        };
        return table;
    }

    std::optional<std::pair<const char*, const char*>> Ifc(std::string_view category)
    {
        for (const IfcRow& r : IfcTable())
            if (category == r.category)
                return std::pair{ r.ifcClass, r.kind };
        return std::nullopt;
    }

    std::optional<const char*> FromIfc(std::string_view ifcClass, std::optional<std::string_view> kind, std::optional<std::string_view> declared)
    {
        if (ifcClass == "IFCBEAMSTANDARDCASE")
            ifcClass = "IFCBEAM";
        auto trim = [](std::string_view t)
        {
            while (!t.empty() && t.front() == '.')
                t.remove_prefix(1);
            while (!t.empty() && t.back() == '.')
                t.remove_suffix(1);
            return t;
        };
        if (declared)
            for (const IfcRow& r : IfcTable())
                if (ifcClass == r.ifcClass && *declared == r.category)
                    return r.category;
        if (kind)
            for (const IfcRow& r : IfcTable())
                if (ifcClass == r.ifcClass && trim(r.kind) == trim(*kind))
                    return r.category;
        for (const IfcRow& r : IfcTable())
            if (ifcClass == r.ifcClass)
                return r.category;
        return std::nullopt;
    }

    Justify DefaultJustify(std::string_view category)
    {
        return MayStand(category) ? Justify::Centre : Justify::Top;
    }

    bool Check(const std::string& who, std::string_view category, const Geom& g, std::string& error)
    {
        if (g.Length() <= Tol)
        {
            error = who + ": too short (" + Text::Shortest(g.Length()) + " mm)";
            return false;
        }
        if (!MayStand(category))
        {
            const std::string c(category);
            if (g.Vertical())
            {
                error = who + ": its ends are one over the other; a " + c + " runs along, and what stands is a column";
                return false;
            }
            const double tilt = std::atan2(std::abs(g.b[2] - g.a[2]), g.PlanLength()) * 180.0 / std::numbers::pi;
            if (tilt > MaxSlope)
            {
                error = who + ": it slopes " + Number(tilt, 1) + "°, more than the " + Text::Shortest(MaxSlope) + "° a " + c + " may; what stands is a column";
                return false;
            }
        }
        return true;
    }

    Geom Geom::Of(const Model& m, const Run& r, const RunType& t)
    {
        const double base = m.Elevation(r.level);
        const double z0 = base + r.offset, z1 = base + r.endOffset.value_or(r.offset);
        const double depth = t.round ? t.width : t.depth;
        Geom g{ { r.start.x, r.start.y, z0 }, { r.end.x, r.end.y, z1 }, t.width, depth, t.round };
        // 竖着的没有顶和底当那条线
        double shift = 0.0;
        if (r.justify == Justify::Top)
            shift = -g.Half();
        else if (r.justify == Justify::Bottom)
            shift = g.Half();
        g.a[2] = z0 + shift;
        g.b[2] = z1 + shift;
        return g;
    }

    double Geom::Length() const
    {
        return std::hypot(PlanLength(), b[2] - a[2]);
    }

    double Geom::PlanLength() const
    {
        return P2{ b[0] - a[0], b[1] - a[1] }.Dist(P2{ 0.0, 0.0 });
    }

    bool Geom::Vertical() const
    {
        return PlanLength() <= Tol;
    }

    double Geom::Half() const
    {
        const double plan = PlanLength();
        return plan > Tol ? depth / 2.0 * Length() / plan : 0.0;
    }

    Ring Geom::Footprint() const
    {
        const P2 pa{ a[0], a[1] }, pb{ b[0], b[1] };
        if (!Vertical())
        {
            const P2 n = (pb - pa).Unit().Perp() * (width / 2.0);
            return { pa - n, pb - n, pb + n, pa + n };
        }
        Ring out;
        if (round)
        {
            for (size_t i = 0; i < Sides; ++i)
                out.push_back(pa + Heading(360.0 * double(i) / double(Sides)) * (width / 2.0));
            return out;
        }
        const double w = width / 2.0, d = depth / 2.0;
        for (const auto& [x, y] : { std::pair{ -w, -d }, std::pair{ w, -d }, std::pair{ w, d }, std::pair{ -w, d } })
            out.push_back(pa + P2{ x, y });
        return out;
    }

    std::pair<double, double> Geom::ZSpan() const
    {
        const double h = Half();
        return { std::min(a[2], b[2]) - h, std::max(a[2], b[2]) + h };
    }

    Geom::PrismOf Geom::Prism() const
    {
        Ring ring = Footprint();
        if (Vertical())
        {
            const auto [lo, hi] = ZSpan();
            return { std::move(ring), Plane::Flat(lo), Plane::Flat(hi) };
        }
        const P2 pa{ a[0], a[1] };
        const P2 g = P2{ b[0] - a[0], b[1] - a[1] }.Unit() * ((b[2] - a[2]) / PlanLength());
        const double z = a[2] - g.Dot(pa);
        return { std::move(ring), Plane{ z - Half(), g }, Plane{ z + Half(), g } };
    }

    std::pair<std::vector<Tri>, size_t> Geom::Tube() const
    {
        const P3 d = Unit(V3::Sub(b, a));
        const double r = width / 2.0;
        const P3 e1 = Vertical() ? P3{ 1.0, 0.0, 0.0 } : Unit(P3{ -d[1], d[0], 0.0 });
        const P3 e2 = V3::Cross(d, e1);
        auto ring = [&](const P3& c)
        {
            std::vector<P3> out;
            for (size_t k = 0; k < Sides; ++k)
            {
                const double t = 2.0 * std::numbers::pi * double(k) / double(Sides);
                out.push_back(V3::Add(c, V3::Add(V3::Mul(e1, r * std::cos(t)), V3::Mul(e2, r * std::sin(t)))));
            }
            return out;
        };
        const std::vector<P3> ra = ring(a), rb = ring(b);
        auto next = [](size_t k) { return (k + 1) % Sides; };
        std::vector<Tri> tris;
        tris.reserve(4 * Sides);
        for (size_t k = 0; k < Sides; ++k)
        {
            tris.push_back({ a, ra[next(k)], ra[k] });
            tris.push_back({ b, rb[k], rb[next(k)] });
        }
        for (size_t k = 0; k < Sides; ++k)
        {
            tris.push_back({ ra[k], ra[next(k)], rb[k] });
            tris.push_back({ ra[next(k)], rb[next(k)], rb[k] });
        }
        return { std::move(tris), 2 * Sides };
    }
}
