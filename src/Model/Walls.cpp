// 移植自 BIMCraft crates/core/src/walls.rs、walls/index.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Walls.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace MiniBIM::Bim::Walls
{
    namespace
    {
        constexpr double kPi  = 3.14159265358979323846;
        constexpr double kTau = 2.0 * kPi;

        double Signum(double x) { return std::signbit(x) ? -1.0 : 1.0; }
        double Radians(double deg) { return deg * kPi / 180.0; }
        double Degrees(double rad) { return rad * 180.0 / kPi; }
        double RemEuclid(double a, double b)
        {
            const double r = std::fmod(a, b);
            return r < 0.0 ? r + std::abs(b) : r;
        }

        // 平面上没有切开的部分之间，去掉连续重复的点
        void Dedup(Ring& ring, double tol)
        {
            ring.erase(std::unique(ring.begin(), ring.end(), [tol](const P2& a, const P2& b) { return a.Dist(b) < tol; }), ring.end());
        }

        // 墙的外侧面到定位线 loc 有多远。核心层从第一个结构层到最后一个；没有结构层时为全部
        double Depth(double thickness, const std::vector<Layer>& layers, Location loc)
        {
            std::optional<std::pair<double, double>> core;
            double at = 0.0;
            for (const Layer& l : layers)
            {
                if (l.function == Function::Structure)
                    core = std::pair{ core ? core->first : at, at + l.thickness };
                at += l.thickness;
            }
            const auto [c0, c1] = core.value_or(std::pair{ 0.0, thickness });
            switch (loc)
            {
            case Location::Centre:         return thickness / 2.0;
            case Location::CoreCentre:     return (c0 + c1) / 2.0;
            case Location::ExteriorFinish: return 0.0;
            case Location::InteriorFinish: return thickness;
            case Location::ExteriorCore:   return c0;
            case Location::InteriorCore:   return c1;
            }
            return thickness / 2.0;
        }

        // 离开节点的一面墙：走向，以及两个墙面和中心线在那里的位置（都沿这个走向）：走向逆时针一侧的面、顺时针一侧的面
        struct Arm
        {
            bool   mine = false;
            P2     way;
            P2     ccw;
            P2     cw;
            P2     centre;
            double thick = 0.0;
        };

        // 墙 w 从起点（atEnd 时从终点）离开节点
        Arm MakeArm(const Model& m, const Wall& w, bool atEnd, bool mine)
        {
            const Axis axis = Axis::Of(w);
            const auto [ext, inner] = Faces(m, w);
            const double s = atEnd ? axis.Length() : 0.0;
            const P2 t = axis.Tangent(s);
            // 从终点离开时往回走：内侧面在逆时针一侧
            const P2 way = atEnd ? t * -1.0 : t;
            const double ccw = atEnd ? inner : ext;
            const double cw  = atEnd ? ext : inner;
            return Arm{ mine, way, axis.At(s, ccw), axis.At(s, cw), axis.At(s, (ext + inner) / 2.0), ext - inner };
        }

        // 墙 id 起点（atEnd 时为终点）处的各面墙，绕节点逆时针排好，包括它自己：端头在这里的，以及它落在其上的墙
        //（从落点往两边各算一面）。第二个返回值：它是否落在一面墙上
        std::pair<std::vector<Arm>, bool> Arms(const Model& m, Id id, bool atEnd, const Index* index)
        {
            const Wall* w = m.GetWall(id);
            if (!w)
                return { {}, false };
            const P2 node = atEnd ? w->end : w->start;
            std::vector<Arm> arms{ MakeArm(m, *w, atEnd, true) };
            bool through = false;
            std::vector<std::pair<Id, const Wall*>> candidates;
            if (index)
            {
                for (const Id other : index->Neighbours(id))
                    if (const Wall* o = m.GetWall(other))
                        candidates.emplace_back(other, o);
            }
            else
            {
                for (const auto& [other, e] : m.elements.Items())
                    if (const Wall* o = e->As<Wall>())
                        candidates.emplace_back(other, o);
            }
            // 先问在哪里相接：高度是否重叠要看两面墙的高度，以及它们贴着的屋顶或楼板
            for (const auto& [other, o] : candidates)
            {
                if (other == id)
                    continue;
                if (o->start.Dist(node) <= Tol)
                {
                    if (Stacked(m, *w, *o))
                        arms.push_back(MakeArm(m, *o, false, false));
                }
                else if (o->end.Dist(node) <= Tol)
                {
                    if (Stacked(m, *w, *o))
                        arms.push_back(MakeArm(m, *o, true, false));
                }
                else
                {
                    // 它落在其上的墙：节点在那面墙的厚度以内、两端之间
                    const Axis ax = Axis::Of(*o);
                    const auto [s, across] = ax.Project(node);
                    const auto [ext, inner] = Faces(m, *o);
                    if (s > Tol && s < ax.Length() - Tol && across >= inner - Tol && across <= ext + Tol &&
                        ax.Distance(node, across) <= Tol && Stacked(m, *w, *o))
                    {
                        const P2 t = ax.Tangent(s);
                        const P2 e = ax.At(s, ext), i = ax.At(s, inner), c = ax.At(s, (ext + inner) / 2.0);
                        arms.push_back(Arm{ false, t, e, i, c, ext - inner });
                        arms.push_back(Arm{ false, t * -1.0, i, e, c, ext - inner });
                        through = true;
                    }
                }
            }
            std::stable_sort(arms.begin(), arms.end(), [](const Arm& a, const Arm& b)
            {
                return std::atan2(a.way.y, a.way.x) < std::atan2(b.way.y, b.way.x);
            });
            return { std::move(arms), through };
        }

        // 墙线 off 与它起点（或终点）封口的交点：沿那里的切线，落到封口的角点之间
        P2 OnCap(const Axis& axis, const std::vector<P2>& cap, bool atEnd, double off)
        {
            const double s = atEnd ? axis.Length() : 0.0;
            const P2 p = axis.At(s, off), t = axis.Tangent(s);
            std::optional<P2> best;
            for (size_t i = 0; i + 1 < cap.size(); ++i)
            {
                const P2 a = cap[i], b = cap[i + 1];
                const auto q = Geom::LineIntersection(p, p + t, a, b);
                if (q && Geom::DistToSegment(*q, a, b) <= 1e-6 && (!best || q->Dist(p) < best->Dist(p)))
                    best = q;
            }
            return best.value_or(p);
        }

        // 墙底立在、墙顶贴着的东西：own 处的水平面，或在贴着的屋顶或楼板上方时那里的平面（凸块），在它外面仍是 own
        struct Bound
        {
            std::vector<std::pair<Ring, Plane>> regions;
            double own = 0.0;

            // 楼板 id 从下面看（top 时从上面看）；没有时为 own。屋顶的坡面在第 2b 步移植屋顶时加上
            static Bound Of(const Model& m, std::optional<Guid> id, bool top, double own)
            {
                Bound b;
                b.own = own;
                const Element* e = id ? m.elements.Get(*id) : nullptr;
                if (const Floor* f = e ? e->As<Floor>() : nullptr)
                {
                    const Plane face = m.FloorTop(*f);
                    const Plane plane{ face.z - (top ? 0.0 : m.FloorDepth(*f)), face.g };
                    for (Ring& c : ConvexPieces(f->boundary))
                        b.regions.emplace_back(std::move(c), plane);
                }
                return b;
            }

            // q 上方的平面：在 q 上方（或离 q 不到 1 毫米）的那部分的；有两个时取低的
            std::optional<Plane> Over(P2 q) const
            {
                std::optional<Plane> best;
                for (const auto& [r, p] : regions)
                {
                    bool near = Geom::Contains(r, q);
                    for (size_t i = 0; !near && i < r.size(); ++i)
                        near = Geom::DistToSegment(q, r[i], r[(i + 1) % r.size()]) <= Tol;
                    if (near && (!best || p.At(q) < best->At(q)))
                        best = p;
                }
                return best;
            }

            double At(P2 q) const
            {
                const auto p = Over(q);
                return p ? p->At(q) : own;
            }

            // ring 在换平面处切开，各部分带它的平面
            std::vector<std::pair<Ring, Plane>> Parts(Ring ring) const
            {
                std::vector<std::pair<Ring, Plane>> out;
                std::vector<Ring> rest{ std::move(ring) };
                for (const auto& [piece, plane] : regions)
                {
                    std::vector<Ring> outside;
                    for (Ring& r : rest)
                    {
                        auto [inside, outer] = Split(std::move(r), piece);
                        if (inside)
                            out.emplace_back(std::move(*inside), plane);
                        for (Ring& o : outer)
                            outside.push_back(std::move(o));
                    }
                    rest = std::move(outside);
                }
                for (Ring& r : rest)
                    out.emplace_back(std::move(r), Plane::Flat(own));
                return out;
            }
        };

        // 墙的底与顶：贴着的东西，或水平面
        std::pair<Bound, Bound> Bounds(const Model& m, const Wall& w)
        {
            return { Bound::Of(m, w.attachBase, true, m.OwnBase(w)), Bound::Of(m, w.attachTop, false, m.OwnTop(w)) };
        }

        constexpr double  kBand          = 6000.0;
        constexpr int64_t kMaxBands      = 128;
        constexpr size_t  kMaxReferences = 1000000;

        int64_t Band(double z)
        {
            // 照 Rust 的 as：NaN 为 0，越界取极值
            const double b = std::floor(z / kBand);
            if (std::isnan(b))
                return 0;
            if (b >= 9.2e18)
                return std::numeric_limits<int64_t>::max();
            if (b <= -9.2e18)
                return std::numeric_limits<int64_t>::min();
            return static_cast<int64_t>(b);
        }

        int64_t SaturatingSub(int64_t a, int64_t b)
        {
            if (b < 0 && a > std::numeric_limits<int64_t>::max() + b)
                return std::numeric_limits<int64_t>::max();
            if (b > 0 && a < std::numeric_limits<int64_t>::min() + b)
                return std::numeric_limits<int64_t>::min();
            return a - b;
        }
    }

    // ── Axis ─────────────────────────────────────────────────────
    Axis::Axis(P2 s, P2 e, double arc) : start(s), end(e)
    {
        const double sweep = Radians(arc), d = s.Dist(e);
        if (!(std::isfinite(sweep) && std::abs(sweep) > 1e-9 && d > 0.0))
            return;
        const double half = std::abs(sweep) / 2.0;
        // 圆心：在弦的中点以外，左转时在弦的左边，超过半圆时在右边
        const P2 c = (s + e) * 0.5 + (e - s).Unit().Perp() * (Signum(sweep) * d / (2.0 * std::tan(half)));
        m_arc = Arc{ c, d / (2.0 * std::sin(half)), std::atan2(s.y - c.y, s.x - c.x), sweep };
    }

    std::optional<P2> Axis::Centre() const
    {
        return m_arc ? std::optional<P2>(m_arc->centre) : std::nullopt;
    }

    std::optional<double> Axis::Radius() const
    {
        return m_arc ? std::optional<double>(m_arc->radius) : std::nullopt;
    }

    double Axis::Length() const
    {
        return m_arc ? m_arc->radius * std::abs(m_arc->sweep) : start.Dist(end);
    }

    P2 Axis::Tangent(double s) const
    {
        if (m_arc)
        {
            const double a = m_arc->a0 + Signum(m_arc->sweep) * s / m_arc->radius;
            return P2(-std::sin(a), std::cos(a)) * Signum(m_arc->sweep);
        }
        return (end - start).Unit();
    }

    P2 Axis::At(double s, double off) const
    {
        if (m_arc)
        {
            // 左转时它的左边朝向圆心
            const double a = m_arc->a0 + Signum(m_arc->sweep) * s / m_arc->radius;
            return m_arc->centre + P2(std::cos(a), std::sin(a)) * (m_arc->radius - Signum(m_arc->sweep) * off);
        }
        const P2 u = (end - start).Unit();
        return start + u * s + u.Perp() * off;
    }

    std::pair<double, double> Axis::Project(P2 p) const
    {
        if (m_arc)
        {
            // 从起点按它转的方向量，以中点为准，所以超过任一端的点落到较近的那端
            const double sg = Signum(m_arc->sweep), mid = std::abs(m_arc->sweep) / 2.0;
            const double t = RemEuclid(sg * (std::atan2(p.y - m_arc->centre.y, p.x - m_arc->centre.x) - m_arc->a0) - mid + kPi, kTau) - kPi + mid;
            return { std::clamp(t, 0.0, std::abs(m_arc->sweep)) * m_arc->radius, sg * (m_arc->radius - p.Dist(m_arc->centre)) };
        }
        const P2 u = (end - start).Unit();
        return { std::clamp((p - start).Dot(u), 0.0, Length()), (p - start).Dot(u.Perp()) };
    }

    double Axis::Distance(P2 p, double off) const
    {
        return p.Dist(At(Project(p).first, off));
    }

    std::vector<double> Axis::Stations(double s0, double s1) const
    {
        size_t n = 1;
        if (m_arc)
        {
            const double r = m_arc->radius;
            const double step = std::min(2.0 * std::acos(1.0 - 1.0 / std::max(r, 2.0)), Radians(5.0));
            const double count = std::ceil(std::abs(s1 - s0) / r / step);
            n = std::isnan(count) ? 1 : static_cast<size_t>(std::clamp(count, 1.0, 720.0));
        }
        std::vector<double> out;
        out.reserve(n + 1);
        for (size_t i = 0; i <= n; ++i)
            out.push_back(s0 + (s1 - s0) * static_cast<double>(i) / static_cast<double>(n));
        return out;
    }

    std::vector<P2> Axis::Points(double s0, double s1, double off) const
    {
        std::vector<P2> out;
        for (const double s : Stations(s0, s1))
            out.push_back(At(s, off));
        return out;
    }

    double SweepThrough(P2 a, P2 p, P2 b)
    {
        const P2 u = a - p, v = b - p;
        const double side = (b - a).Cross(p - a);
        if (std::abs(side) <= 1e-9 * (b - a).Length() * (p - a).Length())
            return 0.0;
        // 弦在 p 处所对角之外的两倍，按圆弧转的方向
        const double sweep = 2.0 * (kPi - std::abs(std::atan2(u.Cross(v), u.Dot(v))));
        return side < 0.0 ? Degrees(sweep) : -Degrees(sweep);
    }

    // ── 墙面 ─────────────────────────────────────────────────────
    std::vector<double> Boundaries(const Model& m, const Wall& w)
    {
        const auto it = m.types.wall.find(w.typeName);
        if (it == m.types.wall.end())
            return { 0.0, 0.0 };
        const WallType& t = it->second;
        const double ext = Depth(t.thickness, t.layers, w.location);
        std::vector<double> out{ ext };
        double at = ext;
        for (const Layer& l : t.layers)
        {
            at -= l.thickness;
            out.push_back(at);
        }
        if (t.layers.empty())
            out.push_back(ext - t.thickness);
        return out;
    }

    std::pair<double, double> Faces(const Model& m, const Wall& w)
    {
        const double t = m.Thickness(w);
        const auto it = m.types.wall.find(w.typeName);
        const double ext = it == m.types.wall.end() ? t / 2.0 : Depth(t, it->second.layers, w.location);
        return { ext, ext - t };
    }

    double Centre(const Model& m, const Wall& w)
    {
        const auto [ext, inner] = Faces(m, w);
        return (ext + inner) / 2.0;
    }

    bool Stacked(const Model& m, const Wall& a, const Wall& b)
    {
        return m.BaseZ(a) < m.TopZ(b) && m.BaseZ(b) < m.TopZ(a);
    }

    // ── Index ────────────────────────────────────────────────────
    Index::Index(const Model& m)
    {
        size_t references = 0;
        for (const auto& [id, e] : m.elements.Items())
        {
            if (const Wall* w = e->As<Wall>())
            {
                const double lo = m.BaseZ(*w), hi = m.TopZ(*w);
                m_spans[id] = { lo, hi };
                const int64_t a = Band(lo), b = Band(hi);
                const int64_t d = SaturatingSub(b, a);
                const int64_t count = d == std::numeric_limits<int64_t>::max() ? d : d + 1;
                if (count < 1 || count > kMaxBands || references + static_cast<size_t>(count) > kMaxReferences)
                    m_broad.push_back(id);
                else
                {
                    references += static_cast<size_t>(count);
                    for (int64_t k = a; k <= b; ++k)
                        m_bands[k].push_back(id);
                }
            }
            else if (const Opening* o = e->AsOpening())
                m_hosts[o->host].push_back(id);
        }
    }

    std::vector<Id> Index::Neighbours(Id id) const
    {
        const auto span = m_spans.find(id);
        if (span == m_spans.end())
            return {};
        // 普通变量（不用结构化绑定）：下面的 lambda 要捕获它们
        const double lo = span->second.first, hi = span->second.second;
        const int64_t a = Band(lo), b = Band(hi);
        std::vector<Id> ids;
        if (a > b || SaturatingSub(b, a) >= kMaxBands)
        {
            for (const auto& [k, _] : m_spans)
                ids.push_back(k);
            return ids;
        }
        ids = m_broad;
        for (auto it = m_bands.lower_bound(a); it != m_bands.end() && it->first <= b; ++it)
            ids.insert(ids.end(), it->second.begin(), it->second.end());
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        std::erase_if(ids, [&](Id other)
        {
            const auto s = m_spans.find(other);
            return s == m_spans.end() || !(lo < s->second.second && s->second.first < hi);
        });
        return ids;
    }

    const std::vector<Id>& Index::Openings(Guid host) const
    {
        static const std::vector<Id> none;
        const auto it = m_hosts.find(host);
        return it == m_hosts.end() ? none : it->second;
    }

    // ── 封口与轮廓 ───────────────────────────────────────────────
    std::vector<P2> Cap(const Model& m, Id id, bool atEnd, const Index* index)
    {
        const Wall* w = m.GetWall(id);
        if (!w)
            return {};
        const auto [arms, through] = Arms(m, id, atEnd, index);
        const size_t n = arms.size();
        const auto mineIt = std::find_if(arms.begin(), arms.end(), [](const Arm& a) { return a.mine; });
        if (mineIt == arms.end())
            return {};
        const size_t i = static_cast<size_t>(mineIt - arms.begin());
        const Arm& prev = arms[(i + n - 1) % n];
        const Arm& me   = arms[i];
        const Arm& next = arms[(i + 1) % n];
        if (n < 2)
            return { me.cw, me.ccw };
        const P2 node = atEnd ? w->end : w->start;
        double limit = 0.0;
        for (const Arm& a : arms)
            limit = std::max(limit, a.thick);
        // 这面墙的墙面与相邻墙相对的墙面的交点，离节点够近（两个方向都算）；否则就是它自己的端头。
        // 交点远在墙身里的不是墙角：以很小的角度相接的两面墙会在几米之外相交
        auto meet = [&](P2 face, P2 other, P2 otherWay)
        {
            const auto c = Geom::LineIntersection(face, face + me.way, other, other + otherWay);
            if (c)
            {
                const double along = (*c - node).Dot(me.way);
                if (along >= -limit && along <= limit)
                    return *c;
            }
            return face;
        };
        const P2 cw = meet(me.cw, prev.ccw, prev.way), ccw = meet(me.ccw, next.cw, next.way);
        if (through)
            return { cw, ccw };
        return { cw, node, ccw };
    }

    std::vector<P2> CentreLine(const Model& m, Id id)
    {
        const Wall* w = m.GetWall(id);
        if (!w)
            return {};
        const Axis axis = Axis::Of(*w);
        std::vector<P2> line = axis.Points(0.0, axis.Length(), Centre(m, *w));
        for (const bool atEnd : { false, true })
        {
            const auto [arms, _] = Arms(m, id, atEnd, nullptr);
            const auto me = std::find_if(arms.begin(), arms.end(), [](const Arm& a) { return a.mine; });
            if (me == arms.end())
                continue;
            double reach = 1.0;
            for (const Arm& a : arms)
                reach = std::max(reach, a.thick);
            reach *= 2.0;
            std::optional<P2> crossing;
            for (const Arm& a : arms)
            {
                if (a.mine)
                    continue;
                const auto c = Geom::LineIntersection(me->centre, me->centre + me->way, a.centre, a.centre + a.way);
                if (c && (!crossing || c->Dist(me->centre) < crossing->Dist(me->centre)))
                    crossing = c;
            }
            if (crossing && crossing->Dist(me->centre) <= reach && !line.empty())
                (atEnd ? line.back() : line.front()) = *crossing;
        }
        return line;
    }

    std::vector<P2> Outline(const Model& m, Id id, const Index* index)
    {
        const Wall* w = m.GetWall(id);
        if (!w)
            return {};
        const Axis axis = Axis::Of(*w);
        const auto [ext, inner] = Faces(m, *w);
        const double len = axis.Length();
        // 两端之间的点
        auto between = [](const std::vector<P2>& pts, Ring& into)
        {
            for (size_t i = 1; i + 1 < pts.size(); ++i)
                into.push_back(pts[i]);
        };
        Ring ring = Cap(m, id, false, index);
        between(axis.Points(0.0, len, ext), ring);
        const std::vector<P2> end = Cap(m, id, true, index);
        ring.insert(ring.end(), end.begin(), end.end());
        between(axis.Points(len, 0.0, inner), ring);
        Dedup(ring, 1e-6);
        return ring;
    }

    std::vector<std::vector<P2>> LayerLines(const Model& m, Id id, const Index* index)
    {
        const Wall* w = m.GetWall(id);
        if (!w)
            return {};
        const Axis axis = Axis::Of(*w);
        const double len = axis.Length();
        const std::vector<P2> start = Cap(m, id, false, index), end = Cap(m, id, true, index);
        std::vector<std::vector<P2>> out;
        for (const double off : Boundaries(m, *w))
        {
            std::vector<P2> line = axis.Points(0.0, len, off);
            if (!line.empty())
            {
                line.front() = OnCap(axis, start, false, off);
                line.back()  = OnCap(axis, end, true, off);
            }
            out.push_back(std::move(line));
        }
        return out;
    }

    // ── 切分 ─────────────────────────────────────────────────────
    std::vector<Ring> ConvexPieces(const Ring& ring)
    {
        if (ring.size() < 3)
            return {};
        Ring ccw = Geom::Area(ring) < 0.0 ? Ring(ring.rbegin(), ring.rend()) : ring;
        const size_t n = ccw.size();
        bool convex = true;
        for (size_t i = 0; i < n && convex; ++i)
            convex = (ccw[(i + 1) % n] - ccw[i]).Cross(ccw[(i + 2) % n] - ccw[(i + 1) % n]) >= -1e-6;
        if (convex)
            return { ccw };
        std::vector<Ring> out;
        for (const auto& t : Geom::Triangulate(ccw))
            out.push_back({ ccw[t[0]], ccw[t[1]], ccw[t[2]] });
        return out;
    }

    std::pair<std::optional<Ring>, std::vector<Ring>> Split(Ring ring, const Ring& piece)
    {
        auto big = [](const Ring& r) { return r.size() >= 3 && std::abs(Geom::Area(r)) > 1.0; };
        Ring inside = std::move(ring);
        std::vector<Ring> outside;
        for (size_t k = 0; k < piece.size(); ++k)
        {
            const P2 a = piece[k], b = piece[(k + 1) % piece.size()];
            // 重复的点不是切开的边
            if (a.Dist(b) <= 1e-6)
                continue;
            auto left = [a, b](P2 q) { return (b - a).Cross(q - a); };
            Ring out = Geom::Clip(inside, [&](P2 q) { return -left(q); });
            if (big(out))
                outside.push_back(std::move(out));
            inside = Geom::Clip(inside, left);
        }
        if (!big(inside))
            return { std::nullopt, std::move(outside) };
        return { std::move(inside), std::move(outside) };
    }

    std::vector<Ring> Minus(const std::vector<Ring>& rings, const Ring& hole)
    {
        // 环的包围盒：离洞的凸块的包围盒很远的块原样保留，不切（大的凹环和大的凹洞各有上百块，块块相切是立方级的）
        auto extent = [](const Ring& r)
        {
            P2 lo(INFINITY, INFINITY), hi(-INFINITY, -INFINITY);
            for (const P2& q : r)
            {
                lo = P2(std::min(lo.x, q.x), std::min(lo.y, q.y));
                hi = P2(std::max(hi.x, q.x), std::max(hi.y, q.y));
            }
            return std::pair{ lo, hi };
        };
        std::vector<std::pair<std::pair<P2, P2>, Ring>> holes;
        for (Ring& p : ConvexPieces(hole))
        {
            const auto e = extent(p);
            holes.emplace_back(e, std::move(p));
        }
        std::vector<Ring> out;
        for (const Ring& r : rings)
        {
            std::vector<Ring> parts = ConvexPieces(r);
            for (const auto& [box, piece] : holes)
            {
                const auto [lo, hi] = box;
                std::vector<Ring> next;
                for (Ring& p : parts)
                {
                    const auto [a, b] = extent(p);
                    if (b.x < lo.x || a.x > hi.x || b.y < lo.y || a.y > hi.y)
                    {
                        if (p.size() >= 3 && std::abs(Geom::Area(p)) > 1.0)
                            next.push_back(std::move(p));
                    }
                    else
                    {
                        for (Ring& o : Split(std::move(p), piece).second)
                            next.push_back(std::move(o));
                    }
                }
                parts = std::move(next);
            }
            for (Ring& p : parts)
                out.push_back(std::move(p));
        }
        return out;
    }

    // ── 高度 ─────────────────────────────────────────────────────
    std::optional<double> Underside(const Model& m, Id id, P2 q)
    {
        const auto p = Bound::Of(m, m.GuidOf(id), false, NAN).Over(q);
        if (!p)
            return std::nullopt;
        return p->At(q);
    }

    double TopAt(const Model& m, const Wall& w, double s)
    {
        return Bounds(m, w).second.At(Axis::Of(w).At(s, Centre(m, w)));
    }

    std::pair<double, double> Heights(const Model& m, const Wall& w)
    {
        if (!w.attachBase && !w.attachTop)
            return { m.OwnBase(w), m.OwnTop(w) };
        const auto [base, top] = Bounds(m, w);
        const Axis axis = Axis::Of(w);
        const auto [ext, inner] = Faces(m, w);
        const double len = axis.Length();
        double lo = INFINITY, hi = -INFINITY;
        for (const double off : { ext, (ext + inner) / 2.0, inner })
        {
            for (int i = 0; i <= 32; ++i)
            {
                const P2 q = axis.At(len * static_cast<double>(i) / 32.0, off);
                lo = std::min(lo, base.At(q));
                hi = std::max(hi, top.At(q));
            }
        }
        return { lo, hi };
    }

    std::vector<Prism> Prisms(const Model& m, Id id, bool openings, const Index* index)
    {
        const Wall* w = m.GetWall(id);
        if (!w)
            return {};
        const Guid guid = m.GuidOf(id);
        const Axis axis = Axis::Of(*w);
        const auto [ext, inner] = Faces(m, *w);
        const double len = axis.Length();
        const double z0 = m.OwnBase(*w);
        // 门窗洞：沿墙的范围与竖向的范围
        struct Hole
        {
            double s0, s1, z0, z1;
        };
        std::vector<Hole> holes;
        auto consider = [&](const Element& e)
        {
            const Opening* o = e.AsOpening();
            if (!o || o->host != guid)
                return;
            const auto t = m.OpeningTypeOf(e);
            if (!t)
                return;
            holes.push_back({ o->offset - t->width / 2.0, o->offset + t->width / 2.0, z0 + o->sill, z0 + o->sill + t->height });
        };
        if (openings)
        {
            if (index)
            {
                for (const Id h : index->Openings(guid))
                    if (const Element* e = m.elements.Get(h))
                        consider(*e);
            }
            else
            {
                for (const auto& [_, e] : m.elements.Items())
                    consider(*e);
            }
        }
        std::vector<double> cuts = axis.Stations(0.0, len);
        for (const Hole& h : holes)
            for (const double s : { h.s0, h.s1 })
                if (s > 0.0 && s < len)
                    cuts.push_back(s);
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), cuts.end());
        const std::vector<P2> start = Cap(m, id, false, index), end = Cap(m, id, true, index);
        const auto [base, top] = Bounds(m, *w);
        std::optional<std::pair<P2, double>> curve;
        if (const auto c = axis.Centre())
            curve = std::pair{ *c, axis.At(0.0, (ext + inner) / 2.0).Dist(*c) };
        std::vector<Prism> out;
        for (size_t k = 0; k + 1 < cuts.size(); ++k)
        {
            const double a = cuts[k], b = cuts[k + 1];
            Ring ring = a <= 0.0 ? start : Ring{ axis.At(a, inner), axis.At(a, ext) };
            if (b >= len)
                ring.insert(ring.end(), end.begin(), end.end());
            else
            {
                ring.push_back(axis.At(b, ext));
                ring.push_back(axis.At(b, inner));
            }
            // 从底部往上到顶部，越过中间上方的门窗洞
            const double mid = (a + b) / 2.0;
            std::vector<std::pair<double, double>> gaps;
            for (const Hole& h : holes)
                if (h.s0 <= mid && mid <= h.s1)
                    gaps.emplace_back(h.z0, h.z1);
            std::stable_sort(gaps.begin(), gaps.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            auto flat = [](double z) { Bound b; b.own = z; return b; };
            std::vector<Bound> floors{ base };
            std::vector<Bound> ceilings;
            for (const auto& [g0, g1] : gaps)
            {
                ceilings.push_back(flat(g0));
                floors.push_back(flat(g1));
            }
            ceilings.push_back(top);
            for (size_t f = 0; f < floors.size() && f < ceilings.size(); ++f)
            {
                for (auto& [part, bottom] : floors[f].Parts(ring))
                {
                    for (auto& [piece, topPlane] : ceilings[f].Parts(part))
                    {
                        // 只要有高度的地方
                        const Plane lower = bottom, upper = topPlane;
                        Ring kept = Geom::Clip(piece, [&](P2 q) { return upper.At(q) - lower.At(q); });
                        if (kept.size() >= 3 && std::abs(Geom::Area(kept)) > 1.0)
                            out.push_back(Prism{ std::move(kept), lower, upper, curve });
                    }
                }
            }
        }
        return out;
    }
}
