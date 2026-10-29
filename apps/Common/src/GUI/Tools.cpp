// 移植自 BIMCraft crates/ui-egui/src/canvas.rs、lib.rs（f3bbb33）的放置工具、选项栏与预览，MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Commands/Commands.h"
#include "Core/UIContext.h"
#include "GUI/Parameters.h"
#include "I18n/Translator.h"
#include "Model/Geom.h"
#include "Model/ModelJson.h"
#include "Model/Text.h"
#include "Style/Theme.hpp"
#include "Widgets/Button.h"
#include "Widgets/ComboBox.h"
#include "Widgets/Dialog.h"
#include "Widgets/Controls.h"
#include "Widgets/Label.h"
#include "Widgets/Panel.h"
#include "Widgets/TextBox.h"
#include "Widgets/ViewportHost.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

// 放东西的工具，照 BIMCraft（同 Revit）：
//   - 墙：每次单击从上一点建一面墙，连成墙链（选项栏的"链"关掉时每面墙单独画）；Enter、Esc 结束，回到起点时自动结束
//   - 门、窗：单击墙，离那一段墙的中点近时放在正中；构件：单击处，空格转 90°
//   - 屋顶：单击墙里面，在上一层按下面一层的墙建迹线屋顶；楼板：沿这一层的墙；天花板、房间：单击处四周围合的区域
//   - 标高：选项栏输入高程后"创建"；轴网：两次单击两端
// 点下去的点都经捕捉（Bim::Snap）：端点、中点、交点、圆心、垂足、墙中心线、角度引导线；Shift 保持水平或竖直。
// 画了起点之后可以直接输入长度（项目单位）按 Enter：沿光标方向（落在引导线上时沿它）量这么长
namespace MiniBIM
{
    namespace
    {
        // 捕捉点的范围、拾取线的范围（逻辑像素）
        constexpr double SnapPx = 10.0;
        constexpr double PickPx = 6.0;

        constexpr Bim::Render::Rgb Accent = { 0, 120, 215 };

        Bim::P2 Round10(Bim::P2 p) { return { std::round(p.x / 10.0) * 10.0, std::round(p.y / 10.0) * 10.0 }; }

        Bim::Json Pt(Bim::P2 p) { return Bim::Json::array({ p.x, p.y }); }

        // 一条像素坐标的折线
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

        void Ring(Bim::Render::Drawing& d, const Bim::Render::View& v, const Bim::Ring& ring, float width)
        {
            std::vector<Bim::Render::Px> pts;
            for (const Bim::P2& q : ring)
                pts.push_back(v.ToPx(q));
            Stroke(d, std::move(pts), width, true);
        }

        // 方向的度数（从正东逆时针），两位小数，去掉末尾的 0
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

        std::string Area(Bim::Units u, double mm2)
        {
            const auto [a, unit] = Bim::UnitsOf::Area(u, mm2);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.2f %s", a, unit);
            return buf;
        }

        bool ReadNumber(const std::string& s, double& out)
        {
            Bim::Json v;
            std::string error;
            if (!Props::Parse(Props::Kind::Number, s, Bim::Units::Millimetres, v, error))
                return false;
            out = v.get<double>();
            return true;
        }
    }

    // =========================================================
    // 工具的切换
    // =========================================================
    void MainFrame::SetTool(Tool tool)
    {
        // 放东西要在平面上（标高在三维视图里也能建）
        if (tool != Tool::Modify && tool != Tool::Level && !m_planLevel)
            ShowPlan(std::nullopt);
        if (tool != Tool::Modify && !m_selection.empty())
            m_selection.clear();
        m_tool = tool;
        m_toolPts.clear();
        m_typedLength.clear();
        m_found.reset();
        BuildOptionsBar();
        UpdateStatus();
        RefreshPanels();
        m_commands.NotifyStateChanged();
        if (m_viewport)
            m_viewport->RequestRender();
    }

    void MainFrame::Escape()
    {
        if (!m_typedLength.empty())
            m_typedLength.clear();
        else if (!m_toolPts.empty())
            m_toolPts.clear();
        else if (m_tool != Tool::Modify)
        {
            SetTool(Tool::Modify);
            return;
        }
        else
        {
            SetSelection({});
            return;
        }
        if (m_viewport)
            m_viewport->RequestRender();
    }

