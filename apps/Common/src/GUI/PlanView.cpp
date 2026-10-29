#include "GUI/MainFrame.h"
#include "Core/Math/Mat4.hpp"
#include "I18n/Translator.h"
#include "Model/Geom.h"
#include "Render/IRenderer.h"
#include "Widgets/ViewportHost.h"
#include <algorithm>
#include <cmath>

// 平面视图：视口里显示一个标高的平面。显示列表由核心库的 Bim::Render 按当前视图（比例、中心）画出，这里把填色的环剖成三角形、
// 把有宽度的折线（含虚线）展开成四边形，一起按画的先后交给 IRenderer：后画的盖住先画的（门的缺口盖住墙的填色）
namespace MiniBIM
{
    namespace
    {
        using Bim::Render::Px;

        // 类别的叫法（英文原文，同命令名称，显示前 Tr）
        const char* CategoryLabel(std::string_view category)
        {
            static const std::pair<const char*, const char*> labels[] = {
                { "level", N_("Level") },   { "grid", N_("Grid") },       { "wall", N_("Wall") },       { "floor", N_("Floor") },
                { "ceiling", N_("Ceiling") }, { "door", N_("Door") },     { "window", N_("Window") },   { "room", N_("Room") },
                { "roof", N_("Roof") },     { "component", N_("Component") }, { "roomseparator", N_("Room Separator") },
            };
            for (const auto& [c, label] : labels)
                if (category == c)
                    return label;
            return "";
        }

        VertexColor ColorOf(Bim::Render::Rgb c) { return VertexColor(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f); }

        void Triangle(std::vector<Vertex_P3_C4>& out, Px a, Px b, Px c, VertexColor color)
        {
            for (const Px& p : { a, b, c })
                out.push_back({ { p.first, p.second, 0.5f }, color });
        }

        // 一段宽 w 的线，两端各伸出半个线宽（方头），转角处两段的方头叠在一起不留缺口
        void Segment(std::vector<Vertex_P3_C4>& out, Px a, Px b, float w, VertexColor color)
        {
            const float dx = b.first - a.first, dy = b.second - a.second;
            const float len = std::hypot(dx, dy);
            if (len < 1e-6f)
                return;
            const float ux = dx / len, uy = dy / len, h = w / 2.0f;
            const Px a0 = { a.first - ux * h - uy * h, a.second - uy * h + ux * h }, a1 = { a.first - ux * h + uy * h, a.second - uy * h - ux * h };
            const Px b0 = { b.first + ux * h - uy * h, b.second + uy * h + ux * h }, b1 = { b.first + ux * h + uy * h, b.second + uy * h - ux * h };
            Triangle(out, a0, b0, b1, color);
            Triangle(out, a0, b1, a1, color);
        }

        // 折线按 dash（实、虚、实、虚……的像素长度）切开的实段
        std::vector<std::pair<Px, Px>> Dashes(const std::vector<Px>& pts, bool closed, const std::optional<std::vector<float>>& dash)
        {
            std::vector<std::pair<Px, Px>> segs;
            for (size_t i = 0; i + 1 < pts.size(); ++i)
                segs.emplace_back(pts[i], pts[i + 1]);
            if (closed && pts.size() > 2)
                segs.emplace_back(pts.back(), pts.front());
            if (!dash || dash->empty())
                return segs;
            float total = 0.0f;
            for (const float d : *dash)
                total += std::max(d, 0.0f);
            if (total <= 0.0f)
                return segs;
            std::vector<std::pair<Px, Px>> out;
            size_t k = 0;
            float left = std::max((*dash)[0], 0.0f);
            for (const auto& [a, b] : segs)
            {
                const float len = std::hypot(b.first - a.first, b.second - a.second);
                float at = 0.0f;
                while (at < len)
                {
                    const float step = std::min(left, len - at);
                    if (k % 2 == 0 && step > 0.0f)
                    {
                        const float t0 = at / len, t1 = (at + step) / len;
                        out.push_back({ { a.first + (b.first - a.first) * t0, a.second + (b.second - a.second) * t0 },
                                        { a.first + (b.first - a.first) * t1, a.second + (b.second - a.second) * t1 } });
                    }
                    at += step;
                    left -= step;
                    if (left <= 1e-6f)
                    {
                        k = (k + 1) % dash->size();
                        left = std::max((*dash)[k], 0.01f);
                    }
                }
            }
            return out;
        }
    }

