// 移植自 BIMCraft crates/core/src/sheet.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
// 图纸：纸、标题栏与放在上面的视图。做法与 BIMCraft 不同：不按固定的每毫米像素画好再缩放，而是直接按界面的取景（纸的毫米到屏幕像素）画，
// 各视图按自己的画法画在它的框里；三维视图的样子与剖面看到的形体不在显示列表里，由界面按 Render::Frames 画进框
#include "Model/Render.h"
#include "Model/Font.h"
#include "Model/Language.h"
#include "Model/Schedule.h"
#include "Model/Units.h"
#include <algorithm>
#include <cmath>

namespace MiniBIM::Bim::Render
{
    namespace
    {
        // 平面每 1 : 1 的纸上 1 毫米画多少像素（同 Render.cpp 的 Density：10 像素高的字在纸上 2 毫米）
        constexpr double Density = 5.0;
        // 视图四周给轴网圆圈等留的纸，毫米
        constexpr double Room = 12.0;
        // 右下角标题栏的大小（边框以内），毫米
        constexpr double TitleWidth = 180.0, TitleHeight = 40.0;
        // 明细表：字与标题的字高、行高与标题行高、格子里字两边的空，纸上毫米
        constexpr double CellText = 2.5, TitleText = 3.5, Row = 6.0, TitleRow = 9.0, Pad = 2.0;

        double Dot3(const P3& a, const P3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

        // 明细表在图纸上：标题、各列标题与各行（合计在最后）的文字，各列的宽（毫米）
        struct Table
        {
            std::vector<std::string>              headings;
            std::vector<std::vector<std::string>> rows;
            std::vector<double>                   widths;
            bool                                  totals = false;
        };

        Table TableOf(const Model& m, const Schedule& sc)
        {
            Schedules::Table t;
            std::string error;
            if (!Schedules::TableOf(m, sc, t, error))
                t = {};
            const auto said = [&](const Schedules::Column& c, const Json& v)
            {
                const std::string s = Schedules::Shown(m.units, c.kind, v);
                return v.is_boolean() ? LanguageOf::Word(m.language, s) : s;
            };
            Table out;
            for (const auto& r : t.rows)
            {
                std::vector<std::string> row;
                for (size_t i = 0; i < r.size() && i < t.columns.size(); ++i)
                    row.push_back(said(t.columns[i], r[i]));
                out.rows.push_back(std::move(row));
            }
            if (t.totals)
            {
                std::vector<std::string> row;
                for (size_t i = 0; i < t.totals->size() && i < t.columns.size(); ++i)
                    row.push_back((*t.totals)[i].is_null() ? std::string() : said(t.columns[i], (*t.totals)[i]));
                if (!row.empty() && row.front().empty())
                    row.front() = LanguageOf::Word(m.language, "Total");
                out.rows.push_back(std::move(row));
                out.totals = true;
            }
            for (const Schedules::Column& c : t.columns)
                out.headings.push_back(Schedules::Heading(m.units, Schedules::Column{ LanguageOf::Word(m.language, c.heading), c.kind }));
            for (size_t i = 0; i < out.headings.size(); ++i)
            {
                double widest = Font::Width(out.headings[i], static_cast<float>(CellText));
                for (const auto& r : out.rows)
                    if (i < r.size())
                        widest = std::max<double>(widest, Font::Width(r[i], static_cast<float>(CellText)));
                out.widths.push_back(widest + 2.0 * Pad);
            }
            return out;
        }

        P2 TableSize(const std::string& name, const Table& t)
        {
            double w = 0.0;
            for (const double x : t.widths)
                w += x;
            w = std::max(w, 2.0 * Pad + Font::Width(name, static_cast<float>(TitleText)));
            return { w, TitleRow + Row * static_cast<double>(t.rows.size() + 1) };
        }

        // 三维视图 v 在相机平面上的轴：向右、向上，与看的方向（从模型指向相机）。同 OrbitCamera 与 BIMCraft 的 Camera::axes
        std::array<P3, 3> Axes(const View3D& v)
        {
            constexpr double Deg = 3.14159265358979323846 / 180.0;
            const double a = v.azimuth * Deg, e = std::clamp(v.elevation, -89.0, 89.0) * Deg;
            const P3 back{ std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e) };
            const P3 right{ -std::sin(a), std::cos(a), 0.0 };
            const P3 up{ back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2], back[0] * right[1] - back[1] * right[0] };
            return { right, up, back };
        }

        // 视图 view 按 1 : scale 在纸上要多大（毫米），框的中心是模型的哪一点（三维视图为相机平面上的原点 middle）
        struct Framing
        {
            P2                   size;
            P2                   centre;
            std::optional<P3>    middle;
            bool                 table = false;
        };

