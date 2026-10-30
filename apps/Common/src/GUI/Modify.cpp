// 移植自 BIMCraft crates/ui-egui/src/canvas.rs、lib.rs（f3bbb33）的选择与修改工具，MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Commands/Commands.h"
#include "Core/UIContext.h"
#include "I18n/Translator.h"
#include "Model/Geom.h"
#include "Model/ModelJson.h"
#include "Model/Walls.h"
#include "Widgets/Dialog.h"
#include "Widgets/ViewportHost.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <set>
#include <tuple>

// 修改，照 BIMCraft（同 Revit）：
//   - 单击选中光标下的元素，几个叠在一起时 Tab 轮换；Ctrl 加进或拿出
//   - 在空白处拖：框选。从左往右拖只选整个在框里的（窗选），从右往左拖碰到框的都选（交叉选）
//   - 在元素上拖：移动选择集（没选中的先选中）；选中一面墙时它两端有夹点，拖夹点只动那一端
//   - 修改工具：移动、复制（两点）、旋转（中心默认在选择集中间，两点定角度）、镜像（拾取一面墙或轴网的线，或画两点）、
//     对齐（先拾取参照线，再拾取要对上去的线）、修剪 / 延伸到角（两面墙，点在要保留的部分上）、拆分（单击墙上一点）、
//     偏移（墙复制或移到光标那一侧，距离在选项栏）
namespace MiniBIM
{
    namespace
    {
        constexpr Bim::Render::Rgb Accent = { 0, 120, 215 };
        constexpr float PickPx = 4.0f;      // 拾取元素的范围（逻辑像素）
        constexpr float GripPx = 8.0f;      // 夹点的范围

        void Stroke(Bim::Render::Drawing& d, std::vector<Bim::Render::Px> pts, float width, bool closed = false,
                    std::optional<std::vector<float>> dash = std::nullopt)
        {
            Bim::Render::Prim p;
            p.kind   = Bim::Render::Prim::Kind::Line;
            p.pts    = std::move(pts);
            p.width  = width;
            p.color  = Accent;
            p.closed = closed;
            p.dash   = std::move(dash);
            d.Push(std::move(p));
        }

        Bim::Json Pt(Bim::P2 p) { return Bim::Json::array({ p.x, p.y }); }

        Bim::P2 Round10(Bim::P2 p) { return { std::round(p.x / 10.0) * 10.0, std::round(p.y / 10.0) * 10.0 }; }

        // 方向 d 离 inc 的整倍数 3° 以内时落上去（同 Revit 的角度捕捉）
        Bim::P2 SnapAngle(Bim::P2 d, double inc)
        {
            double a = std::atan2(d.y, d.x);
            if (inc >= 1.0 && inc <= 180.0)
            {
                const double step = inc * std::numbers::pi / 180.0, near = std::round(a / step) * step;
                if (std::abs(a - near) <= std::min(3.0 * std::numbers::pi / 180.0, 0.4 * step))
                    a = near;
            }
            return { std::cos(a), std::sin(a) };
        }

        // 从 centre 看，from 转到 to 的角度（度，逆时针，(-180, 180]），两个方向都落到角度捕捉上
        double AngleBetween(Bim::P2 centre, Bim::P2 from, Bim::P2 to, double inc)
        {
            const Bim::P2 a = SnapAngle(from - centre, inc), b = SnapAngle(to - centre, inc);
            double d = (std::atan2(b.y, b.x) - std::atan2(a.y, a.x)) * 180.0 / std::numbers::pi;
            if (d > 180.0)
                d -= 360.0;
            else if (d <= -180.0)
                d += 360.0;
            return d;
        }