    void MainFrame::ShowModel3D()
    {
        m_planLevel.reset();
        m_selected.reset();
        m_drag = Drag::None;
        UpdateStatus();
        m_viewport->RequestRender();
    }

    void MainFrame::ShowPlan(std::optional<Bim::Guid> level)
    {
        const Bim::Model& m = m_session.GetModel();
        if (!level)
            level = m_planLevel;
        if (!level || !m.GetLevel(*level))
        {
            const auto levels = m.Levels();
            if (levels.empty())
                return;
            level = m.GuidOf(levels.front().first);
        }
        if (m_planLevel != level)
        {
            m_planLevel = level;
            m_planView.reset();
            m_selected.reset();
        }
        m_planDirty = true;
        m_drag = Drag::None;
        UpdateTitle();
        UpdateStatus();
        m_viewport->RequestRender();
    }

    void MainFrame::StepLevel(int by)
    {
        const Bim::Model& m = m_session.GetModel();
        const auto levels = m.Levels();
        if (levels.empty())
            return;
        size_t at = 0;
        if (m_planLevel)
            for (size_t i = 0; i < levels.size(); ++i)
                if (m.Same(m.GuidOf(levels[i].first), *m_planLevel))
                    at = i;
        // 还在三维视图时先显示最低的标高
        const size_t next = !m_planLevel ? 0 : static_cast<size_t>(std::clamp<long long>(static_cast<long long>(at) + by, 0, static_cast<long long>(levels.size()) - 1));
        ShowPlan(m.GuidOf(levels[next].first));
    }

    void MainFrame::OnPlanModelChanged()
    {
        if (!m_planLevel)
            return;
        const Bim::Model& m = m_session.GetModel();
        if (!m.GetLevel(*m_planLevel))
        {
            m_planLevel.reset();
            ShowPlan(std::nullopt);
            return;
        }
        if (m_selected && !m.elements.Contains(*m_selected))
            m_selected.reset();
        m_planDirty = true;
    }

    std::string MainFrame::PlanStatus() const
    {
        const Bim::Model& m = m_session.GetModel();
        const Bim::Level* l = m_planLevel ? m.GetLevel(*m_planLevel) : nullptr;
        std::string out = l ? Format(Tr("Plan: {0}"), { l->name }) : std::string();
        if (m_selected)
            if (const Bim::Element* e = m.elements.Get(*m_selected))
                out += "    " + Format(Tr("Selected: {0} {1}"), { Tr(CategoryLabel(e->Category())), std::to_string(*m_selected) });
        return out;
    }