        std::optional<Framing> FramingOf(const Model& m, Guid view, double scale)
        {
            const Element* e = m.elements.Get(view);
            if (!e || !(scale > 0.0))
                return std::nullopt;
            if (const Schedule* sc = e->As<Schedule>())
                return Framing{ TableSize(sc->name, TableOf(m, *sc)), {}, std::nullopt, true };
            std::vector<P2> pts;
            std::optional<P3> middle;
            if (m.PlanLevel(view))
                pts = PlanShows(m, view);
            else if (m.GetSection(view))
                pts = SectionShows(m, view, Seen{});
            else if (const View3D* v3 = e->As<View3D>())
            {
                // 建筑的外框从最低到最高：没有墙与楼板时为 10 米见方
                const auto box = m.BuildingBox().value_or(std::array<P2, 2>{ P2{ -5000.0, -5000.0 }, P2{ 5000.0, 5000.0 } });
                const auto [lo, hi] = ModelHeights(m, m.LookOf(view));
                const P3 mid{ (box[0].x + box[1].x) / 2.0, (box[0].y + box[1].y) / 2.0, (lo + hi) / 2.0 };
                const auto [right, up, back] = Axes(*v3);
                double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
                for (int i = 0; i < 8; ++i)
                {
                    const P3 d{ ((i & 1) ? box[1].x : box[0].x) - mid[0], ((i & 2) ? box[1].y : box[0].y) - mid[1], ((i & 4) ? hi : lo) - mid[2] };
                    x0 = std::min(x0, Dot3(d, right));
                    x1 = std::max(x1, Dot3(d, right));
                    y0 = std::min(y0, Dot3(d, up));
                    y1 = std::max(y1, Dot3(d, up));
                }
                const double cx = (x0 + x1) / 2.0, cy = (y0 + y1) / 2.0;
                middle = P3{ mid[0] + right[0] * cx + up[0] * cy, mid[1] + right[1] * cx + up[1] * cy, mid[2] + right[2] * cx + up[2] * cy };
                pts = { P2{ x0 - cx, y0 - cy }, P2{ x1 - cx, y1 - cy } };
            }
            else
                return std::nullopt;
            P2 lo(1e300, 1e300), hi(-1e300, -1e300);
            for (const P2& p : pts)
            {
                lo = P2(std::min(lo.x, p.x), std::min(lo.y, p.y));
                hi = P2(std::max(hi.x, p.x), std::max(hi.y, p.y));
            }
            if (pts.empty())
            {
                lo = P2(-5000.0, -5000.0);
                hi = P2(5000.0, 5000.0);
            }
            const auto side = [&](double d) { return d / scale + 2.0 * Room; };
            return Framing{ P2{ side(hi.x - lo.x), side(hi.y - lo.y) }, (lo + hi) * 0.5, middle, false };
        }

        // text 的左端在纸上 at（毫米），大写字母 height 毫米高
        void Left(Drawing& d, const View& page, std::string_view text, P2 at, double height)
        {
            const float h = static_cast<float>(height * page.scale);
            const Px p = page.ToPx(at);
            d.Text(text, { p.first + Font::Width(text, h) / 2.0f, p.second }, h, Ink);
        }

        std::vector<Px> Rect(const View& page, P2 a, P2 b)
        {
            return { page.ToPx(a), page.ToPx(P2{ b.x, a.y }), page.ToPx(b), page.ToPx(P2{ a.x, b.y }) };
        }

        // 边框（左边留 20 毫米装订，别的边 10 毫米）与右下角的标题栏：项目、图纸名称、比例、图纸编号
        void TitleBlock(Drawing& d, const View& page, const std::string& project, const Sheet& sheet, P2 paper, const std::string& scale)
        {
            d.Stroke(Rect(page, P2{ 20.0, 10.0 }, P2{ paper.x - 10.0, paper.y - 10.0 }), Pen::Heavy, Ink, std::nullopt, true);
            const double width = std::min(TitleWidth, paper.x - 30.0);
            const double x0 = paper.x - 10.0 - width, x1 = paper.x - 10.0, y0 = 10.0, y1 = 10.0 + TitleHeight;
            d.Stroke(Rect(page, P2{ x0, y0 }, P2{ x1, y1 }), Pen::Heavy, Ink, std::nullopt, true);
            d.Stroke({ page.ToPx(P2{ x0, y0 + 26.0 }), page.ToPx(P2{ x1, y0 + 26.0 }) }, Pen::Projection, Ink, std::nullopt, false);
            d.Stroke({ page.ToPx(P2{ x1 - 50.0, y0 }), page.ToPx(P2{ x1 - 50.0, y0 + 26.0 }) }, Pen::Projection, Ink, std::nullopt, false);
            Left(d, page, project, P2{ x0 + 4.0, y0 + 33.0 }, 5.0);
            Left(d, page, sheet.name, P2{ x0 + 4.0, y0 + 17.0 }, 3.5);
            Left(d, page, scale, P2{ x0 + 4.0, y0 + 7.0 }, 2.0);
            d.Text(sheet.number, page.ToPx(P2{ x1 - 25.0, y0 + 13.0 }), static_cast<float>(7.0 * page.scale), Ink);
        }

