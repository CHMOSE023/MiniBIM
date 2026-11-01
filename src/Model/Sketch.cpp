// 移植自 BIMCraft crates/core/src/sketch.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Sketch.h"
#include "Model/Text.h"
#include "Model/Walls.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>

namespace MiniBIM::Bim::Sketch
{
    namespace
    {
        // 找哪些段相碰最多走多少步；嵌套环最多看多少个点
        constexpr size_t MaxWork = 6000000;
        constexpr size_t MaxNest = 15000000;
        // 盒子树的叶子最多几个盒子
        constexpr size_t Leaf = 8;

        using Line = std::array<P2, 2>;
        using Box  = std::array<double, 4>;

        std::string Num(double x) { return Text::Shortest(x); }

        // 在 Tol 以内的点算一个：Tol 见方的格子里找，只看自己和周围八格
        struct Welder
        {
            std::vector<P2> points;
            std::unordered_map<int64_t, std::vector<size_t>> cells;

            static int64_t Key(int64_t x, int64_t y) { return (x << 32) ^ (y & 0xffffffff); }

            size_t Weld(P2 p)
            {
                const int64_t cx = static_cast<int64_t>(std::floor(p.x / Tol)), cy = static_cast<int64_t>(std::floor(p.y / Tol));
                std::optional<size_t> found;
                for (int64_t x = cx - 1; x <= cx + 1; ++x)
                    for (int64_t y = cy - 1; y <= cy + 1; ++y)
                        if (const auto it = cells.find(Key(x, y)); it != cells.end())
                            for (const size_t n : it->second)
                                if ((!found || n < *found) && points[n].Dist(p) <= Tol)
                                    found = n;
                if (found)
                    return *found;
                points.push_back(p);
                cells[Key(cx, cy)].push_back(points.size() - 1);
                return points.size() - 1;
            }
        };

        bool Overlaps(const Box& a, const Box& b) { return a[0] <= b[2] && b[0] <= a[2] && a[1] <= b[3] && b[1] <= a[3]; }

        // 盒子 [x0, y0, x1, y1] 放进一棵树（沿长边一再对半分，叶子几个），找哪些相叠、哪些含一点时不必两两都试
        class Boxes
        {
        public:
            explicit Boxes(const std::vector<Box>& boxes)
            {
                m_order.resize(boxes.size());
                for (size_t i = 0; i < boxes.size(); ++i)
                    m_order[i] = i;
                if (!boxes.empty())
                    Build(boxes, 0, boxes.size());
                for (const size_t i : m_order)
                    m_packed.push_back(boxes[i]);
            }

            // 每对相叠的盒子调一次 f(i, j)，直到它说 false。看完返回 1，被 f 停下返回 0，超过 MaxWork 步返回 -1
            template <class F>
            int Pairs(size_t& work, F f) const
            {
                std::vector<std::pair<size_t, size_t>> stack;
                if (!m_nodes.empty())
                    stack.emplace_back(0, 0);
                while (!stack.empty())
                {
                    const auto [a, b] = stack.back();
                    stack.pop_back();
                    if (++work > MaxWork)
                        return -1;
                    const Node &na = m_nodes[a], &nb = m_nodes[b];
                    if (a != b && !Overlaps(na.bounds, nb.bounds))
                        continue;
                    if (!na.kids && !nb.kids)
                    {
                        for (size_t k = na.from; k < na.to; ++k)
                            for (size_t l = a == b ? k + 1 : nb.from; l < (a == b ? na.to : nb.to); ++l)
                            {
                                ++work;
                                if (Overlaps(m_packed[k], m_packed[l]) && !f(m_order[k], m_order[l]))
                                    return 0;
                            }
                    }
                    else if (na.kids && a == b)
                    {
                        const auto [l, r] = *na.kids;
                        stack.emplace_back(l, l);
                        stack.emplace_back(r, r);
                        stack.emplace_back(l, r);
                    }
                    else if (na.kids && !nb.kids)
                    {
                        stack.emplace_back((*na.kids)[0], b);
                        stack.emplace_back((*na.kids)[1], b);
                    }
                    else if (!na.kids && nb.kids)
                    {
                        stack.emplace_back(a, (*nb.kids)[0]);
                        stack.emplace_back(a, (*nb.kids)[1]);
                    }
                    else
                    {
                        // 大的那个对半分
                        auto area = [](const Box& x) { return (x[2] - x[0]) * (x[3] - x[1]); };
                        if (area(na.bounds) >= area(nb.bounds))
                        {
                            stack.emplace_back((*na.kids)[0], b);
                            stack.emplace_back((*na.kids)[1], b);
                        }
                        else
                        {
                            stack.emplace_back(a, (*nb.kids)[0]);
                            stack.emplace_back(a, (*nb.kids)[1]);
                        }
                    }
                }
                return 1;
            }

