// 移植自 BIMCraft crates/core/src/geom.rs、walls.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Geom.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace MiniBIM::Bim
{
    std::vector<P3> Plane::Lift(const std::vector<P2>& ring) const
    {
        std::vector<P3> out;
        out.reserve(ring.size());
        for (const P2& q : ring)
            out.push_back({ q.x, q.y, At(q) });
        return out;
    }

    std::optional<Plane> Plane::Of(const std::vector<P3>& face)
    {
        if (face.empty())
            return std::nullopt;
        P3 n{ 0.0, 0.0, 0.0 };
        for (size_t i = 0; i < face.size(); ++i)
        {
            const P3& a = face[i];
            const P3& b = face[(i + 1) % face.size()];
            n[0] += (a[1] - b[1]) * (a[2] + b[2]);
            n[1] += (a[2] - b[2]) * (a[0] + b[0]);
            n[2] += (a[0] - b[0]) * (a[1] + b[1]);
        }
        if (std::abs(n[2]) <= 1e-9)
            return std::nullopt;
        const P3& o = face.front();
        const P2 g(-n[0] / n[2], -n[1] / n[2]);
        return Plane{ o[2] - g.x * o[0] - g.y * o[1], g };
    }
}

namespace MiniBIM::Bim::Geom
{
    std::array<P2, 4> EdgeGrips(const std::array<P2, 2>& rect)
    {
        const P2 lo = rect[0], hi = rect[1];
        const P2 mid = (lo + hi) * 0.5;
        return { P2(lo.x, mid.y), P2(hi.x, mid.y), P2(mid.x, lo.y), P2(mid.x, hi.y) };
    }

    std::array<P2, 2> MoveEdge(const std::array<P2, 2>& rect, size_t k, P2 p)
    {
        const P2 lo = rect[0], hi = rect[1];
        switch (k)
        {
        case 0:  return { P2(std::min(p.x, hi.x - 100.0), lo.y), hi };
        case 1:  return { lo, P2(std::max(p.x, lo.x + 100.0), hi.y) };
        case 2:  return { P2(lo.x, std::min(p.y, hi.y - 100.0)), hi };
        default: return { lo, P2(hi.x, std::max(p.y, lo.y + 100.0)) };
        }
    }

    std::vector<Segment> RingEdges(const Ring& ring)
    {
        std::vector<Segment> out;
        out.reserve(ring.size());
        for (size_t i = 0; i < ring.size(); ++i)
            out.emplace_back(ring[i], ring[(i + 1) % ring.size()]);
        return out;
    }

    double Area(const Ring& ring)
    {
        double sum = 0.0;
        for (size_t i = 0; i < ring.size(); ++i)
            sum += ring[i].Cross(ring[(i + 1) % ring.size()]);
        return sum / 2.0;
    }

    double Perimeter(const Ring& ring)
    {
        double sum = 0.0;
        for (size_t i = 0; i < ring.size(); ++i)
            sum += ring[i].Dist(ring[(i + 1) % ring.size()]);
        return sum;
    }

