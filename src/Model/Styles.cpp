// 移植自 BIMCraft crates/core/src/lines.rs、fills.rs、text.rs（f3bbb33）的数据部分，MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Styles.h"
#include "Model/Text.h"
#include <algorithm>
#include <cmath>

namespace MiniBIM::Bim
{
    namespace
    {
        // 点画多长，纸上毫米：几乎没有长度，圆头让它成为与线一样宽的点
        constexpr double kDot = 0.01;
        // 重复短于这么多像素的图案画实线：太小看不清，也太多画不完
        constexpr float kSmallestRepeat = 2.0f;
        // 填充图案里的点画多长，图案的长度单位
        constexpr double kHatchDot = 0.15;
        // 一个图案在一张图里最多做多少事（线与边求交、跨过的实虚段）
        constexpr size_t kMaxWork = 5000000;

        std::string Join(const std::vector<std::string>& v)
        {
            std::string out;
            for (const std::string& s : v)
                out += (out.empty() ? "" : ", ") + s;
            return out;
        }

        template <class T>
        std::vector<std::string> Keys(const std::map<std::string, T>& m)
        {
            std::vector<std::string> out;
            for (const auto& [k, _] : m)
                out.push_back(k);
            return out;
        }

        // 过 base、方向 u 的直线落在 lo 到 hi 的框里的那一段：离 base 的距离
        std::optional<std::pair<double, double>> Reach(P2 base, P2 u, P2 lo, P2 hi)
        {
            double t0 = -INFINITY, t1 = INFINITY;
            const double b[2] = { base.x, base.y }, d[2] = { u.x, u.y }, a[2] = { lo.x, lo.y }, z[2] = { hi.x, hi.y };
            for (int i = 0; i < 2; ++i)
            {
                if (std::abs(d[i]) < 1e-12)
                {
                    if (b[i] < a[i] || b[i] > z[i])
                        return std::nullopt;
                }
                else
                {
                    const double x = (a[i] - b[i]) / d[i], y = (z[i] - b[i]) / d[i];
                    t0 = std::max(t0, std::min(x, y));
                    t1 = std::min(t1, std::max(x, y));
                }
            }
            if (!(t0 < t1))
                return std::nullopt;
            return std::pair{ t0, t1 };
        }

        // 过 base、方向 u（n 与它垂直）的直线在 rings 里的各段：两端在边两侧时穿过那条边，相邻两个交点之间轮流在里面、外面
        std::vector<std::pair<double, double>> Spans(const std::vector<Ring>& rings, P2 base, P2 u, P2 n)
        {
            std::vector<double> ts;
            for (const Ring& r : rings)
                for (const auto& [a, b] : Geom::RingEdges(r))
                {
                    const double sa = (a - base).Dot(n), sb = (b - base).Dot(n);
                    if ((sa > 0.0) != (sb > 0.0))
                    {
                        const P2 x = a + (b - a) * (sa / (sa - sb));
                        ts.push_back((x - base).Dot(u));
                    }
                }
            std::sort(ts.begin(), ts.end());
            std::vector<std::pair<double, double>> out;
            for (size_t i = 0; i + 1 < ts.size(); i += 2)
                if (ts[i + 1] > ts[i])
                    out.emplace_back(ts[i], ts[i + 1]);
            return out;
        }