            // 每个含 p 的盒子调一次 f(i)，直到它说 false
            template <class F>
            void At(P2 p, F f) const
            {
                auto holds = [&](const Box& b) { return b[0] <= p.x && p.x <= b[2] && b[1] <= p.y && p.y <= b[3]; };
                std::vector<size_t> stack;
                if (!m_nodes.empty())
                    stack.push_back(0);
                while (!stack.empty())
                {
                    const Node& node = m_nodes[stack.back()];
                    stack.pop_back();
                    if (!holds(node.bounds))
                        continue;
                    if (node.kids)
                    {
                        stack.push_back((*node.kids)[0]);
                        stack.push_back((*node.kids)[1]);
                        continue;
                    }
                    for (size_t k = node.from; k < node.to; ++k)
                        if (holds(m_packed[k]) && !f(m_order[k]))
                            return;
                }
            }

        private:
            struct Node
            {
                Box    bounds;
                size_t from = 0, to = 0;
                std::optional<std::array<size_t, 2>> kids;
            };

            size_t Build(const std::vector<Box>& boxes, size_t from, size_t to)
            {
                Box b{ INFINITY, INFINITY, -INFINITY, -INFINITY };
                for (size_t k = from; k < to; ++k)
                {
                    const Box& x = boxes[m_order[k]];
                    b = { std::min(b[0], x[0]), std::min(b[1], x[1]), std::max(b[2], x[2]), std::max(b[3], x[3]) };
                }
                const size_t id = m_nodes.size();
                m_nodes.push_back(Node{ b, from, to, std::nullopt });
                if (to - from > Leaf)
                {
                    // 沿长边在中间那个处分开
                    const size_t axis = b[3] - b[1] > b[2] - b[0] ? 1 : 0;
                    auto centre = [&](size_t i) { return boxes[i][axis] + boxes[i][axis + 2]; };
                    const size_t mid = (to - from) / 2;
                    std::nth_element(m_order.begin() + static_cast<long>(from), m_order.begin() + static_cast<long>(from + mid),
                                     m_order.begin() + static_cast<long>(to), [&](size_t x, size_t y) { return centre(x) < centre(y); });
                    const size_t l = Build(boxes, from, from + mid);
                    const size_t r = Build(boxes, from + mid, to);
                    m_nodes[id].kids = std::array<size_t, 2>{ l, r };
                }
                return id;
            }

            std::vector<Box>    m_packed;
            std::vector<size_t> m_order;
            std::vector<Node>   m_nodes;
        };

