// 移植自 BIMCraft crates/core/src/render.rs、pen.rs、styles.rs、view3d.rs 的 crossings（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Render.h"
#include "Model/Font.h"
#include "Model/Sketch.h"
#include "Model/Text.h"
#include "Model/Walls.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace MiniBIM::Bim::Render
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        // 图纸上每毫米的像素
        constexpr double Density = 5.0;
        // 适配视图时四周留的像素
        constexpr double Margin = 70.0;
        // 轴网圆圈的半径，像素
        constexpr float Bubble = 11.0f;

        // 每支笔在 1 : 100 时纸上的宽度，毫米，笔 0（发丝线）起
        constexpr std::array<double, 17> Widths = { 0.0, 0.10, 0.13, 0.15, 0.18, 0.20, 0.22, 0.25, 0.28, 0.30, 0.35, 0.40, 0.50, 0.70, 1.00, 1.40, 2.00 };

        // 注释在视图里画多大，乘它画的像素
        float AnnotationOf(double n, double scale) { return static_cast<float>(std::clamp(n * scale / Density, 0.1, 10.0)); }

        // 不限范围的 AnnotationOf，定线宽用：恶意的比例或缩放下为 0（发丝线），最多一百万
        float PaperOf(double n, double scale)
        {
            const double k = n * scale / Density;
            return std::isfinite(k) && k > 0.0 ? static_cast<float>(std::min(k, 1e6)) : 0.0f;
        }

        // 类别的笔：看到的、剖到的（BIMCraft styles.rs 的默认对象样式；项目自己的对象样式还没移植）
        std::pair<uint8_t, uint8_t> StyleOf(const std::string& category)
        {
            if (category == "door" || category == "window")
                return { Pen::Projection, Pen::Medium };
            return { Pen::Projection, Pen::Cut };
        }

        // 柱、基础与 run 在平面上占的地方与它的底、顶（BIMCraft 的 stands）
        struct Stand
        {
            Ring   outline;
            double bottom = 0.0, top = 0.0;
            // 基础与 run 挂在画它的标高下面，顶面与视图底部齐平：这样也算看得到
            bool   hangs = false;
        };

        std::optional<Stand> Stands(const Model& m, const Element& e)
        {
            if (const Column* col = e.AsColumn())
            {
                const auto [bottom, top] = m.ColumnSpan(*col);
                return Stand{ m.ColumnOutline(e), bottom, top, e.As<Footing>() != nullptr };
            }
            const auto g = m.RunGeom(e);
            if (!g)
                return std::nullopt;
            const auto [bottom, top] = g->ZSpan();
            return Stand{ g->Footprint(), bottom, top, true };
        }

        // 注释的元素用注释的线宽：轴网、标高、房间的标签，尺寸、标记、文字、高程点、详图线、填充区域（BIMCraft 还有剖面与立面的标记）
        bool Annotates(const Model& m, std::optional<Id> owner)
        {
            const Element* e = owner ? m.elements.Get(*owner) : nullptr;
            return e && (e->As<Grid>() || e->As<Level>() || e->As<Room>() || e->IsAnnotation());
        }

        // 环 ring 被过 p、方向 u 的直线穿过的各段：沿线的起止距离，两两一对（移植自 view3d.rs）
        std::vector<std::pair<double, double>> Crossings(const Ring& ring, P2 p, P2 u)
        {
            std::vector<double> cross;
            for (const auto& [a, b] : Geom::RingEdges(ring))
            {
                const P2 e = b - a;
                const double den = u.Cross(e);
                const double t = std::abs(den) > 1e-12 ? (a - p).Cross(u) / den : -1.0;
                if (std::abs(den) > 1e-12 && t >= 0.0 && t < 1.0)
                    cross.push_back((a - p).Cross(e) / den);
            }
            std::sort(cross.begin(), cross.end());
            std::vector<std::pair<double, double>> out;
            for (size_t i = 0; i + 1 < cross.size(); i += 2)
                out.emplace_back(cross[i], cross[i + 1]);
            return out;
        }

        // 圆心 center 周围从 from 到 to 的短弧，n 段
        std::vector<P2> Arc(P2 center, P2 from, P2 to, int n)
        {
            const P2 a = from - center, b = to - center;
            const double start = std::atan2(a.y, a.x);
            double sweep = std::atan2(b.y, b.x) - start;
            if (sweep > kPi)
                sweep -= 2.0 * kPi;
            else if (sweep < -kPi)
                sweep += 2.0 * kPi;
            std::vector<P2> out;
            for (int i = 0; i <= n; ++i)
            {
                const double t = start + sweep * i / n;
                out.push_back(center + P2(std::cos(t), std::sin(t)) * a.Length());
            }
            return out;
        }

        // 把 b 处的箭头填上：b 到 a 的方向
        P2 Arrow(Drawing& d, Px a, Px b)
        {
            const float len = std::max(std::hypot(b.first - a.first, b.second - a.second), 1e-3f);
            const float x = (b.first - a.first) / len, y = (b.second - a.second) / len;
            const float longer = d.PxOf(10.0f), wide = d.PxOf(4.0f);
            Prim head;
            head.kind = Prim::Kind::Fill;
            head.color = Ink;
            head.rings = { { b, { b.first - x * longer - y * wide, b.second - y * longer + x * wide }, { b.first - x * longer + y * wide, b.second - y * longer - x * wide } } };
            d.Push(std::move(head));
            return P2(x, y);
        }

        // 数照 Rust 的 {} 写，到百分位
        std::string Figure(double x) { return Text::Shortest(std::round(x * 100.0) / 100.0 + 0.0); }

        // 坡度箭头：从 tail 升到 head，中间旁边写坡度
        void SlopeArrow(Drawing& d, P2 tail, P2 head, double slope)
        {
            d.Line({ tail, head }, Pen::Projection, Ink, false);
            const Px a = d.view.ToPx(tail), b = d.view.ToPx(head);
            const P2 dir = Arrow(d, a, b);
            const std::string text = Figure(slope) + "\xC2\xB0";
            // 在它左边（屏幕上）
            const float clear = Font::Width(text, d.PxOf(10.0f)) / 2.0f * std::abs(static_cast<float>(dir.y)) +
                                d.PxOf(6.0f) * std::abs(static_cast<float>(dir.x)) + d.PxOf(4.0f);
            d.Text(text, { (a.first + b.first) / 2.0f + static_cast<float>(dir.y) * clear, (a.second + b.second) / 2.0f - static_cast<float>(dir.x) * clear },
                   d.PxOf(10.0f), Ink);
        }

        // 从上看的构件：每个形体的轮廓，被更高的形体盖住的地方（桌面下的腿）不画
        void ComponentOf(Drawing& d, const std::vector<Fam::Piece>& pieces)
        {
            for (size_t i = 0; i < pieces.size(); ++i)
            {
                const Fam::Piece& p = pieces[i];
                for (const auto& [a, b] : Geom::RingEdges(p.outline))
                {
                    const double len = a.Dist(b);
                    if (len < 1e-9)
                        continue;
                    const P2 u = (b - a) * (1.0 / len);
                    std::vector<std::pair<double, double>> hidden;
                    for (size_t j = 0; j < pieces.size(); ++j)
                        if (j != i && pieces[j].top > p.top + 1e-6)
                            for (const auto& [s0, s1] : Crossings(pieces[j].outline, a, u))
                            {
                                const double lo = std::max(s0, 0.0), hi = std::min(s1, len);
                                if (hi > lo)
                                    hidden.emplace_back(lo, hi);
                            }
                    std::sort(hidden.begin(), hidden.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
                    double at = 0.0;
                    for (const auto& [s0, s1] : hidden)
                    {
                        if (s0 > at)
                            d.Line({ a + u * at, a + u * s0 }, Pen::ByCategory, Ink, false);
                        at = std::max(at, s1);
                    }
                    if (at < len)
                        d.Line({ a + u * at, b }, Pen::ByCategory, Ink, false);
                }
            }
        }

        // ── 注释（移植自 render.rs 的 aligned、angular、radial、tag、note、spot_mark、detail_line、filled_region、styled）──
        Rgb Color(const Rgb3& c) { return Rgb{ c[0], c[1], c[2] }; }

        // text 以像素 at 为中心，大写字母 height 像素高，在屏幕上顺时针转 angle 弧度（y 向下）
        void TextTurned(Drawing& d, std::string_view text, Px at, float height, float angle, Rgb color)
        {
            const float s = std::sin(angle), c = std::cos(angle);
            const size_t from = d.prims.size();
            d.Text(text, { 0.0f, 0.0f }, height, color);
            for (size_t i = from; i < d.prims.size(); ++i)
                for (Px& p : d.prims[i].pts)
                    p = { at.first + p.first * c - p.second * s, at.second + p.first * s + p.second * c };
        }

        // 按线样式画像素 pts：笔、颜色、线型图案（纸上的长度；太密时画实线，偏移线、波浪、字的图案先画成实线）
        void Styled(Drawing& d, const Model& m, const LineStyle& st, std::vector<Px> pts, bool closed)
        {
            const float k = static_cast<float>(Density) * d.paper;
            std::optional<std::vector<float>> dash;
            if (st.pattern)
                if (const auto p = m.linePatterns.find(*st.pattern); p != m.linePatterns.end() && p->second.IsDash())
                    dash = p->second.Dash(k);
            d.Stroke(std::move(pts), st.pen, Color(st.color), std::move(dash), closed);
        }

        // 填充图案 p 填 rings（模型毫米；pieces 是去掉了洞的同样的地方，实心填充用），从 origin 起、转 angle 度。
        // 线距不到一像素时太密看不清，不画；多于一张图能放的也不画
        void FillPatternOf(Drawing& d, const FillPattern& p, const std::vector<Ring>& rings, const std::vector<Ring>& pieces, Rgb color, P2 origin,
                           double angle)
        {
            if (p.IsSolid())
            {
                for (const Ring& piece : pieces)
                    d.Fill({ piece }, color);
                return;
            }
            const Px corner = { static_cast<float>(d.view.width), static_cast<float>(d.view.height) };
            const std::array<P2, 2> window = { d.view.ToMm({ 0.0f, corner.second }), d.view.ToMm({ corner.first, 0.0f }) };
            // 制图图案的毫米是视图的图纸上的
            const double k = p.model ? 1.0 : Density * d.paper / d.view.scale;
            double closest = INFINITY;
            for (const HatchLine& l : p.lines)
                closest = std::min(closest, l.spacing);
            if (!(closest * k * d.view.scale >= 1.0))
                return;
            if (const auto strokes = Fills::Strokes(p, rings, k, window, 50000, origin, angle))
                for (const auto& [a, b] : *strokes)
                    d.Line({ a, b }, Pen::Fine, color, false);
        }

        void FilledRegionOf(Drawing& d, const Model& m, const FilledRegion& r)
        {
            const auto t = m.regionTypes.find(r.typeName);
            if (t == m.regionTypes.end())
                return;
            Sketch::Profile profile;
            std::vector<Sketch::Problem> problems;
            if (!Sketch::Solve(r.sketch, profile, problems))
                return;
            d.SetPart(Part::Pattern);
            std::vector<std::pair<const FillPattern*, Rgb>> layers;
            for (const auto& [name, color] : { std::pair{ &t->second.background, t->second.backgroundColor },
                                               std::pair{ &t->second.foreground, t->second.foregroundColor } })
                if (*name)
                    if (const auto p = m.fillPatterns.find(**name); p != m.fillPatterns.end())
                        layers.emplace_back(&p->second, Color(color));
            for (const Geom::Region& region : profile.regions)
            {
                std::vector<Ring> rings{ region.outer };
                rings.insert(rings.end(), region.holes.begin(), region.holes.end());
                // 去掉洞的轮廓：实心填充与遮罩要它
                std::vector<Ring> pieces{ region.outer };
                for (const Ring& hole : region.holes)
                    pieces = Walls::Minus(pieces, hole);
                if (t->second.masking)
                    for (const Ring& piece : pieces)
                        d.Fill({ piece }, Paper);
                for (const auto& [p, color] : layers)
                    FillPatternOf(d, *p, rings, pieces, color, r.patternOrigin, r.patternAngle);
            }
            d.SetPart(Part::Projection);
            // 每条边界线用自己的线样式，否则区域的；不画的线，或区域不画线又没有自己的样式的，不画
            for (const SketchLine& l : r.sketch)
            {
                if (l.invisible)
                    continue;
                const std::optional<std::string>& name = l.style ? l.style : r.lineStyle;
                const auto st = name ? m.lineStyles.find(*name) : m.lineStyles.end();
                if (st == m.lineStyles.end())
                    continue;
                std::vector<Px> px;
                for (const P2& q : Walls::Axis(l.start, l.end, l.arc).Points(0.0, Walls::Axis(l.start, l.end, l.arc).Length(), 0.0))
                    px.push_back(d.view.ToPx(q));
                Styled(d, m, st->second, std::move(px), false);
            }
        }

        void DetailLineOf(Drawing& d, const Model& m, const DetailLine& l)
        {
            const auto st = m.lineStyles.find(l.style);
            if (st == m.lineStyles.end())
                return;
            // 闭合的按环画，虚线与拐角绕着它的第一个点走
            const bool closed = l.curve.IsClosed();
            std::vector<Px> px;
            for (const P2& q : l.curve.Points())
                px.push_back(d.view.ToPx(q));
            if (closed && px.size() > 2)
                px.pop_back();
            Styled(d, m, st->second, std::move(px), closed);
            // 圆心记号：纸上 3 毫米的十字，实线
            if (l.centreMark)
            {
                const float arm = d.PxOf(static_cast<float>(1.5 * Density));
                for (const P2& c : l.curve.Centres())
                {
                    const Px at = d.view.ToPx(c);
                    d.Stroke({ { at.first - arm, at.second }, { at.first + arm, at.second } }, st->second.pen, Color(st->second.color), std::nullopt, false);
                    d.Stroke({ { at.first, at.second - arm }, { at.first, at.second + arm } }, st->second.pen, Color(st->second.color), std::nullopt, false);
                }
            }
        }

        // 对齐尺寸：从每条参照到尺寸线的尺寸界线，交处一个斜短线，每段的长度写在中间上方，从下面或右边读
        void AlignedOf(Drawing& d, const Model& m, const Dimension& dim)
        {
            const auto [n, cross] = m.Crossings(dim);
            if (cross.empty())
                return;
            const double lo = cross.front().first, hi = cross.back().first;
            // 沿尺寸线、沿参照；px 毫米合一像素
            const P2 u = n.Perp();
            const double px = d.k / d.view.scale;
            const auto on = [&](double s) { return dim.at + n * s; };
            d.Line({ on(lo - 4.0 * px), on(hi + 4.0 * px) }, Pen::Projection, Ink, false);
            for (const auto& [s, seg] : cross)
            {
                const P2 c = on(s);
                // 参照在它方向上的范围，从尺寸线量：尺寸界线从它近的一端外一点画到尺寸线外一点
                const double ta = (seg.first - c).Dot(u), tb = (seg.second - c).Dot(u);
                const double nearLo = std::min(ta, tb), nearHi = std::max(ta, tb);
                double from = -4.0 * px, to = 4.0 * px;
                if (nearLo > 0.0)
                    std::tie(from, to) = std::pair{ nearLo - 3.0 * px, -4.0 * px };
                else if (nearHi < 0.0)
                    std::tie(from, to) = std::pair{ nearHi + 3.0 * px, 4.0 * px };
                d.Line({ c + u * from, c + u * to }, Pen::Projection, Ink, false);
                d.Line({ c - (u + n) * (3.0 * px), c + (u + n) * (3.0 * px) }, Pen::Heavy, Ink, false);
            }
            // 线在屏幕上的角度（y 向下）：它朝东或朝北，字从下面或右边读
            const double angle = std::atan2(-n.y, n.x);
            const Px up = { static_cast<float>(std::sin(angle)), static_cast<float>(-std::cos(angle)) };
            for (size_t i = 0; i + 1 < cross.size(); ++i)
            {
                const double s0 = cross[i].first, s1 = cross[i + 1].first;
                const Px at = d.view.ToPx(on((s0 + s1) / 2.0));
                TextTurned(d, UnitsOf::Length(m.units, s1 - s0), { at.first + up.first * d.PxOf(9.0f), at.second + up.second * d.PxOf(9.0f) },
                           d.PxOf(10.0f), static_cast<float>(angle), Ink);
            }
        }

        // 角度尺寸：过 at 的弧在两条线之间，尺寸界线沿它们伸到弧，两端斜短线，角度写在弧中间外面
        void AngularOf(Drawing& d, const Model& m, const Dimension& dim)
        {
            const auto sector = m.AngleSector(dim);
            if (!sector)
                return;
            const auto [o, from, sweep] = *sector;
            const double r = dim.at.Dist(o);
            if (r <= 0.0)
                return;
            const double px = d.k / d.view.scale;
            const auto ray = [](double t) { return P2(std::cos(t), std::sin(t)); };
            std::vector<P2> arc;
            for (int i = 0; i <= 48; ++i)
                arc.push_back(o + ray(from + sweep * i / 48.0) * r);
            d.Line(arc, Pen::Projection, Ink, false);
            std::vector<Segment> lines;
            for (const Reference& x : dim.refs)
            {
                Segment l;
                std::string why;
                if (m.ReferenceLine(x, l, why))
                    lines.push_back(l);
            }
            for (const double t : { from, from + sweep })
            {
                const P2 u = ray(t);
                // 这条射线上它的线伸到多远：尺寸界线从那里画到弧
                double reach = 0.0;
                for (const auto& [a, b] : lines)
                    if (Geom::Parallel(a, b, o, o + u))
                        reach = std::max({ reach, (a - o).Dot(u), (b - o).Dot(u) });
                if (r > reach + 3.0 * px)
                    d.Line({ o + u * (reach + 3.0 * px), o + u * (r + 4.0 * px) }, Pen::Projection, Ink, false);
                const P2 p = o + u * r, k = (u + u.Perp()) * (3.0 * px);
                d.Line({ p - k, p + k }, Pen::Heavy, Ink, false);
            }
            const std::string text = Figure(sweep * 180.0 / kPi) + "\xC2\xB0";
            const P2 mid = ray(from + sweep / 2.0);
            const double clear = Font::Width(text, 10.0f) / 2.0 * std::abs(mid.x) + 5.0 * std::abs(mid.y) + 4.0;
            d.Text(text, d.view.ToPx(o + mid * (r + clear * px)), d.PxOf(10.0f), Ink);
        }

        // 径向尺寸：从圆心（画一个十字）朝 at 的线，到边处一个箭头，半径写在线端外面
        void RadialOf(Drawing& d, const Model& m, const Dimension& dim)
        {
            P2 c;
            double r = 0.0;
            std::string why;
            if (dim.refs.empty() || !m.ReferenceArc(dim.refs.front(), c, r, why))
                return;
            const P2 u = dim.at.Dist(c) > 0.0 ? (dim.at - c).Unit() : P2(1.0, 0.0);
            const P2 edge = c + u * r, end = dim.at.Dist(c) > r ? dim.at : edge;
            d.Line({ c, end }, Pen::Projection, Ink, false);
            const Px a = d.view.ToPx(c), b = d.view.ToPx(edge);
            // 圆太小放不下时箭头从外面进来
            if (r * d.view.scale > d.PxOf(14.0f) || end == edge)
                Arrow(d, a, b);
            else
                Arrow(d, d.view.ToPx(end), b);
            for (const auto& [dx, dy] : { std::pair{ d.PxOf(4.0f), 0.0f }, std::pair{ 0.0f, d.PxOf(4.0f) } })
                d.Stroke({ { a.first - dx, a.second - dy }, { a.first + dx, a.second + dy } }, Pen::Projection, Ink, std::nullopt, false);
            const std::string text = "R " + UnitsOf::Length(m.units, r);
            const Px at = d.view.ToPx(end);
            const float w = Font::Width(text, d.PxOf(10.0f)) / 2.0f + d.PxOf(6.0f);
            d.Text(text, { at.first + static_cast<float>(u.x) * w, at.second - static_cast<float>(u.y) * w }, d.PxOf(10.0f), Ink);
        }

        // 标记：门的编号在方框里、窗的在六边形里，底下垫纸色；别的东西的字段直接写，垫纸色；有引线时引一条线到它指的点
        void TagOf(Drawing& d, const Model& m, const Tag& t)
        {
            const auto anchor = m.Anchor(t.host);
            if (!anchor)
                return;
            const P2 head = *anchor + t.offset;
            if (t.leader)
                d.Line({ *anchor, head }, Pen::Projection, Ink, false);
            const std::string text = m.TagText(t);
            const Px at = d.view.ToPx(head);
            const float x = at.first, y = at.second;
            const size_t from = d.prims.size();
            const float w = std::max(Font::Width(text, 10.0f) / 2.0f + 5.0f, 8.0f), h = 8.0f;
            const Element* host = m.elements.Get(t.host);
            const bool window = host && host->As<Window>();
            std::vector<Px> ring = window ? std::vector<Px>{ { x - w - 4.0f, y }, { x - w, y - h }, { x + w, y - h }, { x + w + 4.0f, y }, { x + w, y + h }, { x - w, y + h } }
                                          : std::vector<Px>{ { x - w, y - h }, { x + w, y - h }, { x + w, y + h }, { x - w, y + h } };
            Prim fill;
            fill.kind  = Prim::Kind::Fill;
            fill.color = Paper;
            fill.rings = { ring };
            d.Push(std::move(fill));
            if (host && host->AsOpening())
                d.Stroke(ring, Pen::Projection, Ink, std::nullopt, true);
            d.Text(text, at, 10.0f, Ink);
            d.Grow(from, at);
        }

        // 高程点：像素 at 上立着的三角，读的高度写在旁边一条线上，按项目单位，带符号：±0、+3000、-600
        void SpotMark(Drawing& d, Px at, std::optional<double> z, Units units)
        {
            const float x = at.first, y = at.second;
            const size_t from = d.prims.size();
            Prim tri;
            tri.kind  = Prim::Kind::Fill;
            tri.color = Ink;
            tri.rings = { { { x, y }, { x + 5.0f, y - 8.0f }, { x - 5.0f, y - 8.0f } } };
            d.Push(std::move(tri));
            std::string text = "?";
            if (z)
            {
                text = UnitsOf::Length(units, *z);
                // 舍入成零的两边都读作零
                if (text.empty() || text[0] != '-')
                    text = (text == UnitsOf::Length(units, 0.0) ? "\xC2\xB1" : "+") + text;
            }
            const float w = Font::Width(text, 10.0f);
            d.Stroke({ { x - 5.0f, y - 8.0f }, { x + 9.0f + w, y - 8.0f } }, Pen::Projection, Ink, std::nullopt, false);
            d.Text(text, { x + 7.0f + w / 2.0f, y - 16.0f }, 10.0f, Ink);
            d.Grow(from, at);
        }

        // 文字注释：从左上角往下一行行按类型写（单线字；宽度系数、粗、斜、下划线），转它的角度；类型有边框时画边框，有引线时画引线和箭头
        void NoteOf(Drawing& d, const Model& m, const Note& n)
        {
            const auto found = m.textTypes.find(n.typeName);
            const TextType t = found != m.textTypes.end() ? found->second : TextType{};
            // 纸上一毫米多少像素；屏幕上顺时针转（视图里逆时针）
            const float k = d.PxOf(static_cast<float>(Density));
            const float angle = -static_cast<float>(n.angle * kPi / 180.0);
            const float sn = std::sin(angle), cs = std::cos(angle);
            const Px at = d.view.ToPx(n.at);
            const auto frame = [&](double x, double y)
            {
                const float fx = static_cast<float>(x) * k, fy = static_cast<float>(y) * k;
                return Px{ at.first + fx * cs - fy * sn, at.second + fx * sn + fy * cs };
            };
            const Rgb color = Color(t.color);
            // 每个字的样式：类型的，override_format 时按各段改
            struct Look
            {
                bool bold, italic, underline;
                bool operator==(const Look&) const = default;
            };
            std::vector<Look> looks;
            for (size_t i = 0, chars = Text::CharCount(n.text); i < chars; ++i)
            {
                Look l{ t.bold, t.italic, t.underline };
                if (n.overrideFormat)
                    for (const TextRun& r : n.runs)
                        if (r.from <= i && i < r.to)
                        {
                            l.bold      = r.bold.value_or(l.bold);
                            l.italic    = r.italic.value_or(l.italic);
                            l.underline = r.underline.value_or(l.underline);
                        }
                looks.push_back(l);
            }
            const double size = t.size, pitch = TextTypes::LinePitch * t.size;
            double width = 0.0;
            size_t index = 0, row = 0;
            d.SetPart(Part::Text);
            std::string_view rest = n.text;
            for (;;)
            {
                const size_t nl = rest.find('\n');
                const std::string_view line = rest.substr(0, nl);
                const double baseline = size + pitch * row;
                // 一样样式的一段段
                double x = 0.0;
                size_t i = 0;
                const std::vector<std::string_view> chars = [&]
                {
                    std::vector<std::string_view> out;
                    for (size_t p = 0; p < line.size();)
                    {
                        size_t len = 1;
                        const unsigned char c = static_cast<unsigned char>(line[p]);
                        if (c >= 0xF0) len = 4;
                        else if (c >= 0xE0) len = 3;
                        else if (c >= 0xC0) len = 2;
                        len = std::min(len, line.size() - p);
                        out.push_back(line.substr(p, len));
                        p += len;
                    }
                    return out;
                }();
                while (i < chars.size())
                {
                    const Look look = index + i < looks.size() ? looks[index + i] : Look{ t.bold, t.italic, t.underline };
                    size_t j = i;
                    std::string piece;
                    while (j < chars.size() && (index + j < looks.size() ? looks[index + j] : look) == look)
                        piece += chars[j++];
                    const double w = Font::Width(piece, static_cast<float>(size)) * t.widthFactor;
                    const float stroke = std::max(static_cast<float>(size / 10.0 * (look.bold ? 2.0 : 1.0)) * k, 1.0f);
                    for (auto& s : Font::Strokes(piece, 0.0f, static_cast<float>(baseline), static_cast<float>(size)))
                    {
                        Prim p;
                        p.kind  = Prim::Kind::Line;
                        p.width = stroke;
                        p.color = color;
                        for (const auto& [sx, sy] : s)
                        {
                            const double lx = x + sx * t.widthFactor + (look.italic ? (baseline - sy) * 0.2 : 0.0);
                            p.pts.push_back(frame(lx, sy));
                        }
                        d.Push(std::move(p));
                    }
                    if (look.underline)
                    {
                        Prim p;
                        p.kind  = Prim::Kind::Line;
                        p.width = std::max(static_cast<float>(size / 10.0) * k, 1.0f);
                        p.color = color;
                        p.pts   = { frame(x, baseline + 0.2 * size), frame(x + w, baseline + 0.2 * size) };
                        d.Push(std::move(p));
                    }
                    x += w;
                    i = j;
                }
                width = std::max(width, x);
                index += chars.size() + 1;
                ++row;
                if (nl == std::string_view::npos)
                    break;
                rest = rest.substr(nl + 1);
            }
            d.SetPart(Part::Projection);
            const double height = size + pitch * (row - 1);
            const double off = t.border ? t.borderOffset : 0.4 * t.size;
            if (t.border)
                d.Stroke({ frame(-off, -off), frame(width + off, -off), frame(width + off, height + off), frame(-off, height + off) }, t.borderPen, color,
                         std::nullopt, true);
            if (n.leader)
            {
                // 从离它指的地方近的一边引出
                const Px to = d.view.ToPx(*n.leader);
                const float dx = to.first - at.first, dy = to.second - at.second;
                const double along = (dx * cs + dy * sn) / std::max(k, 1e-6f);
                const bool right = along > width / 2.0;
                const Px from = frame(right ? width + off : -off, size / 2.0);
                d.Stroke({ from, to }, Pen::Projection, Ink, std::nullopt, false);
                Arrow(d, from, to);
            }
        }

        // 剖切面在 cut 高度穿过的门窗：墙上的缺口、门窗洞的边，以及门扇和开启线，或玻璃
        void OpeningOf(Drawing& d, const Model& m, const Element& e, const Opening& o, const Wall& w, double cut)
        {
            const auto t = m.OpeningTypeOf(e);
            if (!t)
                return;
            const double bottom = m.OwnBase(w) + o.sill;
            if (!(bottom <= cut && cut < bottom + t->height))
                return;
            const Walls::Axis axis = Walls::Axis::Of(w);
            const auto [ext, inner] = Walls::Faces(m, w);
            const double s0 = o.offset - t->width / 2.0, s1 = o.offset + t->width / 2.0;
            // 也清掉墙面外一点，轮廓的外半边在那里；沿圆弧是圆弧
            const double past = 3.0 / d.view.scale;
            std::vector<P2> gap = axis.Points(s0, s1, inner - past);
            for (const P2& q : axis.Points(s1, s0, ext + past))
                gap.push_back(q);
            d.Fill({ gap }, Paper);
            for (const double s : { s0, s1 })
                d.Line({ axis.At(s, inner), axis.At(s, ext) }, Pen::Heavy, Ink, false);
            // 族的：剖切面穿过的形体的轮廓，以及它的平面线（门的开启线）
            if (o.family)
            {
                for (const Fam::Piece& piece : m.OpeningPieces(e))
                    if (piece.bottom <= cut && cut < piece.top)
                        d.Line(piece.outline, Pen::ByCategory, Ink, true);
                d.SetPart(Part::Projection);
                for (const Ring& stroke : m.OpeningStrokes(e))
                    d.Line(stroke, Pen::ByCategory, Ink, false);
                d.SetPart(Part::Cut);
                return;
            }
            if (e.IsDoor())
            {
                // 开向墙的左边（外侧），翻转时右边；铰在靠起点的门框上，翻转时终点
                const double side = o.flipFacing ? inner : ext;
                const double hinge = o.flipHand ? s1 : s0, free = o.flipHand ? s0 : s1;
                const P2 pivot = axis.At(hinge, side);
                const P2 out = axis.Tangent(hinge).Perp() * (o.flipFacing ? -1.0 : 1.0);
                const P2 shut = axis.At(free, side);
                const P2 open = pivot + out * pivot.Dist(shut);
                d.Line({ pivot, open }, Pen::ByCategory, Ink, false);
                // 门扇扫过的是看到的，不是剖到的
                d.SetPart(Part::Projection);
                d.Line(Arc(pivot, open, shut, 24), Pen::ByCategory, Ink, false);
                d.SetPart(Part::Cut);
            }
            else
                for (const double across : { inner, (ext + inner) / 2.0, ext })
                    d.Line(axis.Points(s0, s1, across), Pen::Projection, Ink, false);
        }

        // 轴网：虚线，两端各一个圆圈写着名称
        void GridOf(Drawing& d, P2 start, P2 end, const std::string& name)
        {
            const Px a = d.view.ToPx(start), b = d.view.ToPx(end);
            d.Stroke({ a, b }, Pen::Projection, GridColor, std::vector<float>{ 14.0f, 4.0f }, false);
            const float len = std::max(std::hypot(b.first - a.first, b.second - a.second), 1e-3f);
            const Px dir = { (b.first - a.first) / len, (b.second - a.second) / len };
            for (const auto& [at, k] : { std::pair{ a, -Bubble }, std::pair{ b, Bubble } })
            {
                const size_t from = d.prims.size();
                const Px c = { at.first + dir.first * k, at.second + dir.second * k };
                std::vector<Px> ring;
                for (int i = 0; i < 32; ++i)
                {
                    const double t = i * kPi / 16.0;
                    ring.emplace_back(c.first + Bubble * static_cast<float>(std::cos(t)), c.second + Bubble * static_cast<float>(std::sin(t)));
                }
                d.Stroke(std::move(ring), Pen::Bubble, GridColor, std::nullopt, true);
                d.Text(name, c, 10.0f, GridColor);
                d.Grow(from, at);
            }
        }

        // 每个剖切轮廓是所剖之物边缘的部分，各成一串点：它的边，去掉外侧贴着别的轮廓的那些段（两面墙相接或一面墙撞上另一面）；
        // 两串相接处各自绕过拐角伸出 reach 毫米（TurnCorners）
        void TurnCorners(std::vector<std::pair<Id, std::vector<std::vector<P2>>>>& runs, double reach)
        {
            std::vector<std::pair<P2, P2>> ends;
            for (const auto& [_, rs] : runs)
                for (const auto& r : rs)
                    if (r.size() >= 2 && r.front().Dist(r.back()) > 1e-9)
                    {
                        ends.emplace_back(r[0], r[1]);
                        ends.emplace_back(r[r.size() - 1], r[r.size() - 2]);
                    }
            const auto on = [&](P2 at, P2 from) -> std::optional<P2>
            {
                for (const auto& [p, q] : ends)
                    if (p.Dist(at) < 1e-6 && q.Dist(from) > 1e-9)
                    {
                        const double len = p.Dist(q);
                        return p + (q - p) * (std::min(reach, len) / len);
                    }
                return std::nullopt;
            };
            for (auto& [_, rs] : runs)
                for (auto& r : rs)
                {
                    const size_t n = r.size();
                    if (n < 2 || r[0].Dist(r[n - 1]) <= 1e-9)
                        continue;
                    const auto head = on(r[0], r[1]), tail = on(r[n - 1], r[n - 2]);
                    if (tail)
                        r.push_back(*tail);
                    if (head)
                        r.insert(r.begin(), *head);
                }
        }

        std::vector<std::pair<Id, std::vector<std::vector<P2>>>> CutEdges(const std::vector<std::pair<Id, Ring>>& outlines, double reach)
        {
            // 在边外多远处找别的轮廓：毫米
            constexpr double PastBy = 1.0;
            std::vector<std::pair<P2, P2>> boxes;
            for (const auto& [_, o] : outlines)
            {
                P2 lo(1e300, 1e300), hi(-1e300, -1e300);
                for (const P2& p : o)
                {
                    lo = P2(std::min(lo.x, p.x), std::min(lo.y, p.y));
                    hi = P2(std::max(hi.x, p.x), std::max(hi.y, p.y));
                }
                boxes.emplace_back(lo, hi);
            }
            const auto near = [&](size_t i, size_t j)
            {
                const auto& [a, b] = boxes[i];
                const auto& [c, dd] = boxes[j];
                return i != j && a.x <= dd.x + PastBy && c.x <= b.x + PastBy && a.y <= dd.y + PastBy && c.y <= b.y + PastBy;
            };
            std::vector<std::pair<Id, std::vector<std::vector<P2>>>> out;
            for (size_t i = 0; i < outlines.size(); ++i)
            {
                const auto& [id, o] = outlines[i];
                std::vector<const Ring*> others;
                for (size_t j = 0; j < outlines.size(); ++j)
                    if (near(i, j))
                        others.push_back(&outlines[j].second);
                if (others.empty())
                {
                    Ring ring = o;
                    if (!o.empty())
                        ring.push_back(o.front());
                    out.push_back({ id, { ring } });
                    continue;
                }
                // 逆时针的环外侧在右边
                const bool ccw = Geom::Area(o) > 0.0;
                std::vector<std::vector<P2>> runs;
                for (const auto& [a, b] : Geom::RingEdges(o))
                {
                    const double len = a.Dist(b);
                    if (len <= 0.0)
                        continue;
                    // 在别的轮廓的边穿过它、或在它上面起止的地方切开
                    std::vector<double> ts{ 0.0, 1.0 };
                    for (const Ring* r : others)
                        for (const auto& [c, e] : Geom::RingEdges(*r))
                        {
                            if (const auto t = Geom::Crossing(a, b, c, e))
                                ts.push_back(*t);
                            for (const P2 p : { c, e })
                                if (Geom::DistToSegment(p, a, b) < 1e-6 * std::max(len, 1.0))
                                    ts.push_back(Geom::ParamOnSegment(p, a, b));
                        }
                    std::sort(ts.begin(), ts.end());
                    std::vector<double> kept;
                    for (const double t : ts)
                        if (kept.empty() || (t - kept.back()) * len >= 1e-6)
                            kept.push_back(t);
                    const P2 outOf = ((b - a).Perp() * (ccw ? -1.0 : 1.0)).Unit() * PastBy;
                    for (size_t k = 0; k + 1 < kept.size(); ++k)
                    {
                        const P2 p = a + (b - a) * kept[k], q = a + (b - a) * kept[k + 1];
                        const P2 probe = (p + q) * 0.5 + outOf;
                        if (std::any_of(others.begin(), others.end(), [&](const Ring* r) { return Geom::Contains(*r, probe); }))
                            continue;
                        if (!runs.empty() && runs.back().back().Dist(p) < 1e-9)
                            runs.back().push_back(q);
                        else
                            runs.push_back({ p, q });
                    }
                }
                // 末尾一串结束在第一串开头处：接着绕下去
                if (runs.size() > 1 && runs.front().front().Dist(runs.back().back()) < 1e-9)
                {
                    std::vector<P2> last = std::move(runs.back());
                    runs.pop_back();
                    last.insert(last.end(), runs[0].begin() + 1, runs[0].end());
                    runs[0] = std::move(last);
                }
                out.push_back({ id, std::move(runs) });
            }
            TurnCorners(out, reach);
            return out;
        }

        // 一串剖切边：回到起点的（没有东西贴着的轮廓）闭合画，免得拐角是线的圆头
        void InkCut(Drawing& d, const std::vector<P2>& run)
        {
            if (run.size() > 3 && run.front().Dist(run.back()) < 1e-9)
                d.Line(std::vector<P2>(run.begin(), run.end() - 1), Pen::ByCategory, Ink, true);
            else
                d.Line(run, Pen::ByCategory, Ink, false);
        }

        // 不考虑族的门窗时的平面范围：level 平面的视图范围（高度）
        PlanRange RangeOf(const Model& m, Guid level) { return m.GetPlanRange(level); }
    }

    // ── View ─────────────────────────────────────────────────────
    Px View::ToPx(P2 p) const
    {
        return { static_cast<float>(width / 2.0 + (p.x - cx) * scale), static_cast<float>(height / 2.0 - (p.y - cy) * scale) };
    }

    P2 View::ToMm(Px p) const
    {
        return P2(cx + (p.first - width / 2.0) / scale, cy - (p.second - height / 2.0) / scale);
    }

    double Pen::Mm() const { return n < Widths.size() ? Widths[n] : 0.0; }

    Weights Weights::At(Units units, double n)
    {
        const bool imperial = UnitsOf::Imperial(units);
        const std::array<double, 6> scales = imperial ? std::array<double, 6>{ 12, 24, 48, 96, 192, 384 } : std::array<double, 6>{ 10, 20, 50, 100, 200, 500 };
        const auto row = [](double k)
        {
            std::array<double, 17> r{};
            for (size_t i = 1; i < r.size(); ++i)
                r[i] = std::clamp(std::round(Widths[i] * k * 100.0) / 100.0, 0.01, 20.0);
            return r;
        };
        // 最近的比例（按粗细几倍算；一样近取细的）
        double best = scales[0];
        for (const double s : scales)
        {
            const double off = std::abs(std::log(s / n)), bestOff = std::abs(std::log(best / n));
            if (off < bestOff || (off == bestOff && s < best))
                best = s;
        }
        Weights w;
        w.model      = row(std::pow(100.0 / best, 0.25));
        w.annotation = row(1.0);
        return w;
    }

    double Weights::Mm(uint8_t pen, bool isAnnotation) const
    {
        const auto& r = isAnnotation ? annotation : model;
        return pen < r.size() ? r[pen] : 0.0;
    }

    // ── Drawing ──────────────────────────────────────────────────
    void Drawing::Push(Prim p, std::optional<uint8_t> pen)
    {
        prims.push_back(std::move(p));
        owners.push_back(m_owner);
        parts.push_back(m_part);
        pens.push_back(pen);
    }

    float Drawing::Width(uint8_t pen) const { return static_cast<float>(weights.Mm(pen, false) * Density) * paper; }

    void Drawing::Stroke(std::vector<Px> pts, uint8_t pen, Rgb color, std::optional<std::vector<float>> dash, bool closed)
    {
        Prim p;
        p.kind   = Prim::Kind::Line;
        p.pts    = std::move(pts);
        p.width  = Width(pen);
        p.color  = color;
        p.dash   = std::move(dash);
        p.closed = closed;
        Push(std::move(p), pen);
    }

    void Drawing::Fill(const std::vector<Ring>& rings, Rgb color)
    {
        if (rings.empty())
            return;
        Prim p;
        p.kind  = Prim::Kind::Fill;
        p.color = color;
        for (const Ring& r : rings)
        {
            Ring turned = Geom::Area(r) < 0.0 ? Ring(r.rbegin(), r.rend()) : r;
            turned.erase(std::unique(turned.begin(), turned.end()), turned.end());
            std::vector<Px> px;
            for (const P2& q : turned)
                px.push_back(view.ToPx(q));
            p.rings.push_back(std::move(px));
        }
        Push(std::move(p));
    }

    void Drawing::Line(const std::vector<P2>& pts, uint8_t pen, Rgb color, bool closed)
    {
        std::vector<Px> px;
        for (const P2& q : pts)
            px.push_back(view.ToPx(q));
        Stroke(std::move(px), pen, color, std::nullopt, closed);
    }

    void Drawing::Dashed(const std::vector<P2>& pts, uint8_t pen, Rgb color, bool closed)
    {
        std::vector<Px> px;
        for (const P2& q : pts)
            px.push_back(view.ToPx(q));
        Stroke(std::move(px), pen, color, std::vector<float>{ 10.0f, 5.0f }, closed);
    }

    void Drawing::Text(std::string_view text, Px at, float height, Rgb color)
    {
        const float w = Font::Width(text, height);
        const Part saved = m_part;
        m_part = Part::Text;
        for (auto& line : Font::Strokes(text, -w / 2.0f, height / 2.0f, height))
        {
            Prim p;
            p.kind  = Prim::Kind::Line;
            p.width = std::max(height / 10.0f, 1.0f);
            p.color = color;
            for (const auto& [a, b] : line)
                p.pts.emplace_back(at.first + a, at.second + b);
            Push(std::move(p));
        }
        m_part = saved;
    }

    void Drawing::Grow(size_t from, Px about)
    {
        if (k == 1.0f)
            return;
        const auto at = [&](Px& p) { p = { about.first + (p.first - about.first) * k, about.second + (p.second - about.second) * k }; };
        for (size_t i = from; i < prims.size(); ++i)
        {
            for (auto& r : prims[i].rings)
                for (Px& p : r)
                    at(p);
            for (Px& p : prims[i].pts)
                at(p);
        }
    }

    std::vector<Id> Drawing::PickAll(Px at, float tol) const
    {
        const P2 p(at.first, at.second);
        const auto ringOf = [](const std::vector<Px>& pts)
        {
            Ring r;
            for (const auto& [x, y] : pts)
                r.emplace_back(x, y);
            return r;
        };
        const auto edges = [&](const Ring& pts, bool closed)
        {
            double best = INFINITY;
            for (size_t i = 0; i + 1 < pts.size(); ++i)
                best = std::min(best, Geom::DistToSegment(p, pts[i], pts[i + 1]));
            if (closed && pts.size() >= 2)
                best = std::min(best, Geom::DistToSegment(p, pts.back(), pts.front()));
            return best;
        };
        // 每个元素最近的一项：距离，以及画得多晚
        std::map<Id, std::pair<double, size_t>> near;
        for (size_t order = 0; order < prims.size(); ++order)
        {
            if (!owners[order])
                continue;
            const Id id = *owners[order];
            const Prim& prim = prims[order];
            double dist = INFINITY;
            if (prim.kind == Prim::Kind::Fill)
            {
                size_t inside = 0;
                for (const auto& r : prim.rings)
                    inside += Geom::Contains(ringOf(r), p) ? 1 : 0;
                if (inside % 2 == 1)
                    dist = -1.0;
                else
                    for (const auto& r : prim.rings)
                        dist = std::min(dist, edges(ringOf(r), true));
            }
            else
                dist = std::max(edges(ringOf(prim.pts), prim.closed) - prim.width / 2.0, 0.0);
            const auto it = near.find(id);
            if (dist <= tol && (it == near.end() || dist <= it->second.first))
                near[id] = { dist, order };
        }
        std::vector<std::pair<Id, std::pair<double, size_t>>> found(near.begin(), near.end());
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b)
        {
            if (a.second.first != b.second.first)
                return a.second.first < b.second.first;
            return a.second.second > b.second.second;
        });
        std::vector<Id> out;
        for (const auto& [id, _] : found)
            out.push_back(id);
        return out;
    }

    std::optional<Id> Drawing::Pick(Px at, float tol) const
    {
        const auto all = PickAll(at, tol);
        if (all.empty())
            return std::nullopt;
        return all.front();
    }

    void Drawing::ByCategory(const Model& m)
    {
        for (size_t i = 0; i < pens.size(); ++i)
        {
            if (pens[i] != Pen::ByCategory)
                continue;
            const Element* e = owners[i] ? m.elements.Get(*owners[i]) : nullptr;
            const auto [projection, cut] = StyleOf(e ? e->Category() : "");
            pens[i] = parts[i] == Part::Cut ? cut : projection;
        }
    }

    void Drawing::Weigh(const Model& m)
    {
        for (size_t i = 0; i < prims.size(); ++i)
        {
            if (!pens[i] || prims[i].kind != Prim::Kind::Line)
                continue;
            prims[i].width = static_cast<float>(weights.Mm(*pens[i], Annotates(m, owners[i])) * Density) * paper;
        }
    }

    // ── 平面 ─────────────────────────────────────────────────────
    View Fit(const std::vector<P2>& pts, uint32_t width, uint32_t height, double n)
    {
        P2 lo(1e300, 1e300), hi(-1e300, -1e300);
        for (const P2& p : pts)
        {
            lo = P2(std::min(lo.x, p.x), std::min(lo.y, p.y));
            hi = P2(std::max(hi.x, p.x), std::max(hi.y, p.y));
        }
        if (pts.empty())
        {
            lo = P2(-5000, -5000);
            hi = P2(5000, 5000);
        }
        // 视图与四周的边一起放得下的最大比例：边随注释放大，注释又随比例，所以二分着找
        const auto fits = [&](double scale)
        {
            const auto room = [&](uint32_t px) { return std::max(px - 2.0 * Margin * std::max<double>(AnnotationOf(n, scale), 1.0), 1.0); };
            return std::max(hi.x - lo.x, 1000.0) * scale <= room(width) && std::max(hi.y - lo.y, 1000.0) * scale <= room(height);
        };
        double fitsAt = 0.0, tooBig = std::max<uint32_t>(std::min(width, height), 1) / 1000.0;
        for (int i = 0; i < 48; ++i)
        {
            const double mid = (fitsAt + tooBig) / 2.0;
            (fits(mid) ? fitsAt : tooBig) = mid;
        }
        return View{ fitsAt, (lo.x + hi.x) / 2.0, (lo.y + hi.y) / 2.0, width, height };
    }

    std::vector<P2> PlanShows(const Model& m, Guid level)
    {
        const PlanRange r = RangeOf(m, level);
        const Walls::Index index(m);
        std::vector<P2> extent;
        for (const auto& [id, e] : m.elements.Items())
        {
            if (const Wall* w = e->As<Wall>())
            {
                const double top = m.TopZ(*w);
                if (m.Spans(*w, r.cut) || (r.depth < top && top <= r.cut))
                    for (const P2& q : Walls::Outline(m, id, &index))
                        extent.push_back(q);
            }
            else if (const auto st = Stands(m, *e))
            {
                const double flush = st->hangs ? Tol : 0.0;
                const auto own = e->AsRun() ? m.LevelOf(*e) : std::nullopt;
                if ((st->bottom <= r.cut && r.cut < st->top) || (r.depth - flush < st->top && st->top <= r.cut) || (own && m.Same(*own, level)))
                    extent.insert(extent.end(), st->outline.begin(), st->outline.end());
            }
            else if (const Floor* f = e->As<Floor>(); f && m.Same(f->level, level))
                extent.insert(extent.end(), f->boundary.begin(), f->boundary.end());
            else if (const Ceiling* c = e->As<Ceiling>(); c && m.Same(c->level, level))
                extent.insert(extent.end(), c->boundary.begin(), c->boundary.end());
            else if (const Room* rm = e->As<Room>(); rm && m.Same(rm->level, level))
                extent.push_back(rm->point);
            else if (const Mass* ms = e->As<Mass>(); ms && m.MassCutBy(*ms, r.cut))
                for (const P2& q : m.MassOutline(*ms, r.cut))
                    extent.push_back(q);
            else if (const Component* cp = e->As<Component>(); cp && m.Same(cp->level, level))
                for (const Fam::Piece& p : m.ComponentPieces(*cp))
                    extent.insert(extent.end(), p.outline.begin(), p.outline.end());
            else if (const Grid* g = e->As<Grid>())
            {
                extent.push_back(g->start);
                extent.push_back(g->end);
            }
            else if (const RoomSeparator* rs = e->As<RoomSeparator>(); rs && m.Same(rs->level, level))
            {
                extent.push_back(rs->start);
                extent.push_back(rs->end);
            }
            else if (const Roof* rf = e->As<Roof>())
            {
                const auto above = m.LevelAbove(level);
                if (m.Same(rf->level, level) || (above && m.Same(rf->level, *above)))
                    for (const P2& q : m.RoofOutline(*rf))
                        extent.push_back(q);
            }
            // 这个平面里的注释
            else if (e->IsAnnotation() && m.AnnotationIn(*e, level))
            {
                if (const DetailLine* l = e->As<DetailLine>())
                    for (const P2& q : l->curve.Points())
                        extent.push_back(q);
                else if (const FilledRegion* fr = e->As<FilledRegion>())
                    for (const SketchLine& l : fr->sketch)
                    {
                        extent.push_back(l.start);
                        extent.push_back(l.end);
                    }
                else if (const Note* n = e->As<Note>())
                    extent.push_back(n->at);
                else if (const Spot* sp = e->As<Spot>())
                    extent.push_back(sp->at);
                else if (const Dimension* dm = e->As<Dimension>())
                    extent.push_back(dm->at);
            }
        }
        return extent;
    }

    Drawing Plan(const Model& m, Guid level, uint32_t width, uint32_t height)
    {
        const double n = UnitsOf::DefaultScale(m.units);
        return Draw(m, level, Fit(PlanShows(m, level), width, height, n));
    }

    Drawing Draw(const Model& m, Guid level, View view)
    {
        const double n = UnitsOf::DefaultScale(m.units);
        const Walls::Index index(m);
        Drawing d(view);
        d.k       = AnnotationOf(n, view.scale);
        d.paper   = PaperOf(n, view.scale);
        d.weights = Weights::At(m.units, n);
        const PlanRange range = RangeOf(m, level);
        // 剖切面以下、视图深度以上看得到顶面的楼板：轮廓与洞口的轮廓，坡楼板的坡度箭头
        for (const auto& [id, e] : m.elements.Items())
        {
            const Floor* f = e->As<Floor>();
            if (!f)
                continue;
            const Plane top = m.FloorTop(*f);
            double lo = INFINITY, hi = -INFINITY;
            for (const P2& q : f->boundary)
            {
                lo = std::min(lo, top.At(q));
                hi = std::max(hi, top.At(q));
            }
            if (hi < range.depth || lo > range.cut)
                continue;
            d.Own(id);
            d.Line(f->boundary, Pen::ByCategory, Faint, true);
            for (const Ring& o : f->openings)
                d.Line(o, Pen::ByCategory, Faint, true);
            if (f->arrow && f->slope != 0.0)
                SlopeArrow(d, (*f->arrow)[0], (*f->arrow)[1], f->slope);
        }
        // 看得到的构件，同 Revit 画家具：只要有一部分在剖切面以下、视图深度以上就整个画
        for (const auto& [id, e] : m.elements.Items())
        {
            const Component* c = e->As<Component>();
            if (!c)
                continue;
            const auto pieces = m.ComponentPieces(*c);
            double bottom = INFINITY, top = -INFINITY;
            for (const Fam::Piece& p : pieces)
            {
                bottom = std::min(bottom, p.bottom);
                top    = std::max(top, p.top);
            }
            if (bottom < range.cut && top > range.depth)
            {
                d.Own(id);
                ComponentOf(d, pieces);
            }
        }
        // 剖切面以下看到的墙与柱；低于视图范围底部的变淡
        for (const auto& [id, e] : m.elements.Items())
        {
            const Wall* w = e->As<Wall>();
            const auto st = w ? std::nullopt : Stands(m, *e);
            if (!w && !st)
                continue;
            const double top = w ? m.TopZ(*w) : st->top;
            const double flush = st && st->hangs ? Tol : 0.0;
            if (!(range.depth - flush < top && top <= range.cut))
                continue;
            d.Own(id);
            d.Line(w ? Walls::Outline(m, id, &index) : st->outline, Pen::ByCategory, top > range.bottom - Tol ? Ink : Faint, true);
        }
        // run 看得到而没剖到处的中心线，淡的虚线：Revit 画梁的记号，也是拾取它的地方
        for (const auto& [id, e] : m.elements.Items())
        {
            const auto g = m.RunGeom(*e);
            if (!g || g->Vertical())
                continue;
            const auto [lo, hi] = g->ZSpan();
            const bool seen = range.depth - Tol < hi && hi <= range.cut;
            const bool over = range.cut < lo && lo <= range.top;
            if (seen || over)
            {
                d.Own(id);
                d.Stroke({ d.view.ToPx(P2{ g->a[0], g->a[1] }), d.view.ToPx(P2{ g->b[0], g->b[1] }) }, Pen::ByCategory, Faint,
                         std::vector<float>{ 14.0f, 5.0f }, false);
            }
        }
        d.SetPart(Part::Cut);
        // 剖到的墙与柱
        std::vector<std::pair<Id, Ring>> outlines;
        for (const auto& [id, e] : m.elements.Items())
            if (const Wall* w = e->As<Wall>(); w && m.Spans(*w, range.cut))
                outlines.emplace_back(id, Walls::Outline(m, id, &index));
            else if (const auto st = Stands(m, *e); st && st->bottom <= range.cut && range.cut < st->top)
                outlines.emplace_back(id, st->outline);
        // 先填色再描轮廓；墙与柱相接处只描并集的边（CutEdges）。剖到的先一起填一次，两块填色相接处不见缝
        d.Own(std::nullopt);
        {
            std::vector<Ring> rings;
            for (const auto& [_, o] : outlines)
                rings.push_back(o);
            d.Fill(rings, Poche);
        }
        // 每面墙、每根柱一块填色：一次奇偶填充里重叠的墙会相互抵消
        for (const auto& [id, o] : outlines)
        {
            d.Own(id);
            d.Fill({ o }, Poche);
        }
        for (const auto& [id, runs] : CutEdges(outlines, 1.0 / d.view.scale))
        {
            d.Own(id);
            for (const auto& run : runs)
                InkCut(d, run);
        }
        // 复合墙的层：各层之间一条细线（BIMCraft 还按材质画剖切图案，填充图案移植后再加）
        for (const auto& [id, e] : m.elements.Items())
        {
            const Wall* w = e->As<Wall>();
            if (!w || !m.Spans(*w, range.cut))
                continue;
            const auto t = m.types.wall.find(w->typeName);
            if (t == m.types.wall.end() || t->second.layers.empty())
                continue;
            d.Own(id);
            const auto lines = Walls::LayerLines(m, id, &index);
            d.SetPart(Part::Pattern);
            for (size_t i = 1; i + 1 < lines.size(); ++i)
                d.Line(lines[i], Pen::Projection, Ink, false);
            d.SetPart(Part::Cut);
        }
        for (const auto& [id, e] : m.elements.Items())
        {
            const Opening* o = e->AsOpening();
            const Wall* w = o ? m.GetWall(o->host) : nullptr;
            if (w && m.Spans(*w, range.cut))
            {
                d.Own(id);
                OpeningOf(d, m, *e, *o, *w, range.cut);
            }
        }
        d.SetPart(Part::Projection);
        // 同一标高的房间在同一组线里找边界
        std::optional<std::vector<Segment>> bounding;
        const auto above = m.LevelAbove(level);
        for (const auto& [id, e] : m.elements.Items())
        {
            d.Own(id);
            if (const Grid* g = e->As<Grid>())
                GridOf(d, g->start, g->end, g->name);
            else if (const Room* r = e->As<Room>(); r && m.Same(r->level, level))
            {
                if (!bounding)
                    bounding = m.RoomLines(level, m.roomArea);
                const auto region = Geom::RegionAt(*bounding, r->point, Tol);
                std::string area = LanguageOf::Word(m.language, "NOT ENCLOSED");
                if (region)
                {
                    const auto [value, unit] = UnitsOf::Area(m.units, region->Area());
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "%.2f %s", value, unit);
                    area = buf;
                }
                const Px at = d.view.ToPx(r->point);
                const size_t from = d.prims.size();
                d.Text(r->name, { at.first, at.second - 16.0f }, 13.0f, Ink);
                d.Text(r->number, at, 10.0f, Ink);
                d.Text(area, { at.first, at.second + 14.0f }, 10.0f, Ink);
                d.Grow(from, at);
            }
            else if (const RoomSeparator* rs = e->As<RoomSeparator>(); rs && m.Same(rs->level, level))
                d.Stroke({ d.view.ToPx(rs->start), d.view.ToPx(rs->end) }, Pen::ByCategory, Ink, std::nullopt, false);
            // 剖切面穿过的体量：那里的轮廓，虚线
            else if (const Mass* ms = e->As<Mass>(); ms && m.MassCutBy(*ms, range.cut))
                d.Dashed(m.MassOutline(*ms, range.cut), Pen::ByCategory, MassColor, true);
            else if (const Roof* rf = e->As<Roof>(); rf && m.Same(rf->level, level))
            {
                // 它所在标高的平面：轮廓，以及坡面相交处的屋脊、斜脊、天沟
                std::vector<Slope> faces;
                std::string why;
                if (m.RoofFaces(*rf, faces, why))
                {
                    for (const auto& [loops, _] : faces)
                        for (const Ring& l : loops)
                            d.Line(l, Pen::ByCategory, Ink, true);
                }
                else
                    d.Line(m.RoofOutline(*rf), Pen::ByCategory, Ink, true);
            }
            else if (rf && above && m.Same(rf->level, *above))
            {
                // 在上方：檐口在视图范围顶部以下时，它的边（天井的也是）画虚线
                std::vector<Slope> faces;
                std::string why;
                double eave = INFINITY;
                if (m.RoofFaces(*rf, faces, why))
                    for (const auto& [loops, p] : faces)
                        for (const Ring& l : loops)
                            for (const P2& q : l)
                                eave = std::min(eave, p.At(q));
                if (range.cut < eave && eave <= range.top)
                {
                    std::vector<Ring> rings{ m.RoofOutline(*rf) };
                    rings.insert(rings.end(), rf->courtyards.begin(), rf->courtyards.end());
                    for (const Ring& ring : rings)
                    {
                        std::vector<Px> px;
                        for (const P2& q : ring)
                            px.push_back(d.view.ToPx(q));
                        d.Stroke(std::move(px), Pen::ByCategory, Ink, std::vector<float>{ 10.0f, 5.0f }, true);
                    }
                }
            }
        }
        // 也在上方：剖切面以上、视图范围顶部以下的 run，虚线
        for (const auto& [id, e] : m.elements.Items())
        {
            const auto g = m.RunGeom(*e);
            if (!g)
                continue;
            const double lo = g->ZSpan().first;
            if (!(range.cut < lo && lo <= range.top))
                continue;
            d.Own(id);
            std::vector<Px> px;
            for (const P2& q : g->Footprint())
                px.push_back(d.view.ToPx(q));
            d.Stroke(std::move(px), Pen::ByCategory, Ink, std::vector<float>{ 10.0f, 5.0f }, true);
        }
        // 填充区域盖在模型上，注释之下
        for (const auto& [id, e] : m.elements.Items())
            if (const FilledRegion* fr = e->As<FilledRegion>(); fr && m.AnnotationIn(*e, level))
            {
                d.Own(id);
                FilledRegionOf(d, m, *fr);
            }
        // 注释最后画，在模型之上
        for (const auto& [id, e] : m.elements.Items())
        {
            if (!e->IsAnnotation() || e->As<FilledRegion>() || !m.AnnotationIn(*e, level))
                continue;
            d.Own(id);
            d.SetPart(Part::Projection);
            if (const Dimension* dm = e->As<Dimension>())
            {
                if (dm->kind == DimKind::Aligned)
                    AlignedOf(d, m, *dm);
                else if (dm->kind == DimKind::Angular)
                    AngularOf(d, m, *dm);
                else
                    RadialOf(d, m, *dm);
            }
            else if (const Tag* t = e->As<Tag>())
                TagOf(d, m, *t);
            else if (const Note* n = e->As<Note>())
                NoteOf(d, m, *n);
            else if (const DetailLine* l = e->As<DetailLine>())
                DetailLineOf(d, m, *l);
            else if (const Spot* sp = e->As<Spot>())
                SpotMark(d, d.view.ToPx(sp->at), m.SpotValue(*sp), m.units);
        }
        d.Own(std::nullopt);
        d.ByCategory(m);
        d.Weigh(m);
        return d;
    }

    Drawing Sketch(const Model& m, Guid level, View view, const Element& e)
    {
        Model with = m;
        const Id id = with.Insert(e);
        const Drawing all = Draw(with, level, view);
        Drawing d(view);
        d.weights = all.weights;
        d.k       = all.k;
        d.paper   = all.paper;
        for (size_t i = 0; i < all.prims.size(); ++i)
            if (all.owners[i] == id)
            {
                d.prims.push_back(all.prims[i]);
                d.owners.push_back(all.owners[i]);
                d.parts.push_back(all.parts[i]);
                d.pens.push_back(all.pens[i]);
            }
        return d;
    }
}
