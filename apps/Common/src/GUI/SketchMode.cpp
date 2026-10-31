// 移植自 BIMCraft crates/ui-egui/src/sketch.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Commands/Commands.h"
#include "Core/UIContext.h"
#include "GUI/Parameters.h"
#include "I18n/Translator.h"
#include "Model/Geom.h"
#include "Model/ModelJson.h"
#include "Model/Walls.h"
#include "Widgets/Dialog.h"
#include "Widgets/Ribbon.h"
#include "Widgets/UiLayout.h"
#include "Widgets/ViewportHost.h"
#include <algorithm>
#include <cmath>
#include <set>

// 草图模式，照 BIMCraft（同 Revit 的"编辑边界"和新楼板、天花板、屋顶的"边界线"）：轮廓画成零散的直线和圆弧，
// 存在这里，"完成编辑模式"时交给核心解（Bim::Sketch），解不出时说哪里不对、把那几条线标红，接着画。
// 元素记着的线保留 ID 与 GUID，新画的完成时拿到。
//   - 绘制：直线（连续点，回到起点闭合）、矩形（两角）、拾取墙（沿墙的定位线，弧墙为圆弧）；点都经捕捉，也捕捉草图自己的线
//   - 修改：单击选线（Ctrl 加减），框选，拖选中线的端点（连着的线一起动），Delete 删除，Ctrl+Z 退回一步
//   - 完成之前不离开开始的那个平面
namespace MiniBIM
{
    namespace
    {
        constexpr Bim::Render::Rgb Magenta = { 0xC0, 0x26, 0xD3 };      // Revit 的草图线
        constexpr Bim::Render::Rgb Accent  = { 0, 120, 215 };
        constexpr Bim::Render::Rgb Wrong   = { 220, 40, 40 };
        constexpr float PickPx = 6.0f;
        constexpr float GripPx = 8.0f;

        void Stroke(Bim::Render::Drawing& d, std::vector<Bim::Render::Px> pts, float width, Bim::Render::Rgb color, bool closed = false,
                    std::optional<std::vector<float>> dash = std::nullopt)
        {
            Bim::Render::Prim p;
            p.kind   = Bim::Render::Prim::Kind::Line;
            p.pts    = std::move(pts);
            p.width  = width;
            p.color  = color;
            p.closed = closed;
            p.dash   = std::move(dash);
            d.Push(std::move(p));
        }

        Bim::SketchLine Line(Bim::P2 a, Bim::P2 b)
        {
            Bim::SketchLine l;
            l.start = a;
            l.end   = b;
            return l;
        }

        // 线怎么叫：有 ID 的叫"线 ID"，新画的叫"新线 n"
        std::string Name(const Bim::SketchLine& l, size_t i)
        {
            return l.id != 0 ? Format(Tr("Line {0}"), { std::to_string(l.id) }) : Format(Tr("New line {0}"), { std::to_string(i + 1) });
        }
    }

    // =========================================================
    // 开始、完成、取消
    // =========================================================
    void MainFrame::StartSketch(SketchOf of, Bim::Id edit, std::vector<Bim::SketchLine> lines)
    {
        if (!m_planLevel)
            ShowPlan(std::nullopt);
        if (!m_planLevel)
            return;
        SketchState sk;
        sk.of    = of;
        sk.edit  = edit;
        sk.level = *m_planLevel;
        sk.lines = std::move(lines);
        m_sketch = std::move(sk);
        m_selection.clear();
        SetTool(Tool::SketchLine);
        // 功能区换到"修改"：完成、取消与绘制在那里（选项卡的标题是译文）
        if (MiniGUI::Ribbon* ribbon = m_layout ? m_layout->GetRibbon() : nullptr)
            ribbon->SelectTab(ribbon->FindTab(Tr("Modify")));
    }

    bool MainFrame::CanEditBoundary() const
    {
        if (m_sketch || m_selection.size() != 1)
            return false;
        const Bim::Element* e = m_session.GetModel().elements.Get(m_selection.front());
        if (!e)
            return false;
        if (const Bim::Roof* r = e->As<Bim::Roof>())
            return !r->extrusion;
        return e->As<Bim::Floor>() || e->As<Bim::Ceiling>() || e->As<Bim::FilledRegion>();
    }