    const char* MainFrame::ToolCategory() const
    {
        switch (m_tool)
        {
        case Tool::Wall:      return "wall";
        case Tool::Door:      return "door";
        case Tool::Window:    return "window";
        case Tool::Component: return "component";
        case Tool::Roof:      return "roof";
        case Tool::Floor:     return "floor";
        case Tool::Ceiling:   return "ceiling";
        case Tool::Room:      return "room";
        case Tool::Grid:      return "grid";
        default:              return nullptr;
        }
    }

    const char* MainFrame::ToolPrompt() const
    {
        switch (m_tool)
        {
        case Tool::Modify:    return N_("Click to select, Ctrl adds or takes away, Esc clears; drag or middle-drag to pan, scroll to zoom");
        case Tool::Wall:      return m_toolPts.empty() ? N_("Click to enter the wall's start point.")
                                                       : N_("Click to build the wall to the next point, or type a length and press Enter. Enter or Esc ends the chain, Shift keeps it square.");
        case Tool::Door:      return N_("Click a wall to place the door.");
        case Tool::Window:    return N_("Click a wall to place the window.");
        case Tool::Component: return N_("Click where the component goes; Space turns it a quarter turn.");
        case Tool::Roof:      return N_("Click inside the walls to roof them, on the level above: the overhang runs past their faces.");
        case Tool::Floor:     return N_("Click inside the walls to floor them: the slab runs under their outline, its top at the level.");
        case Tool::Ceiling:   return N_("Click inside a room: a ceiling out to its walls' faces, as high as the Options Bar says.");
        case Tool::Room:      return N_("Click inside walls to place a room; the outline shows the room you'd get.");
        case Tool::Level:     return N_("Type the new level's elevation in the Options Bar, then Create.");
        case Tool::Grid:      return m_toolPts.empty() ? N_("Click to enter the grid's start point.") : N_("Click to enter the grid's end point.");
        }
        return "";
    }

    // =========================================================
    // 选项栏
    // =========================================================
    std::unique_ptr<MiniGUI::Node> MainFrame::CreateOptionsBar()
    {
        using namespace MiniGUI;
        auto bar = std::make_unique<Panel>(Theme::PanelAlt);
        LayoutStyle s;
        s.direction  = FlexDirection::Row;
        s.alignItems = Align::Center;
        s.height     = 34.0f;
        s.padding    = Edges::Make(10.0f, 0.0f, 10.0f, 0.0f);
        s.gap        = 8.0f;
        bar->SetLayoutStyle(s);
        m_optionsBody = bar.get();
        return bar;
    }