        // 两段直线（属于线 pair，按给出的先后）碰在一起时出了什么错：叠在一起、相交、一段的端点在另一段中间。首尾相接是对的
        std::optional<Problem> Meet(Line s, Line t, std::array<size_t, 2> e, std::array<size_t, 2> f, std::array<size_t, 2> pair)
        {
            const auto [a, b] = s;
            const auto [c, d] = t;
            if ((e[0] == f[0] && e[1] == f[1]) || (e[0] == f[1] && e[1] == f[0]))
                return Problem{ Problem::Kind::Overlap, { pair[0], pair[1] } };
            // 在同一条直线上：共有的部分长过 Tol 时叠在一起
            auto off = [](P2 p, P2 u0, P2 u1) { return std::abs((u1 - u0).Unit().Cross(p - u0)); };
            if (off(c, a, b) <= Tol && off(d, a, b) <= Tol)
            {
                const P2 u = (b - a).Unit();
                const double p = (c - a).Dot(u), q = (d - a).Dot(u);
                const double shared = std::min(std::max(p, q), a.Dist(b)) - std::max(std::min(p, q), 0.0);
                if (shared > Tol)
                    return Problem{ Problem::Kind::Overlap, { pair[0], pair[1] } };
                return std::nullopt;
            }
            // 一个端点在另一段上、离它的两端都远
            auto on = [](P2 p, P2 u0, P2 u1) { return Geom::DistToSegment(p, u0, u1) <= Tol && p.Dist(u0) > Tol && p.Dist(u1) > Tol; };
            for (const P2 p : { c, d })
                if (on(p, a, b))
                    return Problem{ Problem::Kind::Cross, { pair[0], pair[1] }, p };
            for (const P2 p : { a, b })
                if (on(p, c, d))
                    return Problem{ Problem::Kind::Cross, { pair[0], pair[1] }, p };
            const auto k = Geom::Crossing(a, b, c, d);
            if (!k)
                return std::nullopt;
            const P2 x = a + (b - a) * *k;
            for (const P2 p : { a, b, c, d })
                if (p.Dist(x) <= Tol)
                    return std::nullopt;
            return Problem{ Problem::Kind::Cross, { pair[0], pair[1] }, x };
        }

        // ring 逆时针（ccw）或顺时针，起点不变
        Ring Wound(const Ring& ring, bool ccw)
        {
            if (ring.empty() || (Geom::Area(ring) > 0.0) == ccw)
                return ring;
            Ring out{ ring.front() };
            for (size_t i = ring.size() - 1; i >= 1; --i)
                out.push_back(ring[i]);
            return out;
        }

        void Cap(std::vector<Problem>& problems)
        {
            if (problems.size() > MaxProblems)
                problems.resize(MaxProblems);
        }

        void SortProblems(std::vector<Problem>& problems)
        {
            std::stable_sort(problems.begin(), problems.end(), [](const Problem& x, const Problem& y)
            {
                const size_t fx = x.lines.empty() ? 0 : x.lines.front(), fy = y.lines.empty() ? 0 : y.lines.front();
                return std::pair{ x.Rank(), fx } < std::pair{ y.Rank(), fy };
            });
        }

        bool InRange(const SketchLine& l)
        {
            auto ok = [](P2 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::abs(p.x) <= MaxCoord && std::abs(p.y) <= MaxCoord; };
            return ok(l.start) && ok(l.end) && std::isfinite(l.arc) && std::abs(l.arc) < 360.0;
        }
    }

    // =========================================================
    // 问题
    // =========================================================
    int Problem::Rank() const
    {
        switch (kind)
        {
        case Kind::Empty: case Kind::TooMany: case Kind::OutOfRange: case Kind::TooShort: return 0;
        case Kind::Cross:    return 1;
        case Kind::Overlap:  return 2;
        case Kind::Branch:   return 3;
        case Kind::Open:     return 4;
        case Kind::NoArea:   return 5;
        case Kind::Separate: return 6;
        }
        return 0;
    }

