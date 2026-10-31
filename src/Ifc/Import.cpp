// 移植自 BIMCraft crates/core/src/import.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Commands/Commands.h"
#include "Ifc/Ifc.h"
#include "Ifc/Step.h"
#include "Model/Skeleton.h"
#include "Model/Text.h"
#include "Model/Walls.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace MiniBIM::Bim::Ifc
{
    namespace
    {
        using Step::Entity;
        using Step::File;
        using Step::V;

        constexpr double kPi = 3.14159265358979323846;

        // ── 读参数 ──────────────────────────────────────────────
        const V* Arg(const Entity& e, size_t i) { return i < e.args.size() ? &e.args[i] : nullptr; }

        std::optional<double> RealOf(const V* v)
        {
            if (!v)
                return std::nullopt;
            double x = 0.0;
            if (v->kind == V::Kind::Real)
                x = v->x;
            else if (v->kind == V::Kind::Int)
                x = static_cast<double>(v->i);
            else if (v->kind == V::Kind::Typed && !v->list.empty())
                return RealOf(&v->list.front());
            else
                return std::nullopt;
            return std::isfinite(x) ? std::optional<double>(x) : std::nullopt;
        }

        std::optional<std::string_view> TextOf(const V* v)
        {
            if (!v)
                return std::nullopt;
            if (v->kind == V::Kind::Str)
            {
                const std::string_view t = Text::Trim(v->s);
                return t.empty() ? std::nullopt : std::optional<std::string_view>(t);
            }
            if (v->kind == V::Kind::Typed && !v->list.empty())
                return TextOf(&v->list.front());
            return std::nullopt;
        }

        std::optional<uint64_t> Reference(const V* v)
        {
            return v && v->kind == V::Kind::Ref ? std::optional<uint64_t>(v->ref) : std::nullopt;
        }

        const std::vector<V>& ListOf(const V* v)
        {
            static const std::vector<V> none;
            return v && v->kind == V::Kind::List ? v->list : none;
        }

        std::vector<uint64_t> RefsOf(const V* v)
        {
            std::vector<uint64_t> out;
            for (const V& x : ListOf(v))
                if (x.kind == V::Kind::Ref)
                    out.push_back(x.ref);
            return out;
        }

        std::optional<std::string_view> Enumeration(const V* v)
        {
            return v && v->kind == V::Kind::Enum ? std::optional<std::string_view>(v->s) : std::nullopt;
        }

        // 整数的列表：下标，从 1 起
        std::vector<size_t> Indices(const V* v)
        {
            std::vector<size_t> out;
            if (v && v->kind == V::Kind::Typed && !v->list.empty())
                return Indices(&v->list.front());
            if (v && v->kind == V::Kind::List)
                for (const V& x : v->list)
                    if (x.kind == V::Kind::Int && x.i >= 0)
                        out.push_back(static_cast<size_t>(x.i));
            return out;
        }

        // 截到名称能有的长度
        std::string Clip(std::string_view text)
        {
            text = Text::Trim(text);
            size_t n = 0, i = 0;
            for (; i < text.size(); ++i)
                if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
                {
                    if (n == MaxText)
                        break;
                    ++n;
                }
            return std::string(text.substr(0, i));
        }

        // 名称里写的数：到百分位，不带多余的 0
        std::string Num(double x) { return Text::Shortest(std::round(x * 100.0) / 100.0 + 0.0); }

        double Round(double x, double k) { return std::round(x * k) / k; }

        // 到 1/10 毫米的整数：比较角点是不是同一个
        int64_t Tenths(double x)
        {
            const double r = std::round(x * 10.0);
            return std::isfinite(r) ? static_cast<int64_t>(std::clamp(r, -9.2e18, 9.2e18)) : 0;
        }

        P3 Unit3(const P3& a)
        {
            const double l = std::sqrt(V3::Dot(a, a));
            return l > 0.0 ? V3::Mul(a, 1.0 / l) : P3{ 0.0, 0.0, 0.0 };
        }

        // 最小与最大
        struct Spread
        {
            std::optional<std::pair<double, double>> v;
            void Add(double x)
            {
                if (!v)
                    v = std::pair{ x, x };
                else
                    v = std::pair{ std::fmin(v->first, x), std::fmax(v->second, x) };
            }
        };

        template <class F> std::optional<std::pair<double, double>> SpanOf(const std::vector<std::vector<P3>>& faces, F f)
        {
            Spread s;
            for (const auto& face : faces)
                for (const P3& p : face)
                    s.Add(f(p));
            return s.v;
        }

        template <class T, class F> std::optional<std::pair<double, double>> SpanOver(const std::vector<T>& xs, F f)
        {
            Spread s;
            for (const T& x : xs)
                s.Add(f(x));
            return s.v;
        }

        // ── 坐标系 ──────────────────────────────────────────────
        struct Frame
        {
            P3 o{ 0.0, 0.0, 0.0 };
            P3 x{ 1.0, 0.0, 0.0 };
            P3 y{ 0.0, 1.0, 0.0 };
            P3 z{ 0.0, 0.0, 1.0 };

            P3 At(const P3& p) const { return V3::Add(o, Dir(p)); }
            P3 Dir(const P3& d) const { return V3::Add(V3::Add(V3::Mul(x, d[0]), V3::Mul(y, d[1])), V3::Mul(z, d[2])); }
            // local（在这个坐标系里给出）在它上一级的坐标系里
            Frame Then(const Frame& local) const { return Frame{ At(local.o), Dir(local.x), Dir(local.y), Dir(local.z) }; }
        };

        // o 处的坐标轴：沿 z，x 转到与它垂直
        Frame Orthonormal(const P3& o, const P3& x, const P3& zIn)
        {
            const P3 z = V3::Dot(zIn, zIn) > 1e-18 ? Unit3(zIn) : P3{ 0.0, 0.0, 1.0 };
            const auto square = [&](const P3& v) { return V3::Sub(v, V3::Mul(z, V3::Dot(v, z))); };
            const P3 sx = square(x);
            const P3 ux = V3::Dot(sx, sx) > 1e-12 ? Unit3(sx) : Unit3(square(std::abs(z[0]) < 0.9 ? P3{ 1.0, 0.0, 0.0 } : P3{ 0.0, 1.0, 0.0 }));
            return Frame{ o, ux, V3::Cross(z, ux), z };
        }

        // 多边形的法线（Newell 法）：长度是面积的两倍
        P3 Newell(const std::vector<P3>& f)
        {
            P3 n{ 0.0, 0.0, 0.0 };
            for (size_t i = 0; i < f.size(); ++i)
                n = V3::Add(n, V3::Cross(f[i], f[(i + 1) % f.size()]));
            return n;
        }

        P3 Normal(const std::vector<P3>& f)
        {
            const P3 n = Newell(f);
            return V3::Dot(n, n) > 1e-12 ? Unit3(n) : P3{ 0.0, 0.0, 0.0 };
        }

        double Area3(const std::vector<P3>& f)
        {
            const P3 n = Newell(f);
            return std::sqrt(V3::Dot(n, n)) / 2.0;
        }

        // 一个环与它沿 up 移过去的副本，围成朝外的面
        void Prism(std::vector<std::vector<P3>>& out, std::vector<P3> bottom, const P3& up)
        {
            // 绕 up 逆时针，所以不论轮廓怎么走，面都朝外
            if (V3::Dot(Newell(bottom), up) < 0.0)
                std::reverse(bottom.begin(), bottom.end());
            std::vector<P3> top;
            for (const P3& p : bottom)
                top.push_back(V3::Add(p, up));
            for (size_t i = 0; i < bottom.size(); ++i)
            {
                const size_t j = (i + 1) % bottom.size();
                out.push_back({ bottom[i], bottom[j], top[j], top[i] });
            }
            out.push_back(std::vector<P3>(bottom.rbegin(), bottom.rend()));
            out.push_back(top);
        }

        // 去掉连续重复的点，以及末尾重复第一个的点
        Ring Open(Ring pts)
        {
            Ring out;
            for (const P2& p : pts)
                if (out.empty() || p.Dist(out.back()) >= 1e-6)
                    out.push_back(p);
            if (out.size() > 1 && out.front().Dist(out.back()) < 1e-6)
                out.pop_back();
            return out;
        }

        // faces 的边里没有两个面共用的那些连成的圈，每个面在平面上转成逆时针：区域的外轮廓逆时针，洞顺时针。
        // 边上有别的面的角点时在那里算两条边。每圈从它的西南角起，按这个顺序：同一个文件每次读出一样
        std::vector<Ring> Loops(const std::vector<const std::vector<P3>*>& faces)
        {
            using K = std::pair<int64_t, int64_t>;
            const auto key = [](P2 p) { return K{ Tenths(p.y), Tenths(p.x) }; };
            std::vector<Ring> rings;
            for (const auto* f : faces)
            {
                Ring ring;
                for (const P3& p : *f)
                    ring.emplace_back(p[0], p[1]);
                if (Geom::Area(ring) < 0.0)
                    std::reverse(ring.begin(), ring.end());
                rings.push_back(std::move(ring));
            }
            std::vector<P2> corners;
            for (const Ring& r : rings)
                corners.insert(corners.end(), r.begin(), r.end());
            std::map<std::pair<K, K>, P2> edges;
            for (const Ring& ring : rings)
                for (const auto& [a, b] : Geom::RingEdges(ring))
                {
                    std::vector<std::pair<double, P2>> on;
                    for (const P2& p : corners)
                        if (Geom::DistToSegment(p, a, b) < 0.05 && p.Dist(a) > 0.05 && p.Dist(b) > 0.05)
                            on.emplace_back(Geom::ParamOnSegment(p, a, b), p);
                    std::stable_sort(on.begin(), on.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
                    std::vector<P2> pts = { a };
                    for (const auto& [_, p] : on)
                        pts.push_back(p);
                    pts.push_back(b);
                    for (size_t i = 0; i + 1 < pts.size(); ++i)
                    {
                        const K ka = key(pts[i]), kb = key(pts[i + 1]);
                        // 两个面共用的边两个方向各走一次，在轮廓里面
                        if (ka != kb && edges.erase({ kb, ka }) == 0)
                            edges[{ ka, kb }] = pts[i];
                    }
                }
            std::map<K, std::pair<K, P2>> next;
            for (const auto& [k, a] : edges)
                next[k.first] = { k.second, a };
            std::vector<Ring> out;
            while (!next.empty())
            {
                Ring ring;
                K k = next.begin()->first;
                for (auto it = next.find(k); it != next.end(); it = next.find(k))
                {
                    ring.push_back(it->second.second);
                    k = it->second.first;
                    next.erase(it);
                }
                if (ring.size() >= 3)
                    out.push_back(std::move(ring));
            }
            return out;
        }

        // 朝上（或朝下）、各点都在 on（平的或斜的平面）半毫米以内的面的轮廓：其中最大的一圈，角点逆时针；以及它里面的洞，顺时针
        std::optional<Geom::Region> Outline(const std::vector<std::vector<P3>>& faces, const Plane& on, bool up)
        {
            std::vector<const std::vector<P3>*> picked;
            for (const auto& f : faces)
            {
                if (f.size() < 3)
                    continue;
                const P3 n = Normal(f);
                bool all = true;
                for (const P3& p : f)
                    all &= std::abs(p[2] - on.At(P2(p[0], p[1]))) <= 0.5;
                if (all && std::abs(n[2]) >= 0.2 && (n[2] > 0.0) == up)
                    picked.push_back(&f);
            }
            std::vector<Ring> rings;
            for (const Ring& l : Loops(picked))
                if (Ring c = Geom::Corners(l); c.size() >= 3)
                    rings.push_back(std::move(c));
            std::stable_sort(rings.begin(), rings.end(), [](const Ring& a, const Ring& b) { return std::abs(Geom::Area(b)) < std::abs(Geom::Area(a)); });
            if (rings.empty())
                return std::nullopt;
            Ring ring = rings.front();
            if (Geom::Area(ring) < 0.0)
                std::reverse(ring.begin(), ring.end());
            Geom::Region out{ ring, {} };
            for (size_t i = 1; i < rings.size(); ++i)
            {
                bool in = Geom::Area(rings[i]) < 0.0;
                for (const P2& p : rings[i])
                    in &= Geom::Contains(ring, p);
                if (in)
                    out.holes.push_back(rings[i]);
            }
            return out;
        }

        // 板的顶面（斜的或平的）：最大的朝上的面所在的平面，它在平面上的轮廓与其中的洞；以及底面（最大的朝下的面所在的平面）
        // 铅垂地在它下面多深
        std::optional<std::tuple<Plane, Geom::Region, double>> TopOf(const std::vector<std::vector<P3>>& faces)
        {
            const auto largest = [&](bool up) -> const std::vector<P3>*
            {
                const std::vector<P3>* best = nullptr;
                for (const auto& f : faces)
                    if (f.size() >= 3 && std::abs(Normal(f)[2]) > 0.2 && (Normal(f)[2] > 0.0) == up && (!best || Area3(f) >= Area3(*best)))
                        best = &f;
                return best;
            };
            const auto* t = largest(true);
            const auto* u = largest(false);
            if (!t || !u)
                return std::nullopt;
            const auto top = Plane::Of(*t), under = Plane::Of(*u);
            if (!top || !under)
                return std::nullopt;
            auto shape = Outline(faces, *top, true);
            if (!shape || shape->outer.empty())
                return std::nullopt;
            const P2 q = shape->outer.front();
            return std::tuple{ *top, *shape, top->At(q) - under->At(q) };
        }

        // 环里面靠中间的一点：组成它的三角形里最大的那个的中点
        std::optional<P2> Inside(const Ring& ring)
        {
            std::optional<P2> best;
            double size = -1.0;
            for (const auto& [a, b, c] : Geom::Triangulate(ring))
            {
                if (a >= ring.size() || b >= ring.size() || c >= ring.size())
                    continue;
                const double s = std::abs((ring[b] - ring[a]).Cross(ring[c] - ring[a]));
                if (s >= size)
                {
                    size = s;
                    best = (ring[a] + ring[b] + ring[c]) * (1.0 / 3.0);
                }
            }
            return best;
        }

        // 两个环的角点相同，准到毫米
        bool Same(const Ring& a, const Ring& b)
        {
            if (a.size() != b.size())
                return false;
            for (const P2& p : a)
                if (std::none_of(b.begin(), b.end(), [&](const P2& q) { return q.Dist(p) < 1.0; }))
                    return false;
            return true;
        }

        // ── 读几何 ──────────────────────────────────────────────
        class Reader
        {
        public:
            const File* f = nullptr;
            // 文件的长度单位是几毫米，平面角单位是几弧度
            double mm  = 1.0;
            double rad = 1.0;
            // 从平面上的每个点减去：放在很远处（场地的地图坐标）的模型移到原点附近
            P2 shift;

            const Entity* Get(uint64_t id) const
            {
                const auto it = f->entities.find(id);
                return it == f->entities.end() ? nullptr : &it->second;
            }
            const Entity* Get(std::optional<uint64_t> id) const { return id ? Get(*id) : nullptr; }
            const Entity* At(const V* v) const { return Get(Reference(v)); }

            std::optional<double> Len(const V* v) const
            {
                const auto x = RealOf(v);
                return x ? std::optional<double>(*x * mm) : std::nullopt;
            }

            // 模型的点在平面上
            P2 Flat(const P3& p) const { return P2(p[0] - shift.x, p[1] - shift.y); }

            std::vector<double> Coords(const V* v) const
            {
                std::vector<double> out;
                for (const V& x : ListOf(v))
                    if (const auto r = RealOf(&x))
                        out.push_back(*r);
                return out;
            }

            std::optional<P3> Point(std::optional<uint64_t> id) const
            {
                const Entity* e = Get(id);
                if (!e)
                    return std::nullopt;
                const std::vector<double> c = Coords(Arg(*e, 0));
                if (c.size() == 2)
                    return P3{ c[0] * mm, c[1] * mm, 0.0 };
                if (c.size() >= 3)
                    return P3{ c[0] * mm, c[1] * mm, c[2] * mm };
                return std::nullopt;
            }

            std::optional<P3> Direction(std::optional<uint64_t> id) const
            {
                const Entity* e = Get(id);
                if (!e)
                    return std::nullopt;
                const std::vector<double> c = Coords(Arg(*e, 0));
                P3 d;
                if (c.size() == 2)
                    d = { c[0], c[1], 0.0 };
                else if (c.size() >= 3)
                    d = { c[0], c[1], c[2] };
                else
                    return std::nullopt;
                return V3::Dot(d, d) > 1e-18 ? std::optional<P3>(Unit3(d)) : std::nullopt;
            }

            // 坐标系，三维或二维的
            Frame Axes(std::optional<uint64_t> id) const
            {
                const Entity* e = Get(id);
                if (!e)
                    return Frame{};
                const auto a = [&](size_t i) { return Reference(Arg(*e, i)); };
                const P3 o = Point(a(0)).value_or(P3{ 0.0, 0.0, 0.0 });
                if (e->name == "IFCAXIS2PLACEMENT3D")
                    return Orthonormal(o, Direction(a(2)).value_or(P3{ 1.0, 0.0, 0.0 }), Direction(a(1)).value_or(P3{ 0.0, 0.0, 1.0 }));
                if (e->name == "IFCAXIS2PLACEMENT2D")
                    return Orthonormal(o, Direction(a(1)).value_or(P3{ 1.0, 0.0, 0.0 }), P3{ 0.0, 0.0, 1.0 });
                return Frame{};
            }

            // 放置把它放的东西放在哪里：一连串的局部放置
            Frame Placement(std::optional<uint64_t> id, size_t depth) const
            {
                const Entity* e = Get(id);
                if (e && e->name == "IFCLOCALPLACEMENT" && depth < 64)
                    return Placement(Reference(Arg(*e, 0)), depth + 1).Then(Axes(Reference(Arg(*e, 1))));
                return Frame{};
            }

            // 笛卡尔变换算子：坐标轴，带缩放
            Frame Operator(std::optional<uint64_t> id) const
            {
                const Entity* e = Get(id);
                if (!e)
                    return Frame{};
                const auto a = [&](size_t i) { return Arg(*e, i); };
                const double s = RealOf(a(3)).value_or(1.0);
                std::optional<P3> z;
                P3 k{ s, s, s };
                if (e->name == "IFCCARTESIANTRANSFORMATIONOPERATOR3D")
                    z = Direction(Reference(a(4)));
                else if (e->name == "IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM")
                {
                    z = Direction(Reference(a(4)));
                    k = { s, RealOf(a(5)).value_or(s), RealOf(a(6)).value_or(s) };
                }
                else if (e->name == "IFCCARTESIANTRANSFORMATIONOPERATOR2D")
                    ;
                else if (e->name == "IFCCARTESIANTRANSFORMATIONOPERATOR2DNONUNIFORM")
                    k = { s, RealOf(a(4)).value_or(s), s };
                else
                    return Frame{};
                const P3 o = Point(Reference(a(2))).value_or(P3{ 0.0, 0.0, 0.0 });
                const Frame fr = Orthonormal(o, Direction(Reference(a(0))).value_or(P3{ 1.0, 0.0, 0.0 }), z.value_or(P3{ 0.0, 0.0, 1.0 }));
                return Frame{ fr.o, V3::Mul(fr.x, k[0]), V3::Mul(fr.y, k[1]), V3::Mul(fr.z, k[2]) };
            }

            std::vector<P3> PointList(std::optional<uint64_t> id) const
            {
                std::vector<P3> out;
                const Entity* e = Get(id);
                if (!e)
                    return out;
                for (const V& p : ListOf(Arg(*e, 0)))
                {
                    const std::vector<double> c = Coords(&p);
                    const auto at = [&](size_t i) { return (i < c.size() ? c[i] : 0.0) * mm; };
                    out.push_back({ at(0), at(1), at(2) });
                }
                return out;
            }

            // 曲线依次的点，在它自己的坐标里；圆弧切成短弦
            std::optional<std::vector<P3>> Curve(std::optional<uint64_t> id, size_t depth) const
            {
                const Entity* e = Get(id);
                if (!e || depth > 16)
                    return std::nullopt;
                const auto a = [&](size_t i) { return Arg(*e, i); };
                if (e->name == "IFCPOLYLINE")
                {
                    std::vector<P3> out;
                    for (const uint64_t p : RefsOf(a(0)))
                        if (const auto q = Point(p))
                            out.push_back(*q);
                    return out;
                }
                if (e->name == "IFCINDEXEDPOLYCURVE")
                {
                    const std::vector<P3> pts = PointList(Reference(a(0)));
                    const std::vector<V>& segments = ListOf(a(1));
                    if (segments.empty())
                        return pts;
                    std::vector<P3> out;
                    for (const V& s : segments)
                        for (const size_t i : Indices(&s))
                            if (i >= 1 && i - 1 < pts.size() && (out.empty() || out.back() != pts[i - 1]))
                                out.push_back(pts[i - 1]);
                    return out;
                }
                if (e->name == "IFCCOMPOSITECURVE" || e->name == "IFCCOMPOSITECURVEONSURFACE")
                {
                    std::vector<P3> out;
                    for (const uint64_t s : RefsOf(a(0)))
                    {
                        const Entity* seg = Get(s);
                        if (!seg)
                            continue;
                        auto pts = Curve(Reference(Arg(*seg, 2)), depth + 1);
                        if (!pts)
                            continue;
                        if (Enumeration(Arg(*seg, 1)) == "F")
                            std::reverse(pts->begin(), pts->end());
                        for (const P3& p : *pts)
                            if (out.empty() || V3::Dot(V3::Sub(out.back(), p), V3::Sub(out.back(), p)) > 1e-12)
                                out.push_back(p);
                    }
                    return out;
                }
                if (e->name == "IFCTRIMMEDCURVE")
                    return Trimmed(*e);
                if (e->name == "IFCCIRCLE")
                {
                    const Frame fr = Axes(Reference(a(0)));
                    const auto r = Len(a(1));
                    if (!r)
                        return std::nullopt;
                    std::vector<P3> out;
                    for (int i = 0; i < 24; ++i)
                    {
                        const double t = i * kPi / 12.0;
                        out.push_back(fr.At({ *r * std::cos(t), *r * std::sin(t), 0.0 }));
                    }
                    return out;
                }
                return std::nullopt;
            }

            // 剪裁过的直线，或剪裁成圆弧的圆
            std::optional<std::vector<P3>> Trimmed(const Entity& e) const
            {
                const Entity* basis = At(Arg(e, 0));
                if (!basis)
                    return std::nullopt;
                // 一个剪裁：一个点，或沿曲线的参数
                using Trim = std::pair<std::optional<P3>, std::optional<double>>;
                const auto trim = [&](const V* v)
                {
                    Trim out;
                    for (const V& t : ListOf(v))
                    {
                        if (t.kind == V::Kind::Ref && !out.first)
                            out.first = Point(t.ref);
                        else if (t.kind == V::Kind::Typed && !out.second && !t.list.empty())
                            out.second = RealOf(&t.list.front());
                    }
                    return out;
                };
                const Trim t1 = trim(Arg(e, 1)), t2 = trim(Arg(e, 2));
                if (basis->name == "IFCLINE")
                {
                    const auto p0 = Point(Reference(Arg(*basis, 0)));
                    if (!p0)
                        return std::nullopt;
                    const Entity* v = At(Arg(*basis, 1));
                    const P3 d = v ? Direction(Reference(Arg(*v, 0))).value_or(P3{ 1.0, 0.0, 0.0 }) : P3{ 1.0, 0.0, 0.0 };
                    const double k = v ? Len(Arg(*v, 1)).value_or(1.0) : 1.0;
                    const auto at = [&](const Trim& t) -> std::optional<P3>
                    {
                        if (t.first)
                            return t.first;
                        if (t.second)
                            return V3::Add(*p0, V3::Mul(d, k * *t.second));
                        return std::nullopt;
                    };
                    const auto a = at(t1), b = at(t2);
                    if (!a || !b)
                        return std::nullopt;
                    return std::vector<P3>{ *a, *b };
                }
                if (basis->name == "IFCCIRCLE")
                {
                    const Frame fr = Axes(Reference(Arg(*basis, 0)));
                    const auto r = Len(Arg(*basis, 1));
                    if (!r)
                        return std::nullopt;
                    const auto angle = [&](const Trim& t) -> std::optional<double>
                    {
                        if (t.first)
                        {
                            const P3 d = V3::Sub(*t.first, fr.o);
                            return std::atan2(V3::Dot(d, fr.y), V3::Dot(d, fr.x));
                        }
                        if (t.second)
                            return *t.second * rad;
                        return std::nullopt;
                    };
                    const auto a1 = angle(t1), a2 = angle(t2);
                    if (!a1 || !a2)
                        return std::nullopt;
                    double ccw = std::fmod(*a2 - *a1, 2.0 * kPi);
                    if (ccw < 0.0)
                        ccw += 2.0 * kPi;
                    const double sweep = Enumeration(Arg(e, 3)) == "F" ? ccw - 2.0 * kPi : ccw;
                    std::vector<P3> out;
                    for (int i = 0; i <= 12; ++i)
                    {
                        const double t = *a1 + sweep * i / 12.0;
                        out.push_back(fr.At({ *r * std::cos(t), *r * std::sin(t), 0.0 }));
                    }
                    return out;
                }
                if (!t1.first || !t2.first)
                    return std::nullopt;
                return std::vector<P3>{ *t1.first, *t2.first };
            }

            // 轮廓的外边，在它自己的平面里
            std::optional<Ring> Profile(std::optional<uint64_t> id, size_t depth) const
            {
                const Entity* e = Get(id);
                if (!e || depth > 16)
                    return std::nullopt;
                const auto a = [&](size_t i) { return Arg(*e, i); };
                const auto placed = [&](const Ring& pts)
                {
                    const Frame pos = Axes(Reference(a(2)));
                    Ring out;
                    for (const P2& p : pts)
                    {
                        const P3 q = pos.At({ p.x, p.y, 0.0 });
                        out.emplace_back(q[0], q[1]);
                    }
                    return out;
                };
                const auto rect = [](double w, double h) { return Ring{ { -w / 2, -h / 2 }, { w / 2, -h / 2 }, { w / 2, h / 2 }, { -w / 2, h / 2 } }; };
                const auto ellipse = [](double rx, double ry)
                {
                    Ring out;
                    for (int i = 0; i < 24; ++i)
                    {
                        const double t = i * kPi / 12.0;
                        out.emplace_back(rx * std::cos(t), ry * std::sin(t));
                    }
                    return out;
                };
                const std::string& n = e->name;
                if (n == "IFCRECTANGLEPROFILEDEF" || n == "IFCROUNDEDRECTANGLEPROFILEDEF" || n == "IFCRECTANGLEHOLLOWPROFILEDEF" || n == "IFCISHAPEPROFILEDEF" ||
                    n == "IFCASYMMETRICISHAPEPROFILEDEF")
                {
                    const auto w = Len(a(3)), h = Len(a(4));
                    if (!w || !h)
                        return std::nullopt;
                    return placed(rect(*w, *h));
                }
                // 型钢：它的外接矩形，高度沿 y
                if (n == "IFCLSHAPEPROFILEDEF" || n == "IFCUSHAPEPROFILEDEF" || n == "IFCCSHAPEPROFILEDEF" || n == "IFCTSHAPEPROFILEDEF" ||
                    n == "IFCZSHAPEPROFILEDEF")
                {
                    const auto d = Len(a(3)), w = Len(a(4));
                    if (!w || !d)
                        return std::nullopt;
                    return placed(rect(*w, *d));
                }
                if (n == "IFCCIRCLEPROFILEDEF" || n == "IFCCIRCLEHOLLOWPROFILEDEF")
                {
                    const auto r = Len(a(3));
                    if (!r)
                        return std::nullopt;
                    return placed(ellipse(*r, *r));
                }
                if (n == "IFCELLIPSEPROFILEDEF")
                {
                    const auto rx = Len(a(3)), ry = Len(a(4));
                    if (!rx || !ry)
                        return std::nullopt;
                    return placed(ellipse(*rx, *ry));
                }
                if (n == "IFCARBITRARYCLOSEDPROFILEDEF" || n == "IFCARBITRARYPROFILEDEFWITHVOIDS")
                {
                    const auto c = Curve(Reference(a(2)), depth + 1);
                    if (!c)
                        return std::nullopt;
                    Ring pts;
                    for (const P3& p : *c)
                        pts.emplace_back(p[0], p[1]);
                    return Open(std::move(pts));
                }
                if (n == "IFCDERIVEDPROFILEDEF")
                {
                    const Frame op = Operator(Reference(a(3)));
                    const auto inner = Profile(Reference(a(2)), depth + 1);
                    if (!inner)
                        return std::nullopt;
                    Ring out;
                    for (const P2& p : *inner)
                    {
                        const P3 q = op.At({ p.x, p.y, 0.0 });
                        out.emplace_back(q[0], q[1]);
                    }
                    return out;
                }
                return std::nullopt;
            }

            // 带洞轮廓里的洞，在它自己的平面里：合起来不超过 MaxPoints 个点的那些
            std::vector<Ring> Voids(std::optional<uint64_t> id, size_t depth) const
            {
                std::vector<Ring> out;
                const Entity* e = Get(id);
                if (!e || e->name != "IFCARBITRARYPROFILEDEFWITHVOIDS")
                    return out;
                size_t total = 0;
                for (const uint64_t c : RefsOf(Arg(*e, 3)))
                {
                    const auto pts = Curve(c, depth + 1);
                    if (!pts)
                        continue;
                    Ring h;
                    for (const P3& p : *pts)
                        h.emplace_back(p[0], p[1]);
                    h = Open(std::move(h));
                    if (h.size() < 3)
                        continue;
                    total += h.size();
                    if (total > MaxPoints)
                        break;
                    out.push_back(std::move(h));
                }
                return out;
            }

            // 表示项 id 的各个面，作为它自己坐标里的多边形
            void Faces(uint64_t id, size_t depth, std::vector<std::vector<P3>>& out) const
            {
                const Entity* e = Get(id);
                if (!e || depth > 16)
                    return;
                const auto a = [&](size_t i) { return Arg(*e, i); };
                const auto polygon = [&](std::vector<P3> pts)
                {
                    if (pts.size() >= 3)
                        out.push_back(std::move(pts));
                };
                const std::string& n = e->name;
                if (n == "IFCEXTRUDEDAREASOLID" || n == "IFCEXTRUDEDAREASOLIDTAPERED")
                {
                    const auto ring = Profile(Reference(a(0)), depth + 1);
                    const auto length = Len(a(3));
                    if (!ring || !length)
                        return;
                    const Frame pos = Axes(Reference(a(1)));
                    const P3 up = V3::Mul(pos.Dir(Direction(Reference(a(2))).value_or(P3{ 0.0, 0.0, 1.0 })), *length);
                    if (ring->size() >= 3)
                    {
                        // 轮廓里的洞：分成凸块
                        std::vector<Ring> parts = { *ring };
                        for (const Ring& v : Voids(Reference(a(0)), depth + 1))
                            parts = Walls::Minus(parts, v);
                        for (const Ring& piece : parts)
                        {
                            std::vector<P3> bottom;
                            for (const P2& p : piece)
                                bottom.push_back(pos.At({ p.x, p.y, 0.0 }));
                            Prism(out, std::move(bottom), up);
                        }
                    }
                }
                // 圆盘沿曲线扫过（电缆、管子）：沿每一段 24 条边的管；空心的当作实心
                else if (n == "IFCSWEPTDISKSOLID")
                {
                    const auto line = Curve(Reference(a(0)), depth + 1);
                    const auto r = Len(a(1));
                    if (!line || !r)
                        return;
                    // 每段 26 个面，几百万个点的曲线会占满内存：超过 MaxPoints 段的面时整个圆盘略去
                    if (out.size() + 26 * (line->size() > 0 ? line->size() - 1 : 0) > 26 * MaxPoints)
                        return;
                    for (size_t i = 0; i + 1 < line->size(); ++i)
                    {
                        const P3 p = (*line)[i], q = (*line)[i + 1];
                        const P3 along = V3::Sub(q, p);
                        if (V3::Dot(along, along) < 1e-12 || *r <= 0.0)
                            continue;
                        const P3 d = Unit3(along);
                        // 横过这一段：水平的，除非它是竖的
                        const P3 across = std::hypot(d[0], d[1]) < 1e-6 ? P3{ 1.0, 0.0, 0.0 } : Unit3(P3{ -d[1], d[0], 0.0 });
                        const P3 up = V3::Cross(d, across);
                        std::vector<P3> ring;
                        for (int k = 0; k < 24; ++k)
                        {
                            const double t = k * kPi / 12.0;
                            ring.push_back(V3::Add(p, V3::Add(V3::Mul(across, *r * std::cos(t)), V3::Mul(up, *r * std::sin(t)))));
                        }
                        Prism(out, std::move(ring), along);
                    }
                }
                // 剪掉的部分不改变元素是什么
                else if (n == "IFCBOOLEANCLIPPINGRESULT" || n == "IFCBOOLEANRESULT")
                {
                    if (const auto first = Reference(a(1)))
                        Faces(*first, depth + 1, out);
                    if (Enumeration(a(0)) == "UNION")
                        if (const auto second = Reference(a(2)))
                            Faces(*second, depth + 1, out);
                }
                else if (n == "IFCFACETEDBREP" || n == "IFCFACETEDBREPWITHVOIDS" || n == "IFCCSGSOLID")
                {
                    if (const auto inner = Reference(a(0)))
                        Faces(*inner, depth + 1, out);
                }
                else if (n == "IFCCLOSEDSHELL" || n == "IFCOPENSHELL" || n == "IFCCONNECTEDFACESET" || n == "IFCSHELLBASEDSURFACEMODEL" ||
                         n == "IFCFACEBASEDSURFACEMODEL")
                {
                    for (const uint64_t face : RefsOf(a(0)))
                        Faces(face, depth + 1, out);
                }
                else if (n == "IFCFACE" || n == "IFCFACESURFACE")
                {
                    std::vector<const Entity*> bounds;
                    for (const uint64_t b : RefsOf(a(0)))
                        if (const Entity* x = Get(b))
                            bounds.push_back(x);
                    const Entity* outer = nullptr;
                    for (const Entity* b : bounds)
                        if (b->name == "IFCFACEOUTERBOUND")
                        {
                            outer = b;
                            break;
                        }
                    if (!outer && !bounds.empty())
                        outer = bounds.front();
                    if (outer)
                    {
                        std::vector<P3> pts;
                        if (const Entity* lp = At(Arg(*outer, 0)))
                            for (const uint64_t p : RefsOf(Arg(*lp, 0)))
                                if (const auto q = Point(p))
                                    pts.push_back(*q);
                        if (Enumeration(Arg(*outer, 1)) == "F")
                            std::reverse(pts.begin(), pts.end());
                        polygon(std::move(pts));
                    }
                }
                else if (n == "IFCTRIANGULATEDFACESET" || n == "IFCTRIANGULATEDIRREGULARNETWORK" || n == "IFCPOLYGONALFACESET")
                {
                    const std::vector<P3> pts = PointList(Reference(a(0)));
                    const bool triangles = n != "IFCPOLYGONALFACESET";
                    const std::vector<size_t> pn = Indices(a(triangles ? 4 : 3));
                    const auto at = [&](size_t i) -> std::optional<P3>
                    {
                        if (!pn.empty())
                        {
                            if (i < 1 || i - 1 >= pn.size())
                                return std::nullopt;
                            i = pn[i - 1];
                        }
                        if (i < 1 || i - 1 >= pts.size())
                            return std::nullopt;
                        return pts[i - 1];
                    };
                    std::vector<std::vector<size_t>> polys;
                    if (triangles)
                        for (const V& t : ListOf(a(3)))
                            polys.push_back(Indices(&t));
                    else
                        for (const uint64_t fi : RefsOf(a(2)))
                            if (const Entity* face = Get(fi))
                                polys.push_back(Indices(Arg(*face, 0)));
                    for (const auto& poly : polys)
                    {
                        std::vector<P3> face;
                        for (const size_t i : poly)
                            if (const auto p = at(i))
                                face.push_back(*p);
                        polygon(std::move(face));
                    }
                }
                else if (n == "IFCMAPPEDITEM")
                {
                    const Entity* map = At(a(0));
                    if (!map)
                        return;
                    std::vector<std::vector<P3>> inner;
                    if (const Entity* rep = At(Arg(*map, 1)))
                        for (const uint64_t item : RefsOf(Arg(*rep, 3)))
                            Faces(item, depth + 1, inner);
                    const Frame fr = Operator(Reference(a(1))).Then(Axes(Reference(Arg(*map, 0))));
                    for (auto& face : inner)
                    {
                        for (P3& p : face)
                            p = fr.At(p);
                        out.push_back(std::move(face));
                    }
                }
                // 一个盒子：它的角或放置，三边的长度
                else if (n == "IFCBOUNDINGBOX" || n == "IFCBLOCK")
                {
                    Frame pos;
                    if (n == "IFCBLOCK")
                        pos = Axes(Reference(a(0)));
                    else
                        pos.o = Point(Reference(a(0))).value_or(P3{ 0.0, 0.0, 0.0 });
                    const auto x = Len(a(1)), y = Len(a(2)), z = Len(a(3));
                    if (x && y && z)
                        Prism(out, { pos.At({ 0.0, 0.0, 0.0 }), pos.At({ *x, 0.0, 0.0 }), pos.At({ *x, *y, 0.0 }), pos.At({ 0.0, *y, 0.0 }) },
                              pos.Dir({ 0.0, 0.0, *z }));
                }
            }

            // 产品的形状表示：各自的标识与表示项
            std::vector<std::pair<std::string_view, std::vector<uint64_t>>> Reps(const Entity& product) const
            {
                std::vector<std::pair<std::string_view, std::vector<uint64_t>>> out;
                const Entity* shape = At(Arg(product, 6));
                if (!shape)
                    return out;
                for (const uint64_t r : RefsOf(Arg(*shape, 2)))
                    if (const Entity* rep = Get(r))
                        out.emplace_back(TextOf(Arg(*rep, 1)).value_or(""), RefsOf(Arg(*rep, 3)));
                return out;
            }

            Frame FrameOf(const Entity& product) const { return Placement(Reference(Arg(product, 5)), 0); }

            // 产品的实体，各个面，在模型坐标里：Body 表示的；没有时线与平面轮廓以外的任何表示的
            std::vector<std::vector<P3>> Body(const Entity& product) const
            {
                const Frame fr = FrameOf(product);
                std::vector<std::vector<P3>> out;
                for (auto& [_, faces] : BodyItems(product))
                    for (auto& face : faces)
                    {
                        for (P3& p : face)
                            p = fr.At(p);
                        out.push_back(std::move(face));
                    }
                return out;
            }

            // 产品的实体，按表示项，各为它在产品自己坐标里的面：Body 表示的；没有时线与平面轮廓以外的任何表示的
            std::vector<std::pair<uint64_t, std::vector<std::vector<P3>>>> BodyItems(const Entity& product) const
            {
                const auto reps = Reps(product);
                const auto items = [&](const std::function<bool(const std::string&)>& keep)
                {
                    std::vector<uint64_t> out;
                    for (const auto& [i, it] : reps)
                        if (keep(Text::AsciiLower(i)))
                            out.insert(out.end(), it.begin(), it.end());
                    return out;
                };
                std::vector<uint64_t> chosen = items([](const std::string& i) { return i == "body" || i == "facetation"; });
                if (chosen.empty())
                    chosen = items([](const std::string& i)
                    {
                        return !(i == "axis" || i == "footprint" || i == "annotation" || i == "plan" || i == "profile" || i == "reference" ||
                                 i == "surface");
                    });
                std::vector<std::pair<uint64_t, std::vector<std::vector<P3>>>> out;
                for (const uint64_t item : chosen)
                {
                    std::vector<std::vector<P3>> faces;
                    Faces(item, 0, faces);
                    out.emplace_back(item, std::move(faces));
                }
                return out;
            }

            // 产品的 Axis 表示，在模型坐标里
            std::optional<std::vector<P3>> AxisOf(const Entity& product) const
            {
                for (const auto& [i, items] : Reps(product))
                {
                    if (Text::AsciiLower(i) != "axis")
                        continue;
                    const Frame fr = FrameOf(product);
                    auto pts = Curve(items.empty() ? std::nullopt : std::optional<uint64_t>(items.front()), 0);
                    if (!pts)
                        return std::nullopt;
                    for (P3& p : *pts)
                        p = fr.At(p);
                    return pts;
                }
                return std::nullopt;
            }
        };

        // 项目的长度单位是几毫米、平面角单位是几弧度
        bool Units(const File& f, double& mm, double& rad, std::string& error)
        {
            const auto si = [](std::optional<std::string_view> prefix)
            {
                static const std::pair<const char*, double> kPrefixes[] = {
                    { "EXA", 1e18 },  { "PETA", 1e15 }, { "TERA", 1e12 }, { "GIGA", 1e9 },   { "MEGA", 1e6 },  { "KILO", 1e3 }, { "HECTO", 1e2 },
                    { "DECA", 1e1 },  { "DECI", 1e-1 }, { "CENTI", 1e-2 }, { "MILLI", 1e-3 }, { "MICRO", 1e-6 }, { "NANO", 1e-9 },
                };
                for (const auto& [p, k] : kPrefixes)
                    if (prefix == p)
                        return k;
                return 1.0;
            };
            // 单位的种类，以及它有多大（国际单位制的基本单位：米、弧度）
            std::function<std::optional<std::pair<std::string, double>>(std::optional<uint64_t>, size_t)> unit =
                [&](std::optional<uint64_t> id, size_t depth) -> std::optional<std::pair<std::string, double>>
            {
                if (!id)
                    return std::nullopt;
                const auto it = f.entities.find(*id);
                if (it == f.entities.end())
                    return std::nullopt;
                const Entity& e = it->second;
                const std::string kind(Enumeration(Arg(e, 1)).value_or(""));
                if (e.name == "IFCSIUNIT")
                    return std::pair{ kind, si(Enumeration(Arg(e, 2))) };
                if ((e.name == "IFCCONVERSIONBASEDUNIT" || e.name == "IFCCONVERSIONBASEDUNITWITHOFFSET") && depth < 8)
                {
                    const auto m = Reference(Arg(e, 3));
                    const auto mt = m ? f.entities.find(*m) : f.entities.end();
                    if (mt == f.entities.end())
                        return std::nullopt;
                    const auto base = unit(Reference(Arg(mt->second, 1)), depth + 1);
                    const auto k = RealOf(Arg(mt->second, 0));
                    if (!base || !k)
                        return std::nullopt;
                    return std::pair{ kind, *k * base->second };
                }
                return std::nullopt;
            };
            // 第一个项目：名称从它来
            std::optional<uint64_t> first;
            for (const auto& [id, e] : f.entities)
                if (e.name == "IFCPROJECT" && (!first || id < *first))
                    first = id;
            if (!first)
            {
                error = "it has no IfcProject";
                return false;
            }
            const Entity& project = f.entities.at(*first);
            const auto a = Reference(Arg(project, 8));
            const auto at = a ? f.entities.find(*a) : f.entities.end();
            mm  = 1.0;
            rad = 1.0;
            if (at != f.entities.end())
                for (const uint64_t u : RefsOf(Arg(at->second, 0)))
                    if (const auto got = unit(u, 0))
                    {
                        if (got->first == "LENGTHUNIT")
                            mm = got->second * 1000.0;
                        else if (got->first == "PLANEANGLEUNIT")
                            rad = got->second;
                    }
            if (!(mm >= 1e-6 && mm <= 1e9) || !(std::isfinite(rad) && rad > 0.0))
            {
                error = "its length or angle unit isn't one MiniBIM can use";
                return false;
            }
            return true;
        }

        // 元素之间的关系：各在哪里、是什么的一部分、什么开在什么上、什么填在什么里，以及它的类型的名称
        struct Links
        {
            std::unordered_map<uint64_t, uint64_t> within;
            std::unordered_map<uint64_t, uint64_t> whole;
            std::unordered_map<uint64_t, std::vector<uint64_t>> parts;
            // 洞口开在哪个元素上；门窗填在哪个洞口里
            std::unordered_map<uint64_t, uint64_t> host;
            std::unordered_map<uint64_t, uint64_t> fills;
            std::unordered_map<uint64_t, std::string> typeName;
            // 元素的类型，以及元素或类型用什么做
            std::unordered_map<uint64_t, uint64_t> typed;
            std::unordered_map<uint64_t, uint64_t> material;

            static Links Read(const File& f, const std::vector<uint64_t>& ids)
            {
                Links l;
                for (const uint64_t id : ids)
                {
                    const Entity& e = f.entities.at(id);
                    const auto a = [&](size_t i) { return Arg(e, i); };
                    if (e.name == "IFCRELCONTAINEDINSPATIALSTRUCTURE")
                    {
                        if (const auto s = Reference(a(5)))
                            for (const uint64_t x : RefsOf(a(4)))
                                l.within[x] = *s;
                    }
                    else if (e.name == "IFCRELAGGREGATES")
                    {
                        if (const auto w = Reference(a(4)))
                            for (const uint64_t x : RefsOf(a(5)))
                            {
                                l.whole[x] = *w;
                                l.parts[*w].push_back(x);
                            }
                    }
                    else if (e.name == "IFCRELVOIDSELEMENT")
                    {
                        const auto h = Reference(a(4)), o = Reference(a(5));
                        if (h && o)
                            l.host[*o] = *h;
                    }
                    else if (e.name == "IFCRELFILLSELEMENT")
                    {
                        const auto o = Reference(a(4)), d = Reference(a(5));
                        if (o && d)
                            l.fills[*d] = *o;
                    }
                    else if (e.name == "IFCRELDEFINESBYTYPE")
                    {
                        if (const auto t = Reference(a(5)))
                        {
                            const auto it = f.entities.find(*t);
                            if (it != f.entities.end())
                                if (const auto name = TextOf(Arg(it->second, 2)))
                                    for (const uint64_t x : RefsOf(a(4)))
                                        l.typeName[x] = std::string(*name);
                            for (const uint64_t x : RefsOf(a(4)))
                                l.typed[x] = *t;
                        }
                    }
                    else if (e.name == "IFCRELASSOCIATESMATERIAL")
                    {
                        if (const auto mat = Reference(a(5)))
                            for (const uint64_t x : RefsOf(a(4)))
                                l.material[x] = *mat;
                    }
                }
                return l;
            }
        };

        template <class M> std::optional<uint64_t> Find(const M& map, uint64_t key)
        {
            const auto it = map.find(key);
            return it == map.end() ? std::nullopt : std::optional<uint64_t>(it->second);
        }

        // 弧墙的两个面，按它的实体的点离圆心多远：内半的中位数与外半的中位数，所以与别的墙相接处（切过弧一点）的角不算
        std::optional<std::pair<double, double>> ArcFaces(std::vector<double> radii)
        {
            radii.erase(std::remove_if(radii.begin(), radii.end(), [](double r) { return !std::isfinite(r); }), radii.end());
            std::sort(radii.begin(), radii.end());
            if (radii.empty())
                return std::nullopt;
            const double mid = (radii.front() + radii.back()) / 2.0;
            std::vector<double> inner, outer;
            for (const double r : radii)
                (r < mid ? inner : outer).push_back(r);
            if (inner.empty() || outer.empty())
                return std::nullopt;
            return std::pair{ inner[inner.size() / 2], outer[outer.size() / 2] };
        }

        // 实体的最高点
        P3 Highest(const std::vector<P3>& pts)
        {
            P3 best{ 0.0, 0.0, 0.0 };
            bool any = false;
            for (const P3& p : pts)
                if (!any || p[2] >= best[2])
                {
                    best = p;
                    any  = true;
                }
            return best;
        }

        // 文件里的墙，还没让差一点就相接的墙相接
        struct RawWall
        {
            uint64_t ifc = 0;
            P2 start, end;
            double arc = 0.0;
            double t   = 0.0;
            std::pair<double, double> z;
            // 它的实体最高处，在平面上
            P2 peak;
            Guid level;
            std::string typeName;
        };

        // 差一点就相接的墙让它们相接：端点在另一面墙的定位线一个墙厚以内的，移到那条线上，所以转角与丁字接头照 MiniBIM 的方式连接
        void Meet(std::vector<RawWall>& walls)
        {
            for (size_t i = 0; i < walls.size(); ++i)
                for (const bool atEnd : { false, true })
                {
                    // 弧墙照文件里的样子
                    if (walls[i].arc != 0.0)
                        continue;
                    const RawWall& w = walls[i];
                    const P2 a = w.start, b = w.end, p = atEnd ? w.end : w.start;
                    const double t = w.t;
                    const auto z = w.z;
                    bool joined = false;
                    for (size_t j = 0; j < walls.size(); ++j)
                        joined |= j != i && (walls[j].start.Dist(p) <= Tol || walls[j].end.Dist(p) <= Tol);
                    if (joined)
                        continue;
                    std::optional<std::pair<double, P2>> best;
                    for (size_t j = 0; j < walls.size(); ++j)
                    {
                        const RawWall& o = walls[j];
                        if (j == i || o.arc != 0.0 || !(o.z.first < z.second && z.first < o.z.second))
                            continue;
                        if (Geom::Parallel(a, b, o.start, o.end))
                            continue;
                        const auto x = Geom::LineIntersection(a, b, o.start, o.end);
                        if (!x)
                            continue;
                        const double reach = std::max(t, o.t);
                        const double along = (*x - o.start).Dot((o.end - o.start).Unit());
                        const double d = x->Dist(p);
                        if (d <= reach && along >= -reach && along <= o.start.Dist(o.end) + reach && (!best || d < best->first))
                            best = std::pair{ d, *x };
                    }
                    // 不能远到墙掉头或没了
                    if (best)
                    {
                        RawWall& m = walls[i];
                        const P2 s = atEnd ? m.start : best->second, e = atEnd ? best->second : m.end;
                        if ((e - s).Dot(b - a) > Tol * (b - a).Length())
                        {
                            m.start = Micron(s);
                            m.end   = Micron(e);
                        }
                    }
                }
        }

        // 元素 id 的材质层组用法是否沿它自己的轴往上叠（.POSITIVE.），所以第一层在它的背面：墙的右边、板的底面；没有用法时为空
        std::optional<bool> StackedUp(const Reader& r, const Links& l, uint64_t id)
        {
            const auto m = Find(l.material, id);
            const Entity* usage = r.Get(m);
            if (!usage || usage->name != "IFCMATERIALLAYERSETUSAGE")
                return std::nullopt;
            return Enumeration(Arg(*usage, 2)) == "POSITIVE";
        }

        // 文件给元素 id（或它的类型）的层：它的材质层组的，各为材质名称、厚度，以及按类别做什么；加起来与 t 差 1 毫米以上时，
        // 或者成不了类型时没有
        std::vector<Layer> LayersOf(const Reader& r, const Links& l, uint64_t id, double t)
        {
            auto m = Find(l.material, id);
            if (!m)
                if (const auto ty = Find(l.typed, id))
                    m = Find(l.material, *ty);
            const Entity* set = r.Get(m);
            if (!set)
                return {};
            if (set->name == "IFCMATERIALLAYERSETUSAGE")
            {
                set = r.At(Arg(*set, 0));
                if (!set)
                    return {};
            }
            if (set->name != "IFCMATERIALLAYERSET" || ListOf(Arg(*set, 0)).size() > MaxLayers)
                return {};
            std::vector<Layer> layers;
            bool told = false;
            for (const uint64_t lid : RefsOf(Arg(*set, 0)))
            {
                const Entity* x = r.Get(lid);
                if (!x || x->name != "IFCMATERIALLAYER")
                    continue;
                const auto len = r.Len(Arg(*x, 1));
                const double thickness = len ? Round(*len, 100.0) : std::nan("");
                const auto what = TextOf(Arg(*x, 5));
                told |= what.has_value();
                Layer layer;
                if (thickness == 0.0)
                    layer.function = Function::Membrane;
                else if (what == "LoadBearing")
                    layer.function = Function::Structure;
                else if (what == "Insulation")
                    layer.function = Function::Thermal;
                else if (what == "Finish")
                    layer.function = Function::Finish;
                else if (what == "Substrate")
                    layer.function = Function::Substrate;
                else if (what == "Membrane")
                    layer.function = Function::Membrane;
                else
                    layer.function = Function::Substrate;
                if (const Entity* mat = r.At(Arg(*x, 0)))
                    if (const auto name = TextOf(Arg(*mat, 0)))
                        layer.material = Clip(*name);
                layer.thickness = thickness;
                layers.push_back(std::move(layer));
            }
            double sum = 0.0;
            bool bad = layers.empty() || layers.size() > MaxLayers;
            for (const Layer& x : layers)
            {
                sum += x.thickness;
                bad |= !(x.thickness >= 0.0 && x.thickness <= MaxCoord);
            }
            if (bad || std::abs(sum - t) > 1.0)
                return {};
            // 文件没说各层做什么：最厚的承重
            if (!told)
            {
                size_t k = 0;
                for (size_t i = 1; i < layers.size(); ++i)
                    if (layers[i].thickness >= layers[k].thickness)
                        k = i;
                layers[k].function = Function::Structure;
            }
            return layers;
        }

        // styles 着的颜色：IFC2x3 里经样式指定；表面样式的，它的着色或渲染的
        // 样式着的颜色与有多透（百分数，取着色或渲染的第二个参数，1 为全透）
        struct Shade
        {
            std::array<uint8_t, 3> color;
            uint8_t                clear = 0;
        };

        std::optional<Shade> Colour(const Reader& r, std::vector<uint64_t> styles)
        {
            std::unordered_set<uint64_t> seen;
            while (!styles.empty())
            {
                const uint64_t id = styles.back();
                styles.pop_back();
                const Entity* x = r.Get(id);
                if (!x || !seen.insert(id).second)
                    continue;
                if (x->name == "IFCPRESENTATIONSTYLEASSIGNMENT")
                    for (const uint64_t s : RefsOf(Arg(*x, 0)))
                        styles.push_back(s);
                else if (x->name == "IFCSURFACESTYLE")
                    for (const uint64_t s : RefsOf(Arg(*x, 2)))
                        styles.push_back(s);
                else if (x->name == "IFCSURFACESTYLESHADING" || x->name == "IFCSURFACESTYLERENDERING")
                {
                    const Entity* c = r.At(Arg(*x, 0));
                    if (c && c->name == "IFCCOLOURRGB")
                    {
                        const auto byte = [&](size_t i) -> std::optional<uint8_t>
                        {
                            const auto v = RealOf(Arg(*c, i));
                            if (!v)
                                return std::nullopt;
                            return static_cast<uint8_t>(std::round(std::clamp(*v, 0.0, 1.0) * 255.0));
                        };
                        const auto red = byte(1), green = byte(2), blue = byte(3);
                        if (red && green && blue)
                        {
                            const auto t = RealOf(Arg(*x, 1));
                            const uint8_t clear = t && std::isfinite(*t) ? static_cast<uint8_t>(std::round(std::clamp(*t, 0.0, 1.0) * 100.0)) : 0;
                            return Shade{ { *red, *green, *blue }, clear };
                        }
                    }
                }
            }
            return std::nullopt;
        }

        // 文件给材质着的颜色与透明度，按名称：各材质的表面样式的
        std::map<std::string, Shade> MaterialColours(const Reader& r, const std::vector<uint64_t>& ids)
        {
            std::map<std::string, Shade> out;
            // 先按表面样式自己的名称（MiniBIM、BIMCraft 与 Revit 都用材质名称给它命名），再用材质的样式盖过去
            for (const uint64_t id : ids)
            {
                const Entity* x = r.Get(id);
                if (!x || x->name != "IFCSURFACESTYLE")
                    continue;
                const auto name = TextOf(Arg(*x, 0));
                const auto c = Colour(r, { id });
                if (name && c)
                    out[Clip(*name)] = *c;
            }
            for (const uint64_t id : ids)
            {
                const Entity* e = r.Get(id);
                if (!e || e->name != "IFCMATERIALDEFINITIONREPRESENTATION")
                    continue;
                const Entity* mat = r.At(Arg(*e, 3));
                const auto name = mat ? TextOf(Arg(*mat, 0)) : std::nullopt;
                if (!name)
                    continue;
                std::vector<uint64_t> styles;
                for (const uint64_t rep : RefsOf(Arg(*e, 2)))
                    if (const Entity* rp = r.Get(rep))
                        for (const uint64_t item : RefsOf(Arg(*rp, 3)))
                            if (const Entity* it = r.Get(item))
                                for (const uint64_t s : RefsOf(Arg(*it, 1)))
                                    styles.push_back(s);
                if (const auto c = Colour(r, styles))
                    out[Clip(*name)] = *c;
            }
            return out;
        }

        // 每个表示项着的表面样式的名称：构件由它做成的形体用什么材质
        std::unordered_map<uint64_t, std::string> ItemStyles(const Reader& r, const std::vector<uint64_t>& ids)
        {
            std::unordered_map<uint64_t, std::string> out;
            for (const uint64_t id : ids)
            {
                const Entity* e = r.Get(id);
                if (!e || e->name != "IFCSTYLEDITEM")
                    continue;
                const auto item = Reference(Arg(*e, 0));
                if (!item)
                    continue;
                std::vector<uint64_t> styles = RefsOf(Arg(*e, 1));
                std::unordered_set<uint64_t> seen;
                while (!styles.empty())
                {
                    const uint64_t s = styles.back();
                    styles.pop_back();
                    const Entity* x = r.Get(s);
                    if (!x || !seen.insert(s).second)
                        continue;
                    if (x->name == "IFCPRESENTATIONSTYLEASSIGNMENT")
                        for (const uint64_t y : RefsOf(Arg(*x, 0)))
                            styles.push_back(y);
                    else if (x->name == "IFCSURFACESTYLE")
                        if (const auto n = TextOf(Arg(*x, 0)))
                            out.try_emplace(*item, Clip(*n));
                }
            }
            return out;
        }

        // 文件给元素 id（或它的类型）关联的材质，按名称：一种材质的，或集、列表、层组里第一个的
        std::optional<std::string> ElementMaterial(const Reader& r, const Links& l, uint64_t id)
        {
            auto m = Find(l.material, id);
            if (!m)
                if (const auto t = Find(l.typed, id))
                    m = Find(l.material, *t);
            const Entity* at = r.Get(m);
            for (int i = 0; i < 8 && at; ++i)
            {
                const std::string& n = at->name;
                if (n == "IFCMATERIAL")
                {
                    const auto t = TextOf(Arg(*at, 0));
                    return t ? std::optional<std::string>(Clip(*t)) : std::nullopt;
                }
                if (n == "IFCMATERIALCONSTITUENTSET")
                {
                    const auto parts = RefsOf(Arg(*at, 2));
                    at = parts.empty() ? nullptr : r.Get(parts.front());
                }
                else if (n == "IFCMATERIALCONSTITUENT")
                    at = r.At(Arg(*at, 2));
                else if (n == "IFCMATERIALLIST" || n == "IFCMATERIALLAYERSET")
                {
                    const auto parts = RefsOf(Arg(*at, 0));
                    at = parts.empty() ? nullptr : r.Get(parts.front());
                }
                else if (n == "IFCMATERIALLAYERSETUSAGE" || n == "IFCMATERIALLAYER")
                    at = r.At(Arg(*at, 0));
                else
                    return std::nullopt;
            }
            return std::nullopt;
        }

        // 三角形从它最小的角点读起，绕向不变：同一个三角形读出来一样
        Tri Settled(const Tri& t)
        {
            const auto least = [](const P3& p, const P3& q) { return p <= q; };
            const auto& [a, b, c] = t;
            if (least(a, b) && least(a, c))
                return { a, b, c };
            if (least(b, c))
                return { b, c, a };
            return { c, a, b };
        }

        // 形体的形状与材质的键：形体一样，键一样
        uint64_t ShapeKey(const std::vector<Fam::Form>& forms)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            const auto eat = [&](const void* data, size_t n)
            {
                const auto* b = static_cast<const unsigned char*>(data);
                for (size_t i = 0; i < n; ++i)
                    h = (h ^ b[i]) * 0x100000001B3ull;
            };
            for (const Fam::Form& f : forms)
            {
                eat(f.material.data(), f.material.size());
                const size_t n = f.material.size();
                eat(&n, sizeof(n));
                if (f.mesh)
                    for (const Tri& t : *f.mesh)
                        eat(t.data(), sizeof(t));
            }
            return h;
        }

        // 形体沿它们的 x 轴镜像：镜像的构件这样显示
        std::vector<Fam::Form> Mirrored(const std::vector<Fam::Form>& forms)
        {
            const auto flip = [](const P3& p) { return P3{ p[0], -p[1] + 0.0, p[2] }; };
            std::vector<Fam::Form> out;
            for (const Fam::Form& f : forms)
            {
                auto mesh = std::make_shared<std::vector<Tri>>();
                if (f.mesh)
                    for (const auto& [a, b, c] : *f.mesh)
                        mesh->push_back(Settled({ flip(a), flip(c), flip(b) }));
                Fam::Form g = f;
                g.mesh = std::move(mesh);
                out.push_back(std::move(g));
            }
            return out;
        }

        // 平面多边形的三角形，绕向不变：在它的平面里切耳；超过 MaxProfile 个角点时为扇形
        std::vector<Tri> Triangles(const std::vector<P3>& face)
        {
            const P3 n = Normal(face);
            if (face.size() < 3 || V3::Dot(n, n) < 0.5)
                return {};
            std::vector<Tri> out;
            if (face.size() > Fam::MaxProfile)
            {
                for (size_t i = 1; i + 1 < face.size(); ++i)
                    out.push_back({ face[0], face[i], face[i + 1] });
                return out;
            }
            const P3 u = Unit3(V3::Cross(n, std::abs(n[0]) < 0.9 ? P3{ 1.0, 0.0, 0.0 } : P3{ 0.0, 1.0, 0.0 }));
            const P3 v = V3::Cross(n, u);
            Ring ring;
            for (const P3& p : face)
                ring.emplace_back(V3::Dot(p, u), V3::Dot(p, v));
            for (const auto& [a, b, c] : Geom::Triangulate(ring))
                if (a < face.size() && b < face.size() && c < face.size())
                    out.push_back({ face[a], face[b], face[c] });
            return out;
        }

        // map 里与 same 说的一样的类型，名为 base 或 base (2)、base (3)……；没有时以 newType 新建
        template <class T, class F> std::string KindOf(std::map<std::string, T>& map, std::string_view baseIn, F same, T newType)
        {
            const std::string base = Clip(baseIn);
            for (int n = 1;; ++n)
            {
                const std::string name = n == 1 ? base : base + " (" + std::to_string(n) + ")";
                const auto it = map.find(name);
                if (it == map.end())
                {
                    map.emplace(name, std::move(newType));
                    return name;
                }
                if (same(it->second))
                    return name;
            }
        }

        // base 或 base (2)……里 taken 还没有的那个；然后它有了
        std::string Fresh(std::set<std::string>& taken, std::string_view baseIn)
        {
            const std::string base = Clip(baseIn);
            std::string name = base;
            for (int n = 2; taken.count(name); ++n)
                name = base + " (" + std::to_string(n) + ")";
            taken.insert(name);
            return name;
        }

        // RoofFrom 读出的屋顶：沿迹线、绕开天井、减去开洞，平的或除山墙外每条边以 slope 倾斜；或者横着扫过的轮廓，高度从 z 起。
        // 它的厚度，以及 z：檐口处底面的高度，或轮廓的 0
        struct RoofRead
        {
            Ring              boundary;
            std::vector<Ring> courtyards;
            std::vector<Ring> openings;
            double            slope = 0.0;
            std::vector<size_t> gables;
            std::optional<Extrusion> extrusion;
            double            t = 0.0;
            double            z = 0.0;
        };

        bool RoofFrom(const Reader& r, const std::vector<std::vector<P3>>& facesIn, RoofRead& out, std::string& error)
        {
            const auto round = [](double x) { return Round(x, 100.0); };
            std::vector<std::vector<P3>> faces;
            for (const auto& f : facesIn)
            {
                std::vector<P3> g;
                for (const P3& p : f)
                {
                    const P2 q = r.Flat(p);
                    g.push_back({ q.x, q.y, p[2] });
                }
                faces.push_back(std::move(g));
            }
            const auto zs = SpanOf(faces, [](const P3& p) { return p[2]; });
            if (!zs)
            {
                error = "it has no body MiniBIM can read";
                return false;
            }
            const auto [z0, z1] = *zs;
            // 朝上的面，各带它的平面（网格的碎片没有）：它们的圈是它的轮廓与开洞
            std::vector<std::pair<const std::vector<P3>*, Plane>> up;
            for (const auto& f : faces)
                if (f.size() >= 3 && Normal(f)[2] > 0.01 && Area3(f) > 1.0)
                    if (const auto p = Plane::Of(f))
                        up.emplace_back(&f, *p);
            std::vector<const std::vector<P3>*> upFaces;
            for (const auto& [f, _] : up)
                upFaces.push_back(f);
            std::vector<Ring> rings;
            for (const Ring& l : Loops(upFaces))
                if (Ring c = Geom::Corners(l); c.size() >= 3)
                    rings.push_back(std::move(c));
            std::stable_sort(rings.begin(), rings.end(), [](const Ring& a, const Ring& b) { return std::abs(Geom::Area(b)) < std::abs(Geom::Area(a)); });
            // 各从西南角起，轮廓逆时针，开洞按顺序：同一个文件每次读出一样
            const auto southWest = [](Ring l)
            {
                for (P2& p : l)
                    p = Micron(p);
                size_t first = 0;
                for (size_t i = 1; i < l.size(); ++i)
                    if (l[i].y < l[first].y || (l[i].y == l[first].y && l[i].x < l[first].x))
                        first = i;
                std::rotate(l.begin(), l.begin() + static_cast<std::ptrdiff_t>(first), l.end());
                return l;
            };
            if (rings.empty())
            {
                error = "its top has no outline";
                return false;
            }
            Ring boundary = southWest(rings.front());
            if (Geom::Area(boundary) < 0.0)
            {
                std::reverse(boundary.begin(), boundary.end());
                std::rotate(boundary.rbegin(), boundary.rbegin() + 1, boundary.rend());
            }
            std::vector<Ring> openings;
            for (size_t i = 1; i < rings.size(); ++i)
                openings.push_back(southWest(rings[i]));
            const auto byFirst = [](const Ring& a, const Ring& b)
            {
                if (a.empty() || b.empty())
                    return a.empty() && !b.empty();
                return std::pair(a.front().y, a.front().x) < std::pair(b.front().y, b.front().x);
            };
            std::stable_sort(openings.begin(), openings.end(), byFirst);
            std::vector<std::pair<const std::vector<P3>*, Plane>> sloped;
            for (const auto& u : up)
                if (u.second.g.Length() > 0.02)
                    sloped.push_back(u);
            if (sloped.empty())
            {
                out = RoofRead{ boundary, {}, openings, 0.0, {}, std::nullopt, round(z1 - z0), z0 };
                return true;
            }
            const auto* mainFace = sloped.front().first;
            Plane along = sloped.front().second;
            for (const auto& s : sloped)
                if (Area3(*s.first) >= Area3(*mainFace))
                {
                    mainFace = s.first;
                    along    = s.second;
                }
            // 厚度：从最大的坡面的顶面到与它平行的底面
            const P3 n = Normal(*mainFace);
            const double top = V3::Dot(n, mainFace->front());
            double t = INFINITY;
            for (const auto& f : faces)
                if (!f.empty() && V3::Dot(Normal(f), n) < -0.999)
                {
                    const double d = std::abs(top - V3::Dot(n, f.front()));
                    if (d > 1.0)
                        t = std::fmin(t, d);
                }
            if (!std::isfinite(t))
            {
                error = "its underside isn't parallel to its top";
                return false;
            }
            t = round(t);
            const auto rise = [&](const Plane& p) { return t * std::hypot(p.g.Length(), 1.0); };
            // 四周一个坡度：每个坡面从迹线的一条边在檐口（顶面最低处）升起；中点更高的边是竖直的
            const double tan = along.g.Length();
            const double slope = round(std::atan(tan) * 180.0 / kPi);
            double lowest = INFINITY;
            for (const auto& [f, _] : up)
                for (const P3& p : *f)
                    lowest = std::fmin(lowest, p[2]);
            const auto touches = [](const std::vector<P3>& f, P2 q)
            {
                Ring ring;
                for (const P3& p : f)
                    ring.emplace_back(p[0], p[1]);
                if (Geom::Contains(ring, q))
                    return true;
                for (const auto& [a, b] : Geom::RingEdges(ring))
                    if (Geom::DistToSegment(q, a, b) < 1.0)
                        return true;
                return false;
            };
            const auto topAt = [&](P2 q)
            {
                double z = INFINITY;
                for (const auto& [f, p] : up)
                    if (touches(*f, q))
                        z = std::fmin(z, p.At(q));
                return z;
            };
            // 沿着它有檐口的洞是天井，顺时针，四周的坡面从它升起；切穿坡面的是开洞
            std::vector<Ring> courtyards, cutOpenings;
            for (Ring& h : openings)
            {
                bool eaves = false;
                for (const auto& [a, b] : Geom::RingEdges(h))
                    eaves |= topAt((a + b) * 0.5) <= lowest + 1.0;
                if (eaves)
                {
                    if (Geom::Area(h) > 0.0)
                    {
                        std::reverse(h.begin(), h.end());
                        std::rotate(h.rbegin(), h.rbegin() + 1, h.rend());
                    }
                    courtyards.push_back(std::move(h));
                }
                else
                    cutOpenings.push_back(std::move(h));
            }
            openings = std::move(cutOpenings);
            std::vector<Ring> all = { boundary };
            all.insert(all.end(), courtyards.begin(), courtyards.end());
            std::vector<Segment> edges;
            for (const Ring& ring : all)
                for (const Segment& s : Geom::RingEdges(ring))
                    edges.push_back(s);
            std::vector<size_t> gables;
            for (size_t i = 0; i < edges.size(); ++i)
                if (topAt((edges[i].first + edges[i].second) * 0.5) > lowest + 1.0)
                    gables.push_back(i);
            const auto isGable = [&](size_t i) { return std::find(gables.begin(), gables.end(), i) != gables.end(); };
            std::vector<Plane> eaves;
            for (size_t i = 0; i < edges.size(); ++i)
                if (!isGable(i))
                {
                    const auto& [a, b] = edges[i];
                    const P2 inward = (b - a).Unit().Perp();
                    eaves.push_back(Plane{ lowest - tan * a.Dot(inward), inward * tan });
                }
            const auto fits = [&](const std::pair<const std::vector<P3>*, Plane>& s)
            {
                const P2 q = s.first->empty() ? P2() : P2((*s.first)[0][0], (*s.first)[0][1]);
                for (const Plane& e : eaves)
                    if (e.g.Dist(s.second.g) < 1e-4 && std::abs(e.At(q) - s.second.At(q)) < 1.0)
                        return true;
                return false;
            };
            std::vector<double> speeds;
            for (size_t i = 0; i < edges.size(); ++i)
                speeds.push_back(isGable(i) ? 0.0 : 1.0);
            double ridge = NAN;
            {
                std::vector<std::vector<Skeleton::Face>> sk;
                std::string why;
                if (Skeleton::Faces(all, speeds, sk, why))
                {
                    ridge = lowest;
                    for (const auto& edgeFaces : sk)
                        for (const auto& face : edgeFaces)
                            for (const auto& [_, k] : face)
                                ridge = std::fmax(ridge, lowest + k * tan);
                }
            }
            bool oneSlope = std::abs(ridge - z1) < 1.0;
            for (const auto& s : sloped)
                oneSlope &= std::abs(s.second.g.Length() - tan) < 1e-4 && fits(s);
            if (oneSlope)
            {
                out = RoofRead{ boundary, courtyards, openings, slope, gables, std::nullopt, t, lowest - rise(along) };
                return true;
            }
            // 否则是横着扫过的一个轮廓：坡面都朝一个方向，沿它是底面的高度
            P2 u = along.g.Unit();
            if (!(u.x > 1e-9 || (std::abs(u.x) <= 1e-9 && u.y > 0.0)))
                u = u * -1.0;
            for (const auto& [_, p] : up)
                if (std::abs(p.g.Cross(u)) > 1e-4)
                {
                    error = "its slopes don't make a roof MiniBIM draws: one slope all round, or one profile swept across";
                    return false;
                }
            std::vector<std::tuple<double, double, Plane>> unders;
            for (const auto& [f, p] : up)
                if (const auto s = SpanOver(*f, [&](const P3& q) { return P2(q[0], q[1]).Dot(u); }))
                    unders.emplace_back(s->first, s->second, Plane{ p.z - rise(p), p.g });
            std::vector<double> cuts;
            for (const auto& [s0, s1, _] : unders)
            {
                cuts.push_back(s0);
                cuts.push_back(s1);
            }
            std::sort(cuts.begin(), cuts.end());
            {
                std::vector<double> kept;
                for (const double c : cuts)
                    if (kept.empty() || std::abs(c - kept.back()) >= 1.0)
                        kept.push_back(c);
                cuts = std::move(kept);
            }
            const auto at = [&](double s, const Plane& p) { return p.z + p.g.Dot(u) * s; };
            std::vector<P2> profile;
            for (size_t i = 0; i + 1 < cuts.size(); ++i)
            {
                const double a = cuts[i], b = cuts[i + 1];
                const Plane* p = nullptr;
                for (const auto& [s0, s1, pl] : unders)
                    if (s0 <= (a + b) / 2.0 && (a + b) / 2.0 <= s1)
                    {
                        p = &pl;
                        break;
                    }
                if (!p)
                {
                    error = "its slopes leave a gap";
                    return false;
                }
                if (!profile.empty() && std::abs(profile.back().y - at(a, *p)) > 1.0)
                {
                    error = "its underside steps up or down";
                    return false;
                }
                if (profile.empty())
                    profile.emplace_back(a, at(a, *p));
                // 与前一段在一条直线上：并成一段
                if (profile.size() >= 2)
                {
                    const P2 x = profile[profile.size() - 2], y = profile.back();
                    if (std::abs((y.y - x.y) / (y.x - x.x) - p->g.Dot(u)) < 1e-6)
                        profile.pop_back();
                }
                profile.emplace_back(b, at(b, *p));
            }
            Spread across;
            for (const auto& [f, _] : up)
                for (const P3& q : *f)
                    across.Add(P2(q[0], q[1]).Dot(u.Perp()));
            if (!across.v || profile.empty())
            {
                error = "it has no body MiniBIM can read";
                return false;
            }
            const auto [c0, c1] = *across.v;
            const P2 first = profile.front(), last = profile.back();
            if (std::abs((last.x - first.x) * (c1 - c0) - Geom::Area(boundary)) > 0.01 * Geom::Area(boundary) || !openings.empty() || !courtyards.empty())
            {
                error = "its outline isn't the rectangle its profile sweeps";
                return false;
            }
            double z = INFINITY;
            for (const P2& p : profile)
                z = std::fmin(z, p.y);
            const P2 o = u * first.x + u.Perp() * c0;
            Extrusion x;
            x.line = { Micron(o), Micron(o + u * (last.x - first.x)) };
            for (const P2& p : profile)
                x.profile.emplace_back(round(p.x - first.x), round(p.y - z));
            x.from = 0.0;
            x.to   = round(c1 - c0);
            out = RoofRead{ {}, {}, openings, 0.0, {}, x, t, z };
            return true;
        }

        // Model::Check 的报错说的是哪个元素：关于元素的报错以它的类别与 ID 开头，`door 12: …`
        std::optional<Id> Culprit(std::string_view error)
        {
            const auto words = Text::SplitWhitespace(error);
            if (words.size() < 2)
                return std::nullopt;
            std::string_view w = words[1];
            while (!w.empty() && w.back() == ':')
                w.remove_suffix(1);
            Id id = 0;
            const auto [end, ec] = std::from_chars(w.data(), w.data() + w.size(), id);
            if (w.empty() || ec != std::errc() || end != w.data() + w.size())
                return std::nullopt;
            return id;
        }

        uint64_t Fnv(std::string_view bytes)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (const char c : bytes)
                h = (h ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
            return h;
        }

        const char* const kWalls[]    = { "IFCWALL", "IFCWALLSTANDARDCASE", "IFCWALLELEMENTEDCASE" };
        const char* const kOpenings[] = { "IFCDOOR", "IFCDOORSTANDARDCASE", "IFCWINDOW", "IFCWINDOWSTANDARDCASE" };
        const char* const kSlabs[]    = { "IFCSLAB", "IFCSLABSTANDARDCASE", "IFCSLABELEMENTEDCASE" };

        template <size_t N> bool OneOf(std::string_view s, const char* const (&names)[N])
        {
            for (const char* n : names)
                if (s == n)
                    return true;
            return false;
        }
    }

    bool Import(std::string_view bytes, std::string_view name, Imported& result, std::string& error)
    {
        File file;
        if (!Step::Parse(bytes, file, error))
            return false;
        if (!Text::StartsWith(file.schema, "IFC"))
        {
            error = "this STEP file is in " + (file.schema.empty() ? std::string("no schema") : file.schema) + ", not IFC";
            return false;
        }
        double mm = 1.0, rad = 1.0;
        if (!Units(file, mm, rad, error))
            return false;
        std::vector<uint64_t> ids;
        ids.reserve(file.entities.size());
        std::unordered_map<std::string, std::vector<uint64_t>> byName;
        for (const auto& [id, e] : file.entities)
            ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        for (const uint64_t id : ids)
            byName[file.entities.at(id).name].push_back(id);
        const Links l = Links::Read(file, ids);
        // 离原点很远（场地在地图坐标上）时模型移近
        Reader probe;
        probe.f = &file;
        probe.mm = mm;
        probe.rad = rad;
        Spread xs, ys;
        for (const auto& [id, _] : l.within)
            if (const Entity* x = probe.Get(id))
            {
                const P3 o = probe.FrameOf(*x).o;
                xs.Add(o[0]);
                ys.Add(o[1]);
            }
        P2 shift;
        if (xs.v && ys.v &&
            std::max({ std::abs(xs.v->first), std::abs(xs.v->second), std::abs(ys.v->first), std::abs(ys.v->second) }) > 1e8)
            shift = P2(std::floor(xs.v->first / 1000.0) * 1000.0, std::floor(ys.v->first / 1000.0) * 1000.0);
        Reader r = probe;
        r.shift = shift;
        const auto e = [&](uint64_t id) { return r.Get(id); };
        // 元素用什么做：加起来是它的厚度时它的各层，以及它多厚
        const auto madeOf = [&](uint64_t id, double t)
        {
            std::vector<Layer> layers = LayersOf(r, l, id, t);
            double sum = 0.0;
            for (const Layer& x : layers)
                sum += x.thickness;
            return std::pair{ layers.empty() ? t : sum, layers };
        };
        // 板的层从上往下：用法从它的底面往上叠时反过来
        const auto topDown = [&](uint64_t id, double t)
        {
            auto [thickness, layers] = madeOf(id, t);
            const Entity* x = e(id);
            const bool upward = x && r.FrameOf(*x).z[2] >= 0.0;
            if (StackedUp(r, l, id) == upward)
                std::reverse(layers.begin(), layers.end());
            return std::pair{ thickness, layers };
        };
        const auto named = [&](std::initializer_list<std::string_view> names)
        {
            std::vector<uint64_t> out;
            for (const std::string_view n : names)
                if (const auto it = byName.find(std::string(n)); it != byName.end())
                    out.insert(out.end(), it->second.begin(), it->second.end());
            std::sort(out.begin(), out.end());
            return out;
        };
        const auto namedIn = [&](const std::vector<std::string>& names)
        {
            std::vector<uint64_t> out;
            for (const std::string& n : names)
                if (const auto it = byName.find(n); it != byName.end())
                    out.insert(out.end(), it->second.begin(), it->second.end());
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        };
        const auto who = [&](uint64_t id)
        {
            const Entity* x = e(id);
            if (!x)
                return "#" + std::to_string(id);
            const auto n = TextOf(Arg(*x, 2));
            return x->name + " #" + std::to_string(id) + (n ? " '" + std::string(*n) + "'" : std::string());
        };
        const auto label = [&](uint64_t id) -> std::optional<std::string>
        {
            if (const auto it = l.typeName.find(id); it != l.typeName.end())
                return it->second;
            const Entity* x = e(id);
            const auto t = x ? TextOf(Arg(*x, 4)) : std::nullopt;
            return t ? std::optional<std::string>(std::string(*t)) : std::nullopt;
        };
        const auto storeyOf = [&](uint64_t id) -> std::optional<uint64_t>
        {
            uint64_t at = id;
            for (int i = 0; i < 8; ++i)
            {
                auto next = Find(l.within, at);
                if (!next)
                    next = Find(l.whole, at);
                if (!next)
                    return std::nullopt;
                at = *next;
                const Entity* x = e(at);
                if (x && x->name == "IFCBUILDINGSTOREY")
                    return at;
            }
            return std::nullopt;
        };

        // 承重的：Pset_*Common 说 LoadBearing 为真的元素
        std::unordered_set<uint64_t> bearing;
        for (const uint64_t rid : named({ "IFCRELDEFINESBYPROPERTIES" }))
        {
            const Entity& rel = *e(rid);
            const Entity* set = r.At(Arg(rel, 5));
            if (!set || set->name != "IFCPROPERTYSET")
                continue;
            const auto setName = TextOf(Arg(*set, 2));
            if (!setName || !Text::StartsWith(*setName, "Pset_") || !Text::EndsWith(*setName, "Common"))
                continue;
            bool loaded = false;
            for (const uint64_t pid : RefsOf(Arg(*set, 4)))
                if (const Entity* p = e(pid); p && p->name == "IFCPROPERTYSINGLEVALUE" && TextOf(Arg(*p, 0)) == "LoadBearing")
                {
                    const V* v = Arg(*p, 2);
                    loaded |= v && v->kind == V::Kind::Typed && !v->list.empty() && v->list.front().kind == V::Kind::Enum && v->list.front().s == "T";
                }
            if (loaded)
                for (const uint64_t x : RefsOf(Arg(rel, 4)))
                    bearing.insert(x);
        }

        const std::vector<uint64_t> projects = named({ "IFCPROJECT" });
        const Entity* project = projects.empty() ? nullptr : e(projects.front());
        std::optional<std::string_view> title = project ? TextOf(Arg(*project, 2)) : std::nullopt;
        if (!title && project)
            title = TextOf(Arg(*project, 5));
        Model m = Model::New(Clip(title.value_or(name)));
        m.elements.Clear();
        m.nextId = FirstId;
        std::vector<std::string> skipped;
        std::unordered_set<uint64_t> seen;
        // 建出的每个元素，以及它在文件里是什么
        std::map<Id, std::string> from;
        // 文件里每个东西建出的第一个元素：它的 GlobalId 是这个元素的 GUID
        std::unordered_map<uint64_t, Id> origin;

        // 楼层成为标高，从低到高
        std::vector<std::tuple<uint64_t, std::string, double>> storeys;
        for (const uint64_t id : named({ "IFCBUILDINGSTOREY" }))
        {
            const Entity& s = *e(id);
            const double z = Reference(Arg(s, 5)) ? r.FrameOf(s).o[2] : r.Len(Arg(s, 9)).value_or(0.0);
            auto n = TextOf(Arg(s, 2));
            if (!n)
                n = TextOf(Arg(s, 7));
            storeys.emplace_back(id, std::string(n.value_or("Level")), Round(z, 1000.0));
        }
        std::stable_sort(storeys.begin(), storeys.end(), [](const auto& a, const auto& b) { return std::get<2>(a) < std::get<2>(b); });
        if (storeys.empty())
            storeys.emplace_back(0, "Level 1", 0.0);
        std::set<std::string> taken;
        std::unordered_map<uint64_t, Id> levelOf;
        for (const auto& [sid, n, z] : storeys)
        {
            Level lv;
            lv.name      = Fresh(taken, n);
            lv.elevation = z;
            const Id level = m.Insert(std::move(lv));
            levelOf[sid] = level;
            origin.try_emplace(sid, level);
            seen.insert(sid);
        }
        std::vector<std::pair<Id, double>> levels;
        for (const auto& [id, lv] : m.Levels())
            levels.emplace_back(id, lv->elevation);
        // 建出的东西先用 ID 指标高、墙，模型建好后换成 GUID（Model::Resolve）
        const auto elevation = [&](Guid level)
        {
            for (const auto& [id, z] : levels)
                if (Guid::OfId(id) == level)
                    return z;
            return 0.0;
        };
        // 元素的标高：它的楼层的，否则在 z 处或以下最高的
        const auto levelFor = [&](uint64_t id, double z)
        {
            if (const auto s = storeyOf(id))
                if (const auto it = levelOf.find(*s); it != levelOf.end())
                    return Guid::OfId(it->second);
            for (auto it = levels.rbegin(); it != levels.rend(); ++it)
                if (it->second <= z + 1.0)
                    return Guid::OfId(it->first);
            return Guid::OfId(levels.empty() ? 0 : levels.front().first);
        };
        const auto levelNear = [&](double z) -> std::optional<Guid>
        {
            for (const auto& [id, lz] : levels)
                if (std::abs(lz - z) <= 1.0)
                    return Guid::OfId(id);
            return std::nullopt;
        };
        // 到 top 处的一个标高（在 base 之上），否则高 top - bottom
        const auto topped = [&](Guid base, double bottom, double top) -> std::pair<std::optional<Guid>, double>
        {
            for (const auto& [id, lz] : levels)
                if (Guid::OfId(id) != base && lz > elevation(base) + Tol && std::abs(lz - top) <= 1.0)
                    return { Guid::OfId(id), top - bottom };
            return { std::nullopt, top - bottom };
        };
        const auto leave = [&](uint64_t id, const std::string& why)
        {
            skipped.push_back(who(id) + ": " + why);
            seen.insert(id);
        };
        const auto made = [&](Id element, uint64_t ifc)
        {
            from[element] = who(ifc);
            origin.try_emplace(ifc, element);
        };

        // 墙：各面沿着的线，以及横过它多厚
        std::vector<RawWall> raw;
        for (const uint64_t id : named({ kWalls[0], kWalls[1], kWalls[2] }))
        {
            const Entity& w = *e(id);
            const auto faces = r.Body(w);
            std::vector<P3> pts;
            for (const auto& f : faces)
                pts.insert(pts.end(), f.begin(), f.end());
            const auto axis = r.AxisOf(w);
            const Frame f = r.FrameOf(w);
            // MiniBIM 的墙层从左边（外侧）列起：墙这样走，让文件的第一层在那里。文件的用法沿它自己的 y 叠层，或从另一侧往上叠
            const auto faced = [&](P2 a, P2 b)
            {
                const double side = P2(f.y[0], f.y[1]).Dot((b - a).Perp());
                const auto up = StackedUp(r, l, id);
                if (up && (*up ? -side : side) < 0.0)
                    return std::pair{ b, a };
                return std::pair{ a, b };
            };
            const auto wallType = [&](double t)
            {
                const std::string base = label(id).value_or("Imported - " + Num(t) + "mm");
                auto [thickness, layers] = madeOf(id, t);
                WallType nt{ thickness, layers };
                const double th = thickness;
                const std::vector<Layer> ls = layers;
                return std::pair{ thickness, KindOf(m.types.wall, base, [&](const WallType& k) { return std::abs(k.thickness - th) < 0.05 && k.layers == ls; },
                                                      std::move(nt)) };
            };
            // 弯的定位线：过它两端与中间的圆弧；墙的线是它的实体绕圆心的中间，厚度是实体的伸展
            std::vector<P2> bent;
            if (axis)
                for (const P3& x : *axis)
                    bent.push_back(r.Flat(x));
            bool curved = false;
            if (bent.size() >= 2)
                for (const P2& x : bent)
                    curved |= Geom::DistToSegment(x, bent.front(), bent.back()) > 1.0;
            if (curved)
            {
                const P2 mid = bent[bent.size() / 2];
                const auto [p, q] = faced(bent.front(), bent.back());
                const double arc = Walls::SweepThrough(p, mid, q);
                const auto c = Walls::Axis(p, q, arc).Centre();
                std::optional<std::pair<double, double>> radii;
                if (c)
                {
                    std::vector<double> ds;
                    for (const P3& x : pts)
                        ds.push_back(r.Flat(x).Dist(*c));
                    radii = ArcFaces(ds);
                }
                const auto zs = SpanOver(pts, [](const P3& x) { return x[2]; });
                if (!radii || !zs)
                {
                    leave(id, "it has no body MiniBIM can read");
                    continue;
                }
                const double r0 = radii->first, r1 = radii->second;
                const auto [t, typeName] = wallType(Round(r1 - r0, 100.0));
                const auto on = [&](P2 x) { return Micron(*c + (x - *c).Unit() * ((r0 + r1) / 2.0)); };
                RawWall rw;
                rw.ifc = id;
                rw.start = on(p);
                rw.end = on(q);
                rw.arc = Round(arc, 1e6);
                rw.t = t;
                rw.z = *zs;
                rw.peak = r.Flat(Highest(pts));
                rw.level = levelFor(id, zs->first);
                rw.typeName = typeName;
                raw.push_back(std::move(rw));
                continue;
            }
            P2 a, b;
            if (axis && axis->size() >= 2 && r.Flat(axis->front()).Dist(r.Flat(axis->back())) > Tol)
            {
                a = r.Flat(axis->front());
                b = r.Flat(axis->back());
            }
            else
            {
                // 没有定位线：沿墙自己的 x，到它的实体为止
                const P2 o = r.Flat(f.o);
                const P2 x = P2(f.x[0], f.x[1]).Unit();
                const auto s = SpanOver(pts, [&](const P3& p) { return (r.Flat(p) - o).Dot(x); });
                if (s && x.Length() > 0.5)
                {
                    a = o + x * s->first;
                    b = o + x * s->second;
                }
                else
                    a = b = o;
            }
            std::tie(a, b) = faced(a, b);
            const P2 u = (b - a).Unit(), n = u.Perp();
            const auto cs = SpanOver(pts, [&](const P3& p) { return (r.Flat(p) - a).Dot(n); });
            const auto zs = SpanOver(pts, [](const P3& p) { return p[2]; });
            if (!cs || !zs)
            {
                leave(id, "it has no body MiniBIM can read");
                continue;
            }
            const auto [c0, c1] = *cs;
            if (a.Dist(b) <= Tol || c1 - c0 <= 0.0 || u.Length() < 0.5)
            {
                leave(id, "it has no length or thickness MiniBIM can read");
                continue;
            }
            const P2 mid = n * ((c0 + c1) / 2.0);
            const auto [t, typeName] = wallType(Round(c1 - c0, 100.0));
            RawWall rw;
            rw.ifc = id;
            rw.start = Micron(a + mid);
            rw.end = Micron(b + mid);
            rw.t = t;
            rw.z = *zs;
            rw.peak = r.Flat(Highest(pts));
            rw.level = levelFor(id, zs->first);
            rw.typeName = typeName;
            raw.push_back(std::move(rw));
        }
        Meet(raw);
        // 建出的每面墙：ID、定位线、底部高度；以及它最高的地方
        std::unordered_map<uint64_t, std::tuple<Id, P2, P2, double>> walls;
        std::vector<std::tuple<Id, P2, double>> peaks;
        for (RawWall& w : raw)
        {
            const auto [topLevel, height] = topped(w.level, w.z.first, w.z.second);
            Wall wall;
            wall.typeName   = w.typeName;
            wall.level      = w.level;
            wall.start      = w.start;
            wall.end        = w.end;
            wall.topLevel   = topLevel;
            wall.height     = Round(height, 1000.0);
            wall.baseOffset = Round(w.z.first - elevation(w.level), 1000.0);
            wall.arc        = w.arc;
            const Id id = m.Insert(std::move(wall));
            made(id, w.ifc);
            walls[w.ifc] = { id, w.start, w.end, w.z.first };
            peaks.emplace_back(id, w.peak, w.z.second);
            seen.insert(w.ifc);
        }

        // 穿过楼板、天花板、屋顶的洞口元素：开在什么上、在它顶上的平面轮廓。MiniBIM 还没有管井：洞在实体里的读作宿主自己的洞，
        // 不在的列入略去
        std::vector<std::tuple<uint64_t, uint64_t, Ring>> cuts;
        for (const uint64_t id : named({ "IFCOPENINGELEMENT", "IFCOPENINGSTANDARDCASE" }))
        {
            const auto h = Find(l.host, id);
            const Entity* host = h ? e(*h) : nullptr;
            if (!host || !(OneOf(host->name, kSlabs) || host->name == "IFCCOVERING" || host->name == "IFCROOF"))
                continue;
            const auto faces = r.Body(*e(id));
            const auto zs = SpanOf(faces, [](const P3& p) { return p[2]; });
            if (!zs)
                continue;
            const auto shape = Outline(faces, Plane::Flat(zs->second), true);
            if (!shape)
                continue;
            Ring ring;
            for (const P2& p : shape->outer)
                ring.push_back(Micron(r.Flat({ p.x, p.y, 0.0 })));
            cuts.emplace_back(id, *h, std::move(ring));
        }
        // 每个楼板、天花板、屋顶建出时有的洞（开洞与天井），按文件里的元素：对照穿过它们的洞口元素
        std::unordered_map<uint64_t, std::vector<Ring>> holesOf;
        // 楼板或天花板 id 顶面的洞，在平面上，各从西南角起，按顺序：同一个文件每次读出一样
        const auto openingsOf = [&](uint64_t id, const std::vector<Ring>& holes)
        {
            std::vector<Ring> openings;
            for (const Ring& h : holes)
            {
                Ring flat;
                for (const P2& p : h)
                    flat.push_back(Micron(r.Flat({ p.x, p.y, 0.0 })));
                openings.push_back(Tidy(flat));
            }
            std::stable_sort(openings.begin(), openings.end(), [](const Ring& a, const Ring& b)
            {
                if (a.empty() || b.empty())
                    return a.empty() && !b.empty();
                return std::pair(a.front().y, a.front().x) < std::pair(b.front().y, b.front().x);
            });
            holesOf[id] = openings;
            return openings;
        };

        // 楼板：顶面的轮廓与其中的洞，以及它在下面多厚
        const auto inWhole = [&](uint64_t id, std::initializer_list<std::string_view> names)
        {
            const auto w = Find(l.whole, id);
            const Entity* x = w ? e(*w) : nullptr;
            return x && std::find(names.begin(), names.end(), x->name) != names.end();
        };
        std::vector<uint64_t> roofs;
        for (const uint64_t id : named({ "IFCROOF" }))
            if (!l.whole.count(id))
                roofs.push_back(id);
        for (const uint64_t id : named({ kSlabs[0], kSlabs[1], kSlabs[2] }))
        {
            const Entity& s = *e(id);
            if (inWhole(id, { "IFCROOF", "IFCSTAIR", "IFCRAMP" }))
                continue;
            // 楼梯的平台板归楼梯（上面已跳过）；不属于楼梯的平台板与别的板一样，读成楼板
            if (Enumeration(Arg(s, 8)) == "ROOF")
            {
                roofs.push_back(id);
                continue;
            }
            const auto faces = r.Body(s);
            const auto zs = SpanOf(faces, [](const P3& p) { return p[2]; });
            if (!zs)
            {
                leave(id, "it has no body MiniBIM can read");
                continue;
            }
            const auto [z0, z1] = *zs;
            // 在最高处是平的，或者是斜的：坡度箭头从最低的角点沿坡向上，一直到对边
            std::optional<std::tuple<Plane, Geom::Region, double>> shape;
            if (auto flat = Outline(faces, Plane::Flat(z1), true))
                shape = std::tuple{ Plane::Flat(z1), *flat, z1 - z0 };
            else
                shape = TopOf(faces);
            if (!shape)
            {
                leave(id, "its top isn't flat");
                continue;
            }
            const auto& [top, region, riseAll] = *shape;
            const double tan = top.g.Length();
            const auto flat = [&](P2 p) { return Micron(r.Flat({ p.x, p.y, 0.0 })); };
            P2 tail = region.outer.empty() ? P2() : region.outer.front();
            for (const P2& q : region.outer)
                if (top.At(q) < top.At(tail))
                    tail = q;
            std::optional<std::array<P2, 2>> arrow;
            if (tan > 1e-9)
            {
                const P2 u = top.g * (1.0 / tan);
                double reach = 0.0;
                for (const P2& q : region.outer)
                    reach = std::fmax(reach, (q - tail).Dot(u));
                arrow = std::array<P2, 2>{ flat(tail), flat(tail + u * reach) };
            }
            const double t0 = Round(riseAll * std::cos(std::atan(tan)), 100.0);
            const std::string base = label(id).value_or("Imported - " + Num(t0) + "mm");
            const auto made2 = topDown(id, t0);
            const double t = made2.first;
            const std::vector<Layer>& layers = made2.second;
            const std::string typeName =
                KindOf(m.types.floor, base, [&](const FloorType& k) { return std::abs(k.thickness - t) < 0.05 && k.layers == layers; }, FloorType{ t, layers });
            const double z = top.At(tail);
            const Guid level = levelNear(z).value_or(levelFor(id, z));
            Floor floor;
            floor.typeName = typeName;
            floor.level    = level;
            for (const P2& p : region.outer)
                floor.boundary.push_back(flat(p));
            floor.openings   = openingsOf(id, region.holes);
            floor.offset     = Round(z - elevation(level), 1000.0) + 0.0;
            floor.slope      = arrow ? Round(std::atan(tan) * 180.0 / kPi, 1e6) : 0.0;
            floor.arrow      = arrow;
            floor.structural = bearing.count(id) != 0;
            const Id fid = m.Insert(std::move(floor));
            made(fid, id);
            seen.insert(id);
        }

        // 天花板：是天花板的覆盖层，轮廓与其中的洞，底面的高度与厚度
        for (const uint64_t id : named({ "IFCCOVERING" }))
        {
            const Entity& c = *e(id);
            if (Enumeration(Arg(c, 8)) != "CEILING")
                continue;
            const auto faces = r.Body(c);
            const auto zs = SpanOf(faces, [](const P3& p) { return p[2]; });
            if (!zs)
            {
                leave(id, "it has no body MiniBIM can read");
                continue;
            }
            const auto [z0, z1] = *zs;
            const auto region = Outline(faces, Plane::Flat(z1), true);
            if (!region)
            {
                leave(id, "its top isn't level");
                continue;
            }
            const double t0 = Round(z1 - z0, 100.0);
            const std::string base = label(id).value_or("Imported - " + Num(t0) + "mm");
            const auto made2 = topDown(id, t0);
            const double t = made2.first;
            const std::vector<Layer>& layers = made2.second;
            const std::string typeName = KindOf(
                m.types.ceiling, base, [&](const CeilingType& k) { return std::abs(k.thickness - t) < 0.05 && k.layers == layers; }, CeilingType{ t, 0.0, layers });
            const Guid level = levelFor(id, z0);
            Ceiling ceiling;
            ceiling.typeName = typeName;
            ceiling.level    = level;
            for (const P2& p : region->outer)
                ceiling.boundary.push_back(Micron(r.Flat({ p.x, p.y, 0.0 })));
            ceiling.openings = openingsOf(id, region->holes);
            ceiling.offset   = Round(z0 - elevation(level), 1000.0) + 0.0;
            const Id cid = m.Insert(std::move(ceiling));
            made(cid, id);
            seen.insert(id);
        }

        // 柱：横过它的截面与它立多高；承重的是结构柱（BIMCraft 还把不是条形的基础读成独立基础，基础随它移植）
        for (const uint64_t id : named({ "IFCCOLUMN", "IFCCOLUMNSTANDARDCASE" }))
        {
            const Entity& c = *e(id);
            const auto faces = r.Body(c);
            const auto zs = SpanOf(faces, [](const P3& p) { return p[2]; });
            if (!zs)
            {
                leave(id, "it has no body MiniBIM can read");
                continue;
            }
            const auto [z0, z1] = *zs;
            // 与它最长的轮廓边垂直：立着的面里一条水平的边（三角形拼的顶面还有对角线）
            const Frame f = r.FrameOf(c);
            std::pair<double, P2> along{ 0.0, P2(f.x[0], f.x[1]).Unit() };
            for (const auto& face : faces)
            {
                if (face.size() < 2 || std::abs(Normal(face)[2]) >= 0.1)
                    continue;
                for (size_t i = 0; i < face.size(); ++i)
                {
                    const P3& p = face[i];
                    const P3& q = face[(i + 1) % face.size()];
                    const P2 d = r.Flat(q) - r.Flat(p);
                    if (std::abs(p[2] - q[2]) < 0.5 && d.Length() > along.first + 1e-6)
                        along = { d.Length(), d.Unit() };
                }
            }
            const P2 u = along.second, v = u.Perp();
            std::vector<P2> plan;
            for (const auto& face : faces)
                for (const P3& p : face)
                    plan.push_back(r.Flat(p));
            const auto su = SpanOver(plan, [&](const P2& p) { return p.Dot(u); });
            const auto sv = SpanOver(plan, [&](const P2& p) { return p.Dot(v); });
            if (!su || !sv)
                continue;
            const P2 at = u * ((su->first + su->second) / 2.0) + v * ((sv->first + sv->second) / 2.0);
            const auto radii = SpanOver(plan, [&](const P2& p) { return p.Dist(at); });
            // 圆的：角点多，离中心都一样远（正方形的四个角也一样远）
            std::set<std::pair<int64_t, int64_t>> corners;
            for (const P2& p : plan)
                corners.emplace(static_cast<int64_t>(std::round(p.x * 2.0)), static_cast<int64_t>(std::round(p.y * 2.0)));
            const bool round = corners.size() >= 12 && radii && radii->second > 0.0 && (radii->second - radii->first) / radii->second < 0.03;
            const auto size = [](double x) { return std::round(x * 100.0) / 100.0; };
            const double w = round ? size(2.0 * radii->second) : size(su->second - su->first);
            const double d = round ? w : size(sv->second - sv->first);
            if (!(w > 0.0) || !(d > 0.0))
            {
                leave(id, "it has no section MiniBIM can read");
                continue;
            }
            const std::string base = label(id).value_or(round ? "Imported Round " + Num(w) : "Imported " + Num(w) + " x " + Num(d));
            const bool structural = bearing.count(id) != 0;
            auto& types = structural ? m.types.structuralColumn : m.types.column;
            Column col;
            col.typeName = KindOf(
                types, base, [&](const ColumnType& k) { return k.round == round && std::abs(k.width - w) < 0.05 && std::abs(k.depth - d) < 0.05; },
                ColumnType{ w, d, round });
            col.level = levelFor(id, z0);
            std::tie(col.topLevel, col.height) = topped(col.level, z0, z1);
            col.height     = Round(col.height, 1000.0);
            col.angle      = round ? 0.0 : Round(std::atan2(u.y, u.x) * 180.0 / kPi, 1e6) + 0.0;
            col.at         = Micron(at);
            col.baseOffset = Round(z0 - elevation(col.level), 1000.0) + 0.0;
            const Id cid = structural ? m.Insert(StructuralColumn{ std::move(col) }) : m.Insert(std::move(col));
            made(cid, id);
            seen.insert(id);
        }

        // 屋顶：平的，或矩形上的坡屋顶，按它朝上的面量
        for (const uint64_t id : roofs)
        {
            const Entity* roof = e(id);
            if (!roof)
                continue;
            auto faces = r.Body(*roof);
            if (const auto it = l.parts.find(id); it != l.parts.end())
                for (const uint64_t part : it->second)
                    if (const Entity* p = e(part))
                    {
                        auto more = r.Body(*p);
                        faces.insert(faces.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
                        seen.insert(part);
                    }
            seen.insert(id);
            RoofRead rd;
            std::string why;
            if (!RoofFrom(r, faces, rd, why))
            {
                leave(id, why);
                continue;
            }
            const std::string base = label(id).value_or("Imported - " + Num(rd.t) + "mm");
            const auto made2 = topDown(id, rd.t);
            const double t = made2.first;
            const std::vector<Layer>& layers = made2.second;
            const std::string typeName =
                KindOf(m.types.roof, base, [&](const RoofType& k) { return std::abs(k.thickness - t) < 0.05 && k.layers == layers; }, RoofType{ t, layers });
            const bool pitched = rd.slope > 0.0;
            const Guid level = pitched ? levelFor(id, rd.z) : levelNear(rd.z).value_or(levelFor(id, rd.z));
            // 坡屋顶从檐口往里 overhang 处到它的标高，或者檐口在它上面 offset；别的放在它上面
            const double below = elevation(level) - rd.z;
            const double overhang = pitched && below > 0.0 ? below / std::tan(rd.slope * kPi / 180.0) : 0.0;
            const double offset = pitched && below > 0.0 ? 0.0 : -below;
            Roof rf;
            rf.typeName   = typeName;
            rf.level      = level;
            rf.boundary   = rd.boundary;
            rf.courtyards = rd.courtyards;
            rf.slope      = rd.slope;
            rf.overhang   = Round(overhang, 1000.0);
            rf.gables     = rd.gables;
            rf.offset     = Round(offset, 100.0) + 0.0;
            rf.openings   = rd.openings;
            rf.extrusion  = rd.extrusion;
            std::vector<Slope> slopes;
            if (!m.RoofFaces(rf, slopes, why))
            {
                leave(id, why);
                continue;
            }
            std::vector<Ring> holes = rf.openings;
            holes.insert(holes.end(), rf.courtyards.begin(), rf.courtyards.end());
            holesOf[id] = holes;
            const Id rid = m.Insert(std::move(rf));
            made(rid, id);
        }

        // 顶部随屋顶走的墙（Revit 写贴着屋顶的墙时把它切到屋顶）：它的最高点在屋顶下面，且墙上方某处屋顶比它低。
        // 穿过屋顶的比屋顶高；只沿一条边碰到屋顶的不高出它
        std::vector<Id> roofIds;
        for (const auto& [id, x] : m.elements.Items())
            if (x->As<Roof>())
                roofIds.push_back(id);
        for (const auto& peak : peaks)
        {
            const Id wid = std::get<0>(peak);
            const P2 q = std::get<1>(peak);
            const double top = std::get<2>(peak);
            const Wall* w = m.GetWall(wid);
            if (!w)
                continue;
            const Walls::Axis axis = Walls::Axis::Of(*w);
            std::vector<P2> along;
            for (int i = 0; i <= 8; ++i)
                along.push_back(axis.At(axis.Length() * i / 8.0, 0.0));
            const auto under = [&](Id rid)
            {
                const auto atPeak = Walls::Underside(m, rid, q);
                if (!atPeak || std::abs(*atPeak - top) > 5.0)
                    return false;
                for (const P2& p : along)
                    if (const auto z = Walls::Underside(m, rid, p); z && *z < top - 5.0)
                        return true;
                return false;
            };
            for (const Id rid : roofIds)
                if (under(rid))
                {
                    if (Element* x = m.elements.GetMut(wid))
                        if (Wall* ww = x->As<Wall>())
                            ww->attachTop = Guid::OfId(rid);
                    break;
                }
        }

        // 穿过楼板、天花板、屋顶的洞口元素，切出的洞不在宿主的实体里的：MiniBIM 还没有管井，列入略去
        for (const auto& cut : cuts)
        {
            const uint64_t id = std::get<0>(cut), h = std::get<1>(cut);
            const Ring& ring = std::get<2>(cut);
            uint64_t host = h;
            if (!holesOf.count(host))
                if (const auto w = Find(l.whole, h); w && holesOf.count(*w))
                    host = *w;
            const auto it = holesOf.find(host);
            if (it == holesOf.end())
                continue;
            if (std::none_of(it->second.begin(), it->second.end(), [&](const Ring& hole) { return Same(hole, ring); }))
                skipped.push_back(who(id) + ": MiniBIM has no shafts yet, so the hole it cuts through " + who(host) + " is left out");
        }

        // 家具、卫生器具、灯具与代理：由它们的实体做成族的构件，各为站立在自己坐标里的网格，面按用什么做分组。
        // 文件给的每个名称一个族（"族:类型"，Revit 与 MiniBIM 都这样命名），每个类型一个类型；文件里的族优先于库里同名的
        const auto styles = ItemStyles(r, ids);
        struct MadeFamily
        {
            Fam::Category category = Fam::Category::Generic;
            std::vector<std::tuple<std::string, std::vector<Fam::Form>, uint64_t>> types;
            std::unordered_map<std::string, std::vector<size_t>> named;
            std::unordered_set<std::string> names;
            size_t forms = 0;
        };
        std::map<std::string, MadeFamily> families;
        struct Placed
        {
            uint64_t    id;
            std::string family;
            std::string typeName;
            bool        mirrored;
            P2          at;
            double      angle;
            double      z;
        };
        std::vector<Placed> placed;
        const std::vector<uint64_t> readable = namedIn(Fam::CategoryOf::ReadClasses());
        for (const Fam::Category category : Fam::CategoryOf::Placed)
            for (const uint64_t id : readable)
            {
                const Entity* x = e(id);
                if (!x || seen.count(id))
                    continue;
                // 它的类与类型说它是哪种族的；别的种类的轮到它时再读
                if (Fam::CategoryOf::FromIfc(x->name, Enumeration(Arg(*x, 8)), TextOf(Arg(*x, 4))) != category)
                    continue;
                const Frame f = r.FrameOf(*x);
                const auto flat = [](const P3& v) { return Unit3(P3{ v[0], v[1], 0.0 }); };
                P3 along{ 1.0, 0.0, 0.0 };
                for (const P3& v : { flat(f.x), flat(f.y), P3{ 1.0, 0.0, 0.0 } })
                    if (V3::Dot(v, v) > 0.5)
                    {
                        along = v;
                        break;
                    }
                const P3 side = V3::Cross(P3{ 0.0, 0.0, 1.0 }, along);
                const auto local = [&](const P3& p)
                {
                    const P3 d = V3::Sub(f.At(p), f.o);
                    const auto k = [](double v) { return Round(v, 1e3) + 0.0; };
                    return P3{ k(V3::Dot(d, along)), k(V3::Dot(d, side)), k(d[2]) };
                };
                const std::string fallback = ElementMaterial(r, l, id).value_or("");
                std::map<std::string, std::vector<Tri>> groups;
                for (auto& [item, faces] : r.BodyItems(*x))
                {
                    const auto st = styles.find(item);
                    auto& tris = groups[st == styles.end() ? fallback : st->second];
                    for (const auto& face : faces)
                    {
                        std::vector<P3> lf;
                        for (const P3& p : face)
                            lf.push_back(local(p));
                        for (const Tri& t : Triangles(lf))
                            tris.push_back(Settled(t));
                    }
                }
                size_t count = 0;
                for (auto it = groups.begin(); it != groups.end();)
                {
                    count += it->second.size();
                    it = it->second.empty() ? groups.erase(it) : std::next(it);
                }
                if (groups.empty())
                {
                    leave(id, "it has no body MiniBIM can read");
                    continue;
                }
                if (count > Fam::MaxTriangles)
                {
                    leave(id, "its body has more than " + std::to_string(Fam::MaxTriangles) + " triangles");
                    continue;
                }
                std::vector<Fam::Form> forms;
                for (auto& [material, mesh] : groups)
                {
                    Fam::Form form;
                    form.mesh     = std::make_shared<const std::vector<Tri>>(std::move(mesh));
                    form.material = material;
                    forms.push_back(std::move(form));
                }
                // 对象类型说明它是什么的产品（接闪器），族与类型在它的名称里
                const bool marked = Fam::CategoryOf::Ifc(category).objectType != nullptr;
                std::optional<std::string> nm = marked ? std::nullopt : label(id);
                if (!nm)
                    if (const auto t = TextOf(Arg(*x, 2)))
                        nm = std::string(*t);
                const std::string full = nm.value_or(x->name);
                std::vector<std::string_view> parts;
                {
                    std::string_view rest = full;
                    for (int i = 0; i < 2; ++i)
                    {
                        const size_t k = rest.find(':');
                        if (k == std::string_view::npos)
                            break;
                        parts.push_back(Text::Trim(rest.substr(0, k)));
                        rest = rest.substr(k + 1);
                    }
                    parts.push_back(Text::Trim(rest));
                }
                const std::string family = Clip(parts[0]);
                const std::string base   = parts.size() > 1 ? Clip(parts[1]) : family;
                MadeFamily& fam = families[family];
                if (fam.types.empty() && fam.forms == 0)
                    fam.category = category;
                // 名称与形状都相同的类型，或它的镜像，按形状的键找；文件给了别的形状的是另一个类型，只要族还放得下它的形体
                const std::vector<Fam::Form> flipped = Mirrored(forms);
                const uint64_t key = ShapeKey(forms), flippedKey = ShapeKey(flipped);
                std::optional<std::pair<std::string, bool>> pick;
                if (const auto it = fam.named.find(base); it != fam.named.end())
                    for (const size_t i : it->second)
                    {
                        const auto& [t, fs, k] = fam.types[i];
                        if (k == key && fs == forms)
                            pick = std::pair{ t, false };
                        else if (k == flippedKey && fs == flipped)
                            pick = std::pair{ t, true };
                        if (pick)
                            break;
                    }
                if (!pick)
                {
                    if (fam.forms + forms.size() > Fam::MaxForms)
                    {
                        leave(id, "family `" + family + "` has more shapes than one family holds (" + std::to_string(Fam::MaxForms) + " forms)");
                        continue;
                    }
                    const size_t already = fam.named.count(base) ? fam.named[base].size() : 0;
                    std::string t = base;
                    for (size_t n = already + 1;; ++n)
                    {
                        t = n == 1 ? base : base + " (" + std::to_string(n) + ")";
                        if (!fam.names.count(t))
                            break;
                    }
                    fam.forms += forms.size();
                    fam.names.insert(t);
                    fam.named[base].push_back(fam.types.size());
                    fam.types.emplace_back(t, std::move(forms), key);
                    pick = std::pair{ t, false };
                }
                const double angle = Round(std::atan2(along[1], along[0]) * 180.0 / kPi, 1e6) + 0.0;
                placed.push_back(Placed{ id, family, pick->first, pick->second, r.Flat(f.o), angle, f.o[2] });
            }
        std::map<std::string, Family> builtFamilies;
        for (auto& [fname, mf] : families)
        {
            // 几个类型：各有自己的形体，由只有它的类型设定的参数显示
            const bool several = mf.types.size() > 1;
            Family family;
            family.category = mf.category;
            if (several)
            {
                Fam::Parameter p;
                p.name = "Variant";
                p.kind = Fam::Kind::Number;
                family.parameters.push_back(std::move(p));
            }
            int k = 1;
            for (auto& [t, forms, _] : mf.types)
            {
                std::map<std::string, Json> set;
                if (several)
                    set["Variant"] = k;
                family.types[t] = std::move(set);
                for (Fam::Form& form : forms)
                {
                    if (several)
                        form.visible = Fam::Expr(std::string("Variant = ") + std::to_string(k));
                    family.forms.push_back(std::move(form));
                }
                ++k;
            }
            builtFamilies[fname] = std::move(family);
        }
        // 不健全的族，它的构件连同原因略去：每个族只查一次
        std::map<std::string, std::string> unsound;
        for (const auto& [fname, f] : builtFamilies)
        {
            std::string why;
            if (!f.Check([](const std::string&) { return true; }, why))
                unsound[fname] = why;
        }
        for (const Placed& p : placed)
        {
            if (const auto it = unsound.find(p.family); it != unsound.end())
            {
                leave(p.id, it->second);
                continue;
            }
            const Guid level = levelFor(p.id, p.z);
            Component c;
            c.family   = p.family;
            c.typeName = p.typeName;
            c.level    = level;
            c.at       = Micron(p.at);
            c.angle    = p.angle;
            c.offset   = Round(p.z - elevation(level), 1000.0) + 0.0;
            c.mirrored = p.mirrored;
            const Id cid = m.Insert(std::move(c));
            made(cid, p.id);
            seen.insert(p.id);
        }
        for (auto& [fname, f] : builtFamilies)
            if (!unsound.count(fname))
                m.families[fname] = std::move(f);

        // 房间分隔线：MiniBIM 与 BIMCraft 写的虚拟元素，它的定位线每段一条；全部加起来最多 MaxPoints 段，
        // 几百万个点的定位线会成几百万个元素
        size_t separations = 0;
        for (const uint64_t id : named({ "IFCVIRTUALELEMENT" }))
        {
            const Entity& x = *e(id);
            if (TextOf(Arg(x, 4)) != Separation)
                continue;
            seen.insert(id);
            const auto axis = r.AxisOf(x);
            if (!axis || axis->size() < 2)
            {
                leave(id, "its room separation line has no axis");
                continue;
            }
            if (separations + axis->size() - 1 > MaxPoints)
            {
                leave(id, "its room separation line would pass the " + std::to_string(MaxPoints) + " segments the lines of a project may have");
                continue;
            }
            separations += axis->size() - 1;
            const Guid level = levelFor(id, axis->front()[2]);
            for (size_t i = 0; i + 1 < axis->size(); ++i)
            {
                const P2 start = Micron(r.Flat((*axis)[i])), end = Micron(r.Flat((*axis)[i + 1]));
                if (start.Dist(end) <= Tol)
                    continue;
                const Id sid = m.Insert(RoomSeparator{ level, start, end });
                made(sid, id);
            }
        }

        // 门窗的编号：它们的标记，除非标记只是元素的 ID（Revit 把门叫 族:类型:123456、标记 123456），或别的门窗先用了。
        // 其余的接着标记所在的序列往下编
        std::set<std::string> marks[2];
        std::unordered_map<uint64_t, std::string> tags;
        const auto openingIds = named({ kOpenings[0], kOpenings[1], kOpenings[2], kOpenings[3] });
        for (const uint64_t id : openingIds)
        {
            const Entity& o = *e(id);
            const auto tag = TextOf(Arg(o, 7));
            if (!tag)
                continue;
            bool idOnly = false;
            if (const auto n = TextOf(Arg(o, 2)))
                if (const size_t k = n->rfind(':'); k != std::string_view::npos)
                    idOnly = Text::Trim(n->substr(k + 1)) == *tag;
            const bool door = Text::StartsWith(o.name, "IFCDOOR");
            if (!idOnly && marks[door ? 1 : 0].insert(std::string(*tag)).second)
                tags[id] = std::string(*tag);
        }

        // 门窗：它们的洞口切开墙的地方
        for (const uint64_t id : openingIds)
        {
            const Entity& o = *e(id);
            const bool isDoor = Text::StartsWith(o.name, "IFCDOOR");
            const auto opening = Find(l.fills, id);
            const auto host = opening ? Find(l.host, *opening) : std::nullopt;
            const auto wall = host ? walls.find(*host) : walls.end();
            if (wall == walls.end())
            {
                leave(id, opening ? "its wall was left out" : "it isn't in a wall");
                continue;
            }
            const Id hostId = std::get<0>(wall->second);
            const P2 a = std::get<1>(wall->second), b = std::get<2>(wall->second);
            const double base = std::get<3>(wall->second);
            auto faces = opening && e(*opening) ? r.Body(*e(*opening)) : std::vector<std::vector<P3>>{};
            if (faces.empty())
                faces = r.Body(o);
            const P2 u = (b - a).Unit();
            const auto ss = SpanOf(faces, [&](const P3& p) { return (r.Flat(p) - a).Dot(u); });
            const auto zs = SpanOf(faces, [](const P3& p) { return p[2]; });
            if (!ss || !zs)
            {
                leave(id, "it has no body MiniBIM can read");
                continue;
            }
            const auto [s0, s1] = *ss;
            const auto [z0, z1] = *zs;
            const auto size = [](double x) { return Round(x, 100.0); };
            const auto given = [&](size_t i) -> std::optional<double>
            {
                const auto v = r.Len(Arg(o, i));
                return v && *v > 0.0 ? v : std::nullopt;
            };
            const double width = size(given(9).value_or(s1 - s0));
            const double height = size(given(8).value_or(z1 - z0));
            const double sill = size(std::max(z0 - base, 0.0));
            const std::string baseName = label(id).value_or("Imported " + Num(width) + " x " + Num(height));
            // 门怎么开：它自己的开启类型（IFC4），否则它的类型的（IfcDoorType，IFC4）或样式的（IfcDoorStyle，IFC2x3）；门扇在哪侧一起读出
            const auto ty = Find(l.typed, id);
            const Entity* typed = ty ? e(*ty) : nullptr;
            std::optional<std::string_view> said = isDoor ? Enumeration(Arg(o, 11)) : std::nullopt;
            if (!said && isDoor && typed)
            {
                if (typed->name == "IFCDOORTYPE")
                    said = Enumeration(Arg(*typed, 10));
                else if (typed->name == "IFCDOORSTYLE")
                    said = Enumeration(Arg(*typed, 8));
            }
            const auto operated = said ? OperationOf::FromIfcDoor(*said) : std::nullopt;
            const std::optional<Operation> operation = operated ? std::optional<Operation>(operated->first) : std::nullopt;
            const OpeningType nt{ width, height, isDoor ? 0.0 : sill, operation };
            const auto same = [&](const OpeningType& k) { return std::abs(k.width - width) < 0.05 && std::abs(k.height - height) < 0.05 && k.operation == operation; };
            const std::string typeName = KindOf(isDoor ? m.types.door : m.types.window, baseName, same, nt);
            std::string mark;
            if (const auto it = tags.find(id); it != tags.end())
            {
                mark = it->second;
                tags.erase(it);
            }
            else
            {
                std::set<std::string>& t = marks[isDoor ? 1 : 0];
                mark = NextNumber(std::vector<std::string>(t.begin(), t.end()));
                t.insert(mark);
            }
            const auto op11 = Enumeration(Arg(o, 11));
            const bool flipHand = operated ? operated->second : (op11 && op11->find("RIGHT") != std::string_view::npos);
            Opening op;
            op.typeName = typeName;
            op.host     = Guid::OfId(hostId);
            op.offset   = Micron(P2((s0 + s1) / 2.0, 0.0)).x;
            op.sill     = sill;
            op.mark     = mark;
            op.flipHand = flipHand;
            Id oid = 0;
            if (isDoor)
            {
                Door d;
                static_cast<Opening&>(d) = std::move(op);
                oid = m.Insert(std::move(d));
            }
            else
            {
                Window w;
                static_cast<Opening&>(w) = std::move(op);
                oid = m.Insert(std::move(w));
            }
            made(oid, id);
            seen.insert(id);
        }

        // 房间：它的平面轮廓里的一点
        std::set<std::string> numbers;
        const auto spaces = named({ "IFCSPACE" });
        for (const uint64_t id : spaces)
        {
            const Entity& s = *e(id);
            if (TextOf(Arg(s, 7)))
                if (const auto n = TextOf(Arg(s, 2)))
                    numbers.insert(std::string(*n));
        }
        const auto nextNumber = [&]()
        {
            const std::string n = NextNumber(std::vector<std::string>(numbers.begin(), numbers.end()));
            numbers.insert(n);
            return n;
        };
        for (const uint64_t id : spaces)
        {
            const Entity& s = *e(id);
            const auto faces = r.Body(s);
            const auto zs = SpanOf(faces, [](const P3& p) { return p[2]; });
            std::optional<P2> point;
            if (zs)
                if (const auto g = Outline(faces, Plane::Flat(zs->first), false))
                    if (const auto p = Inside(g->outer))
                        point = r.Flat({ p->x, p->y, 0.0 });
            if (!point && faces.empty())
                point = r.Flat(r.FrameOf(s).o);
            if (!point)
            {
                leave(id, "its footprint isn't one MiniBIM can read");
                continue;
            }
            const auto longName = TextOf(Arg(s, 7)), shortName = TextOf(Arg(s, 2));
            std::string roomName, number;
            if (longName && shortName)
            {
                roomName = std::string(*longName);
                number   = std::string(*shortName);
            }
            else if (longName || shortName)
            {
                roomName = std::string(longName ? *longName : *shortName);
                number   = nextNumber();
            }
            else
            {
                roomName = "Room";
                number   = nextNumber();
            }
            Room room;
            room.level  = levelFor(id, zs ? zs->first : 0.0);
            room.point  = Micron(*point);
            room.name   = Clip(roomName);
            room.number = Clip(number);
            const Id rid = m.Insert(std::move(room));
            made(rid, id);
            seen.insert(id);
        }

        // 轴网：每条轴线一条轴网线
        std::set<std::string> gridNames;
        for (const uint64_t id : named({ "IFCGRID" }))
        {
            const Entity& g = *e(id);
            const Frame f = r.FrameOf(g);
            seen.insert(id);
            for (const size_t i : { 7, 8, 9 })
                for (const uint64_t aid : RefsOf(Arg(g, i)))
                {
                    const Entity* axis = e(aid);
                    if (!axis)
                        continue;
                    const auto pts = r.Curve(Reference(Arg(*axis, 1)), 0);
                    if (!pts || pts->empty())
                        continue;
                    P2 start = Micron(r.Flat(f.At(pts->front()))), end = Micron(r.Flat(f.At(pts->back())));
                    if (Enumeration(Arg(*axis, 2)) == "F")
                        std::swap(start, end);
                    const auto tagText = TextOf(Arg(*axis, 0));
                    const std::string tag = tagText ? std::string(*tagText) : NextNumber(std::vector<std::string>(gridNames.begin(), gridNames.end()));
                    const Id gid = m.Insert(Grid{ Fresh(gridNames, tag), start, end });
                    made(gid, id);
                }
        }

        // 各层用的材质放进库，按文件的颜色着色
        m.AdoptMaterials();
        for (const auto& [mname, shade] : MaterialColours(r, ids))
            if (const auto it = m.materials.find(mname); it != m.materials.end())
            {
                it->second.color        = shade.color;
                it->second.transparency = shade.clear;
            }
        // 每个元素的 GUID 是建成它的东西的 GlobalId（是 GlobalId 时）；指向元素的都指向它
        std::map<Id, Guid> globals;
        for (const auto& [step, id] : origin)
            if (const Entity* x = e(step))
                if (const auto t = TextOf(Arg(*x, 0)))
                    if (const auto g = Expand(*t))
                        globals[id] = *g;
        // 其余的由文件得出：同一个文件每次读出一样
        const uint64_t seed = Fnv(bytes);
        for (const auto& [id, _] : m.elements.Items())
            globals.try_emplace(id, Guid::Seeded(seed, id));
        m.elements = m.elements.Regiven(globals);
        m.Resolve();
        // 库的 GUID 也由文件得出：同一个文件每次读出一样
        m.library = Library();
        m.ForgetParts();
        m.SyncLibraryWith([seed](const Model&, const Kind* kind, const std::string& entry)
        {
            return Guid::Seeded(seed, Fnv((kind ? kind->ToString() : std::string("part")) + "/" + entry));
        });
        // 违反规则的略去，连同规则；墙带着它的门窗
        std::string why;
        for (size_t i = 0; i <= m.elements.Size() && !m.Check(why); ++i)
        {
            const auto id = Culprit(why);
            if (!id || !from.count(*id))
            {
                error = why;
                return false;
            }
            const size_t colon = why.find(": ");
            skipped.push_back(from[*id] + ": " + (colon == std::string::npos ? why : why.substr(colon + 2)));
            const Guid gone = m.GuidOf(*id);
            m.elements.Remove(*id);
            std::vector<Id> hosted;
            for (const auto& [k, x] : m.elements.Items())
                if (const Opening* o = x->AsOpening(); o && o->host == gone)
                    hosted.push_back(k);
            for (const Id h : hosted)
            {
                skipped.push_back(from[h] + ": its wall was left out");
                m.elements.Remove(h);
            }
        }
        if (!m.Check(error))
            return false;

        // 楼里其余的东西，按种类
        std::map<std::string, size_t> left;
        for (const auto& [id, _] : l.within)
            if (!seen.count(id))
                if (const Entity* x = e(id))
                    ++left[x->name];
        for (const auto& [kind, n] : left)
            skipped.push_back(std::to_string(n) + " " + kind + ": MiniBIM has no element for " + (n == 1 ? "it" : "them"));
        result.model   = std::move(m);
        result.schema  = file.schema;
        result.skipped = std::move(skipped);
        result.moved   = shift != P2() ? std::optional<P2>(shift * -1.0) : std::nullopt;
        return true;
    }
}