    void MainFrame::BuildOptionsBar()
    {
        using namespace MiniGUI;
        if (!m_optionsBody)
            return;
        m_optionsBody->RemoveAllChildren();
        const Bim::Model& m = m_session.GetModel();
        Node* bar = m_optionsBody;
        auto label = [&](const std::string& text) { bar->AddChild<Label>(text, 13.0f); };
        // 一个输入框：改了就记进 into（用时再读）
        auto field = [&](std::string& into, float width, const std::string& hint)
        {
            auto* box = bar->AddChild<TextBox>(into);
            box->SetPlaceholder(hint);
            LayoutStyle w;
            w.width = width;
            box->SetLayoutStyle(w);
            box->SetOnChanged([&into](const std::string& t) { into = t; });
            return box;
        };
        auto check = [&](const std::string& text, bool& into)
        {
            auto* box = bar->AddChild<CheckBox>(text, into);
            box->SetOnChanged([&into](bool on) { into = on; });
        };
        auto combo = [&](std::vector<std::string> items, int at, float width, std::function<void(int)> picked)
        {
            auto* box = bar->AddChild<ComboBox>(std::move(items), at);
            LayoutStyle w;
            w.width = width;
            box->SetLayoutStyle(w);
            box->SetOnChanged(std::move(picked));
        };

        static const std::pair<Tool, const char*> names[] = {
            { Tool::Wall, N_("Modify | Place Wall") },       { Tool::Door, N_("Modify | Place Door") },
            { Tool::Window, N_("Modify | Place Window") },   { Tool::Component, N_("Modify | Place Component") },
            { Tool::Roof, N_("Modify | Create Roof Footprint") }, { Tool::Floor, N_("Modify | Create Floor") },
            { Tool::Ceiling, N_("Modify | Place Ceiling") }, { Tool::Room, N_("Modify | Place Room") },
            { Tool::Level, N_("Modify | Place Level") },     { Tool::Grid, N_("Modify | Place Grid") },
        };
        for (const auto& [t, name] : names)
            if (t == m_tool)
            {
                bar->AddChild<Label>(Tr(name), 13.0f)->SetBold(true);
                bar->AddChild<Separator>(true);
            }
        const std::string unit = Bim::UnitsOf::Name(m.units);
        switch (m_tool)
        {
        case Tool::Modify:
            bar->AddChild<Label>(Tr("Pick a tool on the Architecture tab to place walls, doors, windows, roofs and more."), 13.0f, Theme::TextDim);
            break;
        case Tool::Wall:
        {
            // 高度：上面的某个标高，或未连接时输入的高度
            if (m_planLevel)
            {
                const double z = m.Elevation(*m_planLevel);
                std::vector<std::pair<Bim::Guid, std::string>> above;
                for (const auto& [id, l] : m.Levels())
                    if (l->elevation > z)
                        above.emplace_back(m.GuidOf(id), l->name);
                const std::optional<Bim::Guid> now = m_toolOpt.wallTop ? *m_toolOpt.wallTop
                                                   : above.empty()     ? std::nullopt
                                                                       : std::optional<Bim::Guid>(above.front().first);
                std::vector<std::string> items;
                int at = static_cast<int>(above.size());
                for (size_t i = 0; i < above.size(); ++i)
                {
                    items.push_back(above[i].second);
                    if (now && m.Same(*now, above[i].first))
                        at = static_cast<int>(i);
                }
                items.push_back(Tr("Unconnected"));
                label(Tr("Height:"));
                combo(items, at, 130.0f, [this, above](int i)
                {
                    if (i < 0)
                        return;
                    m_toolOpt.wallTop = static_cast<size_t>(i) < above.size() ? std::optional<Bim::Guid>(above[static_cast<size_t>(i)].first) : std::nullopt;
                    m_platform->Post([this] { BuildOptionsBar(); });
                });
                if (!now)
                    field(m_toolOpt.wallHeight, 70.0f, unit);
            }
            label(Tr("Location Line:"));
            std::vector<std::string> locations;
            int at = 0;
            for (const Bim::Location l : Bim::LocationOf::All)
            {
                if (l == m_toolOpt.wallLocation)
                    at = static_cast<int>(locations.size());
                locations.push_back(Tr(Bim::LocationOf::Label(l)));
            }
            combo(locations, at, 170.0f, [this](int i)
            {
                if (i >= 0)
                    m_toolOpt.wallLocation = Bim::LocationOf::All[static_cast<size_t>(i)];
            });
            check(Tr("Chain"), m_toolOpt.chain);
            break;
        }
        case Tool::Roof:
            label(Tr("Slope:"));
            field(m_toolOpt.roofSlope, 50.0f, "°");
            label(Tr("Overhang:"));
            field(m_toolOpt.roofOverhang, 70.0f, unit);
            check(Tr("Gable"), m_toolOpt.roofGable);
            break;
        case Tool::Ceiling:
            label(Tr("Height Offset:"));
            field(m_toolOpt.ceilingHeight, 70.0f, unit);
            break;
        case Tool::Component:
        {
            label(Tr("Rotation:"));
            auto* box = bar->AddChild<TextBox>(Props::Text(m_toolOpt.componentAngle));
            box->SetPlaceholder("°");
            LayoutStyle w;
            w.width = 60.0f;
            box->SetLayoutStyle(w);
            box->SetOnChanged([this](const std::string& t)
            {
                double a = 0.0;
                if (ReadNumber(t, a))
                    m_toolOpt.componentAngle = a;
            });
            break;
        }
        case Tool::Level:
        {
            label(Tr("Elevation"));
            field(m_toolOpt.levelElevation, 90.0f, unit);
            label(Tr("Name"));
            field(m_toolOpt.levelName, 150.0f, Tr("the next free name"));
            bar->AddChild<Button>(Tr("Create"), [this]
            {
                m_platform->Post([this]
                {
                    const auto z = Bim::UnitsOf::Parse(m_session.GetModel().units, m_toolOpt.levelElevation);
                    if (!z)
                    {
                        MiniGUI::ShowMessageBox(*m_ui, Tr("Could not create the level"),
                                                Format(Tr("`{0}` is not an elevation"), { m_toolOpt.levelElevation }));
                        return;
                    }
                    Bim::Json p{ { "elevation", *z } };
                    // 名称空着时用下一个空着的名称
                    if (!Props::Words(m_toolOpt.levelName).empty())
                        p["name"] = m_toolOpt.levelName;
                    if (RunCommand("level", p, N_("Could not create the level")))
                    {
                        m_toolOpt.levelElevation.clear();
                        m_toolOpt.levelName.clear();
                        SetTool(Tool::Modify);
                    }
                });
            });
            break;
        }
        default:
            break;
        }
    }

