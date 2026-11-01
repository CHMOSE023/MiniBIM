// 移植自 BIMCraft crates/core/src/skeleton.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Skeleton.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <queue>

namespace MiniBIM::Bim::Skeleton
{
    namespace
    {
        // 沿边两点多近算同一点：毫米
        constexpr double Eps   = 1e-6;
        constexpr size_t None  = std::numeric_limits<size_t>::max();
        constexpr double kTau  = 6.283185307179586;

        // Rust 的 f64::total_cmp：-0 在 +0 之前
        int TotalCmp(double a, double b)
        {
            int64_t x = 0, y = 0;
            std::memcpy(&x, &a, sizeof x);
            std::memcpy(&y, &b, sizeof y);
            x ^= static_cast<int64_t>(static_cast<uint64_t>(x >> 63) >> 1);
            y ^= static_cast<int64_t>(static_cast<uint64_t>(y >> 63) >> 1);
            return x < y ? -1 : (x > y ? 1 : 0);
        }

        // Rust 的 rem_euclid
        double RemEuclid(double a, double b)
        {
            const double r = std::fmod(a, b);
            return r < 0.0 ? r + std::abs(b) : r;
        }

        struct Edge
        {
            P2     a;
            P2     d;
            // 向内：多边形逆时针，里面在边的左边
            P2     n;
            double speed = 0.0;
        };

        struct Corner
        {
            P2     p;
            double t = 0.0;
            P2     v;
            // 绕行时它前面的边与后面的边
            size_t left  = 0;
            size_t right = 0;
            size_t prev  = None;
            size_t next  = None;
            size_t node  = 0;
            bool   alive = true;
            // 在平行的两条边之间：没有点能随两条边移动时都留在两条边上，所以其中一条要走
            bool   stuck = false;

            P2 At(double when) const { return p + v * (when - t); }
        };

        enum class Kind
        {
            // 两个角点之间的边缩没了
            Shrink,
            // 一个凹角点撞上一条边
            Split,
        };

        struct Event
        {
            double t = 0.0;
            Kind   kind = Kind::Shrink;
            size_t a = 0;
            size_t b = 0;
        };

        // 堆顶最早；同一时刻边缩没在分裂之前（BIMCraft 的 Ord）
        struct Later
        {
            bool operator()(const Event& x, const Event& y) const
            {
                // x 排在 y 之后：x 更晚，或同一时刻 x 是分裂而 y 是边缩没
                const int c = TotalCmp(y.t, x.t);
                if (c != 0)
                    return c < 0;
                return static_cast<int>(y.kind) < static_cast<int>(x.kind);
            }
        };

        struct Arc
        {
            size_t from;
            size_t to;
            size_t left;
            size_t right;
        };

        struct Wavefront
        {
            std::vector<Edge>   edges;
            std::vector<Corner> corners;
            // 角点出发与相遇的地方：点与时间
            std::vector<std::pair<P2, double>> nodes;
            // 角点留下的线：从节点到节点，在两条边的面之间
            std::vector<Arc>    arcs;
            std::priority_queue<Event, std::vector<Event>, Later> queue;
            // 多近算一点：外轮廓大小的千万分之一
            double eps = 0.0;
            // 在一点相遇的边配不成对：波前缠在一起了
            bool   broken = false;

            static bool Has(const std::vector<size_t>& v, size_t x)
            {
                return std::find(v.begin(), v.end(), x) != v.end();
            }

            // 边 left 与 right 之间的角点，t 时在 p，从 node 出发
            size_t MakeCorner(P2 p, double t, size_t left, size_t right, size_t node)
            {
                const Edge& l = edges[left];
                const Edge& r = edges[right];
                const double det = l.n.Cross(r.n);
                // 它留在两条边上：对每条边 v · n = speed
                P2 v;
                bool stuck = false;
                if (std::abs(det) > 1e-9)
                    v = P2((l.speed * r.n.y - l.n.y * r.speed) / det, (l.n.x * r.speed - l.speed * r.n.x) / det);
                else if (l.n.Dot(r.n) > 0.0 && std::abs(l.speed - r.speed) < 1e-12)
                    v = l.n * l.speed;
                else
                    stuck = true;
                Corner c;
                c.p = p;
                c.t = t;
                c.v = v;
                c.left = left;
                c.right = right;
                c.node = node;
                c.stuck = stuck;
                corners.push_back(c);
                return corners.size() - 1;
            }