    bool Contains(const Ring& ring, P2 p)
    {
        // (a.y > p.y) != (b.y > p.y) 排除了水平边，除法安全
        size_t count = 0;
        for (size_t i = 0; i < ring.size(); ++i)
        {
            const P2 a = ring[i], b = ring[(i + 1) % ring.size()];
            if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y))
                ++count;
        }
        return count % 2 == 1;
    }

    bool Parallel(P2 a, P2 b, P2 c, P2 d)
    {
        return std::abs((b - a).Unit().Cross((d - c).Unit())) <= 1e-3;
    }

    Ring Corners(const Ring& ring)
    {
        const size_t n = ring.size();
        auto pt = [&](size_t i) { return n == 0 ? P2{} : ring[i % n]; };
        Ring out;
        for (size_t i = 0; i < n; ++i)
        {
            const P2 a = pt(i + n - 1), b = pt(i), c = pt(i + 1);
            if (std::abs((b - a).Unit().Cross((c - b).Unit())) > 1e-6)
                out.push_back(b);
        }
        return out;
    }

    Ring Grow(const Ring& ring, double d)
    {
        const size_t n = ring.size();
        auto pt = [&](size_t i) { return n == 0 ? P2{} : ring[i % n]; };
        // 边外移：逆时针走时移到它的右侧
        auto out = [d](P2 a, P2 b)
        {
            const P2 shift = (b - a).Unit().Perp() * -d;
            return Segment{ a + shift, b + shift };
        };
        Ring grown;
        grown.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            const auto [a, b] = out(pt(i + n - 1), pt(i));
            const auto [c, e] = out(pt(i), pt(i + 1));
            grown.push_back(LineIntersection(a, b, c, e).value_or(b));
        }
        return grown;
    }

    Ring Hull(std::vector<P2> points)
    {
        std::sort(points.begin(), points.end(), [](const P2& a, const P2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
        points.erase(std::unique(points.begin(), points.end(), [](const P2& a, const P2& b) { return a.Dist(b) < 1e-9; }), points.end());
        if (points.size() < 3)
            return points;
        Ring out;
        out.reserve(points.size() + 1);
        for (int pass = 0; pass < 2; ++pass)
        {
            const size_t start = out.size();
            auto push = [&](const P2& p)
            {
                while (out.size() >= start + 2 && (out[out.size() - 1] - out[out.size() - 2]).Cross(p - out[out.size() - 2]) <= 0.0)
                    out.pop_back();
                out.push_back(p);
            };
            if (pass == 0)
                for (const P2& p : points) push(p);
            else
                for (auto it = points.rbegin(); it != points.rend(); ++it) push(*it);
            // 每条链的最后一点是另一条的起点
            out.pop_back();
        }
        return out;
    }

    std::vector<std::array<size_t, 3>> Triangulate(const Ring& ring)
    {
        auto pt = [&](size_t i) { return i < ring.size() ? ring[i] : P2{}; };
        std::vector<size_t> left;
        for (size_t i = 0; i < ring.size(); ++i)
            if (left.empty() || pt(left.back()).Dist(pt(i)) >= 1e-6)
                left.push_back(i);
        if (left.size() > 1 && pt(left.front()).Dist(pt(left.back())) < 1e-6)
            left.pop_back();
        if (Area(ring) < 0.0)
            std::reverse(left.begin(), left.end());
        // p 在逆时针三角形 a、b、c 内或边上
        auto inside = [](P2 p, P2 a, P2 b, P2 c)
        {
            return (b - a).Cross(p - a) >= 0.0 && (c - b).Cross(p - b) >= 0.0 && (a - c).Cross(p - c) >= 0.0;
        };
        std::vector<std::array<size_t, 3>> out;
        // O(n³) 的耳切，楼板轮廓够用；轮廓多到上千点时换扫描线
        while (left.size() >= 3)
        {
            const size_t n = left.size();
            auto corner = [&](size_t k) { return std::array<size_t, 3>{ left[(k + n - 1) % n], left[k], left[(k + 1) % n] }; };
            std::optional<size_t> ear;
            for (size_t k = 0; k < n && !ear; ++k)
            {
                const auto [a, b, c] = corner(k);
                if ((pt(b) - pt(a)).Cross(pt(c) - pt(b)) <= 0.0)
                    continue;
                const bool empty = std::all_of(left.begin(), left.end(), [&](size_t j)
                {
                    return j == a || j == b || j == c || !inside(pt(j), pt(a), pt(b), pt(c));
                });
                if (empty)
                    ear = k;
            }
            if (!ear)
                break;
            out.push_back(corner(*ear));
            left.erase(left.begin() + static_cast<std::ptrdiff_t>(*ear));
        }
        return out;
    }

    Ring Clip(const Ring& ring, const std::function<double(P2)>& f)
    {
        Ring kept;
        kept.reserve(ring.size() + 2);
        for (size_t i = 0; i < ring.size(); ++i)
        {
            const P2 a = ring[i], b = ring[(i + 1) % ring.size()];
            const double fa = f(a), fb = f(b);
            if (fa >= 0.0)
                kept.push_back(a);
            if ((fa >= 0.0) != (fb >= 0.0))
                kept.push_back(a + (b - a) * (fa / (fa - fb)));
        }
        return kept;
    }

    double ParamOnSegment(P2 p, P2 a, P2 b)
    {
        const P2 d = b - a;
        const double len2 = d.Dot(d);
        return len2 > 0.0 ? std::clamp((p - a).Dot(d) / len2, 0.0, 1.0) : 0.0;
    }

    double DistToSegment(P2 p, P2 a, P2 b)
    {
        return p.Dist(a + (b - a) * ParamOnSegment(p, a, b));
    }

    std::optional<P2> LineIntersection(P2 a, P2 b, P2 c, P2 d)
    {
        const P2 r = b - a, s = d - c;
        const double den = r.Cross(s);
        if (std::abs(den) <= 1e-12 * r.Length() * s.Length())
            return std::nullopt;
        return a + r * ((c - a).Cross(s) / den);
    }

    std::optional<double> Crossing(P2 a, P2 b, P2 c, P2 d)
    {
        const P2 r = b - a, s = d - c;
        const double den = r.Cross(s);
        if (std::abs(den) <= 1e-12 * r.Length() * s.Length())
            return std::nullopt;   // 平行
        const double t = (c - a).Cross(s) / den;
        const double u = (c - a).Cross(r) / den;
        if (t > 0.0 && t < 1.0 && u > 0.0 && u < 1.0)
            return t;
        return std::nullopt;
    }

    namespace
    {
        // 线段在别的线段穿过或端点落在其上的地方切开
        std::vector<Segment> Split(const std::vector<Segment>& segs, double tol)
        {
            std::vector<Segment> out;
            for (size_t i = 0; i < segs.size(); ++i)
            {
                const auto [a, b] = segs[i];
                std::vector<double> ts{ 0.0, 1.0 };
                for (size_t j = 0; j < segs.size(); ++j)
                {
                    if (i == j)
                        continue;
                    const auto [c, d] = segs[j];
                    // 另一条线段的端点在这条上：T 形相接或共线重叠
                    for (const P2 q : { c, d })
                        if (DistToSegment(q, a, b) <= tol)
                            ts.push_back(ParamOnSegment(q, a, b));
                    if (const auto t = Crossing(a, b, c, d))
                        ts.push_back(*t);
                }
                std::sort(ts.begin(), ts.end());
                for (size_t k = 0; k + 1 < ts.size(); ++k)
                {
                    const P2 p = a + (b - a) * ts[k], q = a + (b - a) * ts[k + 1];
                    if (p.Dist(q) > tol)
                        out.emplace_back(p, q);
                }
            }
            return out;
        }
    }

    std::vector<Ring> Faces(const std::vector<Segment>& segs, double tol)
    {
        // O(n²) 的切分与端点匹配；平面图到上千面墙时换空间网格
        std::vector<P2> nodes;
        auto node = [&](P2 p) -> size_t
        {
            for (size_t i = 0; i < nodes.size(); ++i)
                if (nodes[i].Dist(p) <= tol)
                    return i;
            nodes.push_back(p);
            return nodes.size() - 1;
        };
        std::set<std::pair<size_t, size_t>> edges;
        for (const auto& [a, b] : Split(segs, tol))
        {
            const size_t i = node(a), j = node(b);
            if (i != j)
                edges.insert({ std::min(i, j), std::max(i, j) });
        }
        std::vector<std::vector<size_t>> around(nodes.size());
        for (const auto& [i, j] : edges)
        {
            around[i].push_back(j);
            around[j].push_back(i);
        }
        auto angle = [&](size_t from, size_t to)
        {
            const P2 d = nodes[to] - nodes[from];
            return std::atan2(d.y, d.x);
        };
        for (size_t i = 0; i < around.size(); ++i)
            std::stable_sort(around[i].begin(), around[i].end(), [&](size_t a, size_t b) { return angle(i, a) < angle(i, b); });
        // 每条半边走一次，面在左侧：到每个节点取紧挨着来路顺时针一侧的边
        std::set<std::pair<size_t, size_t>> seen;
        std::vector<Ring> out;
        for (const auto& [i, j] : edges)
        {
            for (const auto& start : { std::pair{ i, j }, std::pair{ j, i } })
            {
                auto [u, v] = start;
                Ring face;
                while (seen.insert({ u, v }).second)
                {
                    face.push_back(nodes[u]);
                    const auto& list = around[v];
                    const auto it = std::find(list.begin(), list.end(), u);
                    const size_t k = it == list.end() ? 0 : static_cast<size_t>(it - list.begin());
                    const size_t w = list[(k + list.size() - 1) % list.size()];
                    u = v;
                    v = w;
                }
                if (!face.empty())
                    out.push_back(std::move(face));
            }
        }
        return out;
    }

    double Region::Area() const
    {
        double a = Geom::Area(outer);
        for (const Ring& h : holes)
            a -= std::abs(Geom::Area(h));
        return a;
    }

    double Region::Perimeter() const
    {
        double p = Geom::Perimeter(outer);
        for (const Ring& h : holes)
            p += Geom::Perimeter(h);
        return p;
    }

    std::optional<Region> RegionAt(const std::vector<Segment>& segs, P2 p, double tol)
    {
        return RegionIn(Faces(segs, tol), p, tol);
    }

    std::optional<Region> RegionIn(const std::vector<Ring>& all, P2 p, double tol)
    {
        const Ring* outer = nullptr;
        for (const Ring& f : all)
            if (Area(f) > tol * tol && Contains(f, p) && (!outer || Area(f) < Area(*outer)))
                outer = &f;
        if (!outer)
            return std::nullopt;
        // 立在它里面的其他块的外轮廓；包着 p 的那块是我们自己
        std::vector<const Ring*> islands;
        for (const Ring& f : all)
            if (Area(f) < 0.0 && !Contains(f, p) && std::all_of(f.begin(), f.end(), [&](const P2& q) { return Contains(*outer, q); }))
                islands.push_back(&f);
        Region r{ *outer, {} };
        for (const Ring* h : islands)
        {
            const bool nested = std::any_of(islands.begin(), islands.end(), [&](const Ring* o)
            {
                return o != h && std::all_of(h->begin(), h->end(), [&](const P2& q) { return Contains(*o, q); });
            });
            if (!nested)
                r.holes.emplace_back(h->rbegin(), h->rend());
        }
        return r;
    }
}