    void MainFrame::EditBoundary()
    {
        if (!CanEditBoundary())
            return;
        const Bim::Id id = m_selection.front();
        Bim::Json r;
        if (!RunCommand("edit_boundary", Bim::Json{ { "id", id } }, N_("Could not edit the boundary"), &r))
            return;
        // 命令给的线：记着的带 ID 与 GUID
        std::vector<Bim::SketchLine> lines;
        for (const Bim::Json& o : r["sketch"])
        {
            Bim::SketchLine l;
            std::string why;
            Bim::FromJson(o["start"], l.start, why);
            Bim::FromJson(o["end"], l.end, why);
            l.arc = o.value("arc", 0.0);
            if (o.contains("id"))
                l.id = o["id"].get<Bim::Id>();
            if (o.contains("guid"))
                Bim::FromJson(o["guid"], l.guid, why);
            if (o.contains("style") && o["style"].is_string())
                l.style = o["style"].get<std::string>();
            l.invisible = o.value("invisible", false);
            lines.push_back(l);
        }
        StartSketch(SketchOf::Edit, id, std::move(lines));
    }

    std::string MainFrame::FriendlyProblems(const std::vector<Bim::Sketch::Problem>& problems) const
    {
        using K = Bim::Sketch::Problem::Kind;
        const Bim::Units units = m_session.GetModel().units;
        auto named = [&](const std::vector<size_t>& ls)
        {
            std::string out;
            for (const size_t i : ls)
                if (i < m_sketch->lines.size())
                    out = out.empty() ? Name(m_sketch->lines[i], i) : Format(Tr("{0} and {1}"), { out, Name(m_sketch->lines[i], i) });
            return out;
        };
        auto place = [&](const Bim::Sketch::Problem& p)
        {
            if (!p.at)
                return named(p.lines);
            return Format(Tr("{0} at x {1}, y {2}"), { named(p.lines), Bim::UnitsOf::WithUnit(units, p.at->x), Bim::UnitsOf::WithUnit(units, p.at->y) });
        };
        std::vector<std::string> out;
        std::vector<int> ranks;
        for (const auto& p : problems)
            if (ranks.empty() || ranks.back() != p.Rank())
                ranks.push_back(p.Rank());
        for (const int rank : ranks)
        {
            std::vector<const Bim::Sketch::Problem*> these;
            for (const auto& p : problems)
                if (p.Rank() == rank)
                    these.push_back(&p);
            const auto& first = *these.front();
            const std::string n = std::to_string(these.size());
            switch (first.kind)
            {
            case K::Empty:      out.push_back(Tr("There are no lines yet: draw the outline.")); break;
            case K::TooMany:    out.push_back(Tr("The sketch has too many lines.")); break;
            case K::OutOfRange: out.push_back(Tr("A line has a point too far out to draw.")); break;
            case K::TooShort:   out.push_back(Tr("A line is too short: its ends are 1 mm apart or less.")); break;
            case K::Cross:      out.push_back(Format(Tr("Lines intersect ({0}): a loop can't cross itself or another loop."), { n })); break;
            case K::Overlap:    out.push_back(Format(Tr("Lines overlap ({0}): one lies along another, or a line is drawn twice."), { n })); break;
            case K::Branch:     out.push_back(Format(Tr("Three or more lines meet at a point ({0}): a loop passes each point once."), { n })); break;
            case K::Open:       out.push_back(Format(Tr("The loop is open: {0} line ends don't meet another line."), { n })); break;
            case K::NoArea:     out.push_back(Tr("A loop encloses no area.")); break;
            case K::Separate:
                out.push_back(Format(Tr("Draw one outline, with any openings inside it: these lines make {0} separate outlines."), { std::to_string(first.count) }));
                break;
            }
            // 前三处各占一行
            const size_t shown = std::min<size_t>(these.size(), 3);
            bool any = false;
            for (size_t i = 0; i < shown; ++i)
                if (const std::string s = place(*these[i]); !s.empty())
                {
                    out.push_back("  " + s);
                    any = true;
                }
            if (any && these.size() > shown)
                out.push_back("  " + Format(Tr("and {0} more"), { std::to_string(these.size() - shown) }));
        }
        if (std::any_of(problems.begin(), problems.end(), [](const auto& p) { return !p.lines.empty(); }))
            out.push_back(Tr("The lines are shown in red."));
        std::string text;
        for (const std::string& line : out)
            text += (text.empty() ? "" : "\n") + line;
        return text;
    }