            // 角点 a 与 b 之间的边从 now 起什么时候缩没（如果会）：它们已经在一起并保持时就是现在
            void Shrink(size_t a, size_t b, double now)
            {
                const Corner& ca = corners[a];
                const Corner& cb = corners[b];
                const P2 d = edges[ca.right].d;
                // 两者沿边的距离：c + t · k
                const double c = (cb.p - cb.v * cb.t - (ca.p - ca.v * ca.t)).Dot(d);
                const double k = (cb.v - ca.v).Dot(d);
                const bool together = std::abs(c + k * now) <= eps && k < 1e-9;
                const double t = together ? now : std::fmax(-c / k, now);
                if ((together || k < -1e-12) && std::isfinite(t))
                    queue.push(Event{ t, Kind::Shrink, a, b });
            }

            // 凹角点 c 从 now 起什么时候撞上除了自己两条边以外的每条边
            void Splits(size_t c, double now)
            {
                const Corner& corner = corners[c];
                const P2 dl = edges[corner.left].d;
                const P2 dr = edges[corner.right].d;
                if (corner.stuck || dl.Cross(dr) >= -1e-12)
                    return;
                const P2 p0 = corner.p - corner.v * corner.t;
                for (size_t f = 0; f < edges.size(); ++f)
                {
                    if (f == corner.left || f == corner.right)
                        continue;
                    const Edge& e = edges[f];
                    const double k = corner.v.Dot(e.n) - e.speed;
                    const double t = -(p0 - e.a).Dot(e.n) / k;
                    if (k < -1e-12 && std::isfinite(t) && t >= now - Eps)
                        queue.push(Event{ std::fmax(t, now), Kind::Split, c, f });
                }
            }

            // 把角点 c 连在 prev 与 next 之间
            void Link(size_t prev, size_t c, size_t next)
            {
                corners[prev].next = c;
                corners[c].prev = prev;
                corners[c].next = next;
                corners[next].prev = c;
            }

            // 刚做出的角点：波前只剩两个角点时连成屋脊，否则排上它的事件。在平行的两条边之间、此刻在同一直线上的角点不能动：
            // 同向而行的两条边中慢的一条、迎面相遇的两条中短的一条立刻走掉，留下一条水平线
            void Settle(size_t c, double now)
            {
                const size_t prev = corners[c].prev;
                const size_t next = corners[c].next;
                if (next == c)
                {
                    corners[c].alive = false;
                }
                else if (next == prev)
                {
                    const Corner& a = corners[c];
                    const Corner& b = corners[next];
                    arcs.push_back(Arc{ a.node, b.node, a.left, a.right });
                    corners[c].alive = false;
                    corners[next].alive = false;
                }
                else if (corners[c].stuck)
                {
                    const Corner& k = corners[c];
                    const Edge& l = edges[k.left];
                    const Edge& r = edges[k.right];
                    const P2 back = corners[prev].At(now);
                    const P2 on = corners[next].At(now);
                    const bool behind = l.n.Dot(r.n) > 0.0 ? l.speed < r.speed : k.p.Dist(back) <= k.p.Dist(on);
                    if (behind)
                        Merge(prev, c, back, now);
                    else
                        Merge(c, next, on, now);
                }
                else
                {
                    Shrink(prev, c, now);
                    Shrink(c, next, now);
                    Splits(c, now);
                }
            }

            // 相邻的角点 a 与 b 在 t 时于 p 相遇：它们之间的边没了，两侧的边之间的一个角点从那里继续
            void Merge(size_t a, size_t b, P2 p, double t)
            {
                const size_t prev = corners[a].prev;
                const size_t next = corners[b].next;
                // 只剩三个角点：相遇于一点
                if (prev == next)
                {
                    const size_t peak = Node(p, t);
                    for (const size_t c : { a, b, next })
                        LeaveArc(c, peak);
                    return;
                }
                const size_t n = Node(p, t);
                const size_t left = corners[a].left;
                const size_t right = corners[b].right;
                LeaveArc(a, n);
                LeaveArc(b, n);
                const size_t c = MakeCorner(p, t, left, right, n);
                Link(prev, c, next);
                Settle(c, t);
            }