    std::string Problem::Said(const std::function<std::string(size_t)>& name) const
    {
        auto place = [](P2 p) { return "[" + Num(std::round(p.x)) + ", " + Num(std::round(p.y)) + "]"; };
        auto list = [&](std::vector<size_t> ls)
        {
            ls.erase(std::unique(ls.begin(), ls.end()), ls.end());
            std::string out;
            for (size_t i = 0; i < ls.size(); ++i)
                out += (i == 0 ? "" : i + 1 == ls.size() ? " and " : ", ") + name(ls[i]);
            return out;
        };
        const bool self = lines.size() == 2 && lines[0] == lines[1];
        switch (kind)
        {
        case Kind::Empty: return "there are no lines: draw the outline";
        case Kind::TooMany:
            if (what == "lines")
                return std::to_string(count) + " lines, more than " + std::to_string(MaxLines);
            if (what == "pieces")
                return "its lines are drawn as " + std::to_string(count) + " straight pieces (arcs are cut into chords), more than " +
                       std::to_string(MaxPieces) + ": draw fewer, or smaller";
            if (what == "steps")
                return "its pieces lie too close together to tell which cross (" + std::to_string(count) + " pairs of them looked at, more than " +
                       std::to_string(MaxWork) + "): draw fewer lines, or farther apart";
            return "the loops are nested too deeply to solve (" + std::to_string(count) + " steps, more than " + std::to_string(MaxNest) + "): draw fewer";
        case Kind::OutOfRange: return name(lines[0]) + " has a point that isn't a number within " + Num(MaxCoord) + " mm, or a curve reaching further";
        case Kind::TooShort:   return name(lines[0]) + " is " + Num(Tol) + " mm long or less";
        case Kind::Cross:
            return self ? name(lines[0]) + " crosses itself at " + place(*at) : list(lines) + " intersect at " + place(*at);
        case Kind::Overlap: return self ? name(lines[0]) + " lies along itself" : list(lines) + " overlap";
        case Kind::Open:    return name(lines[0]) + " ends at " + place(*at) + " without meeting another: the loop is open";
        case Kind::Branch:
        {
            std::vector<size_t> once = lines;
            once.erase(std::unique(once.begin(), once.end()), once.end());
            if (once.size() == 1)
                return name(once[0]) + " passes " + place(*at) + " more than once: a loop passes a point once";
            return list(once) + " meet at " + place(*at) + ": a loop passes a point once";
        }
        case Kind::NoArea:   return "the loop of " + list(lines) + " encloses no area";
        case Kind::Separate: return "the lines make " + std::to_string(count) + " separate outlines, where one is wanted";
        }
        return {};
    }

    std::string Say(const std::vector<Problem>& problems, const std::function<std::string(size_t)>& name)
    {
        std::string out;
        const size_t shown = std::min<size_t>(problems.size(), 5);
        for (size_t i = 0; i < shown; ++i)
            out += (i == 0 ? "" : "; ") + problems[i].Said(name);
        if (problems.size() > shown)
            out += "; and " + std::to_string(problems.size() - shown) + " more";
        return out;
    }

    std::function<std::string(size_t)> Namer(const std::vector<SketchLine>& lines)
    {
        return [lines](size_t i)
        {
            if (i < lines.size() && lines[i].id != 0)
                return "line " + std::to_string(lines[i].id);
            return "line " + std::to_string(i);
        };
    }

    // =========================================================
    // 一条线
    // =========================================================
    std::vector<P2> Points(const SketchLine& l)
    {
        if (l.arc == 0.0)
            return { l.start, l.end };
        const Walls::Axis axis(l.start, l.end, l.arc);
        return axis.Points(0.0, axis.Length(), 0.0);
    }

    double Length(const SketchLine& l) { return l.arc == 0.0 ? l.start.Dist(l.end) : Walls::Axis(l.start, l.end, l.arc).Length(); }

    SketchLine ArcThrough(P2 a, P2 b, P2 through)
    {
        SketchLine l;
        l.start = a;
        l.end   = b;
        if (Geom::DistToSegment(through, a, b) > Tol)
            l.arc = Walls::SweepThrough(a, through, b);
        return l;
    }

