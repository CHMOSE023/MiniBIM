// 移植自 BIMCraft crates/core/src/csg.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Csg.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace MiniBIM::Bim::Csg
{
    namespace
    {
        using namespace V3;

        // 点离平面多近（毫米）算在平面上
        constexpr double Eps    = 1e-5;
        // 面积比这小（平方毫米）的三角形没有值得拿来切的平面
        constexpr double Sliver = 1e-6;

        const char* const TooMuch = "it has too many faces to cut: give it fewer sides, or fewer triangles";

        bool Work(Meter& m, size_t corners, std::string& error)
        {
            const uint64_t need = static_cast<uint64_t>(corners) + 1;
            if (m.work < need)
            {
                error = TooMuch;
                return false;
            }
            m.work -= need;
            return true;
        }

        bool Split(Meter& m, std::string& error)
        {
            if (m.splits < 1)
            {
                error = TooMuch;
                return false;
            }
            --m.splits;
            return true;
        }

        // 凸多边形：从法线 n 指向的一侧看角点逆时针，在 n · p = w 的平面里
        struct Poly
        {
            std::vector<P3> v;
            P3     n{};
            double w = 0.0;

            // 三角形当作多边形；小得没有平面的为空
            static std::optional<Poly> Of(const Tri& t)
            {
                P3 n = Cross(Sub(t[1], t[0]), Sub(t[2], t[0]));
                const double len = std::sqrt(Dot(n, n));
                if (!(len > 2.0 * Sliver))
                    return std::nullopt;
                n = Mul(n, 1.0 / len);
                return Poly{ { t[0], t[1], t[2] }, n, Dot(n, t[0]) };
            }

            void Flip()
            {
                std::reverse(v.begin(), v.end());
                n = Mul(n, -1.0);
                w = -w;
            }

            // 从第一个角点扇形分出的三角形；没有面积的略去
            void Triangles(std::vector<Tri>& out) const
            {
                if (v.empty())
                    return;
                const P3 first = v.front();
                for (size_t i = 1; i + 1 < v.size(); ++i)
                {
                    const P3 m = Cross(Sub(v[i], first), Sub(v[i + 1], first));
                    if (Dot(m, m) > (2.0 * Sliver) * (2.0 * Sliver))
                        out.push_back(Tri{ first, v[i], v[i + 1] });
                }
            }
        };

        // 平面把一个多边形分成什么
        enum class SideKind
        {
            On,      // 全在平面里，same 说法线与平面的同向还是反向
            Front,
            Back,
            Across,  // 跨过平面：前面的部分与后面的部分，太小的没有
        };

        struct Side
        {
            SideKind kind = SideKind::On;
            bool     same = false;
            Poly     poly;
            std::optional<Poly> front, back;
        };

        // p 对平面 n · x = w
        Side Divide(const P3& n, double w, Poly p)
        {
            std::vector<int> kinds;
            kinds.reserve(p.v.size());
            bool anyFront = false, anyBack = false;
            for (const P3& c : p.v)
            {
                const double d = Dot(n, c) - w;
                const int k = d > Eps ? 1 : (d < -Eps ? -1 : 0);
                anyFront = anyFront || k == 1;
                anyBack  = anyBack || k == -1;
                kinds.push_back(k);
            }
            Side s;
            if (!anyFront && !anyBack)
            {
                s.kind = SideKind::On;
                s.same = Dot(n, p.n) > 0.0;
                s.poly = std::move(p);
                return s;
            }
            if (anyFront && !anyBack)
            {
                s.kind = SideKind::Front;
                s.poly = std::move(p);
                return s;
            }
            if (!anyFront && anyBack)
            {
                s.kind = SideKind::Back;
                s.poly = std::move(p);
                return s;
            }
            std::vector<P3> front, back;
            const size_t m = p.v.size();
            for (size_t i = 0; i < m; ++i)
            {
                const P3 a = p.v[i], b = p.v[(i + 1) % m];
                const int ka = kinds[i], kb = kinds[(i + 1) % m];
                if (ka != -1)
                    front.push_back(a);
                if (ka != 1)
                    back.push_back(a);
                // 这个角在前、下一个在后：两者之间的边穿过平面
                if (ka * kb == -1)
                {
                    const P3 c = Add(a, Mul(Sub(b, a), (w - Dot(n, a)) / Dot(n, Sub(b, a))));
                    front.push_back(c);
                    back.push_back(c);
                }
            }
            s.kind = SideKind::Across;
            if (front.size() >= 3)
                s.front = Poly{ std::move(front), p.n, p.w };
            if (back.size() >= 3)
                s.back = Poly{ std::move(back), p.n, p.w };
            return s;
        }

        struct Node
        {
            P3     n{};
            double w = 0.0;
            // 在它的平面里的多边形
            std::vector<Poly> polys;
            std::optional<size_t> front, back;
        };

        // 实体的表面组成的平面树：每个平面前面是实体外，后面是实体内
        struct Bsp
        {
            std::vector<Node> nodes;

            static bool Of(const std::vector<Tri>& tris, Meter& meter, Bsp& out, std::string& error)
            {
                std::vector<Poly> polys;
                for (const Tri& t : tris)
                    if (auto p = Poly::Of(t))
                        polys.push_back(std::move(*p));
                return out.Build(std::move(polys), meter, error);
            }

            static Node NodeOf(const Poly& first) { return Node{ first.n, first.w, {}, std::nullopt, std::nullopt }; }

            // polys 加进树：每个沿树往下走到它所在平面的节点，跑出树的地方以那里的第一个多边形新建节点
            bool Build(std::vector<Poly> polys, Meter& meter, std::string& error)
            {
                if (polys.empty())
                    return true;
                if (nodes.empty())
                    nodes.push_back(NodeOf(polys.front()));
                std::vector<std::pair<size_t, std::vector<Poly>>> stack;
                stack.emplace_back(0, std::move(polys));
                while (!stack.empty())
                {
                    auto [i, list] = std::move(stack.back());
                    stack.pop_back();
                    if (i >= nodes.size())
                        continue;
                    const P3 n = nodes[i].n;
                    const double w = nodes[i].w;
                    std::vector<Poly> here, front, back;
                    for (Poly& p : list)
                    {
                        if (!Work(meter, p.v.size(), error))
                            return false;
                        Side s = Divide(n, w, std::move(p));
                        switch (s.kind)
                        {
                        case SideKind::On: here.push_back(std::move(s.poly)); break;
                        case SideKind::Front: front.push_back(std::move(s.poly)); break;
                        case SideKind::Back: back.push_back(std::move(s.poly)); break;
                        case SideKind::Across:
                            if (!Split(meter, error))
                                return false;
                            if (s.front)
                                front.push_back(std::move(*s.front));
                            if (s.back)
                                back.push_back(std::move(*s.back));
                            break;
                        }
                    }
                    for (Poly& p : here)
                        nodes[i].polys.push_back(std::move(p));
                    for (const bool ahead : { true, false })
                    {
                        std::vector<Poly>& part = ahead ? front : back;
                        if (part.empty())
                            continue;
                        const std::optional<size_t> known = ahead ? nodes[i].front : nodes[i].back;
                        size_t child = 0;
                        if (known)
                            child = *known;
                        else
                        {
                            child = nodes.size();
                            nodes.push_back(NodeOf(part.front()));
                        }
                        (ahead ? nodes[i].front : nodes[i].back) = child;
                        stack.emplace_back(child, std::move(part));
                    }
                }
                return true;
            }

            // polys 在实体外的部分：停在叶子前面的留下，停在后面的在实体里
            bool Clip(std::vector<Poly> polys, Meter& meter, std::vector<Poly>& kept, std::string& error) const
            {
                kept.clear();
                if (nodes.empty())
                {
                    kept = std::move(polys);
                    return true;
                }
                std::vector<std::pair<size_t, std::vector<Poly>>> stack;
                stack.emplace_back(0, std::move(polys));
                while (!stack.empty())
                {
                    auto [i, list] = std::move(stack.back());
                    stack.pop_back();
                    if (i >= nodes.size())
                        continue;
                    const Node& node = nodes[i];
                    std::vector<Poly> front, back;
                    for (Poly& p : list)
                    {
                        if (!Work(meter, p.v.size(), error))
                            return false;
                        Side s = Divide(node.n, node.w, std::move(p));
                        switch (s.kind)
                        {
                        case SideKind::On: (s.same ? front : back).push_back(std::move(s.poly)); break;
                        case SideKind::Front: front.push_back(std::move(s.poly)); break;
                        case SideKind::Back: back.push_back(std::move(s.poly)); break;
                        case SideKind::Across:
                            if (!Split(meter, error))
                                return false;
                            if (s.front)
                                front.push_back(std::move(*s.front));
                            if (s.back)
                                back.push_back(std::move(*s.back));
                            break;
                        }
                    }
                    if (node.front)
                        stack.emplace_back(*node.front, std::move(front));
                    else
                        for (Poly& p : front)
                            kept.push_back(std::move(p));
                    if (node.back)
                        stack.emplace_back(*node.back, std::move(back));
                }
                return true;
            }

            // 自己的多边形剪到 other 的实体之外
            bool ClipTo(const Bsp& other, Meter& meter, std::string& error)
            {
                for (Node& node : nodes)
                {
                    std::vector<Poly> kept;
                    if (!other.Clip(std::move(node.polys), meter, kept, error))
                        return false;
                    node.polys = std::move(kept);
                }
                return true;
            }

            // 里外翻过来：原来的实体变成不是它的一切
            void Invert()
            {
                for (Node& node : nodes)
                {
                    for (Poly& p : node.polys)
                        p.Flip();
                    node.n = Mul(node.n, -1.0);
                    node.w = -node.w;
                    std::swap(node.front, node.back);
                }
            }

            std::vector<Poly> Polygons() const
            {
                std::vector<Poly> out;
                for (const Node& n : nodes)
                    out.insert(out.end(), n.polys.begin(), n.polys.end());
                return out;
            }
        };

        // 包着 tris 的盒子的两个角
        std::pair<P3, P3> Bounds(const std::vector<Tri>& tris)
        {
            P3 lo{ INFINITY, INFINITY, INFINITY }, hi{ -INFINITY, -INFINITY, -INFINITY };
            for (const Tri& t : tris)
                for (const P3& p : t)
                    for (int k = 0; k < 3; ++k)
                    {
                        lo[k] = std::fmin(lo[k], p[k]);
                        hi[k] = std::fmax(hi[k], p[k]);
                    }
            return { lo, hi };
        }
    }

    bool Subtract(const std::vector<Tri>& solid, const std::vector<Tri>& voidShape, Meter& meter, std::vector<Tri>& out, std::string& error)
    {
        for (const auto& [what, n] : { std::pair{ "the form cut", solid.size() }, std::pair{ "the void", voidShape.size() } })
            if (n > MaxTriangles)
            {
                error = std::string(what) + " has " + std::to_string(n) + " triangles; it can't have more than " + std::to_string(MaxTriangles) +
                        " to cut";
                return false;
            }
        const auto [loS, hiS] = Bounds(solid);
        const auto [loV, hiV] = Bounds(voidShape);
        for (int k = 0; k < 3; ++k)
            if (loV[k] > hiS[k] + Eps || hiV[k] < loS[k] - Eps)
            {
                out = solid;
                return true;
            }
        Bsp a, b;
        if (!Bsp::Of(solid, meter, a, error) || !Bsp::Of(voidShape, meter, b, error))
            return false;
        a.Invert();
        if (!a.ClipTo(b, meter, error) || !b.ClipTo(a, meter, error))
            return false;
        b.Invert();
        if (!b.ClipTo(a, meter, error))
            return false;
        b.Invert();
        if (!a.Build(b.Polygons(), meter, error))
            return false;
        a.Invert();
        out.clear();
        for (const Poly& p : a.Polygons())
            p.Triangles(out);
        return true;
    }
}