        std::optional<std::vector<std::array<P2, 2>>> StrokesAt(const FillPattern& p, const std::vector<Ring>& rings, double k,
                                                                std::array<P2, 2> window, size_t max)
        {
            std::vector<std::array<P2, 2>> out;
            P2 lo(INFINITY, INFINITY), hi(-INFINITY, -INFINITY);
            size_t edges = 0;
            for (const Ring& r : rings)
            {
                edges += r.size();
                for (const P2& q : r)
                {
                    lo = P2(std::min(lo.x, q.x), std::min(lo.y, q.y));
                    hi = P2(std::max(hi.x, q.x), std::max(hi.y, q.y));
                }
            }
            edges = std::max<size_t>(edges, 1);
            lo = P2(std::max(lo.x, window[0].x), std::max(lo.y, window[0].y));
            hi = P2(std::min(hi.x, window[1].x), std::min(hi.y, window[1].y));
            if (!(lo.x < hi.x && lo.y < hi.y && std::isfinite(k) && k > 0.0))
                return out;
            const P2 corners[4] = { lo, P2(hi.x, lo.y), hi, P2(lo.x, hi.y) };
            size_t work = 0;
            for (const HatchLine& l : p.lines)
            {
                const double a = l.angle * 3.14159265358979323846 / 180.0;
                const P2 u(std::cos(a), std::sin(a)), n(-std::sin(a), std::cos(a));
                const P2 origin = l.origin * k;
                const double step = l.spacing * k, shift = l.shift * k;
                double c0 = INFINITY, c1 = -INFINITY;
                for (const P2& c : corners)
                {
                    const double x = (c - origin).Dot(n);
                    c0 = std::min(c0, x);
                    c1 = std::max(c1, x);
                }
                const double first = std::ceil(c0 / step), last = std::floor(c1 / step);
                if (!std::isfinite(first) || !std::isfinite(last) || last - first > static_cast<double>(max))
                    return std::nullopt;
                const double count = std::max(last - first + 1.0, 0.0);
                work += static_cast<size_t>(count) * edges;
                if (work > kMaxWork)
                    return std::nullopt;
                std::vector<double> dashes;
                double repeat = 0.0;
                for (const double x : l.dashes)
                {
                    dashes.push_back(x * k);
                    repeat += std::abs(x * k);
                }
                if (!dashes.empty() && !(std::isfinite(repeat) && repeat > 0.0))
                    continue;
                for (double j = first; j <= last; j += 1.0)
                {
                    const P2 base = origin + n * (j * step) + u * (j * shift);
                    const auto w = Reach(base, u, lo, hi);
                    if (!w)
                        continue;
                    for (auto [t0, t1] : Spans(rings, base, u, n))
                    {
                        t0 = std::max(t0, w->first);
                        t1 = std::min(t1, w->second);
                        if (!(t1 > t0))
                            continue;
                        if (dashes.empty())
                        {
                            out.push_back({ base + u * t0, base + u * t1 });
                            continue;
                        }
                        for (double r = std::floor(t0 / repeat); r * repeat <= t1; r += 1.0)
                        {
                            double s = r * repeat;
                            for (const double d : dashes)
                            {
                                const double len = std::abs(d);
                                if (d >= 0.0)
                                {
                                    const double x0 = std::max(s, t0), x1 = std::min(s + (d == 0.0 ? kHatchDot * k : len), t1);
                                    if (x1 > x0)
                                        out.push_back({ base + u * x0, base + u * x1 });
                                }
                                s += len;
                            }
                            work += dashes.size();
                            if (out.size() > max || work > kMaxWork)
                                return std::nullopt;
                        }
                    }
                    if (out.size() > max)
                        return std::nullopt;
                }
            }
            return out;
        }
    }

    // ── 线型图案与线样式 ──────────────────────────────────────────
    double LineSegment::Length() const { return kind == Kind::Dot ? kDot : length; }

