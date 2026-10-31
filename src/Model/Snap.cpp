// 移植自 BIMCraft crates/core/src/snap.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Snap.h"
#include "I18n/Translator.h"
#include "Model/Geom.h"
#include "Model/Walls.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace MiniBIM::Bim::Snap
{
    namespace
    {
        // 方向离引导线多少度以内还捕捉到它（引导线挨得比这更近时更少）
        constexpr double Tolerance = 4.0;
        // 没有可捕捉的点取整到这个，毫米
        constexpr double Step = 10.0;
        // 光标附近最多拿多少条线互相求交
        constexpr size_t MaxNear = 64;
        // 墙面离中心线最远多少还算光标的，毫米：更厚的墙在超过捕捉范围加这么远时不捕捉
        constexpr double MaxHalfThickness = 2500.0;

        double Norm(double a)
        {
            if (!std::isfinite(a))
                return 0.0;
            a = std::fmod(a, 360.0);
            if (a < 0.0)
                a += 360.0;
            return a >= 360.0 ? 0.0 : a;
        }

        // d 的方向：从正东逆时针的度数，[0, 360)
        double Bearing(P2 d)
        {
            return d.x == 0.0 && d.y == 0.0 ? 0.0 : Norm(std::atan2(d.y, d.x) * 180.0 / std::numbers::pi);
        }

        P2 Round(P2 p) { return { std::round(p.x / Step) * Step, std::round(p.y / Step) * Step }; }

        // 平面上可捕捉的东西
        struct Scene
        {
            std::vector<std::pair<P2, Kind>> points;
            // 直线：墙的中心线、轴网
            std::vector<std::pair<P2, P2>> lines;
            // 墙，和它的厚度
            std::vector<std::pair<Walls::Axis, double>> walls;

            // level 的平面上离 p 不超过 window 毫米的东西：先粗看每个元素的范围，几千面墙的平面每面墙几微秒
            static Scene Of(const Model& m, Guid level, P2 p, double window)
            {
                Scene s;
                const std::optional<Guid> above = m.LevelAbove(level);
                const double cut = m.GetPlanRange(level).cut;
                // pts 外框是否在 p 的 window 以内
                auto near = [&](std::span<const P2> pts)
                {
                    double lx = INFINITY, ly = INFINITY, hx = -INFINITY, hy = -INFINITY;
                    for (const P2& q : pts)
                    {
                        lx = std::min(lx, q.x); ly = std::min(ly, q.y);
                        hx = std::max(hx, q.x); hy = std::max(hy, q.y);
                    }
                    return p.x >= lx - window && p.x <= hx + window && p.y >= ly - window && p.y <= hy + window;
                };
                auto ring = [&](const Ring& pts)
                {
                    if (near(pts))
                        for (const P2& q : pts)
                            s.points.emplace_back(q, Kind::Endpoint);
                };
                for (const auto& [id, ptr] : m.elements.Items())
                {
                    const Element& e = *ptr;
                    if (const Wall* w = e.As<Wall>())
                    {
                        const Walls::Axis a = Walls::Axis::Of(*w);
                        // 圆弧可能鼓出两端之外：取它的整个圆的外框
                        std::vector<P2> reach{ a.start, a.end };
                        if (const auto c = a.Centre(); c && a.Radius())
                        {
                            const double r = *a.Radius();
                            reach = { *c + P2{ -r, -r }, *c + P2{ r, r } };
                        }
                        if (!near(reach) || !m.CutBy(*w, level))
                            continue;
                        s.points.emplace_back(a.start, Kind::Endpoint);
                        s.points.emplace_back(a.end, Kind::Endpoint);
                        s.points.emplace_back(a.At(a.Length() / 2.0, 0.0), Kind::Midpoint);
                        if (const auto c = a.Centre())
                            s.points.emplace_back(*c, Kind::Centre);
                        else
                            s.lines.emplace_back(a.start, a.end);
                        s.walls.emplace_back(a, m.Thickness(*w));
                    }
                    else if (const Grid* g = e.As<Grid>())
                    {
                        const P2 ends[] = { g->start, g->end };
                        if (!near(ends))
                            continue;
                        s.points.emplace_back(g->start, Kind::Endpoint);
                        s.points.emplace_back(g->end, Kind::Endpoint);
                        s.lines.emplace_back(g->start, g->end);
                    }
                    else if (const Column* col = e.AsColumn())
                    {
                        const P2 at[] = { col->at };
                        if (m.Same(col->level, level) && near(at))
                            s.points.emplace_back(col->at, Kind::Centre);
                    }
                    else if (const Run* r = e.AsRun())
                    {
                        const P2 ends[] = { r->start, r->end };
                        if (!m.Same(r->level, level) || !near(ends))
                            continue;
                        s.points.emplace_back(r->start, Kind::Endpoint);
                        s.points.emplace_back(r->end, Kind::Endpoint);
                        s.points.emplace_back(r->start + (r->end - r->start) * 0.5, Kind::Midpoint);
                        s.lines.emplace_back(r->start, r->end);
                    }
                    else if (const Component* c = e.As<Component>())
                    {
                        const P2 at[] = { c->at };
                        if (m.Same(c->level, level) && near(at))
                            s.points.emplace_back(c->at, Kind::Centre);
                    }
                    else if (const Floor* f = e.As<Floor>())
                    {
                        if (!m.Same(f->level, level))
                            continue;
                        ring(f->boundary);
                        for (const Ring& o : f->openings)
                            ring(o);
                    }
                    else if (const Ceiling* cl = e.As<Ceiling>())
                    {
                        if (!m.Same(cl->level, level))
                            continue;
                        ring(cl->boundary);
                        for (const Ring& o : cl->openings)
                            ring(o);
                    }
                    else if (const Roof* r = e.As<Roof>())
                    {
                        if (!m.Same(r->level, level) && !(above && m.Same(r->level, *above)))
                            continue;
                        ring(r->boundary);
                        for (const Ring& o : r->courtyards)
                            ring(o);
                        for (const Ring& o : r->openings)
                            ring(o);
                    }
                    // 剖切面穿过的体量：那里的轮廓
                    else if (const Mass* ms = e.As<Mass>(); ms && m.MassCutBy(*ms, cut))
                        ring(m.MassOutline(*ms, cut));
                }
                return s;
            }
        };

        // 线段 lines 彼此相交处
        std::vector<P2> Crossings(const std::vector<std::pair<P2, P2>>& lines)
        {
            auto on = [](P2 q, P2 a, P2 b) { return Geom::DistToSegment(q, a, b) <= Tol; };
            std::vector<P2> out;
            for (size_t i = 0; i < lines.size(); ++i)
                for (size_t j = i + 1; j < lines.size(); ++j)
                {
                    const auto [a, b] = lines[i];
                    const auto [c, d] = lines[j];
                    if (const auto q = Geom::LineIntersection(a, b, c, d); q && on(*q, a, b) && on(*q, c, d))
                        out.push_back(*q);
                }
            return out;
        }

        // 从 l 沿单位向量 u 的射线与线段 a→b 的交点
        std::optional<P2> RayHit(P2 l, P2 u, P2 a, P2 b)
        {
            const P2 e = b - a;
            const double len = e.Length(), den = u.Cross(e);
            if (std::abs(den) <= 1e-12 * len)
                return std::nullopt;
            const double s = (a - l).Cross(e) / den, t = (a - l).Cross(u) / den;
            if (s > 0.0 && t * len >= -Tol && t * len <= len + Tol)
                return l + u * s;
            return std::nullopt;
        }

        // 已有一段在最后一点之前时，它与沿 d 往前的一段的夹角
        std::optional<double> CornerOn(std::span<const P2> from, P2 d)
        {
            if (from.size() < 2)
                return std::nullopt;
            // 够长、算得上一段的一步，不管 d 多长
            return Corner(from[from.size() - 2], from.back(), from.back() + d.Unit() * (2.0 * Tol));
        }

        // 一样近时点的先后：端点、交点、中点、圆心
        int Rank(Kind k)
        {
            switch (k)
            {
            case Kind::Endpoint:     return 0;
            case Kind::Intersection: return 1;
            case Kind::Midpoint:     return 2;
            default:                 return 3;
            }
        }
    }

    const char* Name(Kind k)
    {
        switch (k)
        {
        case Kind::Endpoint:      return N_("Endpoint");
        case Kind::Midpoint:      return N_("Midpoint");
        case Kind::Intersection:  return N_("Intersection");
        case Kind::Centre:        return N_("Centre");
        case Kind::Perpendicular: return N_("Perpendicular");
        case Kind::Nearest:       return N_("Nearest");
        case Kind::Angle:         return N_("Angle");
        case Kind::Free:          return "";
        }
        return "";
    }

    Snaps Snaps::None()
    {
        Snaps o;
        o.endpoint = o.midpoint = o.intersection = o.centre = o.perpendicular = o.nearest = false;
        o.angle = 0.0;
        return o;
    }

    std::vector<double> Rays(std::span<const P2> from, double inc)
    {
        if (!(inc >= 1.0 && inc <= 180.0))
            return {};
        // 转一圈要几条：步长除不尽 360° 时最后一条差一点到 360°
        const size_t n = static_cast<size_t>(std::min(std::ceil(360.0 / inc - 1e-9), 360.0));
        std::vector<double> out;
        for (size_t k = 0; k < n; ++k)
            out.push_back(static_cast<double>(k) * inc);
        if (from.size() >= 2 && from[from.size() - 2].Dist(from.back()) > Tol)
        {
            const double base = Bearing(from.back() - from[from.size() - 2]);
            for (size_t k = 0; k < n; ++k)
                out.push_back(Norm(base + static_cast<double>(k) * inc));
        }
        return out;
    }

    std::optional<double> Corner(P2 prev, P2 last, P2 next)
    {
        if (!(prev.Dist(last) > Tol) || !(last.Dist(next) > Tol))
            return std::nullopt;
        const double turn = Norm(Bearing(next - last) - Bearing(last - prev));
        return 180.0 - (turn > 180.0 ? 360.0 - turn : turn);
    }

    Found At(const Model& m, Guid level, P2 p, Reach reach, std::span<const P2> from, bool square, const Snaps& o,
             std::span<const Segment> extraLines, std::span<const std::pair<P2, Kind>> extraPoints)
    {
        Scene scene = Scene::Of(m, level, p, std::max(reach.point, reach.line) + MaxHalfThickness);
        for (const auto& [a, b] : extraLines)
        {
            scene.points.emplace_back(a, Kind::Endpoint);
            scene.points.emplace_back(b, Kind::Endpoint);
            scene.points.emplace_back((a + b) * 0.5, Kind::Midpoint);
            scene.lines.emplace_back(a, b);
        }
        for (const auto& pk : extraPoints)
            scene.points.push_back(pk);
        const std::optional<P2> last = from.empty() ? std::nullopt : std::optional<P2>(from.back());
        // 保持方正时钉在最后一点上的那个坐标：沿 x 走时是 y
        std::optional<std::pair<bool, double>> pinned;
        if (square && last)
        {
            if (std::abs(p.x - last->x) > std::abs(p.y - last->y))
            {
                p.y = last->y;
                pinned = std::pair{ true, last->y };
            }
            else
            {
                p.x = last->x;
                pinned = std::pair{ false, last->x };
            }
        }
        auto closer = [&](P2 a, P2 b) { return a.Dist(p) < b.Dist(p); };
        // 经过光标附近的线：能与它相交、在它上面作垂足的
        std::vector<std::pair<P2, P2>> near;
        for (const auto& [a, b] : scene.lines)
            if (Geom::DistToSegment(p, a, b) <= reach.point)
                near.emplace_back(a, b);
        std::sort(near.begin(), near.end(), [&](const auto& x, const auto& y)
                  { return Geom::DistToSegment(p, x.first, x.second) < Geom::DistToSegment(p, y.first, y.second); });
        if (near.size() > MaxNear)
            near.resize(MaxNear);

        // 平面上这里有的：端点、中点、圆心，已经点过的点，交点
        auto wanted = [&](Kind k)
        {
            switch (k)
            {
            case Kind::Endpoint: return o.endpoint;
            case Kind::Midpoint: return o.midpoint;
            case Kind::Centre:   return o.centre;
            default:             return true;
            }
        };
        std::vector<std::pair<P2, Kind>> points;
        for (const auto& pk : scene.points)
            if (wanted(pk.second))
                points.push_back(pk);
        if (o.endpoint)
            for (const P2& q : from)
                points.emplace_back(q, Kind::Endpoint);
        if (o.intersection)
            for (const P2& q : Crossings(near))
                points.emplace_back(q, Kind::Intersection);
        const std::pair<P2, Kind>* hit = nullptr;
        for (const auto& pk : points)
        {
            if (!(pk.first.Dist(p) <= reach.point))
                continue;
            if (!hit || pk.first.Dist(p) < hit->first.Dist(p) || (pk.first.Dist(p) == hit->first.Dist(p) && Rank(pk.second) < Rank(hit->second)))
                hit = &pk;
        }
        if (hit)
            return Found{ hit->first, hit->second, std::nullopt };

        // 从最后一点作的垂足
        if (o.perpendicular && last)
        {
            std::optional<P2> foot;
            for (const auto& [a, b] : near)
            {
                const double len = (b - a).Length();
                const P2 u = (b - a).Unit();
                const double t = (*last - a).Dot(u);
                if (!(len > 0.0) || t < 0.0 || t > len)
                    continue;
                const P2 f = a + u * t;
                if (f.Dist(p) <= reach.point && f.Dist(*last) > Step && (!foot || closer(f, *foot)))
                    foot = f;
            }
            if (foot)
                return Found{ Micron(*foot), Kind::Perpendicular, Guide{ *last, Bearing(*foot - *last), CornerOn(from, *foot - *last) } };
        }

        // 过最后一点、光标离得最近的角度引导线
        if (o.angle >= 1.0 && o.angle <= 180.0 && last && p.Dist(*last) > 2.0 * Step)
        {
            const P2 d = p - *last;
            const double tolerance = std::min(Tolerance, 0.4 * o.angle);
            struct Ray { double angle; P2 u; double along, perp; };
            std::optional<Ray> best;
            for (const double a : Rays(from, o.angle))
            {
                const double r = a * std::numbers::pi / 180.0;
                const P2 u{ std::cos(r), std::sin(r) };
                const double along = u.Dot(d), perp = std::abs(u.Cross(d));
                if (along > 0.0 && perp <= reach.line && std::atan(perp / along) * 180.0 / std::numbers::pi <= tolerance && (!best || perp < best->perp))
                    best = Ray{ a, u, along, perp };
            }
            if (best)
            {
                const Guide guide{ *last, best->angle, CornerOn(from, best->u) };
                // 它穿过光标附近的墙或轴网处
                std::optional<P2> cross;
                for (const auto& [a, b] : near)
                    if (const auto q = RayHit(*last, best->u, a, b); q && q->Dist(p) <= reach.point && (!cross || closer(*q, *cross)))
                        cross = *q;
                // 否则正好在它上面的整点，否则长度取最近的 10 毫米
                const P2 grid = Round(p);
                const bool onGrid = std::abs(best->u.Cross(grid - *last)) <= 1e-3 && best->u.Dot(grid - *last) > 0.0;
                if (cross && o.intersection)
                    return Found{ Micron(*cross), Kind::Intersection, guide };
                if (onGrid)
                    return Found{ grid, Kind::Angle, guide };
                return Found{ Micron(*last + best->u * (std::max(std::round(best->along / Step), 1.0) * Step)), Kind::Angle, guide };
            }
        }

        // 墙的中心线上，从它的起点量取最近的 10 毫米
        if (o.nearest)
        {
            const std::pair<Walls::Axis, double>* on = nullptr;
            double best = INFINITY;
            for (const auto& wt : scene.walls)
            {
                const double dist = wt.first.Distance(p, 0.0) - wt.second / 2.0;
                if (dist <= reach.line && dist < best)
                {
                    best = dist;
                    on   = &wt;
                }
            }
            if (on)
            {
                const Walls::Axis& a = on->first;
                return Found{ Micron(a.At(std::round(a.Project(p).first / Step) * Step, 0.0)), Kind::Nearest, std::nullopt };
            }
        }
        // 取整，但不离开保持方正的那条线
        P2 at = Round(p);
        if (pinned)
            (pinned->first ? at.y : at.x) = pinned->second;
        return Found{ at, Kind::Free, std::nullopt };
    }

    Ruler Measure(P2 a, P2 b)
    {
        const P2 d = b - a;
        return Ruler{ d.Length(), d.x, d.y, Bearing(d) };
    }

    double Graduation(double pxPerMm)
    {
        if (!(std::isfinite(pxPerMm) && pxPerMm > 0.0))
            return 100.0;
        const double least = 10.0 / pxPerMm;
        const double decade = std::pow(10.0, std::floor(std::log10(least)));
        for (const double k : { 1.0, 2.0, 5.0, 10.0 })
            if (k * decade >= least * (1.0 - 1e-9))
                return k * decade;
        return 10.0 * decade;
    }
}