    void MainFrame::FinishSketch()
    {
        if (!m_sketch)
            return;
        SketchState& sk = *m_sketch;
        const Bim::Model& m = m_session.GetModel();
        // 填充区域可以是几个环（洞与岛）；楼板、天花板、屋顶只一块
        const Bim::Element* edited = sk.of == SketchOf::Edit ? m.elements.Get(sk.edit) : nullptr;
        const bool loops = sk.of == SketchOf::Region || sk.of == SketchOf::Masking || (edited && edited->As<Bim::FilledRegion>());
        Bim::Geom::Region r;
        Bim::Sketch::Profile profile;
        std::vector<Bim::Sketch::Problem> problems;
        if (loops ? !Bim::Sketch::Solve(sk.lines, profile, problems) : !Bim::Sketch::SolveOne(sk.lines, r, problems))
        {
            sk.wrong.clear();
            for (const auto& p : problems)
                sk.wrong.insert(sk.wrong.end(), p.lines.begin(), p.lines.end());
            MiniGUI::ShowMessageBox(*m_ui, Tr("The sketch isn't finished"), FriendlyProblems(problems));
            m_viewport->RequestRender();
            return;
        }
        const Bim::Json lines = Bim::SketchJson(sk.lines);
        const Bim::Json level = Bim::ToJson(sk.level);
        const char* id = "edit_boundary";
        Bim::Json p;
        switch (sk.of)
        {
        case SketchOf::Floor:
            id = "floor";
            p  = Typed("floor", Bim::Json{ { "level", level }, { "sketch", lines } });
            break;
        case SketchOf::Ceiling:
        {
            id = "ceiling";
            const auto offset = Bim::UnitsOf::Parse(m.units, m_toolOpt.ceilingHeight);
            if (!offset)
            {
                MiniGUI::ShowMessageBox(*m_ui, Tr("Could not place the ceiling"), Format(Tr("`{0}` is not a height"), { m_toolOpt.ceilingHeight }));
                return;
            }
            p = Typed("ceiling", Bim::Json{ { "level", level }, { "sketch", lines }, { "offset", *offset } });
            break;
        }
        case SketchOf::Roof:
        {
            id = "roof";
            Bim::Json slope;
            std::string why;
            if (!Props::Parse(Props::Kind::Number, m_toolOpt.roofSlope, m.units, slope, why))
            {
                MiniGUI::ShowMessageBox(*m_ui, Tr("Could not create the roof"), Format(Tr("`{0}` is not an angle in degrees"), { m_toolOpt.roofSlope }));
                return;
            }
            p = Typed("roof", Bim::Json{ { "level", Bim::ToJson(m.LevelAbove(sk.level).value_or(sk.level)) }, { "slope", slope }, { "sketch", lines } });
            break;
        }
        case SketchOf::Edit:
            p = Bim::Json{ { "id", sk.edit }, { "sketch", lines } };
            break;
        // 填充区域：选项栏选的类型；遮罩区域：遮罩的类型，不画边界线
        case SketchOf::Region:
            id = "filled_region";
            p  = Bim::Json{ { "level", m_planLevel ? Bim::ToJson(PlanKey()) : level }, { "sketch", lines } };
            if (const auto t = m.regionTypes.find(m_toolOpt.regionType); t != m.regionTypes.end() && !t->second.masking)
                p["type"] = m_toolOpt.regionType;
            break;
        case SketchOf::Masking:
        {
            id = "filled_region";
            p  = Bim::Json{ { "level", m_planLevel ? Bim::ToJson(PlanKey()) : level }, { "sketch", lines }, { "line_style", nullptr } };
            const auto t = std::find_if(m.regionTypes.begin(), m.regionTypes.end(), [](const auto& kv) { return kv.second.masking; });
            if (t == m.regionTypes.end())
            {
                MiniGUI::ShowMessageBox(*m_ui, Tr("Could not finish the sketch"), Tr("The project has no masking region type."));
                return;
            }
            p["type"] = t->first;
            break;
        }
        }
        // 解得出，也可能因为它做成的东西被拒绝：命令说为什么
        Bim::Json made;
        if (!RunCommand(id, p, N_("Could not finish the sketch"), &made))
            return;
        m_sketch.reset();
        SetTool(Tool::Modify);
        if (made.contains("id"))
            SetSelection({ made["id"].get<Bim::Id>() });
    }