        // 明细表 sc 画成表格，左上角在纸上 corner（毫米）：标题，再是各列标题与各行，合计在粗一点的线下
        void DrawTable(Drawing& d, const View& page, const Model& m, const Schedule& sc, P2 corner)
        {
            const Table t = TableOf(m, sc);
            const P2 size = TableSize(sc.name, t);
            const double x = corner.x, y = corner.y, w = size.x, h = size.y;
            const auto rule = [&](P2 a, P2 b, uint8_t pen) { d.Stroke({ page.ToPx(a), page.ToPx(b) }, pen, Ink, std::nullopt, false); };
            d.Stroke(Rect(page, P2{ x, y - h }, P2{ x + w, y }), Pen::Heavy, Ink, std::nullopt, true);
            d.Text(sc.name, page.ToPx(P2{ x + w / 2.0, y - TitleRow / 2.0 }), static_cast<float>(TitleText * page.scale), Ink);
            const double top = y - TitleRow;
            rule(P2{ x, top }, P2{ x + w, top }, Pen::Projection);
            const size_t last = t.rows.size();
            for (size_t i = 0; i <= t.rows.size(); ++i)
            {
                const std::vector<std::string>& cells = i == 0 ? t.headings : t.rows[i - 1];
                const double row = top - Row * static_cast<double>(i);
                if (i > 0)
                    rule(P2{ x, row }, P2{ x + w, row }, i == 1 || (t.totals && i == last) ? Pen::Medium : Pen::Fine);
                double left = x;
                for (size_t k = 0; k < cells.size() && k < t.widths.size(); ++k)
                {
                    const float hh = static_cast<float>(CellText * page.scale);
                    const Px at = page.ToPx(P2{ left + Pad, row - Row / 2.0 });
                    d.Text(cells[k], { at.first + Font::Width(cells[k], hh) / 2.0f, at.second }, hh, Ink);
                    left += t.widths[k];
                }
            }
            double left = x;
            for (size_t k = 0; k + 1 < t.widths.size(); ++k)
            {
                left += t.widths[k];
                rule(P2{ left, top }, P2{ left, y - h }, Pen::Fine);
            }
        }

        std::vector<std::pair<Id, const Viewport*>> On(const Model& m, Guid sheet)
        {
            std::vector<std::pair<Id, const Viewport*>> out;
            for (const auto& [id, e] : m.elements.Items())
                if (const Viewport* v = e->As<Viewport>(); v && m.Same(v->sheet, sheet))
                    out.emplace_back(id, v);
            return out;
        }