    void MainFrame::RenderPlan(IRenderer& renderer, int pixelWidth, int pixelHeight)
    {
        const Bim::Model& m = m_session.GetModel();
        const uint32_t w = static_cast<uint32_t>(std::max(pixelWidth, 1)), h = static_cast<uint32_t>(std::max(pixelHeight, 1));
        if (!m_planView)
        {
            m_planView = Bim::Render::Fit(Bim::Render::PlanShows(m, *m_planLevel), w, h, Bim::UnitsOf::DefaultScale(m.units));
            m_planDirty = true;
        }
        if (m_planView->width != w || m_planView->height != h)
        {
            m_planView->width  = w;
            m_planView->height = h;
            m_planDirty = true;
        }
        if (m_planDirty || !m_plan)
        {
            m_plan = std::make_unique<Bim::Render::Drawing>(Bim::Render::Draw(m, *m_planLevel, *m_planView));
            m_planDirty = false;
        }

        // 平面画在白纸上（同 BIMCraft），不随界面主题；选中的元素用强调色
        const VertexColor paper = ColorOf(Bim::Render::Paper);
        const VertexColor picked(0.0f, 0.47f, 0.84f, 1.0f);
        const float fw = static_cast<float>(w), fh = static_cast<float>(h);
        m_planVerts.clear();
        Triangle(m_planVerts, { 0.0f, 0.0f }, { fw, 0.0f }, { fw, fh }, paper);
        Triangle(m_planVerts, { 0.0f, 0.0f }, { fw, fh }, { 0.0f, fh }, paper);
        const Bim::Render::Drawing& d = *m_plan;
        for (size_t i = 0; i < d.prims.size(); ++i)
        {
            const Bim::Render::Prim& p = d.prims[i];
            const bool selected = m_selected && d.owners[i] == m_selected && p.color != Bim::Render::Paper;
            const VertexColor color = selected ? picked : ColorOf(p.color);
            if (p.kind == Bim::Render::Prim::Kind::Fill)
            {
                for (const auto& ring : p.rings)
                {
                    Bim::Ring r;
                    for (const auto& [x, y] : ring)
                        r.emplace_back(x, -y);  // y 向上再剖分，三角形的绕向与模型的相同
                    for (const auto& [a, b, c] : Bim::Geom::Triangulate(r))
                        Triangle(m_planVerts, ring[a], ring[b], ring[c], color);
                }
            }
            else
                for (const auto& [a, b] : Dashes(p.pts, p.closed, p.dash))
                    Segment(m_planVerts, a, b, std::max(p.width, 1.0f), color);
        }
        // 像素（y 向下）到裁剪空间：行向量约定 P' = P · M
        Math::Mat4 px = Math::Mat4::Identity();
        px.m[0]  = 2.0 / fw;
        px.m[5]  = -2.0 / fh;
        px.m[12] = -1.0;
        px.m[13] = 1.0;
        renderer.Submit(m_planVerts, px, PrimitiveType::Triangle, false);
    }

    void MainFrame::OnPlanPointer(const MiniGUI::ViewportPointerEvent& e)
    {
        using namespace MiniGUI;
        if (!m_planView)
            return;
        Bim::Render::View& v = *m_planView;
        switch (e.type)
        {
        case PointerEventType::Down:
            if (m_drag == Drag::None)
            {
                m_lastPixel = e.pixel;
                m_downPixel = e.pixel;
                if (e.button == MouseButton::Left)
                    m_planClick = true;
                else
                    m_drag = Drag::Pan;
            }
            break;
        case PointerEventType::Up:
            if (m_planClick && e.button == MouseButton::Left)
            {
                // 单击：拾取离指针最近、几个像素以内的元素
                m_planClick = false;
                if (m_plan)
                    m_selected = m_plan->Pick({ e.pixel.x, e.pixel.y }, 4.0f * m_dpiScale);
                UpdateStatus();
                m_viewport->RequestRender();
            }
            if (e.buttons == 0)
                m_drag = Drag::None;
            break;
        case PointerEventType::Move:
            // 左键拖出几个像素也是平移
            if (m_planClick && std::hypot(e.pixel.x - m_downPixel.x, e.pixel.y - m_downPixel.y) > 4.0f * m_dpiScale)
            {
                m_planClick = false;
                m_drag = Drag::Pan;
            }
            if (m_drag == Drag::Pan)
            {
                v.cx -= (e.pixel.x - m_lastPixel.x) / v.scale;
                v.cy += (e.pixel.y - m_lastPixel.y) / v.scale;
                m_lastPixel = e.pixel;
                m_planDirty = true;
                m_viewport->RequestRender();
            }
            break;
        case PointerEventType::Wheel:
            if (e.wheelDelta.y != 0.0f)
            {
                // 以指针处为中心缩放：指针下的点不动
                const Bim::P2 at = v.ToMm({ e.pixel.x, e.pixel.y });
                v.scale = std::clamp(v.scale / std::pow(0.85, static_cast<double>(e.wheelDelta.y)), 1e-5, 50.0);
                v.cx = at.x - (e.pixel.x - v.width / 2.0) / v.scale;
                v.cy = at.y + (e.pixel.y - v.height / 2.0) / v.scale;
                m_planDirty = true;
                m_viewport->RequestRender();
            }
            break;
        default:
            break;
        }
    }
}
