// 移植自 BIMCraft crates/core/src/dxf.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Dxf.h"
#include "Model/Text.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <unordered_map>

namespace MiniBIM::Bim::Dxf
{
    namespace
    {
        constexpr double kPi  = 3.14159265358979323846;
        constexpr double kTau = 2.0 * kPi;
        // 放块时最多走过几个图元（块的阵列套阵列）：对付恶意的嵌套
        constexpr size_t MaxWork = 4000000;
        // 一个图元最多几个值：多段线的顶点、填充的边
        constexpr size_t MaxPairs = 2000000;
        // INSERT 的阵列最多几行、几列
        constexpr int64_t MaxArray = 10000;
        // 展平的圆弧、圆、椭圆最多几段
        constexpr size_t CurveSteps = 64;
        // 样条每一跨几段，一共最多几个点
        constexpr size_t SplineSteps     = 8;
        constexpr size_t MaxSplinePoints = 2048;
        // 一段文字最多几个字符
        constexpr size_t MaxChars = 4096;

        double Rad(double deg) { return deg * kPi / 180.0; }
        double Deg(double rad) { return rad * 180.0 / kPi; }
        // Rust 的 rem_euclid：总在 [0, m)
        double RemEuclid(double a, double m)
        {
            const double r = std::fmod(a, m);
            return r < 0.0 ? r + m : r;
        }

        bool EqualsIgnoreCase(std::string_view a, std::string_view b) { return a.size() == b.size() && Bim::Text::AsciiLower(a) == Bim::Text::AsciiLower(b); }

        Rgb HueRgb(double hue, double high, double low)
        {
            const double sector = std::floor(hue / 60.0);
            const double f = hue / 60.0 - sector;
            const double down = high - (high - low) * f, up = low + (high - low) * f;
            double r, g, b;
            switch (static_cast<int64_t>(sector) % 6)
            {
            case 0: r = high, g = up, b = low; break;
            case 1: r = down, g = high, b = low; break;
            case 2: r = low, g = high, b = up; break;
            case 3: r = low, g = down, b = high; break;
            case 4: r = up, g = low, b = high; break;
            default: r = high, g = low, b = down; break;
            }
            return Rgb{ static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b) };
        }

        bool Fits(const Shape& s)
        {
            const auto ok = [](double x) { return std::isfinite(x) && std::abs(x) <= MaxFileCoord; };
            const auto p  = [&](const P2& q) { return ok(q.x) && ok(q.y); };
            if (const Line* l = std::get_if<Line>(&s))
                return p(l->a) && p(l->b);
            if (const Polyline* pl = std::get_if<Polyline>(&s))
                return std::all_of(pl->pts.begin(), pl->pts.end(), p);
            if (const Circle* c = std::get_if<Circle>(&s))
                return p(c->c) && ok(c->r);
            if (const Arc* a = std::get_if<Arc>(&s))
                return p(a->c) && ok(a->r) && ok(a->start) && ok(a->end);
            if (const Point* pt = std::get_if<Point>(&s))
                return p(pt->p);
            if (const Text* t = std::get_if<Text>(&s))
                return p(t->at) && ok(t->height) && ok(t->angle);
            const Fill& f = std::get<Fill>(s);
            for (const auto& r : f.rings)
                if (!std::all_of(r.begin(), r.end(), p))
                    return false;
            return true;
        }

        // $INSUNITS 说的单位：毫米到它，和它的名称；没有单位与不是盖房子用的单位为空
        std::pair<std::optional<double>, const char*> InsUnits(int64_t code)
        {
            switch (code)
            {
            case 1: return { 25.4, "in" };
            case 2: return { 304.8, "ft" };
            case 3: return { 1609344.0, "miles" };
            case 4: return { 1.0, "mm" };
            case 5: return { 10.0, "cm" };
            case 6: return { 1000.0, "m" };
            case 7: return { 1e6, "km" };
            case 8: return { 2.54e-5, "microinches" };
            case 9: return { 0.0254, "mils" };
            case 10: return { 914.4, "yards" };
            case 11: return { 1e-7, "angstroms" };
            case 12: return { 1e-6, "nanometres" };
            case 13: return { 1e-3, "microns" };
            case 14: return { 100.0, "dm" };
            case 15: return { 1e4, "decametres" };
            case 16: return { 1e5, "hectometres" };
            case 18: return { std::nullopt, "astronomical units" };
            case 19: return { std::nullopt, "light years" };
            case 20: return { std::nullopt, "parsecs" };
            case 21: return { 304.8006096012, "US survey feet" };
            default: return { std::nullopt, "unitless" };
            }
        }

        // ── 文件的组码与值 ─────────────────────────────────────
        struct Pair
        {
            int32_t          code = 0;
            std::string_view value;
        };

        // 一次给一对组码与值
        class Pairs
        {
        public:
            explicit Pairs(std::string_view text) : m_text(text) {}

            // 已经给出的行数：报错时说在哪里
            size_t line = 0;

            bool Next(std::optional<Pair>& out, std::string& error)
            {
                if (m_peeked)
                {
                    out = *m_peeked;
                    m_peeked.reset();
                    return true;
                }
                out.reset();
                std::string_view code;
                while (true)
                {
                    const auto l = Line();
                    if (!l)
                        return true;
                    ++line;
                    code = Bim::Text::Trim(*l);
                    if (!code.empty())
                        break;
                }
                int32_t n = 0;
                std::string_view digits = code;
                if (!digits.empty() && digits.front() == '+')
                    digits.remove_prefix(1);
                const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
                if (ec != std::errc() || end != digits.data() + digits.size() || digits.empty())
                {
                    error = "line " + std::to_string(line) + ": `" + std::string(code) + "` isn't a group code";
                    return false;
                }
                // 以换行结尾的文件的最后一行是空的：也不是值
                const size_t save = m_at;
                auto value = Line();
                if (value && Bim::Text::Trim(*value).empty() && m_at >= m_text.size() && !m_more)
                    value.reset();
                if (!value)
                {
                    m_at = save;
                    error = "line " + std::to_string(line) + ": the file ends after group code " + std::to_string(n) + ", with no value";
                    return false;
                }
                ++line;
                // 文字保留它的空格；名称、数与标志的空格无关紧要
                std::string_view v = *value;
                if (n == 1 || n == 3)
                {
                    if (!v.empty() && v.back() == '\r')
                        v.remove_suffix(1);
                }
                else
                    v = Bim::Text::Trim(v);
                out = Pair{ n, v };
                return true;
            }

            bool Peek(std::optional<Pair>& out, std::string& error)
            {
                if (!m_peeked)
                {
                    std::optional<Pair> p;
                    if (!Next(p, error))
                        return false;
                    if (!p)
                    {
                        out.reset();
                        return true;
                    }
                    m_peeked = p;
                }
                out = m_peeked;
                return true;
            }

        private:
            // 照 Rust 的 split('\n')：最后一段（可能为空）也是一行
            std::optional<std::string_view> Line()
            {
                if (m_done)
                    return std::nullopt;
                const size_t nl = m_text.find('\n', m_at);
                if (nl == std::string_view::npos)
                {
                    const std::string_view l = m_text.substr(m_at);
                    m_at   = m_text.size();
                    m_done = true;
                    m_more = false;
                    return l;
                }
                const std::string_view l = m_text.substr(m_at, nl - m_at);
                m_at = nl + 1;
                // 还有下一段（换行之后，哪怕是空的）
                m_more = true;
                return l;
            }

            std::string_view    m_text;
            size_t              m_at   = 0;
            bool                m_done = false;
            // 刚给出的一行之后还有没有一段
            bool                m_more = true;
            std::optional<Pair> m_peeked;
        };

        // 文件里的一个数，或为什么它不是图纸能放的数
        bool Number(size_t line, std::string_view v, double& out, std::string& error)
        {
            const auto x = Bim::Text::ParseNumber(v);
            if (!x)
            {
                error = "line " + std::to_string(line) + ": `" + std::string(v) + "` isn't a number";
                return false;
            }
            if (!std::isfinite(*x))
            {
                error = "line " + std::to_string(line) + ": `" + std::string(v) + "` isn't a number a drawing can hold";
                return false;
            }
            if (std::abs(*x) > MaxFileCoord)
            {
                error = "line " + std::to_string(line) + ": " + std::string(v) + " is past what a drawing holds (" + Bim::Text::Shortest(MaxFileCoord) + ")";
                return false;
            }
            out = *x;
            return true;
        }