    // =========================================================
    // 点
    // =========================================================
    Bim::Snap::Found MainFrame::SnapAt(MiniGUI::Vec2 pixel, std::span<const Bim::P2> from, bool square) const
    {
        const Bim::Render::View& v = *m_planView;
        const double px = m_dpiScale;
        const Bim::Snap::Reach reach{ SnapPx * px / v.scale, PickPx * px / v.scale };
        return Bim::Snap::At(m_session.GetModel(), *m_planLevel, v.ToMm({ pixel.x, pixel.y }), reach, from, square, m_snaps);
    }

    Bim::Json MainFrame::Typed(const std::string& category, Bim::Json p) const
    {
        if (const auto f = m_toolOpt.openingFamilies.find(category); f != m_toolOpt.openingFamilies.end())
            p["family"] = f->second;
        if (const auto t = m_toolOpt.types.find(category); t != m_toolOpt.types.end())
            p["type"] = t->second;
        return p;
    }

    std::optional<Bim::Json> MainFrame::WallTop(std::string& error) const
    {
        const Bim::Model& m = m_session.GetModel();
        std::optional<Bim::Guid> top = m_toolOpt.wallTop ? *m_toolOpt.wallTop : m.LevelAbove(*m_planLevel);
        if (top)
            return Bim::Json{ { "top_level", Bim::ToJson(*top) } };
        const auto h = Bim::UnitsOf::Parse(m.units, m_toolOpt.wallHeight);
        if (!h || *h <= 0.0)
        {
            error = Format(Tr("`{0}` is not a height"), { m_toolOpt.wallHeight });
            return std::nullopt;
        }
        return Bim::Json{ { "height", *h } };
    }

    std::optional<std::pair<std::string, std::string>> MainFrame::Placing() const
    {
        const Bim::Model& m = m_session.GetModel();
        if (m_toolOpt.component)
            if (const auto f = m.families.find(m_toolOpt.component->first); f != m.families.end() && f->second.types.contains(m_toolOpt.component->second))
                return m_toolOpt.component;
        for (const auto& [name, f] : m.families)
            if (!Bim::Fam::CategoryOf::Hosted(f.category) && !f.types.empty())
                return std::pair{ name, f.types.begin()->first };
        return std::nullopt;
    }