            size_t Node(P2 p, double t)
            {
                nodes.emplace_back(p, t);
                return nodes.size() - 1;
            }

            // 角点 c 到达 node 时留下的线
            void LeaveArc(size_t c, size_t node)
            {
                const Corner& k = corners[c];
                arcs.push_back(Arc{ k.node, node, k.left, k.right });
                corners[c].alive = false;
            }

            void Step(const Event& e)
            {
                if (e.kind == Kind::Shrink)
                {
                    const Corner& ca = corners[e.a];
                    const Corner& cb = corners[e.b];
                    if (ca.alive && cb.alive && ca.next == e.b)
                    {
                        const P2 p = ca.At(e.t);
                        Meet(p, e.t, { e.a, e.b });
                    }
                    return;
                }
                if (!corners[e.a].alive)
                    return;
                const P2 p = corners[e.a].At(e.t);
                // 只在它撞上边 f 还在的那一段时
                bool hit = false;
                for (const Corner& x : corners)
                    if (x.alive && x.right == e.b && Geom::DistToSegment(p, x.At(e.t), corners[x.next].At(e.t)) < eps)
                    {
                        hit = true;
                        break;
                    }
                if (hit)
                    Meet(p, e.t, { e.a });
            }

            struct Ray
            {
                double angle;
                size_t edge;
                size_t corner;
                bool   incoming;
            };

