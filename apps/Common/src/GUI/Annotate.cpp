// 移植自 BIMCraft crates/ui-egui/src/canvas.rs、lib.rs（f3bbb33）的注释工具，MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Commands/Commands.h"
#include "Core/UIContext.h"
#include "GUI/Parameters.h"
#include "I18n/Translator.h"
#include "Model/Geom.h"
#include "Model/ModelJson.h"
#include "Widgets/Dialog.h"
#include "Widgets/ViewportHost.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

// 注释选项卡的工具，照 BIMCraft（同 Revit）：
//   - 对齐尺寸：依次拾取平行的墙面、墙中心线、墙端、门窗、轴网，再在空处单击放尺寸线；角度尺寸：两条相交的线，再单击放弧；
//     径向尺寸：圆柱的边，再单击放半径线。Esc 重来
//   - 高程点：单击楼板、屋顶、楼梯、墙或柱，读那里顶面的高程；文字：选项栏输入文字，单击放下
//   - 标记：单击元素标它（门窗的标记、房间的名称、别的为类型）；标记全部：这个平面上还没有标记的门窗
//   - 详图线：两下一段，连成链；测量：两下，尺子留着读长度与角度，再单击重量
// 填充区域、遮罩区域在草图模式里画（SketchMode.cpp）
namespace MiniBIM
{
    namespace
    {
        constexpr Bim::Render::Rgb Accent = { 0, 120, 215 };

        Bim::Json Pt(Bim::P2 p) { return Bim::Json::array({ p.x, p.y }); }

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

    std::optional<Bim::Reference> MainFrame::PickReference(Bim::P2 p) const
    {
        const Bim::Model& m = m_session.GetModel();
        if (!m_planLevel)
            return std::nullopt;
        if (m_tool == Tool::Radial)
            return Bim::ArcNear(m, *m_planLevel, p);
        // 第一条之后：对齐的与它平行，角度的与它相交
        std::optional<Bim::Segment> first;
        Bim::Segment l;
        std::string why;
        if (!m_toolRefs.empty() && m.ReferenceLine(m_toolRefs.front(), l, why))
            first = l;
        const bool aligned = m_tool == Tool::Dimension;
        return Bim::ReferenceNear(m, *m_planLevel, p, [&](const Bim::Segment& x)
        {
            return !first || Bim::Geom::Parallel(first->first, first->second, x.first, x.second) == aligned;
        });
    }

    std::optional<Bim::Dimension> MainFrame::DimensionAt(Bim::P2 at) const
    {
        if (!m_planLevel || m_toolRefs.size() < (m_tool == Tool::Radial ? 1u : 2u))
            return std::nullopt;
        Bim::Dimension d;
        d.level = *m_planLevel;
        d.view  = m_planOf;
        d.kind  = m_tool == Tool::Angular ? Bim::DimKind::Angular : m_tool == Tool::Radial ? Bim::DimKind::Radial : Bim::DimKind::Aligned;
        d.refs  = m_toolRefs;
        d.at    = at;
        return d;
    }