        // 一个图元、表项或块头：种类、从哪行起、按顺序的组码与值
        struct Ent
        {
            std::string_view  kind;
            size_t            line = 0;
            std::vector<Pair> codes;

            std::optional<std::string_view> Str(int32_t code) const
            {
                for (const Pair& p : codes)
                    if (p.code == code)
                        return p.value;
                return std::nullopt;
            }

            bool Num(int32_t code, std::optional<double>& out, std::string& error) const
            {
                out.reset();
                const auto s = Str(code);
                if (!s)
                    return true;
                double x = 0.0;
                if (!Number(line, *s, x, error))
                    return false;
                out = x;
                return true;
            }

            bool Int(int32_t code, std::optional<int64_t>& out, std::string& error) const
            {
                std::optional<double> x;
                if (!Num(code, x, error))
                    return false;
                out = x ? std::optional<int64_t>(static_cast<int64_t>(*x)) : std::nullopt;
                return true;
            }

            // code 处（x）与 code + 10 处（y）的点，各取第一个
            bool Point(int32_t code, std::optional<P2>& out, std::string& error) const
            {
                std::optional<double> x, y;
                if (!Num(code, x, error) || !Num(code + 10, y, error))
                    return false;
                out = x && y ? std::optional<P2>(P2(*x, *y)) : std::nullopt;
                return true;
            }

            // x 在 code 处的每个点，按顺序，各带后面 extra 处的值（或 0）
            bool Points(int32_t code, int32_t extra, std::vector<std::pair<P2, double>>& out, std::string& error) const
            {
                out.clear();
                std::optional<double> x;
                for (const Pair& p : codes)
                {
                    if (p.code == code)
                    {
                        double v = 0.0;
                        if (!Number(line, p.value, v, error))
                            return false;
                        x = v;
                    }
                    else if (p.code == code + 10)
                    {
                        if (x)
                        {
                            double v = 0.0;
                            if (!Number(line, p.value, v, error))
                                return false;
                            out.emplace_back(P2(*x, v), 0.0);
                            x.reset();
                        }
                    }
                    else if (p.code == extra && !out.empty())
                    {
                        double v = 0.0;
                        if (!Number(line, p.value, v, error))
                            return false;
                        out.back().second = v;
                    }
                }
                return true;
            }

            // code 处的每个值，按顺序
            bool All(int32_t code, std::vector<double>& out, std::string& error) const
            {
                out.clear();
                for (const Pair& p : codes)
                    if (p.code == code)
                    {
                        double v = 0.0;
                        if (!Number(line, p.value, v, error))
                            return false;
                        out.push_back(v);
                    }
                return true;
            }
        };

        Color TrueColorOf(int64_t rgb)
        {
            return Color::TrueColor(static_cast<uint8_t>((rgb >> 16) & 0xFF), static_cast<uint8_t>((rgb >> 8) & 0xFF), static_cast<uint8_t>(rgb & 0xFF));
        }

        Color IndexColor(int64_t n)
        {
            if (n == 0)
                return Color{ Color::Kind::ByBlock, 0, {} };
            if (n >= 1 && n <= 255)
                return Color::Indexed(static_cast<uint8_t>(n));
            return Color{};
        }

        // 文件里的线宽，百分之一毫米；随层、随块与默认的没有
        std::optional<double> Weight(std::optional<int64_t> v)
        {
            if (!v || *v < 0)
                return std::nullopt;
            return static_cast<double>(static_cast<int32_t>(*v)) / 100.0;
        }

        // ── 放置 ─────────────────────────────────────────────
        // 块的图元放到哪里：p 落在 o + x * p.x + y * p.y
        struct Xf
        {
            P2 o{ 0.0, 0.0 }, x{ 1.0, 0.0 }, y{ 0.0, 1.0 };

            P2 At(P2 p) const { return o + x * p.x + y * p.y; }
            P2 Vec(P2 v) const { return x * v.x + y * v.y; }
            // inner 之后再 this：点先过 inner
            Xf Then(const Xf& inner) const { return Xf{ At(inner.o), Vec(inner.x), Vec(inner.y) }; }
            // 圆仍是圆时：比例、转的度数、是否镜像
            std::optional<std::tuple<double, double, bool>> Similarity() const
            {
                const double lx = x.Length(), ly = y.Length();
                const bool square = std::abs(x.Dot(y)) <= 1e-9 * lx * ly;
                if (!(lx > 0.0 && std::abs(lx - ly) <= 1e-9 * lx && square))
                    return std::nullopt;
                return std::tuple<double, double, bool>(lx, Deg(std::atan2(x.y, x.x)), x.Cross(y) < 0.0);
            }
        };
        // 朝下的拉伸方向：x 反过来
        constexpr Xf MirrorX{ P2(0.0, 0.0), P2(-1.0, 0.0), P2(0.0, 1.0) };

        // 块里的图元从放它的 INSERT 那里拿的：0 层变成它的图层，随块的颜色与线型
        struct Inherit
        {
            std::string_view                layer;
            Color                           color;
            std::optional<std::string_view> linetype;
        };

        struct Out
        {
            std::vector<Entity>           entities;
            std::map<std::string, size_t> leftOut;
            size_t                        work = 0;

            void Left(const std::string& what) { ++leftOut[what]; }

            bool Push(Entity e, std::string& error)
            {
                // 块里缩放的块能把点带到任何图纸之外，或任何数之外
                if (!Fits(e.shape))
                {
                    Left("past what a drawing holds");
                    return true;
                }
                if (entities.size() >= MaxShapes)
                {
                    error = "it draws more than " + std::to_string(MaxShapes) + " shapes; up to that many";
                    return false;
                }
                entities.push_back(std::move(e));
                return true;
            }
        };

        // 图元的图层、颜色、线型与线宽，加上从它所在的块拿的
        struct Style
        {
            std::string                layer;
            Color                      color;
            std::optional<std::string> linetype;
            std::optional<double>      weight;

            static bool Of(const Ent& e, const Inherit* inh, Style& out, std::string& error)
            {
                std::string_view layer = e.Str(8).value_or("0");
                if (layer == "0" && inh)
                    layer = inh->layer;
                std::optional<int64_t> rgb, n, w;
                if (!e.Int(420, rgb, error) || !e.Int(62, n, error) || !e.Int(370, w, error))
                    return false;
                Color color = rgb ? TrueColorOf(*rgb) : n ? IndexColor(std::abs(*n)) : Color{};
                if (color.kind == Color::Kind::ByBlock && inh)
                    color = inh->color;
                std::optional<std::string> linetype;
                if (const auto lt = e.Str(6))
                {
                    const std::string_view t = Bim::Text::Trim(*lt);
                    if (t.empty() || EqualsIgnoreCase(t, "BYLAYER"))
                        linetype.reset();
                    else if (EqualsIgnoreCase(t, "BYBLOCK"))
                        linetype = inh && inh->linetype ? std::optional<std::string>(std::string(*inh->linetype)) : std::nullopt;
                    else
                        linetype = std::string(t);
                }
                out = Style{ Bim::Text::TakeChars(layer, MaxChars), color, std::move(linetype), Weight(w) };
                return true;
            }
        };

        // 圆弧从 start 逆时针到 end 度：start 放进 [0, 360)，end 在它后面（相等时一整圈）
        Arc ArcOf(P2 c, double r, double start, double end)
        {
            start = RemEuclid(start, 360.0);
            double sweep = RemEuclid(end - start, 360.0);
            if (sweep == 0.0)
                sweep = 360.0;
            return Arc{ c, r, start, start + sweep };
        }