        std::string Degrees(double a)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", std::round(a * 100.0) / 100.0 + 0.0);
            std::string s = buf;
            while (s.back() == '0')
                s.pop_back();
            if (s.back() == '.')
                s.pop_back();
            return s + "°";
        }
    }

    // =========================================================
    // 选择
    // =========================================================
    std::optional<Bim::Id> MainFrame::Hovered() const
    {
        if (!m_plan || !m_hoverPixel)
            return std::nullopt;
        const auto all = m_plan->PickAll({ m_hoverPixel->x, m_hoverPixel->y }, PickPx * m_dpiScale);
        if (all.empty())
            return std::nullopt;
        return all[m_tabIndex % all.size()];
    }

    std::optional<std::pair<Bim::Id, Bim::Align::Line>> MainFrame::HoveredLine() const
    {
        if (!m_plan || !m_hoverPixel || !m_planView)
            return std::nullopt;
        const Bim::Model& m = m_session.GetModel();
        const Bim::P2 mm = m_planView->ToMm({ m_hoverPixel->x, m_hoverPixel->y });
        const double reach = 3.0 * PickPx * m_dpiScale / m_planView->scale;
        std::vector<std::tuple<double, Bim::Id, Bim::Align::Line>> lines;
        for (const Bim::Id id : m_plan->PickAll({ m_hoverPixel->x, m_hoverPixel->y }, 3.0f * PickPx * m_dpiScale))
            if (const auto l = Bim::Align::Nearest(m, id, mm); l && l->second <= reach)
                lines.emplace_back(l->second, id, l->first);
        if (lines.empty())
            return std::nullopt;
        std::stable_sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
        const auto& [_, id, line] = lines[m_tabIndex % lines.size()];
        return std::pair{ id, line };
    }

    std::optional<Bim::Id> MainFrame::HoveredWall() const
    {
        if (!m_plan || !m_hoverPixel)
            return std::nullopt;
        std::vector<Bim::Id> walls;
        for (const Bim::Id id : m_plan->PickAll({ m_hoverPixel->x, m_hoverPixel->y }, PickPx * m_dpiScale))
            if (m_session.GetModel().GetWall(id))
                walls.push_back(id);
        if (walls.empty())
            return std::nullopt;
        return walls[m_tabIndex % walls.size()];
    }

    void MainFrame::PickAt(MiniGUI::Vec2 pixel, uint8_t modifiers)
    {
        m_hoverPixel = pixel;
        const std::optional<Bim::Id> hit = Hovered();
        const bool ctrl = (modifiers & static_cast<uint8_t>(MiniGUI::ModifierKey::Ctrl)) != 0;
        std::vector<Bim::Id> picked = ctrl ? m_selection : std::vector<Bim::Id>{};
        if (hit)
        {
            const auto at = std::find(picked.begin(), picked.end(), *hit);
            if (at == picked.end())
                picked.push_back(*hit);
            else
                picked.erase(at);
        }
        SetSelection(std::move(picked));
    }

    std::optional<Bim::P2> MainFrame::SelectionMiddle() const
    {
        const Bim::Model& m = m_session.GetModel();
        std::vector<Bim::P2> pts;
        for (const Bim::Id id : m_selection)
        {
            const Bim::Element* e = m.elements.Get(id);
            if (!e)
                continue;
            if (const Bim::Wall* w = e->As<Bim::Wall>())
                pts.insert(pts.end(), { w->start, w->end });
            else if (const Bim::Grid* g = e->As<Bim::Grid>())
                pts.insert(pts.end(), { g->start, g->end });
            else if (const Bim::Floor* f = e->As<Bim::Floor>())
                pts.insert(pts.end(), f->boundary.begin(), f->boundary.end());
            else if (const Bim::Ceiling* c = e->As<Bim::Ceiling>())
                pts.insert(pts.end(), c->boundary.begin(), c->boundary.end());
            else if (const Bim::Roof* r = e->As<Bim::Roof>())
                pts.insert(pts.end(), r->boundary.begin(), r->boundary.end());
            else if (const Bim::Room* r = e->As<Bim::Room>())
                pts.push_back(r->point);
            else if (const Bim::Component* c = e->As<Bim::Component>())
                pts.push_back(c->at);
            else if (const Bim::RoomSeparator* s = e->As<Bim::RoomSeparator>())
                pts.insert(pts.end(), { s->start, s->end });
            else if (const Bim::Opening* o = e->AsOpening())
                if (const Bim::Wall* w = m.GetWall(o->host))
                    pts.push_back(Bim::Walls::Axis::Of(*w).At(o->offset, 0.0));
        }
        if (pts.empty())
            return std::nullopt;
        Bim::P2 lo = pts.front(), hi = pts.front();
        for (const Bim::P2& p : pts)
        {
            lo = { std::min(lo.x, p.x), std::min(lo.y, p.y) };
            hi = { std::max(hi.x, p.x), std::max(hi.y, p.y) };
        }
        return Bim::P2{ (lo.x + hi.x) / 2.0, (lo.y + hi.y) / 2.0 };
    }

    // =========================================================
    // 拖动：框选、移动、夹点
    // =========================================================
    void MainFrame::BeginPlanDrag(MiniGUI::Vec2 pixel)
    {
        m_pendingDrag = PlanDrag::Box;
        m_grip.reset();
        m_dragElement.reset();
        if (!m_plan || !m_planView)
            return;
        const Bim::Model& m = m_session.GetModel();
        // 选中的一面墙的两端是夹点
        if (m_selection.size() == 1)
            if (const Bim::Wall* w = m.GetWall(m_selection.front()))
                for (const bool start : { true, false })
                {
                    const auto [x, y] = m_planView->ToPx(start ? w->start : w->end);
                    if (std::hypot(x - pixel.x, y - pixel.y) <= GripPx * m_dpiScale)
                    {
                        m_pendingDrag = PlanDrag::Grip;
                        m_grip = std::pair{ m_selection.front(), start };
                        return;
                    }
                }
        m_hoverPixel = pixel;
        if (const auto id = Hovered())
        {
            m_pendingDrag = PlanDrag::Move;
            m_dragElement = id;
        }
    }

    void MainFrame::UpdatePlanDrag(MiniGUI::Vec2 pixel)
    {
        m_dragNow = pixel;
        // 拖的是没选中的元素：先选中它
        if (m_planDrag == PlanDrag::Move && m_dragElement && !Selected(*m_dragElement))
            SetSelection({ *m_dragElement });
    }

    void MainFrame::FinishPlanDrag(MiniGUI::Vec2 pixel, uint8_t modifiers)
    {
        const PlanDrag drag = m_planDrag;
        m_planDrag = PlanDrag::None;
        if (!m_plan || !m_planView)
            return;
        const Bim::Render::View& v = *m_planView;
        const bool shift = (modifiers & static_cast<uint8_t>(MiniGUI::ModifierKey::Shift)) != 0;
        if (drag == PlanDrag::Box)
        {
            // 每个元素画出来的外框：从左往右拖要整个在框里，从右往左拖碰到就算
            const float x0 = std::min(m_downPixel.x, pixel.x), x1 = std::max(m_downPixel.x, pixel.x);
            const float y0 = std::min(m_downPixel.y, pixel.y), y1 = std::max(m_downPixel.y, pixel.y);
            const bool window = pixel.x >= m_downPixel.x;
            struct Box { float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY; };
            std::map<Bim::Id, Box> boxes;
            const Bim::Render::Drawing& d = *m_plan;
            for (size_t i = 0; i < d.prims.size(); ++i)
            {
                if (!d.owners[i])
                    continue;
                Box& b = boxes[*d.owners[i]];
                auto add = [&](const Bim::Render::Px& q)
                {
                    b.x0 = std::min(b.x0, q.first);  b.y0 = std::min(b.y0, q.second);
                    b.x1 = std::max(b.x1, q.first);  b.y1 = std::max(b.y1, q.second);
                };
                for (const auto& q : d.prims[i].pts)
                    add(q);
                for (const auto& ring : d.prims[i].rings)
                    for (const auto& q : ring)
                        add(q);
            }
            const bool ctrl = (modifiers & static_cast<uint8_t>(MiniGUI::ModifierKey::Ctrl)) != 0;
            std::vector<Bim::Id> picked = ctrl ? m_selection : std::vector<Bim::Id>{};
            for (const auto& [id, b] : boxes)
            {
                const bool in = window ? (b.x0 >= x0 && b.x1 <= x1 && b.y0 >= y0 && b.y1 <= y1)
                                       : (b.x1 >= x0 && b.x0 <= x1 && b.y1 >= y0 && b.y0 <= y1);
                if (in && std::find(picked.begin(), picked.end(), id) == picked.end())
                    picked.push_back(id);
            }
            SetSelection(std::move(picked));
            return;
        }
        if (drag == PlanDrag::Move && !m_selection.empty())
        {
            const Bim::P2 by = Round10(v.ToMm({ pixel.x, pixel.y }) - v.ToMm({ m_downPixel.x, m_downPixel.y }));
            if (by.Length() > Bim::Tol)
                RunCommand("move", Bim::Json{ { "ids", m_selection }, { "by", Pt(by) } }, N_("Could not move"));
            return;
        }
        if (drag == PlanDrag::Grip && m_grip)
        {
            const Bim::Wall* w = m_session.GetModel().GetWall(m_grip->first);
            if (!w)
                return;
            const Bim::P2 fixed = m_grip->second ? w->end : w->start, was = m_grip->second ? w->start : w->end;
            const std::vector<Bim::P2> from{ fixed };
            const Bim::P2 to = SnapAt(pixel, from, shift).at;
            if (to.Dist(was) > Bim::Tol)
                RunCommand("move", Bim::Json{ { "ids", Bim::Json::array({ m_grip->first }) }, { "by", Pt(to - was) },
                                              { "end", m_grip->second ? "start" : "end" } }, N_("Could not move"));
        }
    }

    // =========================================================
    // 修改工具
    // =========================================================
    void MainFrame::Edited(const Bim::Json& made, bool copied)
    {
        std::vector<Bim::Id> ids;
        if (copied && made.contains("ids"))
            for (const Bim::Json& v : made["ids"])
                ids.push_back(v.get<Bim::Id>());
        SetTool(Tool::Modify);
        if (copied)
            SetSelection(std::move(ids));
    }

    void MainFrame::ModifyClick(MiniGUI::Vec2 pixel, uint8_t modifiers)
    {
        const Bim::Model& m = m_session.GetModel();
        const Bim::Render::View& v = *m_planView;
        const bool shift = (modifiers & static_cast<uint8_t>(MiniGUI::ModifierKey::Shift)) != 0;
        const Bim::P2 mm = v.ToMm({ pixel.x, pixel.y });
        m_hoverPixel = pixel;
        const std::optional<Bim::Id> hovered = m_tool == Tool::Trim || m_tool == Tool::Split || m_tool == Tool::Offset ? HoveredWall() : Hovered();
        const Bim::Json ids = m_selection;
        switch (m_tool)
        {
        // 两点：基点，再是它去的地方
        case Tool::Move: case Tool::Copy:
        {
            const bool copy = m_tool == Tool::Copy;
            const Bim::P2 p = SnapAt(pixel, m_toolPts, shift || m_modifyOpt.constrain).at;
            if (m_toolPts.empty())
            {
                m_toolPts.push_back(p);
                break;
            }
            const Bim::P2 by = p - m_toolPts.front();
            if (by.Length() <= Bim::Tol)
                break;
            Bim::Json made;
            if (!RunCommand(copy ? "copy" : "move", Bim::Json{ { "ids", ids }, { "by", Pt(Bim::Micron(by)) } },
                            copy ? N_("Could not copy") : N_("Could not move"), &made))
                break;
            // 复制"多个"时接着从同一个基点复制
            if (copy && m_modifyOpt.multiple)
                break;
            Edited(made, copy);
            break;
        }
        case Tool::Rotate:
        {
            if (m_placeCentre || !m_rotateCentre)
            {
                m_rotateCentre = SnapAt(pixel, {}, false).at;
                m_placeCentre  = false;
                BuildOptionsBar();
                break;
            }
            if (m_toolPts.empty())
            {
                m_toolPts.push_back(mm);
                break;
            }
            const double angle = AngleBetween(*m_rotateCentre, m_toolPts.front(), mm, m_snaps.angle);
            m_toolPts.clear();
            Bim::Json made;
            if (RunCommand("rotate", Bim::Json{ { "ids", ids }, { "about", Pt(*m_rotateCentre) }, { "angle", angle }, { "copy", m_modifyOpt.rotateCopy } },
                           N_("Could not rotate"), &made))
                Edited(made, m_modifyOpt.rotateCopy);
            break;
        }
        // 拾取一面墙或轴网（任何有直线的元素）离光标最近的线作轴
        case Tool::MirrorPick:
            if (const auto line = HoveredLine())
                {
                    Bim::Json made;
                    if (RunCommand("mirror", Bim::Json{ { "ids", ids }, { "axis", Bim::Json::array({ Pt(line->second.first), Pt(line->second.second) }) },
                                                        { "copy", m_modifyOpt.mirrorCopy } }, N_("Could not mirror"), &made))
                        Edited(made, m_modifyOpt.mirrorCopy);
                }
            break;
        case Tool::MirrorDraw:
        {
            const Bim::P2 p = SnapAt(pixel, m_toolPts, shift).at;
            if (m_toolPts.empty())
            {
                m_toolPts.push_back(p);
                break;
            }
            if (p.Dist(m_toolPts.front()) <= Bim::Tol)
                break;
            Bim::Json made;
            if (RunCommand("mirror", Bim::Json{ { "ids", ids }, { "axis", Bim::Json::array({ Pt(m_toolPts.front()), Pt(p) }) }, { "copy", m_modifyOpt.mirrorCopy } },
                           N_("Could not mirror"), &made))
                Edited(made, m_modifyOpt.mirrorCopy);
            else
                m_toolPts.clear();
            break;
        }
        // 先参照线，再要对上去的线；对好后再选下一条参照
        case Tool::Align:
            if (const auto line = HoveredLine())
            {
                if (!m_alignRef)
                    m_alignRef = line->second;
                else
                {
                    const auto ref = *m_alignRef;
                    if (RunCommand("align", Bim::Json{ { "id", line->first }, { "reference", Bim::Json::array({ Pt(ref.first), Pt(ref.second) }) },
                                                       { "line", Bim::Json::array({ Pt(line->second.first), Pt(line->second.second) }) } },
                                   N_("Could not align")))
                        m_alignRef.reset();
                }
            }
            break;
        // 两面墙，单击处是要保留的部分
        case Tool::Trim:
            if (!hovered || !m.GetWall(*hovered))
                break;
            if (!m_trimFirst)
                m_trimFirst = std::pair{ *hovered, mm };
            else if (m_trimFirst->first != *hovered)
            {
                const auto first = *m_trimFirst;
                m_trimFirst.reset();
                RunCommand("corner", Bim::Json{ { "ids", Bim::Json::array({ first.first, *hovered }) }, { "keep", Bim::Json::array({ Pt(first.second), Pt(mm) }) } },
                           N_("Could not trim or extend the walls"));
            }
            break;
        case Tool::Split:
            if (hovered && m.GetWall(*hovered))
                RunCommand("split", Bim::Json{ { "id", *hovered }, { "at", Pt(SnapAt(pixel, {}, false).at) } }, N_("Could not split the wall"));
            break;
        // 墙复制（或移）到光标那一侧，距离按选项栏
        case Tool::Offset:
        {
            const Bim::Wall* w = hovered ? m.GetWall(*hovered) : nullptr;
            if (!w)
                break;
            const auto d = Bim::UnitsOf::Parse(m.units, m_modifyOpt.offset);
            if (w->arc != 0.0 || !d || *d <= 0.0)
            {
                MiniGUI::ShowMessageBox(*m_ui, Tr("Could not offset"), w->arc != 0.0 ? Tr("Offset takes straight walls.")
                                                                                     : Format(Tr("`{0}` is not a distance"), { m_modifyOpt.offset }));
                break;
            }
            const Bim::P2 u = (w->end - w->start).Unit(), n{ -u.y, u.x };
            const Bim::P2 by = Bim::Micron(n * ((mm - w->start).Dot(n) >= 0.0 ? *d : -*d));
            RunCommand(m_modifyOpt.offsetCopy ? "copy" : "move", Bim::Json{ { "ids", Bim::Json::array({ *hovered }) }, { "by", Pt(by) } },
                       N_("Could not offset"));
            break;
        }
        default:
            break;
        }
        UpdateStatus();
        if (m_viewport)
            m_viewport->RequestRender();
    }

    // =========================================================
    // 预览
    // =========================================================
    void MainFrame::BuildModifyPreview(Bim::Render::Drawing& d, std::string& tip)
    {
        if (!m_plan || !m_planView)
            return;
        const Bim::Model& m = m_session.GetModel();
        const Bim::Render::View& v = *m_planView;
        const float px = m_dpiScale;
        // 选择集（或给定的元素）画出来的线，按 f 搬到别处：移动、旋转、镜像会把它们放到哪里
        auto ghost = [&](const std::set<Bim::Id>& of, auto f)
        {
            const Bim::Render::Drawing& plan = *m_plan;
            for (size_t i = 0; i < plan.prims.size(); ++i)
            {
                const Bim::Render::Prim& p = plan.prims[i];
                if (!plan.owners[i] || !of.contains(*plan.owners[i]) || p.kind != Bim::Render::Prim::Kind::Line)
                    continue;
                std::vector<Bim::Render::Px> pts;
                for (const auto& q : p.pts)
                    pts.push_back(f(q));
                Stroke(d, std::move(pts), std::max(1.5f * px, p.width), p.closed, p.dash);
            }
        };
        const std::set<Bim::Id> selection(m_selection.begin(), m_selection.end());
        const std::optional<Bim::P2> mm = m_hoverPixel ? std::optional<Bim::P2>(v.ToMm({ m_hoverPixel->x, m_hoverPixel->y })) : std::nullopt;
        auto line = [&](Bim::P2 a, Bim::P2 b, float w) { Stroke(d, { v.ToPx(a), v.ToPx(b) }, w * px); };
        switch (m_tool)
        {
        case Tool::Modify:
        {
            // 选中的一面墙两端的夹点
            if (m_selection.size() == 1 && m_planDrag != PlanDrag::Grip)
                if (const Bim::Wall* w = m.GetWall(m_selection.front()))
                    for (const Bim::P2& q : { w->start, w->end })
                    {
                        const auto [x, y] = v.ToPx(q);
                        const float r = 4.0f * px;
                        Stroke(d, { { x - r, y - r }, { x + r, y - r }, { x + r, y + r }, { x - r, y + r } }, 2.0f * px, true);
                    }
            if (m_planDrag == PlanDrag::Box)
            {
                // 窗选实线，交叉选虚线
                const float x0 = m_downPixel.x, y0 = m_downPixel.y, x1 = m_dragNow.x, y1 = m_dragNow.y;
                const bool window = x1 >= x0;
                Stroke(d, { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } }, 1.0f * px, true,
                       window ? std::nullopt : std::optional<std::vector<float>>(std::vector<float>{ 6.0f * px, 4.0f * px }));
            }
            else if (m_planDrag == PlanDrag::Move)
            {
                const Bim::P2 by = Round10(v.ToMm({ m_dragNow.x, m_dragNow.y }) - v.ToMm({ m_downPixel.x, m_downPixel.y }));
                const float dx = static_cast<float>(by.x * v.scale), dy = static_cast<float>(-by.y * v.scale);
                ghost(selection, [&](Bim::Render::Px q) { return Bim::Render::Px{ q.first + dx, q.second + dy }; });
                tip = Bim::UnitsOf::WithUnit(m.units, by.Length());
            }
            else if (m_planDrag == PlanDrag::Grip && m_grip)
                if (const Bim::Wall* w = m.GetWall(m_grip->first))
                {
                    const Bim::P2 fixed = m_grip->second ? w->end : w->start;
                    const std::vector<Bim::P2> from{ fixed };
                    const Bim::P2 to = DrawSnap(d, SnapAt(m_dragNow, from, m_hoverShift), tip);
                    line(fixed, to, 2.0f);
                    tip = Bim::UnitsOf::WithUnit(m.units, fixed.Dist(to));
                }
            break;
        }
        case Tool::Move: case Tool::Copy:
        {
            if (!m_hoverPixel)
                break;
            const Bim::P2 next = DrawSnap(d, SnapAt(*m_hoverPixel, m_toolPts, m_hoverShift || m_modifyOpt.constrain), tip);
            if (!m_toolPts.empty())
            {
                const Bim::P2 base = m_toolPts.front();
                const auto a = v.ToPx(base), b = v.ToPx(next);
                ghost(selection, [&](Bim::Render::Px q) { return Bim::Render::Px{ q.first + b.first - a.first, q.second + b.second - a.second }; });
                line(base, next, 1.5f);
                tip = Bim::UnitsOf::WithUnit(m.units, base.Dist(next));
            }
            break;
        }
        case Tool::Rotate:
        {
            if (!m_rotateCentre || m_placeCentre)
            {
                if (m_hoverPixel)
                    DrawSnap(d, SnapAt(*m_hoverPixel, {}, false), tip);
                break;
            }
            // 中心：圈加十字
            const auto [cx, cy] = v.ToPx(*m_rotateCentre);
            const float r = 7.0f * px;
            std::vector<Bim::Render::Px> circle;
            for (int k = 0; k < 16; ++k)
                circle.emplace_back(cx + r * std::cos(k * 0.3927f), cy + r * std::sin(k * 0.3927f));
            Stroke(d, std::move(circle), 2.0f * px, true);
            Stroke(d, { { cx - r * 1.6f, cy }, { cx + r * 1.6f, cy } }, 1.0f * px);
            Stroke(d, { { cx, cy - r * 1.6f }, { cx, cy + r * 1.6f } }, 1.0f * px);
            if (!mm)
                break;
            if (m_toolPts.empty())
                line(*m_rotateCentre, *mm, 1.0f);
            else
            {
                const double angle = AngleBetween(*m_rotateCentre, m_toolPts.front(), *mm, m_snaps.angle);
                line(*m_rotateCentre, m_toolPts.front(), 1.0f);
                line(*m_rotateCentre, *m_rotateCentre + SnapAngle(*mm - *m_rotateCentre, m_snaps.angle) * m_rotateCentre->Dist(*mm), 1.0f);
                // 屏幕的 y 朝下：模型里逆时针，屏幕上顺时针
                const double rad = -angle * std::numbers::pi / 180.0;
                const float s = static_cast<float>(std::sin(rad)), c = static_cast<float>(std::cos(rad));
                ghost(selection, [&](Bim::Render::Px q)
                {
                    const float dx = q.first - cx, dy = q.second - cy;
                    return Bim::Render::Px{ cx + dx * c - dy * s, cy + dx * s + dy * c };
                });
                tip = Degrees(angle);
            }
            break;
        }
        case Tool::MirrorPick: case Tool::MirrorDraw:
        {
            std::optional<Bim::Align::Line> axis;
            if (m_tool == Tool::MirrorPick)
            {
                if (const auto l = HoveredLine())
                    axis = l->second;
            }
            else if (m_hoverPixel)
            {
                const Bim::P2 p = DrawSnap(d, SnapAt(*m_hoverPixel, m_toolPts, m_hoverShift), tip);
                if (!m_toolPts.empty() && p.Dist(m_toolPts.front()) > Bim::Tol)
                    axis = Bim::Align::Line{ m_toolPts.front(), p };
            }
            if (!axis)
                break;
            const auto a = v.ToPx(axis->first), b = v.ToPx(axis->second);
            Stroke(d, { a, b }, 2.0f * px, false, std::vector<float>{ 10.0f * px, 4.0f * px, 2.0f * px, 4.0f * px });
            const float len = std::hypot(b.first - a.first, b.second - a.second);
            if (len < 1e-3f)
                break;
            const float ux = (b.first - a.first) / len, uy = (b.second - a.second) / len;
            ghost(selection, [&](Bim::Render::Px q)
            {
                const float dx = q.first - a.first, dy = q.second - a.second, t = dx * ux + dy * uy;
                return Bim::Render::Px{ a.first + 2.0f * t * ux - dx, a.second + 2.0f * t * uy - dy };
            });
            break;
        }
        // 光标下元素离它最近的线，与已拾取的参照线
        case Tool::Align:
        {
            if (m_alignRef)
                Stroke(d, { v.ToPx(m_alignRef->first), v.ToPx(m_alignRef->second) }, 2.0f * px, false, std::vector<float>{ 8.0f * px, 4.0f * px });
            if (const auto l = HoveredLine())
                line(l->second.first, l->second.second, 3.0f);
            break;
        }
        case Tool::Trim:
            if (m_trimFirst)
                if (const Bim::Wall* w = m.GetWall(m_trimFirst->first))
                    line(w->start, w->end, 3.0f);
            break;
        case Tool::Split:
            if (const auto hovered = HoveredWall(); hovered && m_hoverPixel)
                DrawSnap(d, SnapAt(*m_hoverPixel, {}, false), tip);
            break;
        case Tool::Offset:
        {
            const auto hovered = HoveredWall();
            const Bim::Wall* w = hovered ? m.GetWall(*hovered) : nullptr;
            const auto dist = Bim::UnitsOf::Parse(m.units, m_modifyOpt.offset);
            if (!w || w->arc != 0.0 || !dist || !mm)
                break;
            const Bim::P2 u = (w->end - w->start).Unit(), n{ -u.y, u.x };
            const Bim::P2 by = n * ((*mm - w->start).Dot(n) >= 0.0 ? *dist : -*dist);
            const float dx = static_cast<float>(by.x * v.scale), dy = static_cast<float>(-by.y * v.scale);
            ghost({ *hovered }, [&](Bim::Render::Px q) { return Bim::Render::Px{ q.first + dx, q.second + dy }; });
            break;
        }
        default:
            break;
        }
    }
}