    double Distance(const SketchLine& l, P2 p)
    {
        const std::vector<P2> pts = Points(l);
        double best = INFINITY;
        for (size_t i = 0; i + 1 < pts.size(); ++i)
            best = std::min(best, Geom::DistToSegment(p, pts[i], pts[i + 1]));
        return best;
    }

    P2 Middle(const SketchLine& l)
    {
        if (l.arc == 0.0)
            return (l.start + l.end) * 0.5;
        const Walls::Axis axis(l.start, l.end, l.arc);
        return axis.At(axis.Length() / 2.0, 0.0);
    }

    double Profile::Area() const
    {
        double a = 0.0;
        for (const Geom::Region& r : regions)
            a += r.Area();
        return a;
    }

    // =========================================================
    // 解
    // =========================================================
    bool Solve(const std::vector<SketchLine>& lines, Profile& out, std::vector<Problem>& problems)
    {
        problems.clear();
        out.regions.clear();
        if (lines.empty())
        {
            problems.push_back(Problem{ Problem::Kind::Empty });
            return false;
        }
        if (lines.size() > MaxLines)
        {
            problems.push_back(Problem{ Problem::Kind::TooMany, {}, std::nullopt, lines.size(), "lines" });
            return false;
        }
        // 先看每条线自己：点是数、两端分开
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (!InRange(lines[i]))
                problems.push_back(Problem{ Problem::Kind::OutOfRange, { i } });
            else if (Length(lines[i]) <= Tol)
                problems.push_back(Problem{ Problem::Kind::TooShort, { i } });
        }
        if (!problems.empty())
        {
            Cap(problems);
            return false;
        }
        // 线切成直线段（圆弧切成弦），各知道是哪条线的、两端是哪个点；Tol 以内的端点算一个
        std::vector<std::vector<P2>> drawn;
        size_t count = 0;
        for (const SketchLine& l : lines)
        {
            drawn.push_back(Points(l));
            count += drawn.back().size() - 1;
        }
        if (count > MaxPieces)
        {
            problems.push_back(Problem{ Problem::Kind::TooMany, {}, std::nullopt, count, "pieces" });
            return false;
        }
        Welder welder;
        std::vector<std::pair<Line, size_t>> pieces;
        std::vector<std::array<size_t, 2>> ends;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const std::vector<P2>& pts = drawn[i];
            std::vector<size_t> nodes;
            for (const P2& p : pts)
                nodes.push_back(welder.Weld(p));
            for (size_t k = 0; k + 1 < pts.size(); ++k)
                // 小圆弧落成一个点的弦不算一段
                if (nodes[k] != nodes[k + 1])
                {
                    pieces.push_back({ Line{ pts[k], pts[k + 1] }, i });
                    ends.push_back({ nodes[k], nodes[k + 1] });
                }
        }
        const std::vector<P2>& points = welder.points;
        // 可能相碰的每对段（外框各放大 Tol 后相叠），怎么碰的；同一条线的也看（它可能自己相交）。
        // 一对线同一种问题，只说最先画的那两段的
        std::vector<Box> boxes;
        for (const auto& [l, _] : pieces)
            boxes.push_back({ std::min(l[0].x, l[1].x) - Tol, std::min(l[0].y, l[1].y) - Tol, std::max(l[0].x, l[1].x) + Tol,
                              std::max(l[0].y, l[1].y) + Tol });
        const Boxes tree(boxes);
        std::map<std::tuple<size_t, size_t, int>, std::tuple<size_t, size_t, Problem>> found;
        size_t work = 0;
        const int walked = tree.Pairs(work, [&](size_t i, size_t j)
        {
            const bool first = std::pair{ pieces[i].second, i } <= std::pair{ pieces[j].second, j };
            const size_t p = first ? i : j, q = first ? j : i;
            const std::array<size_t, 2> pair{ pieces[p].second, pieces[q].second };
            if (auto problem = Meet(pieces[p].first, pieces[q].first, ends[p], ends[q], pair))
            {
                const auto key = std::tuple{ pair[0], pair[1], problem->Rank() };
                const auto it = found.find(key);
                if (it == found.end())
                    found.emplace(key, std::tuple{ p, q, *problem });
                else if (std::pair{ p, q } < std::pair{ std::get<0>(it->second), std::get<1>(it->second) })
                    it->second = std::tuple{ p, q, *problem };
            }
            return found.size() <= MaxProblems;
        });
        if (walked < 0)
        {
            problems.push_back(Problem{ Problem::Kind::TooMany, {}, std::nullopt, work, "steps" });
            return false;
        }
        for (const auto& [_, v] : found)
            problems.push_back(std::get<2>(v));
        if (walked == 0)
        {
            SortProblems(problems);
            Cap(problems);
            return false;
        }
        // 每个点要有两段：一段的开着，三段以上的分叉
        std::vector<std::vector<size_t>> at(points.size());
        for (size_t i = 0; i < ends.size(); ++i)
            for (const size_t n : ends[i])
                at[n].push_back(i);
        size_t opens = 0, branches = 0;
        for (size_t n = 0; n < points.size(); ++n)
        {
            if (at[n].size() == 1 && opens <= MaxProblems)
            {
                ++opens;
                problems.push_back(Problem{ Problem::Kind::Open, { pieces[at[n][0]].second }, points[n] });
            }
            else if (at[n].size() > 2 && branches <= MaxProblems)
            {
                ++branches;
                std::vector<size_t> ls;
                for (const size_t p : at[n])
                    ls.push_back(pieces[p].second);
                std::sort(ls.begin(), ls.end());
                problems.push_back(Problem{ Problem::Kind::Branch, ls, points[n] });
            }
        }
        if (!problems.empty())
        {
            SortProblems(problems);
            Cap(problems);
            return false;
        }
        // 每个点都有两段：草图的每一块是一个环，从它的第一段起按画的方向走
        std::vector<bool> used(pieces.size(), false);
        std::vector<std::pair<Ring, std::vector<size_t>>> loops;
        for (size_t start = 0; start < pieces.size(); ++start)
        {
            if (used[start])
                continue;
            Ring ring;
            std::vector<size_t> members;
            size_t piece = start, from = ends[start][0];
            while (!used[piece])
            {
                used[piece] = true;
                const size_t line = pieces[piece].second;
                if (std::find(members.begin(), members.end(), line) == members.end())
                    members.push_back(line);
                ring.push_back(points[from]);
                const size_t to = ends[piece][0] == from ? ends[piece][1] : ends[piece][0];
                from = to;
                for (const size_t p : at[to])
                    if (p != piece)
                    {
                        piece = p;
                        break;
                    }
            }
            if (std::abs(Geom::Area(ring)) <= Tol * Tol)
                problems.push_back(Problem{ Problem::Kind::NoArea, members });
            loops.emplace_back(std::move(ring), std::move(members));
        }
        if (!problems.empty())
        {
            Cap(problems);
            return false;
        }
        // 环不相交也不相碰，所以一个环的任一点在另一个里就是在它里面：在几个里面说它是外轮廓（0 或偶数）还是洞（奇数），
        // 在最里面那个环里。只看外框含这个点的环
        std::vector<Box> bounds;
        for (const auto& [ring, _] : loops)
        {
            Box b{ INFINITY, INFINITY, -INFINITY, -INFINITY };
            for (const P2& p : ring)
                b = { std::min(b[0], p.x), std::min(b[1], p.y), std::max(b[2], p.x), std::max(b[3], p.y) };
            bounds.push_back(b);
        }
        const Boxes nest(bounds);
        std::vector<std::vector<size_t>> inside(loops.size());
        size_t steps = 0;
        for (size_t i = 0; i < loops.size(); ++i)
        {
            const P2 p = loops[i].first.front();
            nest.At(p, [&](size_t j)
            {
                if (j != i)
                {
                    steps += loops[j].first.size();
                    if (steps <= MaxNest && Geom::Contains(loops[j].first, p))
                        inside[i].push_back(j);
                }
                return steps <= MaxNest;
            });
            if (steps > MaxNest)
            {
                problems.push_back(Problem{ Problem::Kind::TooMany, {}, std::nullopt, steps, "loops" });
                return false;
            }
        }
        std::vector<size_t> regionOf(loops.size(), SIZE_MAX);
        for (size_t i = 0; i < loops.size(); ++i)
            if (inside[i].size() % 2 == 0)
            {
                regionOf[i] = out.regions.size();
                out.regions.push_back(Geom::Region{ Wound(loops[i].first, true), {} });
            }
        for (size_t i = 0; i < loops.size(); ++i)
        {
            if (inside[i].size() % 2 == 0)
                continue;
            for (const size_t j : inside[i])
                if (inside[j].size() + 1 == inside[i].size() && regionOf[j] != SIZE_MAX)
                {
                    out.regions[regionOf[j]].holes.push_back(Wound(loops[i].first, false));
                    break;
                }
        }
        return true;
    }

    bool SolveOne(const std::vector<SketchLine>& lines, Geom::Region& out, std::vector<Problem>& problems)
    {
        Profile p;
        if (!Solve(lines, p, problems))
            return false;
        if (p.regions.size() != 1)
        {
            problems = { Problem{ Problem::Kind::Separate, {}, std::nullopt, p.regions.size() } };
            return false;
        }
        out = std::move(p.regions.front());
        return true;
    }

    std::vector<SketchLine> LinesOf(const Ring& outline, const std::vector<Ring>& holes)
    {
        std::vector<SketchLine> out;
        auto add = [&](const Ring& r)
        {
            for (const auto& [a, b] : Geom::RingEdges(r))
            {
                SketchLine l;
                l.start = a;
                l.end   = b;
                out.push_back(l);
            }
        };
        add(outline);
        for (const Ring& h : holes)
            add(h);
        return out;
    }

    bool Draws(const std::vector<SketchLine>& lines, const Ring& outline, const std::vector<Ring>& holes)
    {
        auto same = [](const Ring& a, const Ring& b)
        {
            const size_t n = a.size();
            if (n == 0 || n != b.size())
                return false;
            for (size_t k = 0; k < n; ++k)
            {
                bool fwd = true, back = true;
                for (size_t i = 0; i < n && (fwd || back); ++i)
                {
                    fwd  = fwd && a[i].Dist(b[(i + k) % n]) <= Tol;
                    back = back && a[i].Dist(b[(k + n - i) % n]) <= Tol;
                }
                if (fwd || back)
                    return true;
            }
            return false;
        };
        Geom::Region r;
        std::vector<Problem> problems;
        if (!SolveOne(lines, r, problems) || !same(r.outer, outline) || r.holes.size() != holes.size())
            return false;
        return std::all_of(r.holes.begin(), r.holes.end(), [&](const Ring& h)
                           { return std::any_of(holes.begin(), holes.end(), [&](const Ring& g) { return same(h, g); }); });
    }

    bool Check(const std::string& who, const std::vector<SketchLine>& lines, std::string& error)
    {
        if (lines.size() > MaxLines)
        {
            error = who + ": its sketch has " + std::to_string(lines.size()) + " lines, more than " + std::to_string(MaxLines);
            return false;
        }
        size_t pieces = 0;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (!InRange(lines[i]))
            {
                error = who + ": sketch line " + std::to_string(i) + " has a point that isn't a number within " + Num(MaxCoord) +
                        " mm, or a curve reaching further";
                return false;
            }
            pieces += Points(lines[i]).size() - 1;
        }
        if (pieces > MaxPieces)
        {
            error = who + ": its sketch is drawn as " + std::to_string(pieces) + " straight pieces, more than " + std::to_string(MaxPieces);
            return false;
        }
        return true;
    }
}