            // 角点 must 与在那里的其他角点在 t 时于 p 相遇，也许还有它们撞上的某条边的中段。进来与出去的边配成新的角点：
            // 每条出去的边与逆时针方向紧挨着它的进来的边配对，波前在两者之间；沿同一直线的两条先把它们之间剩下的封上
            void Meet(P2 p, double t, const std::vector<size_t>& must)
            {
                std::vector<size_t> here;
                for (size_t i = 0; i < corners.size(); ++i)
                    if (corners[i].alive && (Has(must, i) || corners[i].At(t).Dist(p) < eps))
                        here.push_back(i);
                // 各条：它从 p 出发的方向、边、它另一端的角点、是否进来
                std::vector<Ray> rays;
                const auto angle = [](P2 d) { return std::atan2(d.y, d.x); };
                for (const size_t i : here)
                {
                    const Corner& c = corners[i];
                    if (!Has(here, c.prev))
                        rays.push_back(Ray{ angle(edges[c.left].d * -1.0), c.left, c.prev, true });
                    if (!Has(here, c.next))
                        rays.push_back(Ray{ angle(edges[c.right].d), c.right, c.next, false });
                }
                for (size_t x = 0; x < corners.size(); ++x)
                {
                    const Corner& c = corners[x];
                    if (c.alive && !Has(here, x) && !Has(here, c.next)
                        && Geom::DistToSegment(p, c.At(t), corners[c.next].At(t)) < eps)
                    {
                        const P2 d = edges[c.right].d;
                        rays.push_back(Ray{ angle(d * -1.0), c.right, x, true });
                        rays.push_back(Ray{ angle(d), c.right, c.next, false });
                    }
                }
                const size_t node = Node(p, t);
                for (const size_t i : here)
                    LeaveArc(i, node);
                std::vector<Ray> outs, ins;
                for (const Ray& r : rays)
                    (r.incoming ? ins : outs).push_back(r);
                struct Pair
                {
                    double turn;
                    size_t i;
                    size_t j;
                };
                std::vector<Pair> pairs;
                for (size_t i = 0; i < outs.size(); ++i)
                    for (size_t j = 0; j < ins.size(); ++j)
                    {
                        const double turn = RemEuclid(ins[j].angle - outs[i].angle, kTau);
                        pairs.push_back(Pair{ turn > kTau - 1e-9 ? 0.0 : turn, i, j });
                    }
                std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return TotalCmp(x.turn, y.turn) < 0; });
                std::vector<bool> usedOut(outs.size(), false), usedIn(ins.size(), false);
                std::vector<size_t> made;
                for (const Pair& pr : pairs)
                {
                    if (usedOut[pr.i] || usedIn[pr.j])
                        continue;
                    usedOut[pr.i] = true;
                    usedIn[pr.j] = true;
                    const size_t c = MakeCorner(p, t, ins[pr.j].edge, outs[pr.i].edge, node);
                    Link(ins[pr.j].corner, c, outs[pr.i].corner);
                    made.push_back(c);
                }
                if (outs.size() != ins.size())
                {
                    broken = true;
                    return;
                }
                for (const size_t c : made)
                    if (corners[c].alive)
                        Settle(c, t);
            }
        };

        double AreaOf(const Face& face)
        {
            Ring ring;
            ring.reserve(face.size());
            for (const auto& [q, when] : face)
                ring.push_back(q);
            return Geom::Area(ring);
        }

        // ring 在回到经过过的点处切开：剩下的，然后是中间绕过的每一圈，即在那里碰着它的洞
        //（绕着天井合拢的坡面在天井一个不动的角点处碰到自己）
        std::vector<Face> Pinched(Face ring)
        {
            const auto same = [](const std::pair<P2, double>& a, const std::pair<P2, double>& b) { return a.first.Dist(b.first) < Eps; };
            std::vector<Face> loops;
            for (;;)
            {
                // 回到起点时，后面的是绕回来的一圈；在别处时，是两次经过之间的那段
                size_t back = 0;
                for (size_t j = 2; j < ring.size(); ++j)
                    if (same(ring[j], ring[0]))
                    {
                        back = j;
                        break;
                    }
                if (back != 0)
                {
                    loops.emplace_back(ring.begin() + static_cast<std::ptrdiff_t>(back), ring.end());
                    ring.resize(back);
                    continue;
                }
                size_t from = 0, to = 0;
                for (size_t j = 2; j < ring.size() && to == 0; ++j)
                    for (size_t i = 1; i < j; ++i)
                        if (same(ring[i], ring[j]))
                        {
                            from = i;
                            to = j;
                            break;
                        }
                if (to == 0)
                    break;
                loops.emplace_back(ring.begin() + static_cast<std::ptrdiff_t>(from), ring.begin() + static_cast<std::ptrdiff_t>(to));
                ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(from), ring.begin() + static_cast<std::ptrdiff_t>(to));
            }
            loops.insert(loops.begin(), std::move(ring));
            return loops;
        }
    }

    bool Faces(const std::vector<Ring>& rings, const std::vector<double>& speeds, std::vector<std::vector<Face>>& out,
               std::string& error)
    {
        static const Ring empty;
        const Ring& outline = rings.empty() ? empty : rings.front();
        // 每个点，以及它所在环上的下一个
        std::vector<P2> points;
        std::vector<size_t> next;
        for (const Ring& r : rings)
        {
            for (size_t i = 0; i < r.size(); ++i)
                next.push_back(points.size() + (i + 1) % r.size());
            points.insert(points.end(), r.begin(), r.end());
        }
        const size_t n = points.size();
        if (outline.size() < 3 || speeds.size() != n || Geom::Area(outline) <= 0.0)
        {
            error = "it needs a counter-clockwise outline of three points or more";
            return false;
        }
        for (size_t k = 1; k < rings.size(); ++k)
            if (rings[k].size() < 3 || Geom::Area(rings[k]) >= 0.0)
            {
                error = "a hole in it goes clockwise round three points or more";
                return false;
            }
        if (std::all_of(speeds.begin(), speeds.end(), [](double s) { return s <= 0.0; }))
        {
            error = "none of its edges slopes";
            return false;
        }
        std::vector<size_t> prev(n, 0);
        for (size_t i = 0; i < n; ++i)
            prev[next[i]] = i;
        Wavefront w;
        for (size_t i = 0; i < n; ++i)
        {
            const P2 a = points[i], b = points[next[i]];
            const P2 d = (b - a).Unit();
            w.edges.push_back(Edge{ a, d, d.Perp(), speeds[i] });
        }
        for (const Edge& e : w.edges)
            if (!e.d.IsFinite() || e.d.Length() < 0.5)
            {
                error = "two of its points are the same";
                return false;
            }
        double size = 0.0;
        for (const P2& q : outline)
            size = std::max(size, q.Dist(outline[0]));
        w.eps = size * 1e-7;
        for (const P2& q : points)
            w.nodes.emplace_back(q, 0.0);
        for (size_t i = 0; i < n; ++i)
            w.MakeCorner(points[i], 0.0, prev[i], i, i);
        for (size_t i = 0; i < n; ++i)
            w.Link(prev[i], i, next[i]);
        for (size_t i = 0; i < n; ++i)
            if (i < w.corners.size() && w.corners[i].alive)
                w.Settle(i, 0.0);
        size_t budget = 100 * n * n + 1000;
        while (!w.queue.empty())
        {
            const Event e = w.queue.top();
            w.queue.pop();
            if (budget == 0)
            {
                error = "its skeleton doesn't settle";
                return false;
            }
            --budget;
            w.Step(e);
        }
        bool anyLeft = false, allUpright = true;
        for (const Corner& c : w.corners)
            if (c.alive)
            {
                anyLeft = true;
                if (!(w.edges[c.left].speed <= 0.0 && w.edges[c.right].speed <= 0.0))
                    allUpright = false;
            }
        if (!w.broken && anyLeft && allUpright)
        {
            error = "its upright edges fence in part of it that no slope reaches; let one of them slope";
            return false;
        }
        if (w.broken || anyLeft)
        {
            error = "MiniBIM can't find where its slopes meet";
            return false;
        }
        // 每条边的面：沿边走，再从边的终点经它挨着的线回到起点；然后剩下的各圈，即面里的洞（绕着天井合拢的坡面与从天井升起的坡面）。
        // 面在两侧的线（同一条边的两段又相遇处）不算
        std::vector<std::vector<Face>> result;
        result.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            if (speeds[i] <= 0.0)
            {
                result.emplace_back();
                continue;
            }
            std::vector<std::pair<size_t, size_t>> arcs;
            for (const Arc& a : w.arcs)
                if ((a.left == i) != (a.right == i))
                    arcs.emplace_back(a.from, a.to);
            // 从 path 的最后一个节点经各条线绕回它的第一个
            const auto walk = [&](std::vector<size_t> path, Face& face) -> bool {
                const size_t start = path.front();
                size_t at = path.back();
                while (at != start)
                {
                    size_t k = arcs.size();
                    for (size_t x = 0; x < arcs.size(); ++x)
                        if (arcs[x].first == at || arcs[x].second == at)
                        {
                            k = x;
                            break;
                        }
                    if (k == arcs.size())
                    {
                        error = "its slopes don't close";
                        return false;
                    }
                    const auto [a, b] = arcs[k];
                    // Rust 的 swap_remove
                    arcs[k] = arcs.back();
                    arcs.pop_back();
                    at = a == at ? b : a;
                    if (at != start)
                        path.push_back(at);
                }
                face.clear();
                for (const size_t k : path)
                    if (k < w.nodes.size())
                    {
                        if (!face.empty() && face.back().first.Dist(w.nodes[k].first) < Eps)
                            continue;
                        face.push_back(w.nodes[k]);
                    }
                if (face.size() > 2 && face.front().first.Dist(face.back().first) < Eps)
                    face.pop_back();
                return true;
            };
            Face first;
            if (!walk({ i, next[i] }, first))
                return false;
            std::vector<Face> loops = Pinched(std::move(first));
            while (!arcs.empty())
            {
                const auto [a, b] = arcs.back();
                arcs.pop_back();
                Face more;
                if (!walk({ a, b }, more))
                    return false;
                for (Face& f : Pinched(std::move(more)))
                    loops.push_back(std::move(f));
            }
            // 外圈，然后是其中的洞，顺时针
            std::vector<Face> faces;
            faces.push_back(std::move(loops.front()));
            for (size_t k = 1; k < loops.size(); ++k)
            {
                Face& h = loops[k];
                if (h.size() < 3)
                    continue;
                if (AreaOf(h) > 0.0)
                    std::reverse(h.begin(), h.end());
                faces.push_back(std::move(h));
            }
            result.push_back(std::move(faces));
        }
        out = std::move(result);
        return true;
    }
}