    bool LinePattern::Check(const std::string& name, std::string& error) const
    {
        const std::string who = "line pattern `" + name + "`";
        if (segments.empty() || segments.size() > Lines::MaxSegments)
        {
            error = who + ": a pattern needs 1 to " + std::to_string(Lines::MaxSegments) + " segments";
            return false;
        }
        const auto valid = [](double v) { return std::isfinite(v) && v >= Lines::MinSegment && v <= Lines::MaxSegment; };
        for (size_t i = 0; i < segments.size(); ++i)
        {
            const LineSegment& s = segments[i];
            if (s.kind != LineSegment::Kind::Dot && !valid(s.Length()))
            {
                error = who + ": segment " + std::to_string(i + 1) + " is " + Text::Shortest(s.Length()) + " mm; a segment is " +
                        Text::Shortest(Lines::MinSegment) + " to " + Text::Shortest(Lines::MaxSegment) + " mm on paper";
                return false;
            }
            bool bad = false;
            switch (s.kind)
            {
            case LineSegment::Kind::Offset:
                bad = s.count < 2 || s.count > 9 || !valid(s.distance);
                break;
            case LineSegment::Kind::Wave:
            case LineSegment::Kind::Zigzag:
                bad = !(std::isfinite(s.frequency) && s.frequency >= 0.1 && s.frequency <= 100.0 && valid(s.amplitude));
                break;
            case LineSegment::Kind::Text:
                bad = !valid(s.size) || Text::Trim(s.text).empty() || Text::CharCount(s.text) > 256 ||
                      std::any_of(s.text.begin(), s.text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
                break;
            default:
                break;
            }
            if (bad)
            {
                error = who + ": segment " + std::to_string(i + 1) +
                        ": offsets need 2–9 lines and positive spacing; waves need 0.1–100 cycles and positive amplitude; text needs "
                        "1–256 characters on one line and a size of 0.05–100 mm";
                return false;
            }
        }
        return true;
    }

    bool LinePattern::IsDash() const
    {
        if (segments.empty() || segments.size() % 2 != 0)
            return false;
        for (size_t i = 0; i < segments.size(); ++i)
        {
            const auto k = segments[i].kind;
            const bool ok = i % 2 == 0 ? (k == LineSegment::Kind::Dash || k == LineSegment::Kind::Dot) : k == LineSegment::Kind::Space;
            if (!ok)
                return false;
        }
        return true;
    }

    std::optional<std::vector<float>> LinePattern::Dash(float pxPerMm) const
    {
        if (!IsDash())
            return std::nullopt;
        std::vector<float> px;
        float repeat = 0.0f;
        for (const LineSegment& s : segments)
        {
            px.push_back(static_cast<float>(s.Length()) * pxPerMm);
            repeat += px.back();
        }
        if (!(std::isfinite(repeat) && repeat >= kSmallestRepeat))
            return std::nullopt;
        return px;
    }

    namespace Lines
    {
        std::map<std::string, LinePattern> Patterns(Language language)
        {
            using K = LineSegment::Kind;
            const auto d = [](double l) { return LineSegment{ K::Dash, l }; };
            const auto s = [](double l) { return LineSegment{ K::Space, l }; };
            const LineSegment dot{ K::Dot };
            const std::pair<const char*, std::vector<LineSegment>> all[] = {
                { "Dash", { d(3.0), s(1.5) } },
                { "Long Dash", { d(8.0), s(2.0) } },
                { "Dot", { dot, s(1.0) } },
                { "Dash Dot", { d(5.0), s(1.5), dot, s(1.5) } },
                { "Hidden", { d(1.5), s(1.0) } },
                { "Centre", { d(10.0), s(1.5), d(2.0), s(1.5) } },
            };
            std::map<std::string, LinePattern> out;
            for (const auto& [n, segs] : all)
                out[LanguageOf::Word(language, n)] = LinePattern{ segs };
            return out;
        }

        std::map<std::string, LineStyle> Styles(Language language)
        {
            const auto pattern = [&](const char* n) { return std::optional<std::string>(LanguageOf::Word(language, n)); };
            const std::pair<const char*, LineStyle> all[] = {
                { "Hairline", { 0, {}, std::nullopt } },
                { Thin, { 1, {}, std::nullopt } },
                { "Medium Lines", { 4, {}, std::nullopt } },
                { "Wide Lines", { 10, {}, std::nullopt } },
                { "Hidden Lines", { 2, {}, pattern("Hidden") } },
                { "Centre Lines", { 2, {}, pattern("Centre") } },
                { "Overhead Lines", { 2, {}, pattern("Dash") } },
            };
            std::map<std::string, LineStyle> out;
            for (const auto& [n, s] : all)
                out[LanguageOf::Word(language, n)] = s;
            return out;
        }

        const std::string* DefaultStyle(const std::map<std::string, LineStyle>& styles, Language language)
        {
            for (const std::string& n : { LanguageOf::Word(language, Thin), std::string(Thin) })
                if (const auto it = styles.find(n); it != styles.end())
                    return &it->first;
            const LineStyle thin{ 1, {}, std::nullopt };
            for (const auto& [n, s] : styles)
                if (s == thin)
                    return &n;
            return styles.empty() ? nullptr : &styles.begin()->first;
        }

        bool CheckStyle(const LineStyle& s, const std::string& name, const std::map<std::string, LinePattern>& patterns, std::string& error)
        {
            const std::string who = "line style `" + name + "`";
            if (s.pen > 16)
            {
                error = who + ": a line weight is a pen, 0 (a hairline) to 16, not " + std::to_string(s.pen);
                return false;
            }
            if (s.pattern && !patterns.count(*s.pattern))
            {
                error = who + ": there's no line pattern `" + *s.pattern + "` (there are: " + Solid + ", " + Join(Keys(patterns)) + ")";
                return false;
            }
            return true;
        }
    }

    // ── 填充图案与填充区域类型 ──────────────────────────────────
    bool FillPattern::Check(const std::string& name, std::string& error) const
    {
        const std::string who = "fill pattern `" + name + "`";
        using namespace Fills;
        if (lines.size() > MaxFamilies)
        {
            error = who + ": " + std::to_string(lines.size()) + " line families, more than " + std::to_string(MaxFamilies);
            return false;
        }
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const HatchLine& l = lines[i];
            const std::string fam = who + " line " + std::to_string(i + 1);
            if (!(std::isfinite(l.angle) && l.origin.IsFinite() && std::isfinite(l.shift) && std::abs(l.shift) <= MaxSpacing))
            {
                error = fam + ": its angle, origin and shift are numbers, the shift within 100000";
                return false;
            }
            if (std::abs(l.origin.x) > MaxSpacing || std::abs(l.origin.y) > MaxSpacing)
            {
                error = fam + ": its origin is within 100000 of the pattern's";
                return false;
            }
            if (!(std::isfinite(l.spacing) && l.spacing >= MinSpacing && l.spacing <= MaxSpacing))
            {
                error = fam + ": its lines are 0.05 to 100000 apart, not " + Text::Shortest(l.spacing);
                return false;
            }
            if (l.dashes.size() > MaxDashes)
            {
                error = fam + ": " + std::to_string(l.dashes.size()) + " dashes and spaces, more than " + std::to_string(MaxDashes);
                return false;
            }
            double sum = 0.0;
            bool space = false, dash = false;
            for (const double x : l.dashes)
            {
                if (!std::isfinite(x) || std::abs(x) > MaxSpacing)
                {
                    error = fam + ": a dash or a space is a length within 100000";
                    return false;
                }
                sum += std::abs(x);
                (x < 0.0 ? space : dash) = true;
            }
            if (!l.dashes.empty() && (sum < MinSpacing || !space || !dash))
            {
                error = fam + ": dashes need a dash (0 is a dot) and a space (a negative length) among them, and 0.05 or more in all";
                return false;
            }
        }
        return true;
    }

