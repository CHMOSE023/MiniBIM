// 移植自 BIMCraft crates/core/src/curves.rs（f3bbb33）的多段线与圆，MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Curve.h"
#include "Model/Model.h"
#include "Model/Text.h"
#include "Model/Walls.h"
#include <algorithm>
#include <cmath>

namespace MiniBIM::Bim
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        // 一条线最多切成这么多段画
        constexpr size_t kMaxDrawn = 200000;

        // 多段线的各段
        std::vector<Walls::Axis> Segments(const Curve& c)
        {
            std::vector<Walls::Axis> out;
            if (c.kind != Curve::Kind::Poly)
                return out;
            const size_t n = c.points.size();
            const size_t count = c.closed && n >= 2 ? n : (n == 0 ? 0 : n - 1);
            for (size_t i = 0; i < count; ++i)
                out.emplace_back(c.points[i], c.points[(i + 1) % n], i < c.arcs.size() ? c.arcs[i] : 0.0);
            return out;
        }

        // 圆切成多少段：弦离圆不超过 1 毫米，至少 24 段
        size_t CircleSteps(double r)
        {
            if (!(r > 1.0))
                return 24;
            const double step = 2.0 * std::acos(std::max(0.0, 1.0 - 1.0 / r));
            return std::clamp<size_t>(static_cast<size_t>(std::ceil(2.0 * kPi / std::max(step, 1e-6))), 24, 4096);
        }
    }

    Curve Curve::Line(P2 a, P2 b) { return Curve{ Kind::Poly, { a, b } }; }

    Curve Curve::Arc(P2 a, P2 b, double sweep)
    {
        Curve c = Line(a, b);
        if (sweep != 0.0)
            c.arcs = { sweep };
        return c;
    }

    Curve Curve::CircleOf(P2 centre, double radius)
    {
        Curve c;
        c.kind   = Kind::Circle;
        c.centre = centre;
        c.radius = radius;
        return c;
    }

    const char* Curve::Name() const
    {
        if (kind == Kind::Circle)
            return "circle";
        if (!closed && points.size() == 2)
            return !arcs.empty() && arcs[0] != 0.0 ? "arc" : "line";
        return "polyline";
    }

    bool Curve::IsClosed() const { return kind == Kind::Circle || closed; }

    P2 Curve::Start() const
    {
        if (kind == Kind::Circle)
            return centre + P2(radius, 0.0);
        return points.empty() ? P2{} : points.front();
    }

    P2 Curve::End() const
    {
        if (kind == Kind::Circle || closed)
            return Start();
        return points.empty() ? P2{} : points.back();
    }

    double Curve::Length() const
    {
        if (kind == Kind::Circle)
            return 2.0 * kPi * radius;
        double s = 0.0;
        for (const Walls::Axis& a : Segments(*this))
            s += a.Length();
        return s;
    }

    std::vector<P2> Curve::Points() const
    {
        std::vector<P2> out;
        if (kind == Kind::Circle)
        {
            const size_t n = CircleSteps(radius);
            for (size_t i = 0; i <= n; ++i)
            {
                const double t = 2.0 * kPi * static_cast<double>(i % n) / static_cast<double>(n);
                out.push_back(centre + P2(std::cos(t), std::sin(t)) * radius);
            }
            return out;
        }
        const auto segs = Segments(*this);
        for (size_t i = 0; i < segs.size(); ++i)
        {
            std::vector<P2> pts = segs[i].Points(0.0, segs[i].Length(), 0.0);
            if (pts.empty())
                continue;
            // 各段自己的两端照给的点：圆弧上算出来的端点差一点舍入误差
            pts.front() = segs[i].start;
            pts.back()  = segs[i].end;
            out.insert(out.end(), pts.begin() + (out.empty() ? 0 : 1), pts.end());
        }
        if (segs.empty() && !points.empty())
            out = points;
        return out;
    }

    std::vector<P2> Curve::Centres() const
    {
        if (kind == Kind::Circle)
            return { centre };
        std::vector<P2> out;
        for (const Walls::Axis& a : Segments(*this))
            if (const auto c = a.Centre())
                out.push_back(*c);
        return out;
    }

    std::optional<std::pair<P2, double>> Curve::Round() const
    {
        if (kind == Kind::Circle)
            return std::pair{ centre, radius };
        if (std::string_view(Name()) != "arc")
            return std::nullopt;
        const Walls::Axis a(points[0], points[1], arcs[0]);
        if (const auto c = a.Centre(); c && a.Radius())
            return std::pair{ *c, *a.Radius() };
        return std::nullopt;
    }

    Curve Curve::Map(const std::function<P2(P2)>& f, bool mirror) const
    {
        Curve c = *this;
        if (kind == Kind::Circle)
        {
            c.centre = f(centre);
            return c;
        }
        for (P2& p : c.points)
            p = f(p);
        if (mirror)
            for (double& a : c.arcs)
                a = -a + 0.0;
        return c;
    }

    double Curve::Distance(P2 p) const
    {
        if (kind == Kind::Circle)
            return std::abs(p.Dist(centre) - radius);
        const std::vector<P2> pts = Points();
        double best = pts.size() == 1 ? p.Dist(pts[0]) : INFINITY;
        for (size_t i = 0; i + 1 < pts.size(); ++i)
            best = std::min(best, Geom::DistToSegment(p, pts[i], pts[i + 1]));
        return best;
    }

    bool Curve::Check(const std::string& who, std::string& error) const
    {
        const auto fine = [](P2 p) { return p.IsFinite() && std::abs(p.x) <= MaxCoord && std::abs(p.y) <= MaxCoord; };
        if (kind == Kind::Circle)
        {
            if (!fine(centre))
            {
                error = who + ": its centre isn't a point within " + Text::Shortest(MaxCoord) + " mm";
                return false;
            }
            if (!(std::isfinite(radius) && radius > Tol && radius <= MaxCoord) ||
                std::max(std::abs(centre.x), std::abs(centre.y)) + radius > MaxCoord)
            {
                error = who + ": its radius is " + Text::Shortest(radius) + " mm; it's more than 1 mm and at most " + Text::Shortest(MaxCoord);
                return false;
            }
            return true;
        }
        const size_t n = points.size();
        if (n < 2)
        {
            error = who + ": a line needs two points or more, not " + std::to_string(n);
            return false;
        }
        if (n > MaxPoints)
        {
            error = who + ": a line has at most " + std::to_string(MaxPoints) + " points";
            return false;
        }
        const bool straight = std::all_of(arcs.begin(), arcs.end(), [](double a) { return a == 0.0; });
        if (closed && n < 3 && straight)
        {
            error = who + ": a closed line needs three points or more, or arcs";
            return false;
        }
        if (!std::all_of(points.begin(), points.end(), fine))
        {
            error = who + ": a point isn't a number within " + Text::Shortest(MaxCoord) + " mm";
            return false;
        }
        bool distinct = true;
        for (size_t i = 0; i + 1 < n; ++i)
            distinct = distinct && points[i].Dist(points[i + 1]) > 1e-6;
        if (!distinct || (closed && points.front().Dist(points.back()) <= 1e-6))
        {
            error = who + ": two points one after another are the same; a line runs between points apart";
            return false;
        }
        const size_t segs = closed ? n : n - 1;
        if (!arcs.empty() && arcs.size() != segs)
        {
            error = who + ": `arcs` gives one turn for each of its " + std::to_string(segs) + " segments, not " + std::to_string(arcs.size());
            return false;
        }
        for (const double a : arcs)
            if (!(std::isfinite(a) && std::abs(a) < 360.0))
            {
                error = who + ": an arc turns " + Text::Shortest(a) + "°; it turns less than a whole circle, either way";
                return false;
            }
        size_t drawn = 0;
        for (const Walls::Axis& a : Segments(*this))
        {
            if (const auto c = a.Centre(); c && a.Radius() && std::max(std::abs(c->x), std::abs(c->y)) + *a.Radius() > MaxCoord)
            {
                error = who + ": an arc of it is part of a circle that reaches further than " + Text::Shortest(MaxCoord) +
                        " mm from the origin: it turns almost nothing, or almost a whole circle, so its circle is huge";
                return false;
            }
            drawn += a.Stations(0.0, a.Length()).size();
        }
        if (drawn > kMaxDrawn)
        {
            error = who + ": its arcs take more than " + std::to_string(kMaxDrawn) + " points to draw within 1 mm; use fewer, or smaller ones";
            return false;
        }
        if (Length() <= Tol)
        {
            error = who + ": it is 1 mm long or less";
            return false;
        }
        return true;
    }
}