    void MainFrame::CancelSketch()
    {
        if (!m_sketch)
            return;
        const std::optional<Bim::Id> edited = m_sketch->of == SketchOf::Edit ? std::optional<Bim::Id>(m_sketch->edit) : std::nullopt;
        m_sketch.reset();
        SetTool(Tool::Modify);
        if (edited)
            SetSelection({ *edited });
    }

    // =========================================================
    // 改草图
    // =========================================================
    void MainFrame::SketchChanging()
    {
        m_sketch->history.push_back(m_sketch->lines);
        if (m_sketch->history.size() > 100)
            m_sketch->history.erase(m_sketch->history.begin());
        m_sketch->picked.clear();
        m_sketch->wrong.clear();
        m_commands.NotifyStateChanged();
    }

    void MainFrame::UndoSketch()
    {
        if (!m_sketch || m_sketch->history.empty())
            return;
        m_sketch->lines = std::move(m_sketch->history.back());
        m_sketch->history.pop_back();
        m_sketch->picked.clear();
        m_sketch->wrong.clear();
        m_toolPts.clear();
        m_commands.NotifyStateChanged();
        m_viewport->RequestRender();
    }

    void MainFrame::DeleteSketchLines()
    {
        if (!m_sketch || m_sketch->picked.empty())
            return;
        const std::set<size_t> picked = m_sketch->picked;
        SketchChanging();
        std::vector<Bim::SketchLine> kept;
        for (size_t i = 0; i < m_sketch->lines.size(); ++i)
            if (!picked.contains(i))
                kept.push_back(m_sketch->lines[i]);
        m_sketch->lines = std::move(kept);
        m_viewport->RequestRender();
    }

    std::optional<size_t> MainFrame::SketchLineAt(MiniGUI::Vec2 pixel) const
    {
        if (!m_sketch || !m_planView)
            return std::nullopt;
        const Bim::P2 p = m_planView->ToMm({ pixel.x, pixel.y });
        const double reach = PickPx * m_dpiScale / m_planView->scale;
        std::optional<size_t> best;
        double near = reach;
        for (size_t i = 0; i < m_sketch->lines.size(); ++i)
            if (const double d = Bim::Sketch::Distance(m_sketch->lines[i], p); d <= near)
            {
                near = d;
                best = i;
            }
        return best;
    }

    // 直线：每点一下从上一点画一条；回到起点时环闭合，下一条重新开始
    void MainFrame::SketchPoint(Bim::P2 p)
    {
        if (!m_sketch)
            return;
        if (m_toolPts.empty())
            m_toolPts.push_back(p);
        else if (const Bim::P2 last = m_toolPts.back(); last.Dist(p) > Bim::Tol)
        {
            const bool closes = m_toolPts.front().Dist(p) <= Bim::Tol;
            // 只画了一条就回到起点：第二条会叠在第一条上，两条线围不成环
            if (closes && m_toolPts.size() < 3)
            {
                MiniGUI::ShowMessageBox(*m_ui, Tr("Sketch"), Tr("A loop needs three lines: click a point that isn't the start."));
                return;
            }
            SketchChanging();
            m_sketch->lines.push_back(Line(last, p));
            if (closes)
                m_toolPts.clear();
            else
                m_toolPts.push_back(p);
        }
        UpdateStatus();
        m_viewport->RequestRender();
    }