    void MainFrame::PlacePoint(Bim::P2 p)
    {
        const Bim::Model& m = m_session.GetModel();
        const Bim::Json level = Bim::ToJson(*m_planLevel);
        switch (m_tool)
        {
        // 墙链：第一下是起点，之后每一下从上一点建一面墙
        case Tool::Wall:
        {
            if (m_toolPts.empty())
            {
                m_toolPts.push_back(p);
                break;
            }
            const Bim::P2 first = m_toolPts.front(), last = m_toolPts.back();
            if (last.Dist(p) <= Bim::Tol)
                break;
            std::string error;
            const auto top = WallTop(error);
            if (!top)
            {
                MiniGUI::ShowMessageBox(*m_ui, Tr("Could not build the wall"), error);
                break;
            }
            Bim::Json params = Typed("wall", Bim::Json{ { "points", Bim::Json::array({ Pt(last), Pt(p) }) }, { "level", level },
                                                        { "location", Bim::LocationOf::Name(m_toolOpt.wallLocation) } });
            params.update(*top);
            if (RunCommand("wall", params, N_("Could not build the wall")))
            {
                // 回到起点时墙链闭合；不连成链时每面墙单独画
                if (!m_toolOpt.chain || first.Dist(p) <= Bim::Tol)
                    m_toolPts.clear();
                else
                    m_toolPts.push_back(p);
            }
            break;
        }
        case Tool::Grid:
            if (m_toolPts.empty())
                m_toolPts.push_back(p);
            else if (m_toolPts.front().Dist(p) > Bim::Tol)
            {
                const Bim::P2 a = m_toolPts.front();
                m_toolPts.clear();
                RunCommand("grid", Bim::Json{ { "start", Pt(a) }, { "end", Pt(p) } }, N_("Could not place the grid"));
            }
            break;
        case Tool::Component:
            if (const auto what = Placing())
                RunCommand("component", Bim::Json{ { "family", what->first }, { "type", what->second }, { "at", Pt(p) }, { "level", level },
                                                   { "angle", m_toolOpt.componentAngle } },
                           N_("Could not place the component"));
            break;
        default:
            break;
        }
        (void)m;
        UpdateStatus();
        if (m_viewport)
            m_viewport->RequestRender();
    }

    void MainFrame::ToolClick(MiniGUI::Vec2 pixel, uint8_t modifiers)
    {
        if (!m_planLevel || !m_planView)
            return;
        const Bim::Model& m = m_session.GetModel();
        const Bim::Render::View& v = *m_planView;
        const bool shift = (modifiers & static_cast<uint8_t>(MiniGUI::ModifierKey::Shift)) != 0;
        const Bim::P2 at = Round10(v.ToMm({ pixel.x, pixel.y }));
        const Bim::Json level = Bim::ToJson(*m_planLevel);
        m_typedLength.clear();
        switch (m_tool)
        {
        case Tool::Wall: case Tool::Grid:
            PlacePoint(SnapAt(pixel, m_toolPts, shift).at);
            return;
        case Tool::Component:
            PlacePoint(SnapAt(pixel, {}, false).at);
            return;
        // 离那一段墙的中点近时放在正中
        case Tool::Door: case Tool::Window:
        {
            const bool door = m_tool == Tool::Door;
            const char* c = door ? "door" : "window";
            Bim::Json p = Typed(c, Bim::Json{ { "at", Pt(at) }, { "level", level } });
            Bim::Opening o;
            std::string error;
            if (Bim::OpeningOf(m, p, door, o, error))
                if (const auto middle = m.Centred(o, SnapPx * m_dpiScale / v.scale))
                    p["offset"] = *middle;
            RunCommand(c, p, door ? N_("Could not place the door") : N_("Could not place the window"));
            break;
        }
        case Tool::Room:
            RunCommand("room", Bim::Json{ { "point", Pt(at) }, { "level", level } }, N_("Could not place the room"));
            break;
        case Tool::Ceiling:
        {
            const auto offset = Bim::UnitsOf::Parse(m.units, m_toolOpt.ceilingHeight);
            if (!offset)
            {
                MiniGUI::ShowMessageBox(*m_ui, Tr("Could not place the ceiling"), Format(Tr("`{0}` is not a height"), { m_toolOpt.ceilingHeight }));
                break;
            }
            RunCommand("ceiling", Typed("ceiling", Bim::Json{ { "point", Pt(at) }, { "level", level }, { "offset", *offset } }),
                       N_("Could not place the ceiling"));
            break;
        }
        // 建好的楼板、屋顶选中，回到修改
        case Tool::Floor: case Tool::Roof:
        {
            Bim::Json p;
            const char* c = m_tool == Tool::Floor ? "floor" : "roof";
            if (m_tool == Tool::Floor)
                p = Typed("floor", Bim::Json{ { "level", level } });
            else
            {
                double slope = 0.0;
                const auto overhang = Bim::UnitsOf::Parse(m.units, m_toolOpt.roofOverhang);
                if (!ReadNumber(m_toolOpt.roofSlope, slope) || !overhang)
                {
                    MiniGUI::ShowMessageBox(*m_ui, Tr("Could not create the roof"),
                                            Format(Tr("The slope `{0}` and the overhang `{1}` must be numbers"), { m_toolOpt.roofSlope, m_toolOpt.roofOverhang }));
                    break;
                }
                const auto on = m.LevelAbove(*m_planLevel).value_or(*m_planLevel);
                p = Typed("roof", Bim::Json{ { "level", Bim::ToJson(on) }, { "slope", slope }, { "overhang", *overhang }, { "gable", m_toolOpt.roofGable } });
            }
            Bim::Json made;
            if (RunCommand(c, p, m_tool == Tool::Floor ? N_("Could not create the floor") : N_("Could not create the roof"), &made))
            {
                SetTool(Tool::Modify);
                if (made.contains("id") && made["id"].is_number_unsigned())
                    SetSelection({ made["id"].get<Bim::Id>() });
            }
            break;
        }
        default:
            break;
        }
    }