        // 标题栏写的比例：各视图的比例一样时就是它，否则"比例见图"
        std::string ScaleNote(const Model& m, const std::vector<const Viewport*>& views)
        {
            if (views.empty())
                return {};
            for (const Viewport* v : views)
                if (v->scale != views.front()->scale)
                    return LanguageOf::Word(m.language, "SCALE AS INDICATED");
            return LanguageOf::Word(m.language, "SCALE") + " " + UnitsOf::Scale(m.units, views.front()->scale);
        }
    }

    std::optional<P2> ViewportSize(const Model& m, Guid view, double scale)
    {
        const auto f = FramingOf(m, view, scale);
        return f ? std::optional<P2>(f->size) : std::nullopt;
    }

    P2 SpotOn(const Model& m, Guid sheet, Guid view, double scale)
    {
        const Sheet* sh = m.GetSheet(sheet);
        const P2 paper = (sh ? PaperOf(sh->size) : std::nullopt).value_or(P2{ 420.0, 297.0 });
        const P2 middle{ (20.0 + paper.x - 10.0) / 2.0, (10.0 + TitleHeight + paper.y - 10.0) / 2.0 };
        const P2 taken = ViewportSize(m, view, scale).value_or(P2{ 0.0, 0.0 });
        std::optional<double> right;
        for (const auto& [id, v] : On(m, sheet))
            if (const auto s = ViewportSize(m, v->view, v->scale))
                right = std::max(right.value_or(-1e300), v->at.x + s->x / 2.0);
        // 边框以内，离它 5 毫米
        const double east = paper.x - 15.0, north = paper.y - 15.0;
        if (!right)
            return middle;
        if (*right + 10.0 + taken.x <= east)
            return { *right + 10.0 + taken.x / 2.0, middle.y };
        return { east - taken.x / 2.0, north - taken.y / 2.0 };
    }

    View FitPaper(P2 paper, uint32_t width, uint32_t height)
    {
        View v;
        v.width  = std::max<uint32_t>(width, 1);
        v.height = std::max<uint32_t>(height, 1);
        v.cx     = paper.x / 2.0;
        v.cy     = paper.y / 2.0;
        v.scale  = std::max(std::min((v.width - 40.0) / paper.x, (v.height - 40.0) / paper.y), 0.05);
        return v;
    }

    std::vector<Frame> Frames(const Model& m, Guid sheet, const View& page)
    {
        std::vector<Frame> out;
        for (const auto& [id, vp] : On(m, sheet))
        {
            const auto f = FramingOf(m, vp->view, vp->scale);
            if (!f)
                continue;
            const Px at = page.ToPx(vp->at);
            const float w = static_cast<float>(f->size.x * page.scale), h = static_cast<float>(f->size.y * page.scale);
            Frame fr;
            fr.viewport = id;
            fr.view     = vp->view;
            fr.table    = f->table;
            fr.middle   = f->middle;
            fr.lo       = { at.first - w / 2.0f, at.second - h / 2.0f };
            fr.hi       = { at.first + w / 2.0f, at.second + h / 2.0f };
            // 视图的中心落在框的中心：取景与界面一样大，只是平移、缩放
            const double s = page.scale / vp->scale;
            fr.inner.scale  = s;
            fr.inner.width  = page.width;
            fr.inner.height = page.height;
            fr.inner.cx     = f->centre.x - (at.first - page.width / 2.0) / s;
            fr.inner.cy     = f->centre.y + (at.second - page.height / 2.0) / s;
            out.push_back(fr);
        }
        return out;
    }

    Drawing DrawSheet(const Model& m, Guid sheet, const View& page, const SheetCuts& cuts)
    {
        Drawing d(page);
        const Sheet* sh = m.GetSheet(sheet);
        const auto paper = sh ? PaperOf(sh->size) : std::nullopt;
        if (!paper)
            return d;
        // 纸自己的线（标题栏、视图标题、明细表的线）是注释，按项目默认的比例定宽；视图按各自的比例
        d.weights = Weights::At(m.units, UnitsOf::DefaultScale(m.units));
        d.k       = static_cast<float>(page.scale / Density);
        d.paper   = d.k;
        d.Own(m.elements.IdOf(sheet));
        std::vector<const Viewport*> scaled;
        for (const auto& [id, vp] : On(m, sheet))
        {
            const Element* e = m.elements.Get(vp->view);
            if (e && !e->As<Schedule>())
                scaled.push_back(vp);
        }
        TitleBlock(d, page, m.name, *sh, *paper, ScaleNote(m, scaled));
        for (const Frame& f : Frames(m, sheet, page))
        {
            d.Own(f.viewport);
            const Element* e = m.elements.Get(f.view);
            const Viewport* vp = m.elements.Get(f.viewport)->As<Viewport>();
            if (const Schedule* sc = e->As<Schedule>())
            {
                // 明细表的标题是它的第一行，下面不另写视图标题
                DrawTable(d, page, m, *sc, page.ToMm(f.lo));
                continue;
            }
            std::optional<Drawing> inner;
            if (m.PlanLevel(f.view))
                inner = Draw(m, f.view, f.inner);
            else if (m.GetSection(f.view))
            {
                const auto c = cuts.find(f.view);
                inner = c != cuts.end() ? DrawSection(m, f.view, f.inner, c->second.first, c->second.second)
                                        : DrawSection(m, f.view, f.inner, {}, Seen{});
            }
            // 视图里画的都归视口：点它就选中视口。视图自己的名称（不归任何元素的文字）让给视口的标题
            if (inner)
                for (size_t i = 0; i < inner->prims.size(); ++i)
                {
                    if (!inner->owners[i] && inner->parts[i] == Part::Text)
                        continue;
                    d.SetPart(inner->parts[i]);
                    d.Push(std::move(inner->prims[i]));
                }
            d.SetPart(Part::Projection);
            // 标题在框下：视图名称写在一条线上，比例写在线下
            const std::string* name = m.ViewName(f.view);
            const std::string title = name ? *name : std::string();
            const P2 corner = page.ToMm({ f.lo.first, f.hi.second });
            const double x = corner.x + Room;
            Left(d, page, title, P2{ x, corner.y - 5.0 }, 3.0);
            const double line = Font::Width(title, 3.0f) + 10.0;
            d.Stroke({ page.ToPx(P2{ x, corner.y - 8.0 }), page.ToPx(P2{ x + line, corner.y - 8.0 }) }, Pen::Medium, Ink, std::nullopt, false);
            Left(d, page, UnitsOf::Scale(m.units, vp->scale), P2{ x, corner.y - 11.0 }, 2.0);
        }
        d.Own(std::nullopt);
        return d;
    }
}