        // shape 经 xf：圆仍是圆时圆与圆弧还是它们，否则先展平
        Shape Transformed(Shape shape, const Xf& xf)
        {
            const auto at = [&](P2 p) { return xf.At(p); };
            if (Line* l = std::get_if<Line>(&shape))
                return Line{ at(l->a), at(l->b) };
            if (Polyline* pl = std::get_if<Polyline>(&shape))
            {
                for (P2& p : pl->pts)
                    p = at(p);
                return shape;
            }
            if (Point* pt = std::get_if<Point>(&shape))
                return Point{ at(pt->p) };
            if (Fill* f = std::get_if<Fill>(&shape))
            {
                for (auto& r : f->rings)
                    for (P2& p : r)
                        p = at(p);
                return shape;
            }
            if (const Circle* c = std::get_if<Circle>(&shape))
            {
                if (const auto s = xf.Similarity())
                    return Circle{ at(c->c), c->r * std::get<0>(*s) };
                std::vector<P2> pts = ArcPoints(c->c, c->r, 0.0, 360.0);
                pts.pop_back();
                for (P2& p : pts)
                    p = at(p);
                return Polyline{ std::move(pts), true };
            }
            if (const Arc* a = std::get_if<Arc>(&shape))
            {
                if (const auto s = xf.Similarity())
                {
                    const auto [k, turn, mirror] = *s;
                    const double st = mirror ? turn - a->end : a->start + turn;
                    const double en = mirror ? turn - a->start : a->end + turn;
                    return ArcOf(at(a->c), a->r * k, st, en);
                }
                std::vector<P2> pts = ArcPoints(a->c, a->r, a->start, a->end - a->start);
                for (P2& p : pts)
                    p = at(p);
                return Polyline{ std::move(pts), false };
            }
            Text t = std::get<Text>(std::move(shape));
            double k = xf.x.Length(), turn = Deg(std::atan2(xf.x.y, xf.x.x));
            if (const auto s = xf.Similarity())
                k = std::get<0>(*s), turn = std::get<1>(*s);
            t.at = at(t.at);
            t.height *= k;
            t.angle += turn;
            return t;
        }

        // 从 a 到 b、凸度（圆心角四分之一的正切，正为逆时针）为 bulge 的圆弧上的点，两端都在
        std::vector<P2> BulgePoints(P2 a, P2 b, double bulge)
        {
            const double theta = 4.0 * std::atan(bulge);
            const double d = a.Dist(b);
            const double r = d / (2.0 * std::abs(std::sin(theta / 2.0)));
            const P2 c = (a + b) * 0.5 + (b - a).Unit().Perp() * (d / 2.0 / std::tan(theta / 2.0));
            const double start = Deg(std::atan2((a - c).y, (a - c).x));
            std::vector<P2> pts = ArcPoints(c, r, start, Deg(theta));
            if (!pts.empty())
            {
                pts.front() = a;
                pts.back()  = b;
            }
            return pts;
        }

        // 顶点各带离开它的那段的凸度的多段线：弯的段展平
        std::optional<Shape> PolylineOf(const std::vector<std::pair<P2, double>>& pts, bool closed)
        {
            if (pts.size() < 2)
                return std::nullopt;
            if (std::all_of(pts.begin(), pts.end(), [](const auto& p) { return p.second == 0.0; }))
            {
                Polyline pl{ {}, closed };
                for (const auto& [p, _] : pts)
                    pl.pts.push_back(p);
                return pl;
            }
            std::vector<P2> out;
            const size_t last = closed ? pts.size() : pts.size() - 1;
            for (size_t i = 0; i < last; ++i)
            {
                const auto& [a, bulge] = pts[i];
                const P2 b = pts[(i + 1) % pts.size()].first;
                if (bulge == 0.0 || a.Dist(b) == 0.0)
                {
                    out.push_back(a);
                    continue;
                }
                const std::vector<P2> bent = BulgePoints(a, b, bulge);
                out.insert(out.end(), bent.begin(), bent.end() - (bent.empty() ? 0 : 1));
            }
            if (!closed)
                out.push_back(pts.back().first);
            return Polyline{ std::move(out), closed };
        }

        bool EllipseOf(const Ent& e, std::optional<Shape>& out, std::string& error)
        {
            out.reset();
            std::optional<P2> c, m;
            std::optional<double> ratio, u0o, u1o;
            if (!e.Point(10, c, error) || !e.Point(11, m, error) || !e.Num(40, ratio, error))
                return false;
            if (!c || !m || !ratio)
                return true;
            if (*ratio <= 0.0 || m->Length() == 0.0)
                return true;
            if (!e.Num(41, u0o, error) || !e.Num(42, u1o, error))
                return false;
            const double u0 = u0o.value_or(0.0), u1 = u1o.value_or(kTau);
            double sweep = RemEuclid(u1 - u0, kTau);
            const bool whole = sweep < 1e-9;
            if (whole)
                sweep = kTau;
            const size_t n = std::clamp<size_t>(static_cast<size_t>(std::ceil(sweep / kTau * CurveSteps)), 2, CurveSteps);
            const P2 minor = m->Perp() * *ratio;
            std::vector<P2> pts;
            for (size_t i = 0; i <= n; ++i)
            {
                const double u = u0 + sweep * static_cast<double>(i) / static_cast<double>(n);
                pts.push_back(*c + *m * std::cos(u) + minor * std::sin(u));
            }
            if (whole)
                pts.pop_back();
            out = Polyline{ std::move(pts), whole };
            return true;
        }

        // degree 次、节点 knots、带权的控制点 controls 的 B 样条在 u 处的点：齐次坐标里的 de Boor 递推
        std::optional<P2> DeBoor(size_t degree, const std::vector<double>& knots, const std::vector<std::pair<P2, double>>& controls, double u)
        {
            const size_t n = controls.size();
            size_t k = degree;
            while (k + 1 < n)
            {
                if (k + 1 >= knots.size())
                    return std::nullopt;
                if (!(u >= knots[k + 1]))
                    break;
                ++k;
            }
            std::vector<std::array<double, 3>> d;
            for (size_t j = 0; j <= degree; ++j)
            {
                const size_t at = j + k - degree;
                if (at >= controls.size())
                    return std::nullopt;
                const auto& [p, w] = controls[at];
                d.push_back({ p.x * w, p.y * w, w });
            }
            for (size_t r = 1; r <= degree; ++r)
                for (size_t j = degree; j >= r; --j)
                {
                    const size_t i0 = j + k - degree, i1 = j + 1 + k - r;
                    if (i0 >= knots.size() || i1 >= knots.size())
                        return std::nullopt;
                    const double t0 = knots[i0], t1 = knots[i1];
                    const double a = t1 > t0 ? (u - t0) / (t1 - t0) : 0.0;
                    const auto p = d[j - 1], q = d[j];
                    for (int i = 0; i < 3; ++i)
                        d[j][i] = (1.0 - a) * p[i] + a * q[i];
                    if (j == 0)
                        break;
                }
            const auto [x, y, w] = d[degree];
            if (!(std::abs(w) > 1e-12))
                return std::nullopt;
            return P2(x / w, y / w);
        }

        bool SplineOf(const Ent& e, std::optional<Shape>& out, std::string& error)
        {
            out.reset();
            std::optional<int64_t> flags, deg;
            std::vector<double> knots;
            std::vector<std::pair<P2, double>> controls, fit;
            if (!e.Int(70, flags, error) || !e.Int(71, deg, error) || !e.All(40, knots, error) || !e.Points(10, 41, controls, error) ||
                !e.Points(11, 0, fit, error))
                return false;
            const bool closed = (flags.value_or(0) & 1) != 0;
            const size_t degree = static_cast<size_t>(std::clamp<int64_t>(deg.value_or(3), 1, 11));
            if (controls.size() > degree && knots.size() == controls.size() + degree + 1)
            {
                for (auto& [p, w] : controls)
                    w = w > 0.0 ? w : 1.0;
                const double u0 = knots[degree], u1 = knots[controls.size()];
                const size_t n = std::clamp((controls.size() - degree) * SplineSteps, SplineSteps, MaxSplinePoints);
                std::vector<P2> pts;
                for (size_t i = 0; i <= n; ++i)
                    if (const auto p = DeBoor(degree, knots, controls, u0 + (u1 - u0) * static_cast<double>(i) / static_cast<double>(n)))
                        pts.push_back(*p);
                if (pts.size() >= 2)
                    out = Polyline{ std::move(pts), closed };
                return true;
            }
            if (fit.size() >= 2)
            {
                Polyline pl{ {}, closed };
                for (const auto& [p, _] : fit)
                    pl.pts.push_back(p);
                out = std::move(pl);
            }
            return true;
        }