    void MainFrame::PlaceTyped()
    {
        const std::string typed = std::move(m_typedLength);
        m_typedLength.clear();
        if (m_toolPts.empty() || !m_hoverPixel || !m_planView)
            return;
        const Bim::Model& m = m_session.GetModel();
        const Bim::P2 base = m_toolPts.back();
        const Bim::P2 cursor = m_planView->ToMm({ m_hoverPixel->x, m_hoverPixel->y });
        // 沿捕捉到的方向，否则光标的方向（离角度捕捉的倍数 3° 以内时落上去）
        Bim::P2 toward;
        if (m_found && m_found->at.Dist(base) > Bim::Tol)
            toward = (m_found->at - base).Unit();
        else
        {
            double a = std::atan2(cursor.y - base.y, cursor.x - base.x);
            const double inc = m_snaps.angle;
            if (inc >= 1.0 && inc <= 180.0)
            {
                const double step = inc * std::numbers::pi / 180.0, near = std::round(a / step) * step;
                if (std::abs(a - near) <= std::min(3.0 * std::numbers::pi / 180.0, 0.4 * step))
                    a = near;
            }
            toward = { std::cos(a), std::sin(a) };
        }
        const auto len = Bim::UnitsOf::Parse(m.units, typed);
        if (!len || *len <= 0.0)
        {
            MiniGUI::ShowMessageBox(*m_ui, Tr("Not a length"), Format(Tr("`{0}` is not a length"), { typed }));
            return;
        }
        PlacePoint(Bim::Micron(base + toward * *len));
    }

    bool MainFrame::OnPlanKey(const MiniGUI::KeyEvent& e)
    {
        using MiniGUI::Key;
        if (e.type != MiniGUI::KeyEventType::Down || !m_planLevel || m_tool == Tool::Modify)
            return false;
        switch (e.key)
        {
        case Key::Enter:
            if (!m_typedLength.empty())
                PlaceTyped();
            else if (m_tool == Tool::Wall)
                m_toolPts.clear();      // 结束墙链
            break;
        case Key::Backspace:
            if (m_typedLength.empty())
                return false;
            {
                std::u32string u = Bim::Text::Utf32(m_typedLength);
                u.pop_back();
                m_typedLength.clear();
                for (const char32_t c : u)
                    Bim::Text::AppendUtf8(m_typedLength, c);
            }
            break;
        case Key::Space:
            if (m_tool != Tool::Component)
                return false;
            m_toolOpt.componentAngle = std::fmod(m_toolOpt.componentAngle + 90.0, 360.0);
            BuildOptionsBar();
            break;
        default:
            return false;
        }
        UpdateStatus();
        m_viewport->RequestRender();
        return true;
    }

    bool MainFrame::OnPlanText(std::string_view text)
    {
        // 画了起点之后输入的是长度（Enter 时用）；空格转构件，不算
        if (m_toolPts.empty() || (m_tool != Tool::Wall && m_tool != Tool::Grid) || text == " " || m_typedLength.size() > 40)
            return false;
        for (const char c : text)
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
        m_typedLength += text;
        m_viewport->RequestRender();
        return true;
    }

