// 移植自 BIMCraft crates/rvt/src/sketch.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/element_record_plan_profiles.rs and
// src/partition_beam_axes.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Sketch.h).
#include "Rvt/Sketch.h"
#include "Rvt/Bytes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace MiniBIM::Rvt::Sketch
{
    namespace
    {
        using Curves::BoundedArc;
        using Curves::BoundedLine;
        using Box = std::array<double, 6>;

        // 顶点重合的容差（英尺）
        constexpr double VertexEps = 1e-6;
        // 两个空着的端点相距多远以内仍是一个顶点（英尺）：一毫米半，是画图的取整而不是缺口（Snowdon 样例有一块楼板的端点差
        // 0.0002 英尺，另一块差 0.0009）
        constexpr double SnapEps = 0.005;
        // 三个顶点共线时相对叉积的上限
        constexpr double CollinearEps = 1e-9;
        // 圆弧的弦离圆弧最远多少（英尺）
        constexpr double ArcTolerance = 0.0005;
        // 画一段圆弧最多用几个点
        constexpr size_t MaxArcPoints = 512;
        // 曲线的端点最多超出它的包围盒多远（英尺）
        constexpr double BoxEps = 1e-4;
        // FromEdgesClosed 最多替草图连几个空着的端点：楼板或屋顶有几个，恶意的文件更多
        constexpr size_t MaxClosing = 8;
        constexpr size_t None = std::numeric_limits<size_t>::max();

        // 圆弧画成从起点到终点的点，每条弦离圆弧不超过 ArcTolerance。扫过的角为 0 或超过一整圈时为空
        std::optional<std::vector<Curves::Vec3>> ArcPoints(const BoundedArc& arc)
        {
            const double sweep = arc.endAngle - arc.startAngle;
            if (!(std::isfinite(sweep) && sweep > 0.0 && sweep <= 2 * std::numbers::pi))
                return std::nullopt;
            const double step  = 2.0 * std::acos(std::clamp(1.0 - ArcTolerance / arc.radius, -1.0, 1.0));
            const double steps = std::ceil(sweep / step);
            // Rust 的 `as usize` 是饱和的（NaN 为 0）
            const size_t count = !(steps >= 1.0) ? 1 : steps >= double(MaxArcPoints) ? MaxArcPoints : static_cast<size_t>(steps);
            std::vector<Curves::Vec3> points;
            points.reserve(count + 1);
            for (size_t i = 0; i <= count; ++i)
                points.push_back(arc.Point(arc.startAngle + sweep * static_cast<double>(i) / static_cast<double>(count)));
            return points;
        }

        bool InBox(const Curves::Vec3& p, const Box& b) { return p[0] >= b[0] - BoxEps && p[0] <= b[3] + BoxEps && p[1] >= b[1] - BoxEps && p[1] <= b[4] + BoxEps; }

        // 曲线的平面范围离文件给它画的包围盒最差的一边有多远。直线记录能读成圆弧、圆弧记录也能读成直线，是包围盒说它是哪种：
        // 圆弧记录的直线读法常常落在圆弧的包围盒里，所以只看"端点在包围盒里"不够
        double Misfit(const Edge& e, const Box& b)
        {
            constexpr double max = std::numeric_limits<double>::max();
            Pt lo{ max, max }, hi{ -max, -max };
            const auto take = [&](const Pt& p)
            {
                lo = { std::min(lo.x, p.x), std::min(lo.y, p.y) };
                hi = { std::max(hi.x, p.x), std::max(hi.y, p.y) };
            };
            take(e.a);
            for (const Pt& p : e.mid)
                take(p);
            take(e.b);
            return std::max({ std::abs(lo.x - b[0]), std::abs(lo.y - b[1]), std::abs(hi.x - b[3]), std::abs(hi.y - b[4]) });
        }

        bool Same(const Pt& a, const Pt& b) { return std::abs(a.x - b.x) <= VertexEps && std::abs(a.y - b.y) <= VertexEps; }

        bool Near(double a, double b) { return std::abs(a - b) <= VertexEps; }

        // 带容差匹配的顶点表
        struct Vertices
        {
            std::vector<Pt>     points;
            std::vector<size_t> degree;

            size_t Intern(const Pt& p)
            {
                for (size_t i = 0; i < points.size(); ++i)
                    if (Same(points[i], p))
                        return i;
                points.push_back(p);
                degree.push_back(0);
                return points.size() - 1;
            }
        };

        using Loop = std::vector<Pt>;

        // 顶点之间的边 (a, b)，以及它们两端之间从 a 起按顺序的点
        struct Graph
        {
            Vertices                               vertices;
            std::vector<std::pair<size_t, size_t>> edges;
            std::vector<std::vector<Pt>>           mids;
            std::vector<size_t>                    source;   // 每条边来自草图的哪条曲线

            // 加一条边；它把顶点连到自己、重复一条边或给某个顶点第三条边时返回 false
            bool Push(const Pt& a, const Pt& b, std::vector<Pt> mid)
            {
                if (edges.size() >= MaxEdges)
                    return false;
                const size_t ia = vertices.Intern(a), ib = vertices.Intern(b);
                if (ia == ib)
                    return false;
                for (size_t i = 0; i < edges.size(); ++i)
                {
                    const auto [p, q] = edges[i];
                    if (((p == ia && q == ib) || (p == ib && q == ia)) && mids[i] == mid)
                        return false;
                }
                if (++vertices.degree[ia] > 2 || ++vertices.degree[ib] > 2)
                    return false;
                edges.emplace_back(ia, ib);
                mids.push_back(std::move(mid));
                return true;
            }

            // 去掉没闭合的块之后边围成的环，以及去掉的是哪些边：留下的块的每个顶点恰好两条边，有空着的端点的块整块去掉
            std::pair<std::vector<Loop>, std::vector<size_t>> LoopsOfClosedPieces() const
            {
                // 沿着边走出每个顶点属于哪一块
                std::vector<size_t> piece(vertices.points.size(), None);
                size_t next = 0;
                for (const auto& [a, b] : edges)
                {
                    if (piece[a] == None && piece[b] == None)
                    {
                        piece[a] = piece[b] = next++;
                    }
                    else if (piece[a] == None)
                        piece[a] = piece[b];
                    else if (piece[b] == None)
                        piece[b] = piece[a];
                    else if (piece[a] != piece[b])
                    {
                        const size_t p = piece[a], q = piece[b];
                        std::replace(piece.begin(), piece.end(), q, p);
                    }
                }
                std::vector<bool> closed(next, true);
                for (size_t i = 0; i < vertices.degree.size(); ++i)
                    if (vertices.degree[i] != 2 && piece[i] != None)
                        closed[piece[i]] = false;
                std::vector<size_t> kept, dropped;
                for (size_t i = 0; i < edges.size(); ++i)
                    (closed[piece[edges[i].first]] ? kept : dropped).push_back(i);
                return { Walk(kept), std::move(dropped) };
            }

            // 边围成的环；不是每个顶点恰好两条边时为空
            std::optional<std::vector<Loop>> Loops() const
            {
                if (std::any_of(vertices.degree.begin(), vertices.degree.end(), [](size_t d) { return d != 2; }))
                    return std::nullopt;
                std::vector<size_t> all(edges.size());
                for (size_t i = 0; i < all.size(); ++i)
                    all[i] = i;
                return Walk(all);
            }

            // kept 里的边围成的环，它们的每个顶点恰好有其中两条
            std::vector<Loop> Walk(const std::vector<size_t>& kept) const
            {
                std::vector<std::vector<size_t>> at(vertices.points.size());
                for (const size_t i : kept)
                {
                    at[edges[i].first].push_back(i);
                    at[edges[i].second].push_back(i);
                }
                std::vector<bool> used(edges.size(), false);
                std::vector<Loop> loops;
                for (const size_t first : kept)
                {
                    if (used[first])
                        continue;
                    used[first] = true;
                    const size_t start = edges[first].first;
                    size_t current     = edges[first].second;
                    Loop ring{ vertices.points[start] };
                    ring.insert(ring.end(), mids[first].begin(), mids[first].end());
                    while (current != start)
                    {
                        ring.push_back(vertices.points[current]);
                        // 留下的块的每个顶点有两条边，所以总有一条没走过
                        const auto it = std::find_if(at[current].begin(), at[current].end(), [&](size_t e) { return !used[e]; });
                        if (it == at[current].end())
                            break;
                        const size_t next = *it;
                        used[next] = true;
                        const auto [a, b] = edges[next];
                        if (a == current)
                        {
                            ring.insert(ring.end(), mids[next].begin(), mids[next].end());
                            current = b;
                        }
                        else
                        {
                            ring.insert(ring.end(), mids[next].rbegin(), mids[next].rend());
                            current = a;
                        }
                    }
                    loops.push_back(std::move(ring));
                }
                return loops;
            }
        };

        double SignedArea(const Loop& points)
        {
            const size_t n = points.size();
            double sum = 0;
            for (size_t i = 0; i < n; ++i)
                sum += points[i].x * points[(i + 1) % n].y - points[(i + 1) % n].x * points[i].y;
            return sum / 2.0;
        }

        // p 是否在闭合的环里（奇偶规则）
        bool Inside(const Loop& polygon, const Pt& p)
        {
            if (polygon.empty())
                return false;
            bool within = false;
            Pt previous = polygon.back();
            for (const Pt& current : polygon)
            {
                const auto [x0, y0] = std::pair{ previous.x, previous.y };
                const auto [x1, y1] = std::pair{ current.x, current.y };
                if ((y1 > p.y) != (y0 > p.y) && p.x < x1 + (p.y - y1) * (x0 - x1) / (y0 - y1))
                    within = !within;
                previous = current;
            }
            return within;
        }

        // 去掉落在两个相邻顶点连线上的顶点
        Loop MergeCollinear(const Loop& points)
        {
            const size_t n = points.size();
            Loop out;
            for (size_t i = 0; i < n; ++i)
            {
                const Pt previous = points[(i + n - 1) % n], current = points[i], next = points[(i + 1) % n];
                const Pt first{ current.x - previous.x, current.y - previous.y }, second{ next.x - current.x, next.y - current.y };
                const double cross = first.x * second.y - first.y * second.x;
                const double scale = std::hypot(first.x, first.y) * std::hypot(second.x, second.y);
                if (!(scale > 0.0 && std::abs(cross / scale) < CollinearEps))
                    out.push_back(current);
            }
            return out;
        }

        // 把环排成区域与洞：环的深度是有几个更大的环包着它。深度为偶数的开始一块区域，奇数的是它所在的环的洞。
        // 只有一部分在另一个环里的环根本不是区域
        std::optional<std::vector<Piece>> Pieces(const std::vector<Loop>& loops)
        {
            std::vector<Loop> merged;
            merged.reserve(loops.size());
            for (const Loop& one : loops)
            {
                Loop simple = MergeCollinear(one);
                if (simple.size() < 3)
                    return std::nullopt;
                merged.push_back(std::move(simple));
            }
            std::stable_sort(merged.begin(), merged.end(), [](const Loop& a, const Loop& b) { return std::abs(SignedArea(a)) > std::abs(SignedArea(b)); });
            std::vector<size_t> container, depth;
            for (size_t index = 0; index < merged.size(); ++index)
            {
                const Loop& one = merged[index];
                size_t parent = None;
                for (size_t earlier = 0; earlier < index; ++earlier)
                {
                    const size_t within = static_cast<size_t>(std::count_if(one.begin(), one.end(), [&](const Pt& p) { return Inside(merged[earlier], p); }));
                    if (within == one.size())
                        parent = earlier;
                    else if (within > 0)
                        return std::nullopt;
                }
                depth.push_back(parent == None ? 0 : depth[parent] + 1);
                container.push_back(parent);
            }
            std::vector<Piece> out;
            std::vector<size_t> pieceOf(merged.size(), None);
            for (size_t index = 0; index < merged.size(); ++index)
            {
                Loop ring = merged[index];
                if (depth[index] % 2 == 0)
                {
                    if (SignedArea(ring) < 0.0)
                        std::reverse(ring.begin(), ring.end());
                    pieceOf[index] = out.size();
                    out.push_back({ std::move(ring), {} });
                }
                else
                {
                    if (SignedArea(ring) > 0.0)
                        std::reverse(ring.begin(), ring.end());
                    const size_t c = container[index];
                    if (c == None || pieceOf[c] == None)
                        return std::nullopt;
                    out[pieceOf[c]].holes.push_back(std::move(ring));
                }
            }
            return out;
        }

        // 每个端点有几条曲线在那里相接，留下不是恰好两条的
        std::vector<Pt> Dangling(const std::vector<Edge>& edges)
        {
            std::vector<std::pair<Pt, size_t>> ends;
            for (const Edge& e : edges)
                for (const Pt& p : { e.a, e.b })
                {
                    const auto it = std::find_if(ends.begin(), ends.end(), [&](const auto& held) { return Same(held.first, p); });
                    if (it != ends.end())
                        ++it->second;
                    else
                        ends.emplace_back(p, 1);
                }
            std::vector<Pt> out;
            for (const auto& [p, n] : ends)
                if (n != 2)
                    out.push_back(p);
            return out;
        }

        struct Pair
        {
            double d = 0;
            size_t i = 0, j = 0;
        };

        // 只有 span 合适的那一对空着的顶点；没有或不止一对时为空。斜跨包围盒对角的一对胜过沿一条轴跨满它的直的子线
        // （包围盒比线松的线段）
        std::optional<std::pair<Pt, Pt>> SoleCandidate(const Graph& g, const std::array<double, 4>& span)
        {
            const auto [x0, y0, x1, y1] = span;
            std::vector<size_t> open;
            for (size_t i = 0; i < g.vertices.points.size(); ++i)
            {
                const Pt& p = g.vertices.points[i];
                if (g.vertices.degree[i] < 2 && p.x >= x0 - VertexEps && p.x <= x1 + VertexEps && p.y >= y0 - VertexEps && p.y <= y1 + VertexEps)
                    open.push_back(i);
            }
            std::vector<std::pair<Pt, Pt>> corner, spanning;
            for (size_t slot = 0; slot < open.size(); ++slot)
                for (size_t other = slot + 1; other < open.size(); ++other)
                {
                    const size_t ia = open[slot], ib = open[other];
                    if (std::any_of(g.edges.begin(), g.edges.end(), [&](const auto& e) { return (e.first == ia && e.second == ib) || (e.first == ib && e.second == ia); }))
                        continue;
                    const Pt a = g.vertices.points[ia], b = g.vertices.points[ib];
                    const bool fillsX = Near(std::min(a.x, b.x), x0) && Near(std::max(a.x, b.x), x1);
                    const bool fillsY = Near(std::min(a.y, b.y), y0) && Near(std::max(a.y, b.y), y1);
                    if (fillsX && fillsY)
                        corner.emplace_back(a, b);
                    else if ((fillsX && Near(a.y, b.y)) || (fillsY && Near(a.x, b.x)))
                        spanning.emplace_back(a, b);
                }
            const auto& tier = corner.empty() ? spanning : corner;
            if (tier.size() != 1)
                return std::nullopt;
            return tier[0];
        }
    }

    std::optional<std::pair<std::optional<BoundedLine>, std::optional<BoundedArc>>> SketchCurve(std::span<const uint8_t> data)
    {
        static constexpr uint8_t tag[4] = { 0x04, 0x00, 0x08, 0x01 };
        for (size_t at = Bytes::Find(data, tag); at != Bytes::npos; at = Bytes::Find(data, tag, at + sizeof tag))
        {
            auto line = Curves::LineAt(data, at);
            auto arc  = Curves::ArcValuesAt(data, at);
            if (line || arc)
                return std::pair{ line, arc };
        }
        return std::nullopt;
    }

    std::optional<Edge> EdgeOf(std::span<const uint8_t> data, const Box& bbox)
    {
        const auto curve = SketchCurve(data);
        if (!curve)
            return std::nullopt;
        const auto& [line, arc] = *curve;
        std::optional<Edge> straight, curved;
        if (line && InBox(line->Start(), bbox) && InBox(line->End(), bbox))
        {
            const auto s = line->Start(), e = line->End();
            if (std::hypot(s[0] - e[0], s[1] - e[1]) > VertexEps)
                straight = Edge{ { s[0], s[1] }, { e[0], e[1] }, {} };
        }
        if (arc)
            if (const auto points = ArcPoints(*arc); points && std::all_of(points->begin(), points->end(), [&](const auto& p) { return InBox(p, bbox); }))
            {
                Edge c{ { points->front()[0], points->front()[1] }, { points->back()[0], points->back()[1] }, {} };
                for (size_t i = 1; i + 1 < points->size(); ++i)
                    c.mid.push_back({ (*points)[i][0], (*points)[i][1] });
                curved = std::move(c);
            }
        if (straight && curved)
            return Misfit(*curved, bbox) < Misfit(*straight, bbox) ? curved : straight;
        return straight ? straight : curved;
    }

    std::optional<std::vector<Piece>> FromEdges(const std::vector<Edge>& edges)
    {
        if (edges.size() < 3)
            return std::nullopt;
        Graph g;
        for (const Edge& e : edges)
            if (!g.Push(e.a, e.b, e.mid))
                return std::nullopt;
        const auto loops = g.Loops();
        return loops ? Pieces(*loops) : std::nullopt;
    }

    std::optional<std::pair<std::vector<Piece>, std::vector<size_t>>> FromEdgesOpen(const std::vector<Edge>& edges)
    {
        if (edges.size() < 3)
            return std::nullopt;
        Graph g;
        for (size_t i = 0; i < edges.size(); ++i)
        {
            if (!g.Push(edges[i].a, edges[i].b, edges[i].mid))
                return std::nullopt;
            g.source.push_back(i);
        }
        auto [loops, dropped] = g.LoopsOfClosedPieces();
        if (loops.empty())
            return std::nullopt;
        auto pieces = Pieces(loops);
        if (!pieces)
            return std::nullopt;
        for (size_t& i : dropped)
            i = g.source[i];
        return std::pair{ std::move(*pieces), std::move(dropped) };
    }

    std::optional<std::vector<Piece>> FromEdgesClosed(const std::vector<Edge>& edges)
    {
        if (edges.size() < 3 || edges.size() > MaxEdges)
            return std::nullopt;
        const std::vector<Pt> dangling = Dangling(edges);
        if (dangling.empty() || dangling.size() > MaxClosing || dangling.size() % 2 != 0)
            return std::nullopt;
        // 近的先连，每个端点只用一次：画图时一滑留下的缺口走短的那边闭合
        std::vector<Pair> pairs;
        for (size_t i = 0; i < dangling.size(); ++i)
            for (size_t j = i + 1; j < dangling.size(); ++j)
                pairs.push_back({ std::hypot(dangling[i].x - dangling[j].x, dangling[i].y - dangling[j].y), i, j });
        std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.d < y.d; });
        std::vector<bool> taken(dangling.size(), false);
        std::vector<Edge> all = edges;
        for (const Pair& p : pairs)
        {
            if (taken[p.i] || taken[p.j])
                continue;
            taken[p.i] = taken[p.j] = true;
            all.push_back({ dangling[p.i], dangling[p.j], {} });
        }
        return FromEdges(all);
    }

    std::optional<std::vector<Piece>> FromEdgesSnapped(const std::vector<Edge>& edges)
    {
        // 下面的端点匹配要两两比较：像图本身的边一样限住
        if (edges.size() > MaxEdges)
            return std::nullopt;
        if (auto pieces = FromEdges(edges))
            return pieces;
        // 每个端点与在那里相接的曲线条数；没有环单独留下的端点才是可以对齐的
        const std::vector<Pt> dangling = Dangling(edges);
        // 几个端点可能是画图的取整；多了是损坏，那时什么也不猜
        if (dangling.empty() || dangling.size() > 8)
            return std::nullopt;
        // 值得连的端点对，近的先，每个端点只用一次
        std::vector<Pair> pairs;
        for (size_t i = 0; i < dangling.size(); ++i)
            for (size_t j = i + 1; j < dangling.size(); ++j)
            {
                const double d = std::hypot(dangling[i].x - dangling[j].x, dangling[i].y - dangling[j].y);
                if (d > VertexEps && d <= SnapEps)
                    pairs.push_back({ d, i, j });
            }
        std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.d < y.d; });
        std::vector<bool> taken(dangling.size(), false);
        std::vector<std::pair<Pt, Pt>> moves;
        for (const Pair& p : pairs)
        {
            if (taken[p.i] || taken[p.j])
                continue;
            taken[p.i] = taken[p.j] = true;
            moves.emplace_back(dangling[p.i], dangling[p.j]);
        }
        if (moves.empty())
            return std::nullopt;
        const auto moved = [&](const Pt& p)
        {
            for (const auto& [from, to] : moves)
                if (Same(from, p))
                    return to;
            return p;
        };
        std::vector<Edge> joined;
        joined.reserve(edges.size());
        for (const Edge& e : edges)
            joined.push_back({ moved(e.a), moved(e.b), e.mid });
        return FromEdges(joined);
    }

    std::optional<std::vector<Piece>> FromBoxes(const std::vector<std::array<double, 4>>& boxes)
    {
        if (boxes.size() < 3 || boxes.size() > MaxEdges)
            return std::nullopt;
        Graph g;
        std::vector<std::array<double, 4>> pending;
        for (const auto& box : boxes)
        {
            const auto [x0, y0, x1, y1] = box;
            if (!std::all_of(box.begin(), box.end(), [](double v) { return std::isfinite(v); }))
                return std::nullopt;
            const bool nx = Near(x0, x1), ny = Near(y0, y1);
            if (nx && ny)
                return std::nullopt;
            if (nx)
            {
                if (!g.Push({ x0, y0 }, { x0, y1 }, {}))
                    return std::nullopt;
            }
            else if (ny)
            {
                if (!g.Push({ x0, y0 }, { x1, y0 }, {}))
                    return std::nullopt;
            }
            else
                pending.push_back(box);
        }
        while (!pending.empty())
        {
            std::optional<std::pair<size_t, std::pair<Pt, Pt>>> found;
            for (size_t i = 0; i < pending.size() && !found; ++i)
                if (const auto c = SoleCandidate(g, pending[i]))
                    found = std::pair{ i, *c };
            if (!found)
                return std::nullopt;
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(found->first));
            if (!g.Push(found->second.first, found->second.second, {}))
                return std::nullopt;
        }
        const auto loops = g.Loops();
        return loops ? Pieces(*loops) : std::nullopt;
    }
}