        // 文件写的文字：%%d 度、%%p 正负、%%c 直径、%%% 百分号，%%u、%%o 上下划线（去掉），\U+XXXX 一个字符，
        // 多行文字的格式码（\P 换行、{…} 组、\f…; 字体、\H…; 字高等）去掉
        std::string Decode(std::string_view s)
        {
            std::string out;
            size_t i = 0;
            const auto peek = [&]() -> int { return i < s.size() ? static_cast<unsigned char>(s[i]) : -1; };
            // 一整个 UTF-8 字符（从 i - 1 的首字节起）
            const auto copyRest = [&](unsigned char lead)
            {
                out.push_back(static_cast<char>(lead));
                while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
                    out.push_back(s[i++]);
            };
            while (i < s.size())
            {
                if (out.size() >= MaxChars)
                    break;
                const unsigned char c = static_cast<unsigned char>(s[i++]);
                if (c == '%' && peek() == '%')
                {
                    ++i;
                    if (i >= s.size())
                    {
                        out += "%%";
                        continue;
                    }
                    const unsigned char o = static_cast<unsigned char>(s[i++]);
                    switch (o < 128 ? std::tolower(o) : o)
                    {
                    case 'd': out += "\xC2\xB0"; break;
                    case 'p': out += "\xC2\xB1"; break;
                    case 'c': out += "\xC3\x98"; break;
                    case '%': out += '%'; break;
                    case 'u':
                    case 'o': break;
                    default:
                        out += "%%";
                        copyRest(o < 128 ? static_cast<unsigned char>(std::tolower(o)) : o);
                        break;
                    }
                }
                else if (c == '{' || c == '}')
                    continue;
                else if (c == '\\')
                {
                    if (i >= s.size())
                    {
                        out += '\\';
                        continue;
                    }
                    const unsigned char o = static_cast<unsigned char>(s[i++]);
                    switch (o)
                    {
                    case 'P': out += '\n'; break;
                    case '~': out += ' '; break;
                    case '\\': out += '\\'; break;
                    case '{': out += '{'; break;
                    case '}': out += '}'; break;
                    case 'U':
                        if (peek() == '+')
                        {
                            ++i;
                            const std::string_view hex = s.substr(i, std::min<size_t>(4, s.size() - i));
                            i += hex.size();
                            uint32_t cp = 0;
                            const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), cp, 16);
                            if (ec == std::errc() && end == hex.data() + hex.size() && !hex.empty() && !(cp >= 0xD800 && cp <= 0xDFFF))
                                Bim::Text::AppendUtf8(out, static_cast<char32_t>(cp));
                            else
                                out += "\\U+" + std::string(hex);
                        }
                        else
                            out += "\\U";
                        break;
                    case 'S':
                    {
                        // 叠起来的分数：两部分，斜杠隔开
                        const size_t semi = s.find(';', i);
                        std::string body(s.substr(i, semi == std::string_view::npos ? std::string_view::npos : semi - i));
                        i = semi == std::string_view::npos ? s.size() : semi + 1;
                        std::replace(body.begin(), body.end(), '^', '/');
                        std::replace(body.begin(), body.end(), '#', '/');
                        out += body;
                        break;
                    }
                    case 'A': case 'C': case 'F': case 'f': case 'H': case 'Q': case 'T': case 'W': case 'p':
                    {
                        const size_t semi = s.find(';', i);
                        i = semi == std::string_view::npos ? s.size() : semi + 1;
                        break;
                    }
                    case 'L': case 'l': case 'O': case 'o': case 'K': case 'k': case 'N': break;
                    default:
                        out += '\\';
                        copyRest(o);
                        break;
                    }
                }
                else
                    copyRest(c);
            }
            return out;
        }

        bool TextOf(const Ent& e, std::optional<Shape>& out, std::string& error)
        {
            out.reset();
            std::optional<P2> first, second;
            std::optional<double> height, angle;
            if (!e.Point(10, first, error) || !e.Num(40, height, error))
                return false;
            const auto words = e.Str(1);
            if (!first || !height || !words)
                return true;
            if (*height <= 0.0)
                return true;
            std::string w = Decode(*words);
            std::replace(w.begin(), w.end(), '\n', ' ');
            if (Bim::Text::Trim(w).empty())
                return true;
            std::optional<int64_t> ho, vo;
            if (!e.Int(72, ho, error) || !e.Int(73, vo, error) || !e.Point(11, second, error) || !e.Num(50, angle, error))
                return false;
            const int64_t h = ho.value_or(0), v = vo.value_or(0);
            P2 at = *first;
            Align align;
            if ((h == 0 && v == 0) || h == 3 || h == 5 || !second)
                ;
            else if (h == 4)
                at = *second, align = Align{ Horizontal::Center, Vertical::Middle };
            else
            {
                at = *second;
                align.h = h == 1 ? Horizontal::Center : h == 2 ? Horizontal::Right : Horizontal::Left;
                align.v = v == 1 ? Vertical::Bottom : v == 2 ? Vertical::Middle : v == 3 ? Vertical::Top : Vertical::Baseline;
            }
            out = Text{ at, *height, angle.value_or(0.0), std::move(w), align };
            return true;
        }

        // 多行文字的各行，各是一段文字，从它的对齐点放的地方往下排
        bool MTextOf(const Ent& e, std::vector<Shape>& out, std::string& error)
        {
            out.clear();
            std::optional<P2> at, dir;
            std::optional<double> height, angleO, factor;
            std::optional<int64_t> attachO;
            if (!e.Point(10, at, error) || !e.Num(40, height, error))
                return false;
            if (!at || !height || *height <= 0.0)
                return true;
            std::string raw;
            for (const Pair& p : e.codes)
                if (p.code == 3)
                    raw += p.value;
            raw += e.Str(1).value_or("");
            const std::string words = Decode(raw);
            // 照 Rust 的 lines()：按 \n 分，去掉行尾的 \r，末尾的空行不算
            std::vector<std::string> lines;
            {
                size_t a = 0;
                while (a < words.size())
                {
                    const size_t nl = words.find('\n', a);
                    std::string l = words.substr(a, nl == std::string::npos ? std::string::npos : nl - a);
                    if (!l.empty() && l.back() == '\r')
                        l.pop_back();
                    lines.push_back(std::move(l));
                    if (nl == std::string::npos)
                        break;
                    a = nl + 1;
                }
            }
            if (std::all_of(lines.begin(), lines.end(), [](const std::string& l) { return Bim::Text::Trim(l).empty(); }))
                return true;
            if (!e.Num(50, angleO, error) || !e.Point(11, dir, error) || !e.Int(71, attachO, error) || !e.Num(44, factor, error))
                return false;
            double angle = 0.0;
            if (angleO)
                angle = *angleO;
            else if (dir && dir->Length() > 0.0)
                angle = Deg(std::atan2(dir->y, dir->x));
            const int64_t attach = std::clamp<int64_t>(attachO.value_or(1), 1, 9) - 1;
            const Horizontal h = attach % 3 == 0 ? Horizontal::Left : attach % 3 == 1 ? Horizontal::Center : Horizontal::Right;
            const double spacing = *height * 5.0 / 3.0 * (factor && *factor > 0.0 ? *factor : 1.0);
            const double block = *height + spacing * static_cast<double>(lines.size() - 1);
            const P2 up(-std::sin(Rad(angle)), std::cos(Rad(angle)));
            const P2 top = *at + up * (attach / 3 == 0 ? 0.0 : attach / 3 == 1 ? block / 2.0 : block);
            for (size_t i = 0; i < lines.size(); ++i)
                out.push_back(Text{ top - up * (*height + spacing * static_cast<double>(i)), *height, angle, lines[i], Align{ h, Vertical::Baseline } });
            return true;
        }

        // 在图元的值里往前走：填充的边界按顺序排着，一个值接一个
        struct Cursor
        {
            const Ent& e;
            size_t     k = 0;

            // 从这里往后 code 处的下一个值，没有时为空；走过它
            bool Take(int32_t code, std::optional<double>& out, std::string& error)
            {
                out.reset();
                for (size_t j = k; j < e.codes.size(); ++j)
                    if (e.codes[j].code == code)
                    {
                        k = j + 1;
                        double v = 0.0;
                        if (!Number(e.line, e.codes[j].value, v, error))
                            return false;
                        out = v;
                        return true;
                    }
                return true;
            }
        };

        // 填充的各边界，每个一个环：多段线的（凸度展平），或首尾相接的边
        bool HatchOf(const Ent& e, std::optional<Shape>& out, std::string& error)
        {
            out.reset();
            std::optional<int64_t> flags70, pathsO;
            if (!e.Int(70, flags70, error))
                return false;
            const auto name = e.Str(2);
            const bool solid = (flags70.value_or(0) & 1) != 0 || (name && EqualsIgnoreCase(*name, "SOLID"));
            size_t start = e.codes.size();
            for (size_t j = 0; j < e.codes.size(); ++j)
                if (e.codes[j].code == 91)
                {
                    start = j;
                    break;
                }
            if (start == e.codes.size())
                return true;
            Cursor cur{ e, start + 1 };
            if (!e.Int(91, pathsO, error))
                return false;
            const int64_t paths = std::clamp<int64_t>(pathsO.value_or(0), 0, 10000);
            std::vector<std::vector<P2>> rings;
            const auto join = [](std::vector<P2>& ring, const std::vector<P2>& pts)
            {
                for (const P2& p : pts)
                    if (ring.empty() || ring.back().Dist(p) > 1e-9)
                        ring.push_back(p);
            };
            const auto count = [](std::optional<double> v) { return static_cast<size_t>(std::clamp(v.value_or(0.0), 0.0, static_cast<double>(MaxPairs))); };
            std::optional<double> a, b, c, d, f, g;
            for (int64_t path = 0; path < paths; ++path)
            {
                std::optional<double> fl;
                if (!cur.Take(92, fl, error))
                    return false;
                if (!fl)
                    break;
                const int64_t pflags = static_cast<int64_t>(*fl);
                std::vector<P2> ring;
                if (pflags & 2)
                {
                    if (!cur.Take(72, a, error) || !cur.Take(73, b, error) || !cur.Take(93, c, error))
                        return false;
                    const bool bulged = a.value_or(0.0) != 0.0;
                    const size_t n = count(c);
                    std::vector<std::pair<P2, double>> pts;
                    for (size_t i = 0; i < n; ++i)
                    {
                        if (!cur.Take(10, a, error) || !cur.Take(20, b, error))
                            return false;
                        if (!a || !b)
                            break;
                        double bulge = 0.0;
                        if (bulged)
                        {
                            if (!cur.Take(42, c, error))
                                return false;
                            bulge = c.value_or(0.0);
                        }
                        pts.emplace_back(P2(*a, *b), bulge);
                    }
                    if (const auto pl = PolylineOf(pts, true))
                        join(ring, std::get<Polyline>(*pl).pts);
                }
                else
                {
                    if (!cur.Take(93, c, error))
                        return false;
                    const size_t n = count(c);
                    for (size_t i = 0; i < n; ++i)
                    {
                        std::optional<double> kind;
                        if (!cur.Take(72, kind, error))
                            return false;
                        if (!kind)
                            break;
                        const int64_t kd = static_cast<int64_t>(*kind);
                        // 圆弧与椭圆弧的起止，顺时针时反过来
                        const auto sweepOf = [&](double& s, double& t, std::string& err) -> bool
                        {
                            std::optional<double> so, to, ccw;
                            if (!cur.Take(50, so, err) || !cur.Take(51, to, err) || !cur.Take(73, ccw, err))
                                return false;
                            s = so.value_or(0.0), t = to.value_or(360.0);
                            if (ccw.value_or(1.0) == 0.0)
                            {
                                const double s0 = s;
                                s = -t, t = -s0;
                            }
                            return true;
                        };
                        if (kd == 1)
                        {
                            if (!cur.Take(10, a, error) || !cur.Take(20, b, error) || !cur.Take(11, c, error) || !cur.Take(21, d, error))
                                return false;
                            if (!a || !b || !c || !d)
                                break;
                            join(ring, { P2(*a, *b), P2(*c, *d) });
                        }
                        else if (kd == 2)
                        {
                            if (!cur.Take(10, a, error) || !cur.Take(20, b, error) || !cur.Take(40, c, error))
                                return false;
                            if (!a || !b || !c)
                                break;
                            double s = 0.0, t = 0.0;
                            if (!sweepOf(s, t, error))
                                return false;
                            double w = RemEuclid(t - s, 360.0);
                            const double sweep = w == 0.0 ? 360.0 : w;
                            if (*c > 0.0)
                                join(ring, ArcPoints(P2(*a, *b), *c, s, sweep));
                        }
                        else if (kd == 3)
                        {
                            if (!cur.Take(10, a, error) || !cur.Take(20, b, error) || !cur.Take(11, c, error) || !cur.Take(21, d, error) ||
                                !cur.Take(40, f, error))
                                return false;
                            if (!a || !b || !c || !d || !f)
                                break;
                            double s = 0.0, t = 0.0;
                            if (!sweepOf(s, t, error))
                                return false;
                            double w = RemEuclid(t - s, 360.0);
                            const double sweep = w == 0.0 ? 360.0 : w;
                            const P2 centre(*a, *b), m(*c, *d);
                            const double ratio = *f;
                            if (ratio > 0.0 && m.Length() > 0.0)
                            {
                                const size_t steps = std::clamp<size_t>(static_cast<size_t>(std::ceil(sweep / 360.0 * CurveSteps)), 2, CurveSteps);
                                const P2 minor = m.Perp() * ratio;
                                std::vector<P2> pts;
                                for (size_t j = 0; j <= steps; ++j)
                                {
                                    const double u = Rad(s + sweep * static_cast<double>(j) / static_cast<double>(steps));
                                    pts.push_back(centre + m * std::cos(u) + minor * std::sin(u));
                                }
                                join(ring, pts);
                            }
                        }
                        else if (kd == 4)
                        {
                            std::optional<double> dg, rat, per, nko, nco, nfo;
                            if (!cur.Take(94, dg, error) || !cur.Take(73, rat, error) || !cur.Take(74, per, error) || !cur.Take(95, nko, error) ||
                                !cur.Take(96, nco, error))
                                return false;
                            const size_t degree = static_cast<size_t>(std::clamp(dg.value_or(3.0), 1.0, 11.0));
                            const bool rational = rat.value_or(0.0) != 0.0;
                            const size_t nk = count(nko), nc = count(nco);
                            std::vector<double> knots;
                            knots.reserve(std::min<size_t>(nk, 1 << 16));
                            for (size_t j = 0; j < nk; ++j)
                            {
                                if (!cur.Take(40, a, error))
                                    return false;
                                if (!a)
                                    break;
                                knots.push_back(*a);
                            }
                            std::vector<std::pair<P2, double>> controls;
                            controls.reserve(std::min<size_t>(nc, 1 << 16));
                            for (size_t j = 0; j < nc; ++j)
                            {
                                if (!cur.Take(10, a, error) || !cur.Take(20, b, error))
                                    return false;
                                if (!a || !b)
                                    break;
                                double w = 1.0;
                                if (rational)
                                {
                                    if (!cur.Take(42, c, error))
                                        return false;
                                    w = c && *c > 0.0 ? *c : 1.0;
                                }
                                controls.emplace_back(P2(*a, *b), w);
                            }
                            if (!cur.Take(97, nfo, error))
                                return false;
                            const size_t nf = count(nfo);
                            std::vector<P2> fit;
                            for (size_t j = 0; j < nf; ++j)
                            {
                                if (!cur.Take(11, a, error) || !cur.Take(21, b, error))
                                    return false;
                                if (!a || !b)
                                    break;
                                fit.emplace_back(*a, *b);
                            }
                            if (controls.size() > degree && knots.size() == controls.size() + degree + 1)
                            {
                                const double u0 = knots[degree], u1 = knots[controls.size()];
                                const size_t steps = std::clamp((controls.size() - degree) * SplineSteps, SplineSteps, MaxSplinePoints);
                                std::vector<P2> pts;
                                for (size_t j = 0; j <= steps; ++j)
                                    if (const auto p = DeBoor(degree, knots, controls, u0 + (u1 - u0) * static_cast<double>(j) / static_cast<double>(steps)))
                                        pts.push_back(*p);
                                join(ring, pts);
                            }
                            else
                                join(ring, fit);
                        }
                        else
                            break;
                    }
                }
                if (ring.size() >= 2 && ring.front().Dist(ring.back()) <= 1e-9)
                    ring.pop_back();
                if (ring.size() >= 3)
                    rings.push_back(std::move(ring));
            }
            if (!rings.empty())
                out = Fill{ std::move(rings), solid };
            return true;
        }

        // ── 各节 ─────────────────────────────────────────────
        struct Block
        {
            P2               base;
            std::vector<Ent> ents;
        };

        class Reader
        {
        public:
            explicit Reader(std::string_view text) : m_p(text) {}

            bool Read(Drawing& out, std::string& error)
            {
                std::optional<Pair> pr;
                bool sawEntities = false;
                std::vector<Ent> ents;
                while (true)
                {
                    if (!m_p.Next(pr, error))
                        return false;
                    if (!pr)
                        break;
                    if (pr->code != 0)
                        continue;
                    if (pr->value == "EOF")
                        break;
                    if (pr->value != "SECTION")
                        continue;
                    std::optional<Pair> nm;
                    if (!m_p.Next(nm, error))
                        return false;
                    if (!nm || nm->code != 2)
                    {
                        error = "line " + std::to_string(m_p.line) + ": a SECTION with no name";
                        return false;
                    }
                    bool ok = true;
                    if (nm->value == "HEADER")
                        ok = Header(error);
                    else if (nm->value == "TABLES")
                        ok = Tables(error);
                    else if (nm->value == "BLOCKS")
                        ok = Blocks(error);
                    else if (nm->value == "ENTITIES")
                    {
                        ok = Entities(ents, error);
                        sawEntities = true;
                    }
                    else
                        ok = Skip(error);
                    if (!ok)
                        return false;
                }
                if (!sawEntities)
                {
                    error = "it isn't a DXF: there's no ENTITIES section";
                    return false;
                }
                const auto var = [&](std::string_view name, int32_t code) -> std::optional<std::string_view>
                {
                    const auto it = m_header.find(name);
                    if (it == m_header.end())
                        return std::nullopt;
                    for (const Pair& p : it->second)
                        if (p.code == code)
                            return p.value;
                    return std::nullopt;
                };
                int64_t units = 0;
                if (const auto v = var("$INSUNITS", 70))
                    if (const auto x = Bim::Text::ParseNumber(*v))
                        units = std::isfinite(*x) ? static_cast<int64_t>(*x) : 0;
                const auto [scale, unit] = InsUnits(units);
                Drawing d;
                d.scale   = scale;
                d.unit    = unit;
                d.version = std::string(Bim::Text::Trim(var("$ACADVER", 1).value_or("")));
                std::string ignored;
                d.ltscale = 1.0;
                if (const auto v = var("$LTSCALE", 40))
                {
                    double x = 0.0;
                    if (Number(0, *v, x, ignored) && x > 0.0)
                        d.ltscale = x;
                }
                const auto corner = [&](std::string_view name) -> std::optional<P2>
                {
                    const auto x = var(name, 10), y = var(name, 20);
                    double a = 0.0, b = 0.0;
                    std::string e;
                    if (!x || !y || !Number(0, *x, a, e) || !Number(0, *y, b, e))
                        return std::nullopt;
                    return P2(a, b);
                };
                const auto lo = corner("$EXTMIN"), hi = corner("$EXTMAX");
                if (lo && hi && lo->x <= hi->x && lo->y <= hi->y)
                    d.extents = std::array<P2, 2>{ *lo, *hi };
                Out o;
                if (!Place(o, ents, Xf{}, nullptr, 0, error))
                    return false;
                d.layers = std::move(m_layers);
                for (const Entity& e : o.entities)
                    if (std::none_of(d.layers.begin(), d.layers.end(), [&](const Layer& l) { return l.name == e.layer; }))
                    {
                        Layer l;
                        l.name = e.layer;
                        d.layers.push_back(std::move(l));
                    }
                d.linetypes = std::move(m_linetypes);
                d.entities  = std::move(o.entities);
                d.leftOut   = std::move(o.leftOut);
                out = std::move(d);
                return true;
            }

        private:
            // 走过这一节的末尾
            bool Skip(std::string& error)
            {
                std::optional<Pair> p;
                while (true)
                {
                    if (!m_p.Next(p, error))
                        return false;
                    if (!p || (p->code == 0 && p->value == "ENDSEC"))
                        return true;
                }
            }

            bool Header(std::string& error)
            {
                std::optional<std::string_view> name;
                std::optional<Pair> p;
                while (true)
                {
                    if (!m_p.Next(p, error))
                        return false;
                    if (!p || (p->code == 0 && p->value == "ENDSEC"))
                        return true;
                    if (p->code == 9)
                        name = p->value;
                    else if (name)
                        m_header[std::string(*name)].push_back(*p);
                }
            }

            // (0, kind) 之后到下一个组码 0 之前的各对
            bool ReadEnt(std::string_view kind, Ent& out, std::string& error)
            {
                out = Ent{ kind, m_p.line, {} };
                std::optional<Pair> p;
                while (true)
                {
                    if (!m_p.Peek(p, error))
                        return false;
                    if (!p || p->code == 0)
                        return true;
                    m_p.Next(p, error);
                    out.codes.push_back(*p);
                    if (out.codes.size() > MaxPairs)
                    {
                        error = "line " + std::to_string(out.line) + ": one " + std::string(kind) + " holds more than " + std::to_string(MaxPairs) + " values";
                        return false;
                    }
                }
            }

            bool Tables(std::string& error)
            {
                std::optional<Pair> p;
                while (true)
                {
                    if (!m_p.Next(p, error))
                        return false;
                    if (!p)
                        return true;
                    if (p->code != 0)
                        continue;
                    if (p->value == "ENDSEC")
                        return true;
                    Ent e;
                    if (!ReadEnt(p->value, e, error))
                        return false;
                    if (p->value == "LAYER")
                    {
                        std::optional<int64_t> rgb, n, flags, w;
                        if (!e.Int(420, rgb, error) || !e.Int(62, n, error) || !e.Int(70, flags, error) || !e.Int(370, w, error))
                            return false;
                        Layer l;
                        l.off = false;
                        if (rgb)
                            l.color = TrueColorOf(*rgb);
                        else if (n && *n < 0)
                            l.color = IndexColor(-*n), l.off = true;
                        else if (n)
                            l.color = IndexColor(*n);
                        else
                            l.color = Color::Indexed(7);
                        l.name = std::string(e.Str(2).value_or("0"));
                        const auto lt = e.Str(6);
                        l.linetype = lt && !lt->empty() ? std::string(*lt) : std::string("Continuous");
                        l.weight   = Weight(w);
                        l.frozen   = (flags.value_or(0) & 1) != 0;
                        m_layers.push_back(std::move(l));
                    }
                    else if (p->value == "LTYPE")
                    {
                        Linetype lt;
                        lt.name = std::string(e.Str(2).value_or(""));
                        if (!e.All(49, lt.dashes, error))
                            return false;
                        m_linetypes.push_back(std::move(lt));
                    }
                }
            }

            bool Blocks(std::string& error)
            {
                std::optional<std::pair<std::string, Block>> open;
                std::optional<Pair> p;
                while (true)
                {
                    if (!m_p.Next(p, error))
                        return false;
                    if (!p)
                        return true;
                    if (p->code != 0)
                        continue;
                    if (p->value == "ENDSEC")
                        return true;
                    Ent e;
                    if (!ReadEnt(p->value, e, error))
                        return false;
                    if (p->value == "BLOCK")
                    {
                        std::optional<P2> base;
                        if (!e.Point(10, base, error))
                            return false;
                        open.emplace(std::string(e.Str(2).value_or("")), Block{ base.value_or(P2(0.0, 0.0)), {} });
                    }
                    else if (p->value == "ENDBLK")
                    {
                        if (open)
                        {
                            m_blocks[open->first] = std::move(open->second);
                            open.reset();
                        }
                    }
                    else if (open)
                        open->second.ents.push_back(std::move(e));
                }
            }

            bool Entities(std::vector<Ent>& ents, std::string& error)
            {
                ents.clear();
                std::optional<Pair> p;
                while (true)
                {
                    if (!m_p.Next(p, error))
                        return false;
                    if (!p)
                        return true;
                    if (p->code != 0)
                        continue;
                    if (p->value == "ENDSEC")
                        return true;
                    Ent e;
                    if (!ReadEnt(p->value, e, error))
                        return false;
                    ents.push_back(std::move(e));
                }
            }

            // ents 经 xf 放好，块里的带着从它的 INSERT 拿的
            bool Place(Out& out, const std::vector<Ent>& ents, const Xf& xf0, const Inherit* inh, size_t depth, std::string& error) const
            {
                size_t i = 0;
                while (i < ents.size())
                {
                    const Ent& e = ents[i++];
                    if (++out.work > MaxWork)
                    {
                        error = "its blocks place more than " + std::to_string(MaxWork) + " entities; up to that many";
                        return false;
                    }
                    std::optional<int64_t> space;
                    if (!e.Int(67, space, error))
                        return false;
                    if (space.value_or(0) == 1)
                    {
                        out.Left("paper space");
                        continue;
                    }
                    Style style;
                    if (!Style::Of(e, inh, style, error))
                        return false;
                    std::optional<double> z;
                    if (!e.Num(230, z, error))
                        return false;
                    const Xf xf = z && *z < 0.0 ? xf0.Then(MirrorX) : xf0;
                    std::optional<Shape> shape;
                    const std::string_view k = e.kind;
                    if (k == "LINE")
                    {
                        std::optional<P2> a, b;
                        if (!e.Point(10, a, error) || !e.Point(11, b, error))
                            return false;
                        if (a && b)
                            shape = Line{ *a, *b };
                    }
                    else if (k == "CIRCLE")
                    {
                        std::optional<P2> c;
                        std::optional<double> r;
                        if (!e.Point(10, c, error) || !e.Num(40, r, error))
                            return false;
                        if (c && r && *r > 0.0)
                            shape = Circle{ *c, *r };
                    }
                    else if (k == "ARC")
                    {
                        std::optional<P2> c;
                        std::optional<double> r, s, t;
                        if (!e.Point(10, c, error) || !e.Num(40, r, error))
                            return false;
                        if (c && r && *r > 0.0)
                        {
                            if (!e.Num(50, s, error) || !e.Num(51, t, error))
                                return false;
                            shape = ArcOf(*c, *r, s.value_or(0.0), t.value_or(360.0));
                        }
                    }
                    else if (k == "LWPOLYLINE")
                    {
                        std::optional<int64_t> flags;
                        std::vector<std::pair<P2, double>> pts;
                        if (!e.Int(70, flags, error) || !e.Points(10, 42, pts, error))
                            return false;
                        shape = PolylineOf(pts, (flags.value_or(0) & 1) != 0);
                    }
                    else if (k == "POLYLINE")
                    {
                        std::optional<int64_t> flagsO;
                        if (!e.Int(70, flagsO, error))
                            return false;
                        const int64_t flags = flagsO.value_or(0);
                        std::vector<std::pair<P2, double>> pts;
                        while (i < ents.size() && ents[i].kind == "VERTEX")
                        {
                            const Ent& v = ents[i++];
                            std::optional<int64_t> vflags;
                            if (!v.Int(70, vflags, error))
                                return false;
                            // 样条拟合的多段线还留着框架的控制点；它们不画
                            if ((flags & 4) && (vflags.value_or(0) & 16))
                                continue;
                            std::optional<P2> p;
                            std::optional<double> bulge;
                            if (!v.Point(10, p, error))
                                return false;
                            if (p)
                            {
                                if (!v.Num(42, bulge, error))
                                    return false;
                                pts.emplace_back(*p, bulge.value_or(0.0));
                            }
                        }
                        if (i < ents.size() && ents[i].kind == "SEQEND")
                            ++i;
                        if (flags & (16 | 64))
                        {
                            out.Left("POLYLINE mesh");
                            continue;
                        }
                        shape = PolylineOf(pts, (flags & 1) != 0);
                    }
                    else if (k == "POINT")
                    {
                        std::optional<P2> p;
                        if (!e.Point(10, p, error))
                            return false;
                        if (p)
                            shape = Point{ *p };
                    }
                    else if (k == "ELLIPSE")
                    {
                        if (!EllipseOf(e, shape, error))
                            return false;
                    }
                    else if (k == "SPLINE")
                    {
                        if (!SplineOf(e, shape, error))
                            return false;
                    }
                    else if (k == "TEXT" || k == "ATTRIB")
                    {
                        if (k == "ATTRIB")
                        {
                            std::optional<int64_t> flags;
                            if (!e.Int(70, flags, error))
                                return false;
                            if (flags.value_or(0) & 1)
                                continue;
                        }
                        if (!TextOf(e, shape, error))
                            return false;
                    }
                    else if (k == "MTEXT")
                    {
                        std::vector<Shape> lines;
                        if (!MTextOf(e, lines, error))
                            return false;
                        for (Shape& l : lines)
                            if (!out.Push(Entity{ style.layer, style.color, style.linetype, style.weight, Transformed(std::move(l), xf) }, error))
                                return false;
                        continue;
                    }
                    else if (k == "HATCH")
                    {
                        if (!HatchOf(e, shape, error))
                            return false;
                    }
                    else if (k == "SOLID" || k == "TRACE")
                    {
                        std::optional<P2> a, b, c, d;
                        if (!e.Point(10, a, error) || !e.Point(11, b, error) || !e.Point(12, c, error) || !e.Point(13, d, error))
                            return false;
                        if (a && b && c)
                        {
                            std::vector<P2> ring{ *a, *b };
                            if (d && d->Dist(*c) > 0.0)
                                ring.push_back(*d);
                            ring.push_back(*c);
                            shape = Fill{ { std::move(ring) }, true };
                        }
                    }
                    else if (k == "INSERT")
                    {
                        if (!Insert(out, e, style, xf, depth, error))
                            return false;
                        continue;
                    }
                    else if (k == "DIMENSION")
                    {
                        if (const auto name = e.Str(2))
                            if (!PlaceBlock(out, *name, style, xf, depth, error))
                                return false;
                        continue;
                    }
                    else if (k == "SEQEND" || k == "VERTEX" || k == "ATTDEF")
                        continue;
                    else
                    {
                        out.Left(std::string(k));
                        continue;
                    }
                    if (shape)
                    {
                        if (!out.Push(Entity{ std::move(style.layer), style.color, std::move(style.linetype), style.weight, Transformed(std::move(*shape), xf) },
                                      error))
                            return false;
                    }
                    else
                        out.Left("empty");
                }
                return true;
            }

            // INSERT 说的块，放在它的点，缩放、转过，照它的阵列放几次
            bool Insert(Out& out, const Ent& e, const Style& style, const Xf& xf, size_t depth, std::string& error) const
            {
                const auto name = e.Str(2);
                if (!name)
                {
                    out.Left("INSERT of no block");
                    return true;
                }
                std::optional<P2> atO;
                std::optional<double> sxO, syO, turnO, dxO, dyO;
                std::optional<int64_t> colsO, rowsO;
                if (!e.Point(10, atO, error) || !e.Num(41, sxO, error) || !e.Num(42, syO, error))
                    return false;
                const P2 at = atO.value_or(P2(0.0, 0.0));
                const double sx = sxO.value_or(1.0), sy = syO.value_or(1.0);
                if (sx == 0.0 || sy == 0.0)
                {
                    out.Left("empty");
                    return true;
                }
                if (!e.Num(50, turnO, error) || !e.Int(70, colsO, error) || !e.Int(71, rowsO, error) || !e.Num(44, dxO, error) || !e.Num(45, dyO, error))
                    return false;
                const double turn = Rad(turnO.value_or(0.0));
                const int64_t cols = std::clamp<int64_t>(colsO.value_or(1), 1, MaxArray), rows = std::clamp<int64_t>(rowsO.value_or(1), 1, MaxArray);
                const double dx = dxO.value_or(0.0), dy = dyO.value_or(0.0);
                const P2 ex(std::cos(turn), std::sin(turn)), ey(-std::sin(turn), std::cos(turn));
                for (int64_t row = 0; row < rows; ++row)
                    for (int64_t col = 0; col < cols; ++col)
                    {
                        const P2 o = at + ex * (dx * static_cast<double>(col)) + ey * (dy * static_cast<double>(row));
                        const Xf local{ o, ex * sx, ey * sy };
                        if (!PlaceBlock(out, *name, style, xf.Then(local), depth, error))
                            return false;
                    }
                return true;
            }

            // 名为 name 的块，它的基点在 xf 的原点
            bool PlaceBlock(Out& out, std::string_view name, const Style& style, const Xf& xf, size_t depth, std::string& error) const
            {
                const auto it = m_blocks.find(std::string(name));
                if (it == m_blocks.end())
                {
                    out.Left("INSERT of a block the file doesn't define");
                    return true;
                }
                if (depth >= MaxDepth)
                {
                    out.Left("a block nested deeper than " + std::to_string(MaxDepth));
                    return true;
                }
                const Xf unbased{ P2(-it->second.base.x, -it->second.base.y), P2(1.0, 0.0), P2(0.0, 1.0) };
                const Inherit inh{ style.layer, style.color,
                                   style.linetype ? std::optional<std::string_view>(*style.linetype) : std::nullopt };
                return Place(out, it->second.ents, xf.Then(unbased), &inh, depth + 1, error);
            }

            Pairs                                            m_p;
            std::map<std::string, std::vector<Pair>, std::less<>> m_header;
            std::vector<Layer>                               m_layers;
            std::vector<Linetype>                            m_linetypes;
            std::unordered_map<std::string, Block>           m_blocks;
        };
    }

    std::optional<Rgb> Color::ToRgb() const
    {
        switch (kind)
        {
        case Kind::Index: return Aci(index);
        case Kind::True: return rgb;
        default: return std::nullopt;
        }
    }

    std::optional<Rgb> Aci(uint8_t i)
    {
        switch (i)
        {
        case 1: return Rgb{ 255, 0, 0 };
        case 2: return Rgb{ 255, 255, 0 };
        case 3: return Rgb{ 0, 255, 0 };
        case 4: return Rgb{ 0, 255, 255 };
        case 5: return Rgb{ 0, 0, 255 };
        case 6: return Rgb{ 255, 0, 255 };
        case 8: return Rgb{ 128, 128, 128 };
        case 9: return Rgb{ 192, 192, 192 };
        default: break;
        }
        if (i >= 10 && i <= 249)
        {
            const double hue = static_cast<double>((i - 10) / 10) * 15.0;
            const int shade = (i - 10) % 10;
            static constexpr double highs[] = { 255.0, 204.0, 153.0, 127.0, 76.0 };
            const double high = highs[std::min(shade / 2, 4)];
            const double low = shade % 2 == 1 ? high / 2.0 : 0.0;
            return HueRgb(hue, high, low);
        }
        if (i >= 250)
        {
            static constexpr uint8_t greys[] = { 51, 91, 132, 173, 214, 255 };
            const uint8_t g = greys[i - 250];
            return Rgb{ g, g, g };
        }
        return std::nullopt;
    }

    std::optional<std::array<P2, 2>> Union(const std::vector<std::array<P2, 2>>& boxes)
    {
        if (boxes.empty())
            return std::nullopt;
        std::array<P2, 2> out = boxes.front();
        for (const auto& [a, b] : boxes)
        {
            out[0] = P2(std::min(out[0].x, a.x), std::min(out[0].y, a.y));
            out[1] = P2(std::max(out[1].x, b.x), std::max(out[1].y, b.y));
        }
        return out;
    }

    std::array<P2, 2> Bounds(const Shape& s)
    {
        std::vector<P2> pts;
        if (const Line* l = std::get_if<Line>(&s))
            pts = { l->a, l->b };
        else if (const Polyline* pl = std::get_if<Polyline>(&s))
            pts = pl->pts;
        else if (const Circle* c = std::get_if<Circle>(&s))
            pts = { c->c - P2(c->r, c->r), c->c + P2(c->r, c->r) };
        else if (const Arc* a = std::get_if<Arc>(&s))
        {
            // 两端，以及经过圆心的东、北、西、南之处
            const auto at = [&](double deg) { return a->c + P2(std::cos(Rad(deg)), std::sin(Rad(deg))) * a->r; };
            pts = { at(a->start), at(a->end) };
            for (int k = 0; k < 8; ++k)
            {
                const double q = static_cast<double>(k) * 90.0;
                if (q > a->start && q < a->end)
                    pts.push_back(at(q));
            }
        }
        else if (const Point* p = std::get_if<Point>(&s))
            pts = { p->p };
        else if (const Text* t = std::get_if<Text>(&s))
            pts = { t->at };
        else
            for (const auto& r : std::get<Fill>(s).rings)
                pts.insert(pts.end(), r.begin(), r.end());
        std::vector<std::array<P2, 2>> boxes;
        for (const P2& p : pts)
            boxes.push_back({ p, p });
        return Union(boxes).value_or(std::array<P2, 2>{});
    }

    std::vector<P2> ArcPoints(P2 c, double r, double start, double sweep)
    {
        const size_t n = std::clamp<size_t>(static_cast<size_t>(std::ceil(std::abs(sweep) / 360.0 * static_cast<double>(CurveSteps))), 2, CurveSteps);
        std::vector<P2> out;
        out.reserve(n + 1);
        for (size_t i = 0; i <= n; ++i)
        {
            const double a = Rad(start + sweep * static_cast<double>(i) / static_cast<double>(n));
            out.push_back(c + P2(std::cos(a), std::sin(a)) * r);
        }
        return out;
    }

    const Layer* Drawing::LayerOf(std::string_view name) const
    {
        for (const Layer& l : layers)
            if (l.name == name)
                return &l;
        return nullptr;
    }

    std::optional<std::array<P2, 2>> Drawing::Bounds() const
    {
        std::vector<std::array<P2, 2>> boxes;
        boxes.reserve(entities.size());
        for (const Entity& e : entities)
            boxes.push_back(Dxf::Bounds(e.shape));
        return Union(boxes);
    }

    bool Read(std::string_view bytes, Drawing& out, std::string& error)
    {
        if (bytes.size() > MaxBytes)
        {
            error = "it's " + std::to_string(bytes.size()) + " bytes; up to " + std::to_string(MaxBytes);
            return false;
        }
        if (Bim::Text::StartsWith(bytes, "AutoCAD Binary DXF"))
        {
            error = "it's a binary DXF; save it as an ASCII DXF (any version) and bring that in";
            return false;
        }
        if (Bim::Text::StartsWith(bytes, "\xEF\xBB\xBF"))
            bytes.remove_prefix(3);
        // 2007 以前的文件按代码页写；Latin-1 照原样读出西文，从不失败
        const std::string lossy = Bim::Text::Lossy(bytes);
        if (lossy.size() == bytes.size() && lossy == bytes)
            return ReadText(bytes, out, error);
        std::string latin;
        latin.reserve(bytes.size() * 2);
        for (const char ch : bytes)
            Bim::Text::AppendUtf8(latin, static_cast<char32_t>(static_cast<unsigned char>(ch)));
        return ReadText(latin, out, error);
    }

    bool ReadText(std::string_view text, Drawing& out, std::string& error)
    {
        Reader r(text);
        return r.Read(out, error);
    }
}