    void MainFrame::SketchClick(MiniGUI::Vec2 pixel, uint8_t modifiers)
    {
        if (!m_sketch)
            return;
        const bool shift = (modifiers & static_cast<uint8_t>(MiniGUI::ModifierKey::Shift)) != 0;
        switch (m_tool)
        {
        case Tool::SketchLine:
            SketchPoint(SnapAt(pixel, m_toolPts, shift).at);
            return;
        // 矩形：两个对角
        case Tool::SketchRect:
        {
            const Bim::P2 p = SnapAt(pixel, m_toolPts, false).at;
            if (m_toolPts.empty())
            {
                m_toolPts.push_back(p);
                break;
            }
            const Bim::P2 a = m_toolPts.front();
            if (std::abs(a.x - p.x) <= Bim::Tol || std::abs(a.y - p.y) <= Bim::Tol)
                break;
            SketchChanging();
            const Bim::P2 b{ p.x, a.y }, c = p, d{ a.x, p.y };
            for (const auto& [u, w] : { std::pair{ a, b }, std::pair{ b, c }, std::pair{ c, d }, std::pair{ d, a } })
                m_sketch->lines.push_back(Line(u, w));
            m_toolPts.clear();
            break;
        }
        // 拾取墙：沿墙的定位线，弧墙为圆弧
        case Tool::SketchPick:
        {
            m_hoverPixel = pixel;
            if (const auto id = HoveredWall())
                if (const Bim::Wall* w = m_session.GetModel().GetWall(*id))
                {
                    SketchChanging();
                    Bim::SketchLine l = Line(w->start, w->end);
                    l.arc = w->arc;
                    m_sketch->lines.push_back(l);
                }
            break;
        }
        default:
            break;
        }
        UpdateStatus();
        m_viewport->RequestRender();
    }

    bool MainFrame::LeavingSketch()
    {
        if (!m_sketch)
            return false;
        MiniGUI::ShowMessageBox(*m_ui, Tr("Sketch"), Tr("Finish or cancel the sketch before leaving its plan."));
        return true;
    }

    void MainFrame::BeginSketchDrag(MiniGUI::Vec2 pixel)
    {
        m_sketchBox = true;
        m_dragElement.reset();
        m_sketch->grip.reset();
        if (!m_planView)
            return;
        // 从选中线的端点拖：夹点
        const double reach = GripPx * m_dpiScale / m_planView->scale;
        const Bim::P2 p = m_planView->ToMm({ pixel.x, pixel.y });
        for (const size_t i : m_sketch->picked)
            for (const Bim::P2 q : { m_sketch->lines[i].start, m_sketch->lines[i].end })
                if (q.Dist(p) <= reach)
                {
                    m_sketch->grip = q;
                    m_sketchBox    = false;
                    return;
                }
    }