    void MainFrame::AnnotateClick(MiniGUI::Vec2 pixel, uint8_t modifiers)
    {
        (void)modifiers;
        const Bim::Model& m = m_session.GetModel();
        const Bim::P2 mm = Bim::Micron(m_planView->ToMm({ pixel.x, pixel.y }));
        const Bim::Json level = Bim::ToJson(PlanKey());
        m_hoverPixel = pixel;
        switch (m_tool)
        {
        // 单击参照加上它；参照够了之后在空处单击（角度、径向：任何地方）放尺寸线
        case Tool::Dimension: case Tool::Angular: case Tool::Radial:
        {
            const size_t full = m_tool == Tool::Dimension ? std::numeric_limits<size_t>::max() : m_tool == Tool::Angular ? 2 : 1;
            if (m_toolRefs.size() < full)
                if (const auto r = PickReference(mm))
                {
                    if (std::find(m_toolRefs.begin(), m_toolRefs.end(), *r) == m_toolRefs.end())
                        m_toolRefs.push_back(*r);
                    break;
                }
            const auto d = DimensionAt(mm);
            if (!d)
                break;
            Bim::Json refs = Bim::Json::array();
            for (const Bim::Reference& r : d->refs)
                refs.push_back(Bim::Json{ { "id", Bim::ToJson(r.id) }, { "face", Bim::FaceOf::Name(r.face) } });
            const char* kind = m_tool == Tool::Angular ? "angular" : m_tool == Tool::Radial ? "radial" : "aligned";
            if (RunCommand("dimension", Bim::Json{ { "level", level }, { "at", Pt(mm) }, { "kind", kind }, { "refs", refs } },
                           N_("Could not place the dimension")))
                m_toolRefs.clear();
            break;
        }
        case Tool::Spot:
            RunCommand("spot", Bim::Json{ { "at", Pt(SnapAt(pixel, {}, false).at) }, { "level", level } }, N_("Could not place the spot elevation"));
            break;
        case Tool::Text:
        {
            if (Props::Words(m_toolOpt.noteText).empty())
            {
                MiniGUI::ShowMessageBox(*m_ui, Tr("Could not place the text"), Tr("Type the note in the Options Bar first."));
                break;
            }
            Bim::Json p{ { "text", m_toolOpt.noteText }, { "at", Pt(SnapAt(pixel, {}, false).at) }, { "level", level } };
            if (m.textTypes.contains(m_toolOpt.textType))
                p["type"] = m_toolOpt.textType;
            RunCommand("text", p, N_("Could not place the text"));
            break;
        }
        // 光标下有点可指的元素（注释除外）
        case Tool::Tag:
            if (const auto id = Hovered())
                if (const Bim::Element* e = m.elements.Get(*id); e && !e->IsAnnotation() && m.Anchor(m.GuidOf(*id)))
                    RunCommand("tag", Bim::Json{ { "id", *id }, { "leader", m_toolOpt.tagLeader } }, N_("Could not tag the element"));
            break;
        default:
            break;
        }
        UpdateStatus();
        if (m_viewport)
            m_viewport->RequestRender();
    }

    void MainFrame::TagAll()
    {
        if (!m_planLevel)
            ShowPlan(std::nullopt);
        if (!m_planLevel)
            return;
        RunCommand("tag", Bim::Json{ { "level", Bim::ToJson(PlanKey()) }, { "leader", m_toolOpt.tagLeader } }, N_("Could not tag the doors and windows"));
    }