    namespace Fills
    {
        std::map<std::string, FillPattern> Patterns(Language language)
        {
            const auto s = [](double angle, double spacing) { return HatchLine{ angle, {}, 0.0, spacing, {} }; };
            const std::pair<const char*, FillPattern> all[] = {
                { SolidFill, { false, {} } },
                { "Diagonal Up", { false, { s(45.0, 1.5) } } },
                { "Diagonal Down", { false, { s(135.0, 1.5) } } },
                { "Diagonal Crosshatch", { false, { s(45.0, 2.0), s(135.0, 2.0) } } },
                { "Crosshatch", { false, { s(0.0, 2.0), s(90.0, 2.0) } } },
                { "Horizontal", { false, { s(0.0, 1.5) } } },
                { "Vertical", { false, { s(90.0, 1.5) } } },
                // 每毫米一个点，下一排错开半步
                { "Sand", { false, { HatchLine{ 0.0, {}, 0.5, 1.0, { 0.0, -1.0 } } } } },
                // 每皮 75 高，225 一条竖缝，上下皮错开半块砖
                { "Brick 225 x 75", { true, { s(0.0, 75.0), HatchLine{ 90.0, {}, 75.0, 112.5, { 75.0, -75.0 } } } } },
                { "Tile 300", { true, { s(0.0, 300.0), s(90.0, 300.0) } } },
            };
            std::map<std::string, FillPattern> out;
            for (const auto& [n, p] : all)
                out[LanguageOf::Word(language, n)] = p;
            return out;
        }

