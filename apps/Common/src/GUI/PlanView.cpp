#include "GUI/MainFrame.h"
#include "Core/Math/Mat4.hpp"
#include "I18n/Translator.h"
#include "Model/Geom.h"
#include "Model/ModelJson.h"
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
                { "mass", N_("Mass") },       { "dimension", N_("Dimension") }, { "text", N_("Text Note") },
                { "spot", N_("Spot Elevation") }, { "tag", N_("Tag") }, { "line", N_("Detail Line") }, { "region", N_("Filled Region") },
                { "section", N_("Section") }, { "elevation", N_("Elevation View") },
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
        if (LeavingSketch())
            return;
        // 放东西的工具只在平面上（标高除外）
        if (m_tool != Tool::Modify && m_tool != Tool::Level)
            SetTool(Tool::Modify);
        m_planLevel.reset();
        m_planOf.reset();
        m_sectionOf.reset();
        m_drag = Drag::None;
        UpdateTitle();
        UpdateStatus();
        RefreshPanels();
        m_viewport->RequestRender();
    }

    void MainFrame::ShowPlan(std::optional<Bim::Guid> level)
    {
        const Bim::Model& m = m_session.GetModel();
        // 给了标高时显示它自己的平面
        if (level && m_planOf)
        {
            m_planOf.reset();
            m_planView.reset();
        }
        if (!level)
            level = m_planLevel;
        if (m_sketch && level && !m.Same(*level, m_sketch->level) && LeavingSketch())
            return;
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
            m_toolPts.clear();      // 画了一半的不跟到别的标高
        }
        m_sectionOf.reset();
        m_planDirty = true;
        m_drag = Drag::None;
        UpdateTitle();
        UpdateStatus();
        RefreshPanels();
        m_viewport->RequestRender();
    }

    void MainFrame::ShowPlanView(Bim::Guid view)
    {
        const Bim::Model& m = m_session.GetModel();
        const auto level = m.PlanLevel(view);
        if (!level || !m.GetPlanView(view))
            return;
        if (m_sketch && !m.Same(*level, m_sketch->level) && LeavingSketch())
            return;
        if (m_planOf != view || m_planLevel != level)
        {
            if (m_planLevel != level)
                m_toolPts.clear();
            m_planLevel = level;
            m_planOf    = m.GuidOf(view);
            m_planView.reset();
        }
        m_sectionOf.reset();
        m_planDirty = true;
        m_drag = Drag::None;
        UpdateTitle();
        UpdateStatus();
        RefreshPanels();
        m_viewport->RequestRender();
    }

    void MainFrame::DuplicatePlan(const char* mode)
    {
        if (!m_planLevel)
            return;
        Bim::Json made;
        if (!RunCommand("duplicate_view", Bim::Json{ { "view", Bim::ToJson(PlanKey()) }, { "mode", mode } }, N_("Could not duplicate the view"), &made))
            return;
        const Bim::Model& m = m_session.GetModel();
        if (made.contains("id"))
            if (const Bim::Element* e = m.elements.Get(made["id"].get<Bim::Id>()); e && e->As<Bim::PlanView>())
                ShowPlanView(m.GuidOf(made["id"].get<Bim::Id>()));
    }

    void MainFrame::ToggleCrop()
    {
        if (!m_planLevel)
            return;
        const bool cropped = m_session.GetModel().CropOf(PlanKey()).has_value();
        Bim::Json p{ { "view", Bim::ToJson(PlanKey()) } };
        if (cropped)
            p["region"] = nullptr;
        if (RunCommand("crop", p, N_("Could not crop the view")))
        {
            m_planView.reset();
            m_planDirty = true;
            m_viewport->RequestRender();
        }
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
        const Bim::Model& m = m_session.GetModel();
        // 删掉、撤销掉的元素离开选择集
        std::erase_if(m_selection, [&](Bim::Id id) { return !m.elements.Contains(id); });
        if (m_sectionOf)
        {
            // 显示的剖面删掉了：回到三维视图
            if (!m.GetSection(*m_sectionOf))
                ShowModel3D();
            m_sectionDirty      = true;
            m_sectionCutVersion = 0;
            return;
        }
        if (!m_planLevel)
            return;
        if (!m.GetLevel(*m_planLevel))
        {
            m_planLevel.reset();
            m_planOf.reset();
            ShowPlan(std::nullopt);
            return;
        }
        // 显示的平面视图删掉了：回到它的标高的平面
        if (m_planOf && !m.GetPlanView(*m_planOf))
        {
            m_planOf.reset();
            m_planView.reset();
        }
        m_planDirty = true;
    }

    bool MainFrame::Selected(Bim::Id id) const
    {
        return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
    }

    std::string MainFrame::PlanStatus() const
    {
        const Bim::Model& m = m_session.GetModel();
        const std::string* name = m_planLevel ? m.ViewName(PlanKey()) : m_sectionOf ? m.ViewName(*m_sectionOf) : nullptr;
        const bool elevation = m_sectionOf && m.elements.Get(*m_sectionOf)->As<Bim::Elevation>();
        std::string out = !name        ? std::string()
                          : elevation  ? Format(Tr("Elevation: {0}"), { *name })
                          : m_sectionOf ? Format(Tr("Section: {0}"), { *name })
                                        : Format(Tr("Plan: {0}"), { *name });
        if (m_selection.size() == 1)
        {
            if (const Bim::Element* e = m.elements.Get(m_selection.front()))
                out += "    " + Format(Tr("Selected: {0} {1}"), { Tr(CategoryLabel(e->Category())), std::to_string(m_selection.front()) });
        }
        else if (!m_selection.empty())
            out += "    " + Format(TrN("{0} element selected", "{0} elements selected", static_cast<long long>(m_selection.size())),
                                   { std::to_string(m_selection.size()) });
        return out;
    }

    void MainFrame::RenderPlan(IRenderer& renderer, int pixelWidth, int pixelHeight)
    {
        const Bim::Model& m = m_session.GetModel();
        const uint32_t w = static_cast<uint32_t>(std::max(pixelWidth, 1)), h = static_cast<uint32_t>(std::max(pixelHeight, 1));
        if (!m_planView)
        {
            // 框住整个平面：放进没被功能区、面板挡住的区域（m_fitInsets；留边过大时按视口的 20% 算），以它的中心为中心
            const double l = m_fitInsets[0], t = m_fitInsets[1];
            const double fw = std::max(w - l - m_fitInsets[2], w * 0.2), fh = std::max(h - t - m_fitInsets[3], h * 0.2);
            Bim::Render::View v = Bim::Render::Fit(Bim::Render::PlanShows(m, PlanKey()), static_cast<uint32_t>(fw), static_cast<uint32_t>(fh),
                                                   m.ScaleOf(PlanKey()));
            // Fit 把范围的中心放在 (fw/2, fh/2)；整个视口以 (w/2, h/2) 为 (cx, cy)，挪到区域中心 (l + fw/2, t + fh/2)
            v.width  = w;
            v.height = h;
            v.cx -= (l + fw / 2.0 - w / 2.0) / v.scale;
            v.cy += (t + fh / 2.0 - h / 2.0) / v.scale;
            m_planView  = v;
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
            m_plan = std::make_unique<Bim::Render::Drawing>(Bim::Render::Draw(m, PlanKey(), *m_planView));
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
        // 修改工具下光标下的元素也高亮（拖动、框选时不）
        // 修改工具下光标下的元素；修剪、拆分、偏移是光标下的墙（对齐、镜像拾取轴画它们自己的线）
        const bool walls = m_tool == Tool::Trim || m_tool == Tool::Split || m_tool == Tool::Offset;
        const std::optional<Bim::Id> hovered = m_planDrag != PlanDrag::None ? std::nullopt
                                             : m_tool == Tool::Modify    ? Hovered()
                                             : walls                     ? HoveredWall()
                                                                         : std::nullopt;
        for (size_t i = 0; i < d.prims.size(); ++i)
        {
            const Bim::Render::Prim& p = d.prims[i];
            const bool selected = d.owners[i] && (Selected(*d.owners[i]) || (hovered && *d.owners[i] == *hovered)) && p.color != Bim::Render::Paper;
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
        // 工具会放下的东西、捕捉记号，画在最上面
        Bim::Render::Drawing preview(*m_planView);
        BuildPreview(preview);
        for (const Bim::Render::Prim& p : preview.prims)
        {
            const VertexColor color = ColorOf(p.color);
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

    void MainFrame::ShowSection(Bim::Guid view)
    {
        const Bim::Model& m = m_session.GetModel();
        if (!m.GetSection(view) || LeavingSketch())
            return;
        if (m_tool != Tool::Modify)
            SetTool(Tool::Modify);
        if (!m_sectionOf || !m.Same(*m_sectionOf, view))
        {
            m_sectionOf = m.GuidOf(view);
            m_sectionView.reset();
        }
        m_planLevel.reset();
        m_planOf.reset();
        m_sectionDirty      = true;
        m_sectionCutVersion = 0;
        m_drag = Drag::None;
        UpdateTitle();
        UpdateStatus();
        RefreshPanels();
        m_viewport->RequestRender();
    }

    void MainFrame::MakeElevations()
    {
        Bim::Json result;
        if (!RunCommand("elevation", Bim::Json::object(), N_("Could not make the elevations"), &result))
            return;
        // 做出一个时显示它，几个时显示第一个
        const Bim::Json id = result.contains("id") ? result["id"] : result.contains("ids") && !result["ids"].empty() ? result["ids"][0] : Bim::Json();
        if (id.is_number_unsigned())
            ShowSection(m_session.GetModel().GuidOf(id.get<Bim::Id>()));
    }

    void MainFrame::RenderSection(IRenderer& renderer, int pixelWidth, int pixelHeight)
    {
        const Bim::Model& m = m_session.GetModel();
        const Bim::Section* sv = m_sectionOf ? m.GetSection(*m_sectionOf) : nullptr;
        if (!sv)
            return;
        const Bim::Guid key = *m_sectionOf;
        const uint32_t w = static_cast<uint32_t>(std::max(pixelWidth, 1)), h = static_cast<uint32_t>(std::max(pixelHeight, 1));
        // 剖到的与看到的：场景或剖面变了时重算
        if (m_sectionCutVersion != m_sceneVersion || m_sectionCutVersion == 0)
        {
            const Bim::Look look = m.LookOf(key);
            m_sectionCut = m_scene.Cut(sv->start, sv->end, sv->depth, sv->crop, [&](Bim::Id id) { return look.Shows(id); });
            m_sectionCutVersion = std::max<uint64_t>(m_sceneVersion, 1);
            const Bim::P2 u = (sv->end - sv->start).Unit(), n = u.Perp();
            m_sectionFar = 1.0;
            for (const Vertex_P3_C4& p : m_sectionCut.faces)
                m_sectionFar = std::max(m_sectionFar, (Bim::P2{ p.pos.x, p.pos.y } - sv->start).Dot(n));
            m_sectionDirty = true;
        }
        const Bim::Render::Seen seen{ m_sectionCut.first, m_sectionCut.last, m_sectionCut.lo, m_sectionCut.hi, m_sectionCut.any };
        if (!m_sectionView)
        {
            // 同平面：放进没被挡住的区域
            const double l = m_fitInsets[0], t = m_fitInsets[1];
            const double fw = std::max(w - l - m_fitInsets[2], w * 0.2), fh = std::max(h - t - m_fitInsets[3], h * 0.2);
            Bim::Render::View v = Bim::Render::Fit(Bim::Render::SectionShows(m, key, seen), static_cast<uint32_t>(fw), static_cast<uint32_t>(fh), m.ScaleOf(key));
            v.width  = w;
            v.height = h;
            v.cx -= (l + fw / 2.0 - w / 2.0) / v.scale;
            v.cy += (t + fh / 2.0 - h / 2.0) / v.scale;
            m_sectionView  = v;
            m_sectionDirty = true;
        }
        if (m_sectionView->width != w || m_sectionView->height != h)
        {
            m_sectionView->width  = w;
            m_sectionView->height = h;
            m_sectionDirty = true;
        }
        if (m_sectionDirty || !m_section)
        {
            Bim::Render::SectionCuts cuts;
            for (const auto& [id, rings] : m_sectionCut.cuts)
                cuts.emplace_back(id, rings);
            m_section = std::make_unique<Bim::Render::Drawing>(Bim::Render::DrawSection(m, key, *m_sectionView, cuts, seen));
            m_sectionDirty = false;
        }
        const float fw = static_cast<float>(w), fh = static_cast<float>(h);
        Math::Mat4 px = Math::Mat4::Identity();
        px.m[0]  = 2.0 / fw;
        px.m[5]  = -2.0 / fh;
        px.m[12] = -1.0;
        px.m[13] = 1.0;
        // 白纸
        m_planVerts.clear();
        Triangle(m_planVerts, { 0.0f, 0.0f }, { fw, 0.0f }, { fw, fh }, ColorOf(Bim::Render::Paper));
        Triangle(m_planVerts, { 0.0f, 0.0f }, { fw, fh }, { 0.0f, fh }, ColorOf(Bim::Render::Paper));
        renderer.Submit(m_planVerts, px, PrimitiveType::Triangle, false);

        // 看到的形体：白色的面挡住后面的边（消隐）。世界坐标到裁剪空间：沿线、向上随视图平移缩放，深度是离线多远
        const Bim::Render::View& v = *m_sectionView;
        const Bim::P2 u = (sv->end - sv->start).Unit(), n = u.Perp();
        const double kx = v.scale * 2.0 / fw, ky = v.scale * 2.0 / fh, kz = 1.0 / (m_sectionFar * 1.01 + 1.0);
        Math::Mat4 world = Math::Mat4::Identity();
        world.m[0]  = kx * u.x;  world.m[1]  = 0.0; world.m[2]  = kz * n.x;
        world.m[4]  = kx * u.y;  world.m[5]  = 0.0; world.m[6]  = kz * n.y;
        world.m[8]  = 0.0;       world.m[9]  = ky;  world.m[10] = 0.0;
        world.m[12] = -kx * (u.Dot(sv->start) + v.cx);
        world.m[13] = -ky * v.cy;
        world.m[14] = -kz * n.Dot(sv->start);
        if (!m_sectionCut.faces.empty())
            renderer.Submit(m_sectionCut.faces, world, PrimitiveType::Triangle, true);
        // 边往看的人那边挪一点，免得与所在的面争深度
        const double toward = std::max(1.0, m_sectionFar * 1e-3);
        const VertexColor ink(0.18f, 0.18f, 0.20f, 1.0f);
        m_edgeVerts.clear();
        m_edgeVerts.reserve(m_sectionCut.edges.size());
        for (const Math::Point3& p : m_sectionCut.edges)
            m_edgeVerts.push_back({ { static_cast<float>(p.x - n.x * toward), static_cast<float>(p.y - n.y * toward), static_cast<float>(p.z) }, ink });
        if (!m_edgeVerts.empty())
            renderer.Submit(m_edgeVerts, world, PrimitiveType::Line, true);

        // 剖到的、标高、轴网、名称：盖在上面
        const VertexColor picked(0.0f, 0.47f, 0.84f, 1.0f);
        m_planVerts.clear();
        const Bim::Render::Drawing& d = *m_section;
        for (size_t i = 0; i < d.prims.size(); ++i)
        {
            const Bim::Render::Prim& p = d.prims[i];
            const bool selected = d.owners[i] && Selected(*d.owners[i]) && p.color != Bim::Render::Paper;
            const VertexColor color = selected ? picked : ColorOf(p.color);
            if (p.kind == Bim::Render::Prim::Kind::Fill)
            {
                for (const auto& ring : p.rings)
                {
                    Bim::Ring r;
                    for (const auto& [x, y] : ring)
                        r.emplace_back(x, -y);
                    for (const auto& [a, b, c] : Bim::Geom::Triangulate(r))
                        Triangle(m_planVerts, ring[a], ring[b], ring[c], color);
                }
            }
            else
                for (const auto& [a, b] : Dashes(p.pts, p.closed, p.dash))
                    Segment(m_planVerts, a, b, std::max(p.width, 1.0f), color);
        }
        renderer.Submit(m_planVerts, px, PrimitiveType::Triangle, false);
    }

    void MainFrame::OnSectionPointer(const MiniGUI::ViewportPointerEvent& e)
    {
        using namespace MiniGUI;
        if (!m_sectionView)
            return;
        Bim::Render::View& v = *m_sectionView;
        switch (e.type)
        {
        case PointerEventType::Down:
            if (m_drag == Drag::None)
            {
                m_lastPixel = e.pixel;
                m_downPixel = e.pixel;
                m_drag      = Drag::Pan;
            }
            break;
        case PointerEventType::Up:
            // 单击：选中光标下剖到的元素、标高或轴网（Ctrl 加进或拿出）
            if (e.button == MouseButton::Left && std::hypot(e.pixel.x - m_downPixel.x, e.pixel.y - m_downPixel.y) <= 4.0f * m_dpiScale && m_section)
            {
                const std::optional<Bim::Id> hit = m_section->Pick({ e.pixel.x, e.pixel.y }, 6.0f * m_dpiScale);
                const bool ctrl = (e.modifiers & static_cast<uint8_t>(ModifierKey::Ctrl)) != 0;
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
            if (e.buttons == 0)
                m_drag = Drag::None;
            break;
        case PointerEventType::Move:
            if (m_drag == Drag::Pan)
            {
                v.cx -= (e.pixel.x - m_lastPixel.x) / v.scale;
                v.cy += (e.pixel.y - m_lastPixel.y) / v.scale;
                m_lastPixel    = e.pixel;
                m_sectionDirty = true;
                m_viewport->RequestRender();
            }
            break;
        case PointerEventType::Wheel:
            if (e.wheelDelta.y != 0.0f)
            {
                const Bim::P2 at = v.ToMm({ e.pixel.x, e.pixel.y });
                v.scale = std::clamp(v.scale / std::pow(0.85, static_cast<double>(e.wheelDelta.y)), 1e-5, 50.0);
                v.cx = at.x - (e.pixel.x - v.width / 2.0) / v.scale;
                v.cy = at.y + (e.pixel.y - v.height / 2.0) / v.scale;
                m_sectionDirty = true;
                m_viewport->RequestRender();
            }
            break;
        default:
            break;
        }
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
            if (m_drag == Drag::None && m_planDrag == PlanDrag::None)
            {
                m_lastPixel = e.pixel;
                m_downPixel = e.pixel;
                if (e.button == MouseButton::Left)
                {
                    m_planClick = true;
                    // 草图模式里修改的是草图线：拖出来时是夹点或框选
                    if (m_tool == Tool::Modify && m_sketch)
                    {
                        BeginSketchDrag(e.pixel);
                        m_pendingDrag = PlanDrag::Move;
                    }
                    else if (m_tool == Tool::Modify)
                        BeginPlanDrag(e.pixel);
                }
                else
                    m_drag = Drag::Pan;
            }
            break;
        case PointerEventType::Up:
            if (m_sketch && m_tool == Tool::Modify && e.button == MouseButton::Left && (m_planDrag != PlanDrag::None || m_planClick))
            {
                FinishSketchDrag(e.pixel, e.modifiers, m_planDrag != PlanDrag::None);
                m_planDrag = PlanDrag::None;
            }
            else if (m_planDrag != PlanDrag::None && e.button == MouseButton::Left)
                FinishPlanDrag(e.pixel, e.modifiers);
            else if (m_planClick && e.button == MouseButton::Left && m_tool != Tool::Modify)
                ToolClick(e.pixel, e.modifiers);
            else if (m_planClick && e.button == MouseButton::Left)
                PickAt(e.pixel, e.modifiers);
            if (e.button == MouseButton::Left)
            {
                m_planClick   = false;
                m_pendingDrag = PlanDrag::None;
            }
            if (e.buttons == 0)
                m_drag = Drag::None;
            break;
        case PointerEventType::Move:
        {
            // 预览跟着光标：放东西的工具的，修改时光标下的元素
            const bool moved = !m_hoverPixel || m_hoverPixel->x != e.pixel.x || m_hoverPixel->y != e.pixel.y;
            m_hoverPixel = e.pixel;
            m_hoverShift = (e.modifiers & static_cast<uint8_t>(ModifierKey::Shift)) != 0;
            if (moved)
                m_tabIndex = 0;
            // 左键拖出几个像素：修改工具下是框选、拖动或夹点，别的工具下平移
            if (m_planClick && std::hypot(e.pixel.x - m_downPixel.x, e.pixel.y - m_downPixel.y) > 4.0f * m_dpiScale)
            {
                m_planClick = false;
                if (m_pendingDrag != PlanDrag::None)
                    m_planDrag = m_pendingDrag;
                else
                    m_drag = Drag::Pan;
            }
            if (m_planDrag != PlanDrag::None)
                UpdatePlanDrag(e.pixel);
            if (m_drag == Drag::Pan)
            {
                v.cx -= (e.pixel.x - m_lastPixel.x) / v.scale;
                v.cy += (e.pixel.y - m_lastPixel.y) / v.scale;
                m_lastPixel = e.pixel;
                m_planDirty = true;
            }
            m_viewport->RequestRender();
            break;
        }
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