    void MainFrame::FinishSketchDrag(MiniGUI::Vec2 pixel, uint8_t modifiers, bool dragged)
    {
        const bool add = (modifiers & static_cast<uint8_t>(MiniGUI::ModifierKey::Ctrl)) != 0 ||
                         (modifiers & static_cast<uint8_t>(MiniGUI::ModifierKey::Shift)) != 0;
        SketchState& sk = *m_sketch;
        if (dragged && sk.grip)
        {
            // 那一点上所有的线端一起移到放下的地方（捕捉过的）
            const Bim::P2 from = *sk.grip, to = SnapAt(pixel, {}, false).at;
            sk.grip.reset();
            const std::set<size_t> picked = sk.picked;
            SketchChanging();
            for (Bim::SketchLine& l : sk.lines)
            {
                if (l.start.Dist(from) <= Bim::Tol)
                    l.start = to;
                if (l.end.Dist(from) <= Bim::Tol)
                    l.end = to;
            }
            sk.picked = picked;
        }
        else if (dragged && m_sketchBox && m_planView)
        {
            // 从左往右只选整个在框里的，从右往左碰到的都选
            const Bim::P2 a = m_planView->ToMm({ m_downPixel.x, m_downPixel.y }), b = m_planView->ToMm({ pixel.x, pixel.y });
            const double x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
            const bool window = pixel.x >= m_downPixel.x;
            auto inside = [&](Bim::P2 q) { return q.x >= x0 && q.x <= x1 && q.y >= y0 && q.y <= y1; };
            const Bim::Ring box{ { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
            if (!add)
                sk.picked.clear();
            for (size_t i = 0; i < sk.lines.size(); ++i)
            {
                const std::vector<Bim::P2> pts = Bim::Sketch::Points(sk.lines[i]);
                bool in = std::all_of(pts.begin(), pts.end(), inside);
                if (!window && !in)
                {
                    in = std::any_of(pts.begin(), pts.end(), inside);
                    for (size_t k = 0; !in && k + 1 < pts.size(); ++k)
                        for (const auto& [c, d] : Bim::Geom::RingEdges(box))
                            in = in || Bim::Geom::Crossing(pts[k], pts[k + 1], c, d).has_value();
                }
                if (in)
                    sk.picked.insert(i);
            }
        }
        else
        {
            const auto hit = SketchLineAt(pixel);
            if (!add)
                sk.picked.clear();
            if (hit && !sk.picked.erase(*hit))
                sk.picked.insert(*hit);
        }
        m_sketchBox = false;
        m_commands.NotifyStateChanged();
        m_viewport->RequestRender();
    }

    // =========================================================
    // 画
    // =========================================================
    void MainFrame::BuildSketchPreview(Bim::Render::Drawing& d, std::string& tip)
    {
        const SketchState& sk = *m_sketch;
        const Bim::Render::View& v = *m_planView;
        const float px = m_dpiScale;
        const bool gripping = m_planDrag == PlanDrag::Move && sk.grip && m_hoverPixel;
        const Bim::P2 gripTo = gripping ? SnapAt(*m_hoverPixel, {}, false).at : Bim::P2{};
        auto moved = [&](Bim::P2 q) { return gripping && q.Dist(*sk.grip) <= Bim::Tol ? gripTo : q; };
        const std::optional<size_t> hovered = m_tool == Tool::Modify && m_hoverPixel ? SketchLineAt(*m_hoverPixel) : std::nullopt;
        for (size_t i = 0; i < sk.lines.size(); ++i)
        {
            Bim::SketchLine l = sk.lines[i];
            l.start = moved(l.start);
            l.end   = moved(l.end);
            std::vector<Bim::Render::Px> pts;
            for (const Bim::P2& q : Bim::Sketch::Points(l))
                pts.push_back(v.ToPx(q));
            const bool wrong = std::find(sk.wrong.begin(), sk.wrong.end(), i) != sk.wrong.end();
            const bool lit = sk.picked.contains(i) || hovered == i;
            Stroke(d, std::move(pts), (lit || wrong ? 3.0f : 2.0f) * px, lit ? Accent : wrong ? Wrong : Magenta);
            if (hovered == i)
                tip = Name(sk.lines[i], i);
        }
        // 选中线两端的夹点
        for (const size_t i : sk.picked)
            for (const Bim::P2 q : { sk.lines[i].start, sk.lines[i].end })
            {
                const auto [x, y] = v.ToPx(moved(q));
                const float r = 4.0f * px;
                Stroke(d, { { x - r, y - r }, { x + r, y - r }, { x + r, y + r }, { x - r, y + r } }, 1.5f * px, Accent, true);
            }
        if (!m_hoverPixel)
            return;
        switch (m_tool)
        {
        case Tool::SketchLine: case Tool::SketchRect:
        {
            const Bim::P2 next = DrawSnap(d, SnapAt(*m_hoverPixel, m_toolPts, m_hoverShift && m_tool == Tool::SketchLine), tip);
            if (m_toolPts.empty())
                break;
            const Bim::P2 a = m_toolPts.back();
            if (m_tool == Tool::SketchLine)
            {
                Stroke(d, { v.ToPx(a), v.ToPx(next) }, 2.0f * px, Accent);
                tip = Bim::UnitsOf::WithUnit(m_session.GetModel().units, a.Dist(next));
            }
            else
            {
                const Bim::P2 b{ next.x, a.y }, c{ a.x, next.y };
                Stroke(d, { v.ToPx(a), v.ToPx(b), v.ToPx(next), v.ToPx(c) }, 2.0f * px, Accent, true);
                tip = Bim::UnitsOf::WithUnit(m_session.GetModel().units, std::abs(next.x - a.x)) + " × " +
                      Bim::UnitsOf::WithUnit(m_session.GetModel().units, std::abs(next.y - a.y));
            }
            break;
        }
        case Tool::Modify:
            if (m_planDrag == PlanDrag::Move && m_sketchBox)
            {
                const float x0 = m_downPixel.x, y0 = m_downPixel.y, x1 = m_dragNow.x, y1 = m_dragNow.y;
                Stroke(d, { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } }, 1.0f * px, Accent, true,
                       x1 >= x0 ? std::nullopt : std::optional<std::vector<float>>(std::vector<float>{ 6.0f * px, 4.0f * px }));
            }
            else if (gripping)
                DrawSnap(d, SnapAt(*m_hoverPixel, {}, false), tip);
            break;
        default:
            break;
        }
    }
}