    void MainFrame::BuildAnnotatePreview(Bim::Render::Drawing& d, std::string& tip)
    {
        if (!m_planLevel || !m_planView)
            return;
        const Bim::Model& m = m_session.GetModel();
        const Bim::Render::View& v = *m_planView;
        const float px = m_dpiScale;
        // 还不在模型里的注释：照平面会画的样子，用强调色
        auto sketched = [&](const Bim::Element& e)
        {
            Bim::Render::Drawing s = Bim::Render::Sketch(m, PlanKey(), v, e);
            for (Bim::Render::Prim& p : s.prims)
            {
                p.color = Accent;
                d.Push(std::move(p));
            }
        };
        // 参照：直线，或径向的圆
        auto reference = [&](const Bim::Reference& r, bool picked)
        {
            const std::optional<std::vector<float>> dash = picked ? std::optional<std::vector<float>>(std::vector<float>{ 6.0f * px, 4.0f * px }) : std::nullopt;
            Bim::Segment l;
            Bim::P2 centre;
            double radius = 0.0;
            std::string why;
            if (m_tool == Tool::Radial && m.ReferenceArc(r, centre, radius, why))
            {
                std::vector<Bim::Render::Px> c;
                for (int k = 0; k < 48; ++k)
                    c.push_back(v.ToPx(centre + Bim::P2{ std::cos(k * 0.1309), std::sin(k * 0.1309) } * radius));
                Stroke(d, std::move(c), 2.0f * px, true, dash);
            }
            else if (m.ReferenceLine(r, l, why))
                Stroke(d, { v.ToPx(l.first), v.ToPx(l.second) }, 2.0f * px, false, dash);
        };
        for (const Bim::Reference& r : m_toolRefs)
            reference(r, true);
        if (!m_hoverPixel)
            return;
        const Bim::P2 mm = v.ToMm({ m_hoverPixel->x, m_hoverPixel->y });
        auto reading = [&](Bim::P2 a, Bim::P2 b)
        {
            return Bim::UnitsOf::WithUnit(m.units, a.Dist(b)) + "  " + Degrees(Bim::Snap::Measure(a, b).angle);
        };
        switch (m_tool)
        {
        case Tool::Dimension: case Tool::Angular: case Tool::Radial:
        {
            const size_t full = m_tool == Tool::Dimension ? std::numeric_limits<size_t>::max() : m_tool == Tool::Angular ? 2 : 1;
            if (m_toolRefs.size() < full)
                if (const auto r = PickReference(mm))
                {
                    reference(*r, false);
                    break;
                }
            if (const auto dim = DimensionAt(Bim::Micron(mm)))
            {
                sketched(Bim::Element{ *dim });
                std::string values;
                for (const double x : m.DimensionValues(*dim))
                {
                    const std::string one = m_tool == Tool::Angular ? Degrees(x) : Bim::UnitsOf::WithUnit(m.units, x);
                    values += (values.empty() ? "" : " | ") + (m_tool == Tool::Radial ? "R " + one : one);
                }
                tip = values;
            }
            break;
        }
        case Tool::Spot:
        {
            Bim::Spot sp;
            sp.view = PlanKey();
            sp.at   = DrawSnap(d, SnapAt(*m_hoverPixel, {}, false), tip);
            if (const auto host = m.SpotHost(*m_planLevel, sp.at))
            {
                sp.host = m.GuidOf(*host);
                sketched(Bim::Element{ sp });
                if (const auto z = m.SpotValue(sp))
                    tip = Bim::UnitsOf::WithUnit(m.units, *z);
            }
            break;
        }
        case Tool::Text:
        {
            const Bim::P2 at = DrawSnap(d, SnapAt(*m_hoverPixel, {}, false), tip);
            if (Props::Words(m_toolOpt.noteText).empty())
                break;
            Bim::Note n;
            n.view = PlanKey();
            n.at   = at;
            n.text = m_toolOpt.noteText;
            if (m.textTypes.contains(m_toolOpt.textType))
                n.typeName = m_toolOpt.textType;
            else if (const std::string* t = Bim::TextTypes::Default(m.textTypes))
                n.typeName = *t;
            sketched(Bim::Element{ n });
            break;
        }
        // 会放下的标记：光标下的元素有点可指时
        case Tool::Tag:
            if (const auto id = Hovered())
                if (const Bim::Element* e = m.elements.Get(*id); e && !e->IsAnnotation())
                {
                    const Bim::Guid g = m.GuidOf(*id);
                    if (m.Anchor(g))
                        sketched(Bim::Element{ Bim::Tag{ g, std::nullopt, m.TagOffset(g), m_toolOpt.tagLeader, std::nullopt } });
                }
            break;
        case Tool::DetailLine:
        {
            const Bim::P2 next = DrawSnap(d, SnapAt(*m_hoverPixel, m_toolPts, m_hoverShift), tip);
            if (m_toolPts.empty())
                break;
            const Bim::P2 last = m_toolPts.back();
            if (last.Dist(next) > Bim::Tol)
            {
                Bim::DetailLine l;
                l.view  = PlanKey();
                l.curve = Bim::Curve::Line(last, next);
                if (m.lineStyles.contains(m_toolOpt.lineStyle))
                    l.style = m_toolOpt.lineStyle;
                else if (const std::string* s = Bim::Lines::DefaultStyle(m.lineStyles, m.language))
                    l.style = *s;
                sketched(Bim::Element{ l });
            }
            tip = !m_typedLength.empty() ? m_typedLength + " (Enter)" : (tip.empty() ? reading(last, next) : tip + "    " + reading(last, next));
            break;
        }
        // 测量：两点之间的尺子；量完的留着，直到下一下
        case Tool::Measure:
        {
            const Bim::P2 next = DrawSnap(d, SnapAt(*m_hoverPixel, m_toolPts, m_hoverShift), tip);
            if (m_toolPts.size() == 1)
            {
                Stroke(d, { v.ToPx(m_toolPts[0]), v.ToPx(next) }, 2.0f * px);
                tip = reading(m_toolPts[0], next);
            }
            else if (m_toolPts.size() == 2)
            {
                Stroke(d, { v.ToPx(m_toolPts[0]), v.ToPx(m_toolPts[1]) }, 2.0f * px);
                tip = reading(m_toolPts[0], m_toolPts[1]);
            }
            break;
        }
        default:
            break;
        }
    }
}