    // =========================================================
    // 预览
    // =========================================================
    void MainFrame::BuildPreview(Bim::Render::Drawing& d)
    {
        if (m_planTipBox)
            m_planTipBox->SetVisible(false);
        m_found.reset();
        if (m_tool == Tool::Modify || m_tool == Tool::Level || !m_hoverPixel || !m_planLevel || !m_planView)
            return;
        const Bim::Model& m = m_session.GetModel();
        const Bim::Render::View& v = *m_planView;
        const float px = m_dpiScale;
        const Bim::P2 mm = v.ToMm({ m_hoverPixel->x, m_hoverPixel->y });
        std::string tip;
        // 一个还不在模型里的元素：照平面会画的样子，用强调色
        auto sketched = [&](const Bim::Element& e)
        {
            Bim::Render::Drawing s = Bim::Render::Sketch(m, *m_planLevel, v, e);
            for (Bim::Render::Prim& p : s.prims)
            {
                p.color = Accent;
                if (p.kind == Bim::Render::Prim::Kind::Fill)
                    continue;      // 填色会盖住下面的墙：只画线
                d.Push(std::move(p));
            }
        };
        // 捕捉到的点：记号与引导线
        auto marker = [&](const Bim::Snap::Found& f)
        {
            m_found = f;
            const auto [x, y] = v.ToPx(f.at);
            const float r = 6.0f * px, w = 2.0f * px;
            if (f.guide)
            {
                const auto [gx, gy] = v.ToPx(f.guide->from);
                const float dx = x - gx, dy = y - gy, len = std::hypot(dx, dy);
                if (len > 1.0f)
                {
                    const float ux = dx / len, uy = dy / len, back = std::min(len, 4000.0f) + 24.0f * px, on = 28.0f * px;
                    Stroke(d, { { x - ux * back, y - uy * back }, { x + ux * on, y + uy * on } }, 1.0f * px, false,
                           std::vector<float>{ 6.0f * px, 4.0f * px });
                }
            }
            using K = Bim::Snap::Kind;
            switch (f.kind)
            {
            case K::Endpoint:
                Stroke(d, { { x - r, y - r }, { x + r, y - r }, { x + r, y + r }, { x - r, y + r } }, w, true);
                break;
            case K::Midpoint:
                Stroke(d, { { x, y - r }, { x + r, y + r * 0.8f }, { x - r, y + r * 0.8f } }, w, true);
                break;
            case K::Intersection:
                Stroke(d, { { x - r, y - r }, { x + r, y + r } }, w);
                Stroke(d, { { x - r, y + r }, { x + r, y - r } }, w);
                break;
            case K::Centre:
            {
                std::vector<Bim::Render::Px> c;
                for (int k = 0; k < 16; ++k)
                    c.emplace_back(x + r * std::cos(k * 0.3927f), y + r * std::sin(k * 0.3927f));
                Stroke(d, std::move(c), w, true);
                break;
            }
            case K::Perpendicular:
                Stroke(d, { { x - r, y + r * 0.8f }, { x + r, y + r * 0.8f } }, w);
                Stroke(d, { { x, y + r * 0.8f }, { x, y - r } }, w);
                break;
            case K::Nearest:
                Stroke(d, { { x, y - r }, { x + r, y }, { x, y + r }, { x - r, y } }, w, true);
                break;
            case K::Angle:
                Stroke(d, { { x - r * 0.4f, y }, { x + r * 0.4f, y } }, 3.0f * px);
                break;
            case K::Free:
                Stroke(d, { { x - r * 0.7f, y - r * 0.7f }, { x + r * 0.7f, y - r * 0.7f }, { x + r * 0.7f, y + r * 0.7f }, { x - r * 0.7f, y + r * 0.7f } }, w, true);
                break;
            }
            if (f.kind != K::Free && f.kind != K::Angle)
                tip = Tr(Bim::Snap::Name(f.kind));
            return f.at;
        };
        const Bim::Json level = Bim::ToJson(*m_planLevel);
        switch (m_tool)
        {
        case Tool::Wall: case Tool::Grid:
        {
            const Bim::P2 next = marker(SnapAt(*m_hoverPixel, m_toolPts, m_hoverShift));
            if (!m_toolPts.empty())
            {
                const Bim::P2 last = m_toolPts.back();
                Stroke(d, { v.ToPx(last), v.ToPx(next) }, 2.0f * px);
                // 长度与角度（转角处是与前一段的夹角，否则从正东量），或输入的长度
                std::optional<double> angle;
                if (m_toolPts.size() >= 2)
                    angle = Bim::Snap::Corner(m_toolPts[m_toolPts.size() - 2], last, next);
                const std::string reading = Bim::UnitsOf::WithUnit(m.units, last.Dist(next)) + "  " +
                                            Degrees(angle.value_or(Bim::Snap::Measure(last, next).angle));
                tip = !m_typedLength.empty() ? m_typedLength + " (Enter)" : (tip.empty() ? reading : tip + "    " + reading);
            }
            break;
        }
        case Tool::Component:
        {
            const Bim::P2 at = marker(SnapAt(*m_hoverPixel, {}, false));
            if (const auto what = Placing())
            {
                Bim::Component c;
                c.family   = what->first;
                c.typeName = what->second;
                c.level    = *m_planLevel;
                c.at       = at;
                c.angle    = m_toolOpt.componentAngle;
                sketched(Bim::Element{ c });
            }
            break;
        }
        case Tool::Door: case Tool::Window:
        {
            const bool door = m_tool == Tool::Door;
            Bim::Json p = Typed(door ? "door" : "window", Bim::Json{ { "at", Pt(Round10(mm)) }, { "level", level } });
            Bim::Opening o;
            std::string error;
            if (Bim::OpeningOf(m, p, door, o, error))
            {
                if (const auto middle = m.Centred(o, SnapPx * m_dpiScale / v.scale))
                    o.offset = *middle;
                if (door)
                    sketched(Bim::Element{ Bim::Door{ o } });
                else
                    sketched(Bim::Element{ Bim::Window{ o } });
                // 离那一段墙两端多远
                if (const auto span = m.OpeningSpan(o))
                    tip = Bim::UnitsOf::Length(m.units, o.offset - span->first) + " | " + Bim::UnitsOf::Length(m.units, span->second - o.offset);
            }
            break;
        }
        case Tool::Room: case Tool::Ceiling:
        {
            const auto lines = m_tool == Tool::Room ? m.RoomLines(*m_planLevel, m.roomArea) : m.FaceLines(*m_planLevel);
            if (const auto g = Bim::Geom::RegionAt(lines, mm, Bim::Tol))
            {
                Ring(d, v, g->outer, 2.0f * px);
                for (const Bim::Ring& h : g->holes)
                    Ring(d, v, h, 2.0f * px);
                tip = Area(m.units, std::abs(g->Area()));
            }
            break;
        }
        case Tool::Floor:
            if (const auto outline = Bim::FloorOutline(m, *m_planLevel))
            {
                Ring(d, v, outline->first, 2.0f * px);
                for (const Bim::Ring& r : outline->second)
                    Ring(d, v, r, 2.0f * px);
            }
            break;
        case Tool::Roof:
        {
            const auto overhang = Bim::UnitsOf::Parse(m.units, m_toolOpt.roofOverhang);
            std::string error;
            const auto on = m.LevelAbove(*m_planLevel).value_or(*m_planLevel);
            if (overhang)
                if (const auto f = Bim::RoofFootprint(m, on, *overhang, error))
                {
                    Ring(d, v, f->first, 2.0f * px);
                    for (const Bim::Ring& r : f->second)
                        Ring(d, v, r, 2.0f * px);
                }
            break;
        }
        default:
            break;
        }
        // 读数跟着光标，在它右上方
        if (m_planTipBox && !tip.empty())
        {
            m_planTip->SetText(tip);
            MiniGUI::LayoutStyle& s = m_planTipBox->EditLayoutStyle();
            s.left = m_hoverPixel->x / px + 14.0f;
            s.top  = std::max(0.0f, m_hoverPixel->y / px - 30.0f);
            m_planTipBox->SetVisible(true);
        }
    }
}