        std::map<std::string, RegionType> RegionTypes(Language language)
        {
            const auto fg = [&](const char* p) { return RegionType{ LanguageOf::Word(language, p) }; };
            RegionType masking;
            masking.masking = true;
            const std::pair<const char*, RegionType> all[] = {
                { SolidBlack, fg(SolidFill) }, { "Diagonal Up", fg("Diagonal Up") }, { "Crosshatch", fg("Crosshatch") },
                { "Sand", fg("Sand") },        { "Brick", fg("Brick 225 x 75") },    { Masking, masking },
            };
            std::map<std::string, RegionType> out;
            for (const auto& [n, t] : all)
                out[LanguageOf::Word(language, n)] = t;
            return out;
        }

        const std::string* DefaultRegionType(const std::map<std::string, RegionType>& types, Language language)
        {
            for (const std::string& n : { LanguageOf::Word(language, SolidBlack), std::string(SolidBlack) })
                if (const auto it = types.find(n); it != types.end())
                    return &it->first;
            return types.empty() ? nullptr : &types.begin()->first;
        }

        bool CheckRegionType(const RegionType& t, const std::string& name, const std::map<std::string, FillPattern>& patterns, std::string& error)
        {
            for (const auto* p : { &t.foreground, &t.background })
                if (*p && !patterns.count(**p))
                {
                    error = "filled region type `" + name + "`: there's no fill pattern `" + **p + "` (there are: " + Join(Keys(patterns)) + ")";
                    return false;
                }
            return true;
        }

        std::optional<std::vector<std::array<P2, 2>>> Strokes(const FillPattern& p, const std::vector<Ring>& rings, double k,
                                                              std::array<P2, 2> window, size_t max, P2 origin, double angle)
        {
            if (origin == P2{} && angle == 0.0)
                return StrokesAt(p, rings, k, window, max);
            const double a = angle * 3.14159265358979323846 / 180.0, sn = std::sin(a), cs = std::cos(a);
            const auto into = [&](P2 q)
            {
                const P2 d = q - origin;
                return P2(d.x * cs + d.y * sn, d.y * cs - d.x * sn);
            };
            const auto back = [&](P2 q) { return origin + P2(q.x * cs - q.y * sn, q.x * sn + q.y * cs); };
            std::vector<Ring> turned;
            for (const Ring& r : rings)
            {
                Ring t;
                for (const P2& q : r)
                    t.push_back(into(q));
                turned.push_back(std::move(t));
            }
            P2 lo(INFINITY, INFINITY), hi(-INFINITY, -INFINITY);
            for (const P2& c : { window[0], P2(window[1].x, window[0].y), window[1], P2(window[0].x, window[1].y) })
            {
                const P2 q = into(c);
                lo = P2(std::min(lo.x, q.x), std::min(lo.y, q.y));
                hi = P2(std::max(hi.x, q.x), std::max(hi.y, q.y));
            }
            auto out = StrokesAt(p, turned, k, { lo, hi }, max);
            if (out)
                for (auto& [x, y] : *out)
                {
                    x = back(x);
                    y = back(y);
                }
            return out;
        }
    }

    // ── 文字类型 ──────────────────────────────────────────────────
    bool TextType::Check(const std::string& name, std::string& error) const
    {
        const std::string who = "text type `" + name + "`";
        const size_t n = Text::CharCount(Text::Trim(font));
        if (n == 0 || n > 256 || std::any_of(font.begin(), font.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; }))
        {
            error = who + ": a font is named by up to 256 characters";
            return false;
        }
        if (!(size > 0.0 && size <= 100.0))
        {
            error = who + ": its size, " + Text::Shortest(size) + " mm, isn't above 0 and at most 100";
            return false;
        }
        if (!(widthFactor >= 0.1 && widthFactor <= 10.0))
        {
            error = who + ": its width factor, " + Text::Shortest(widthFactor) + ", isn't from 0.1 to 10";
            return false;
        }
        if (!(borderOffset >= 0.0 && borderOffset <= 50.0))
        {
            error = who + ": its border offset, " + Text::Shortest(borderOffset) + " mm, isn't from 0 to 50";
            return false;
        }
        if (borderPen > 16)
        {
            error = who + ": its border pen is 0 to 16, not " + std::to_string(borderPen);
            return false;
        }
        return true;
    }

    namespace TextTypes
    {
        std::map<std::string, TextType> Defaults(Language)
        {
            std::map<std::string, TextType> out;
            for (const auto& [size, name] : { std::pair{ 2.5, "2.5mm" }, std::pair{ 3.5, "3.5mm" }, std::pair{ 5.0, "5mm" } })
            {
                TextType t;
                t.size    = size;
                out[name] = t;
            }
            return out;
        }

        const std::string* Default(const std::map<std::string, TextType>& types)
        {
            if (const auto it = types.find("2.5mm"); it != types.end())
                return &it->first;
            return types.empty() ? nullptr : &types.begin()->first;
        }

        std::string ForSize(std::map<std::string, TextType>& types, double size)
        {
            TextType wanted;
            wanted.size = size;
            for (const auto& [n, t] : types)
                if (t == wanted)
                    return n;
            const std::string base = Text::Shortest(std::round(size * 100.0) / 100.0) + "mm";
            std::string name = base;
            for (int i = 2; types.count(name); ++i)
                name = base + " " + std::to_string(i);
            types[name] = wanted;
            return name;
        }

        bool CheckRuns(const std::string& who, const std::vector<TextRun>& runs, size_t len, std::string& error)
        {
            if (runs.size() > MaxRuns)
            {
                error = who + ": it has " + std::to_string(runs.size()) + " formatting runs; a note holds up to " + std::to_string(MaxRuns);
                return false;
            }
            size_t end = 0;
            for (size_t i = 0; i < runs.size(); ++i)
            {
                const TextRun& r = runs[i];
                if (r.from >= r.to || r.to > len)
                {
                    error = who + ": run " + std::to_string(i) + " is of characters " + std::to_string(r.from) + " to " + std::to_string(r.to) +
                            "; it needs some of the text's " + std::to_string(len);
                    return false;
                }
                if (r.from < end)
                {
                    error = who + ": run " + std::to_string(i) + " starts at " + std::to_string(r.from) +
                            ", inside the run before it; give them in order, apart";
                    return false;
                }
                if (!r.bold && !r.italic && !r.underline)
                {
                    error = who + ": run " + std::to_string(i) + " overrides nothing; give it `bold`, `italic` or `underline`";
                    return false;
                }
                end = r.to;
            }
            return true;
        }
    }
}
