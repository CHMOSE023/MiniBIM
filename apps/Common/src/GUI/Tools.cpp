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
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numbers>

// 放东西的工具，照 BIMCraft（同 Revit）：
//   - 墙：每次单击从上一点建一面墙，连成墙链（选项栏的"链"关掉时每面墙单独画）；Enter、Esc 结束，回到起点时自动结束
//   - 门、窗：单击墙，离那一段墙的中点近时放在正中；构件：单击处，空格转 90°
//   - 屋顶：单击墙里面，在上一层按下面一层的墙建迹线屋顶；楼板：沿这一层的墙；天花板、房间：单击处四周围合的区域
//   - 标高：选项栏输入高程后"创建"；轴网：两次单击两端
//   - 结构柱、独立基础（Stand）：单击处；梁、条形基础与别的 run（Run）：单击两端，连成链时每一下从上一点再放一根
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

        // Stand、Run 工具的上下文选项卡标题（BIMCraft structure.rs 的 title）
        const char* PlaceTitle(std::string_view c)
        {
            static const std::pair<const char*, const char*> titles[] = {
                { "beam", N_("Modify | Place Beam") },          { "stripfooting", N_("Modify | Place Wall Footing") },
                { "pipe", N_("Modify | Place Pipe") },          { "conduit", N_("Modify | Place Conduit") },
                { "cabletray", N_("Modify | Place Cable Tray") }, { "wire", N_("Modify | Place Wire") },
                { "downconductor", N_("Modify | Place Down Conductor") }, { "groundconductor", N_("Modify | Place Earthing Conductor") },
                { "cable", N_("Modify | Place Cabling") },
                { "structuralcolumn", N_("Modify | Place Structural Column") }, { "footing", N_("Modify | Place Isolated Footing") },
            };
            for (const auto& [category, title] : titles)
                if (c == category)
                    return title;
            return nullptr;
        }

        // 卫浴、电气、防雷、数据的设备：构件工具只放这几类的族（移植自 BIMCraft mep.rs 的 FIXTURES）
        struct Fixture
        {
            const char*                           key;
            std::initializer_list<Bim::Fam::Category> kinds;
            const char*                           title;
        };
        using C = Bim::Fam::Category;
        const Fixture Fixtures[] = {
            { "plumbing", { C::Plumbing, C::Drain }, N_("Modify | Place Plumbing Fixture") },
            { "fitting", { C::PipeFitting }, N_("Modify | Place Pipe Fitting") },
            { "accessory", { C::PipeAccessory }, N_("Modify | Place Pipe Accessory") },
            { "equipment", { C::Equipment }, N_("Modify | Place Electrical Equipment") },
            { "device", { C::Outlet, C::Switch }, N_("Modify | Place Device") },
            { "lighting", { C::Lighting }, N_("Modify | Place Lighting Fixture") },
            { "airterminal", { C::AirTerminal }, N_("Modify | Place Air Terminal") },
            { "earthing", { C::Earthing }, N_("Modify | Place Earthing Electrode") },
            { "inspection", { C::InspectionBox }, N_("Modify | Place Inspection Box") },
            { "dataoutlet", { C::DataOutlet }, N_("Modify | Place Data Outlet") },
            { "voiceoutlet", { C::VoiceOutlet }, N_("Modify | Place Voice Outlet") },
            { "rack", { C::Rack }, N_("Modify | Place Rack") },
            { "patchpanel", { C::PatchPanel }, N_("Modify | Place Patch Panel") },
        };

        const Fixture* FixtureOf(const char* key)
        {
            if (key)
                for (const Fixture& f : Fixtures)
                    if (std::string_view(key) == f.key)
                        return &f;
            return nullptr;
        }

        const std::pair<Bim::Justify, const char*> Justifications[] = {
            { Bim::Justify::Top, N_("Top") }, { Bim::Justify::Centre, N_("Centre") }, { Bim::Justify::Bottom, N_("Bottom") },
        };
    }

    // =========================================================
    // 工具的切换
    // =========================================================
    void MainFrame::SetTool(Tool tool)
    {
        // 放东西要在平面上（标高在三维视图里也能建）
        if (tool != Tool::Modify && tool != Tool::Level && !m_planLevel)
            ShowPlan(std::nullopt);
        if (NeedsSelection(tool) && m_selection.empty())
            return;
        // 草图模式里只有修改和画草图线的工具；画草图线的工具只在草图模式里
        if (tool != Tool::Modify && m_sketch.has_value() != SketchTool(tool))
            return;
        m_tool = tool;
        if (tool != Tool::Stand && tool != Tool::Run && tool != Tool::Component)
            m_toolOf = nullptr;
        if ((ToolCategory() || tool == Tool::Level) && !m_selection.empty())
            m_selection.clear();
        m_toolPts.clear();
        m_rotateCentre = tool == Tool::Rotate ? SelectionMiddle() : std::nullopt;
        m_placeCentre  = false;
        m_alignRef.reset();
        m_trimFirst.reset();
        m_toolRefs.clear();
        m_typedLength.clear();
        m_found.reset();
        BuildOptionsBar();
        UpdateStatus();
        RefreshPanels();
        m_commands.NotifyStateChanged();
        if (m_viewport)
            m_viewport->RequestRender();
    }

    void MainFrame::SetToolOf(Tool tool, const char* category)
    {
        if (m_tool == tool && m_toolOf && std::string_view(m_toolOf) == category)
        {
            SetTool(Tool::Modify);
            return;
        }
        if (tool == Tool::Run)
            KeepRunOptions(category);
        m_toolOf = category;
        SetTool(tool);
        // 没能换（草图模式里）：类别也不换
        if (m_tool != tool)
            m_toolOf = nullptr;
    }

    void MainFrame::KeepRunOptions(const char* category)
    {
        ToolOptions& o = m_toolOpt;
        if (std::string_view(o.runLoaded) == category)
            return;
        o.runKept[o.runLoaded] = { o.runJustify, o.runOffset };
        if (const auto kept = o.runKept.find(category); kept != o.runKept.end())
            std::tie(o.runJustify, o.runOffset) = kept->second;
        else
        {
            o.runJustify = Bim::Runs::DefaultJustify(category);
            o.runOffset  = "0";
        }
        o.runLoaded = category;
    }

    void MainFrame::Escape()
    {
        if (!m_typedLength.empty())
            m_typedLength.clear();
        else if (!m_toolPts.empty() || m_alignRef || m_trimFirst || !m_toolRefs.empty())
        {
            m_toolPts.clear();
            m_alignRef.reset();
            m_trimFirst.reset();
            m_toolRefs.clear();
        }
        else if (m_tool != Tool::Modify)
        {
            SetTool(Tool::Modify);
            return;
        }
        // 草图模式里不退出草图（完成或取消在功能区与选项栏），只放下选中的线
        else if (m_sketch)
            m_sketch->picked.clear();
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
        case Tool::Column:    return "column";
        case Tool::Component: return "component";
        case Tool::Roof:      return "roof";
        case Tool::Floor:     return "floor";
        case Tool::Ceiling:   return "ceiling";
        case Tool::Room:      return "room";
        case Tool::Grid:      return "grid";
        case Tool::Stand: case Tool::Run: return m_toolOf;
        case Tool::Mass:      return "mass";
        default:              return nullptr;
        }
    }

    const char* MainFrame::ToolPrompt() const
    {
        switch (m_tool)
        {
        case Tool::Modify:    return m_sketch ? N_("Click a sketch line to select it, Ctrl adds; drag a selected line's end to move it. Delete removes, Ctrl+Z steps back.")
                                              : N_("Click to select, Ctrl adds or takes away, Esc clears; drag or middle-drag to pan, scroll to zoom");
        case Tool::Wall:      return m_toolPts.empty() ? N_("Click to enter the wall's start point.")
                                                       : N_("Click to build the wall to the next point, or type a length and press Enter. Enter or Esc ends the chain, Shift keeps it square.");
        case Tool::Door:      return N_("Click a wall to place the door.");
        case Tool::Window:    return N_("Click a wall to place the window.");
        case Tool::Column:    return N_("Click to place a column, as high as the Options Bar says.");
        case Tool::Stand:     return m_toolOf && std::string_view(m_toolOf) == "structuralcolumn"
                                         ? N_("Click to place a structural column, as high as the Options Bar says.")
                                         : N_("Click to place an isolated footing: its top at the Options Bar's offset, as thick as it says.");
        case Tool::Run:       return m_toolPts.empty() ? N_("Click the start of the run: a chain of clicks makes one for each step, Esc ends it.")
                                                       : N_("Click the end of the run, or type a length and press Enter. Esc stops the chain.");
        case Tool::Component: return N_("Click where the component goes; Space turns it a quarter turn.");
        case Tool::Mass:      return m_toolPts.empty()    ? N_("Click the first corner of the mass's footprint.")
                                   : m_toolPts.size() < 3 ? N_("Click the footprint's next corner. Esc cancels.")
                                                          : N_("Click the next corner; Enter, or the first corner again, finishes the mass. Esc cancels.");
        case Tool::Roof:      return N_("Click inside the walls to roof them, on the level above: the overhang runs past their faces.");
        case Tool::Floor:     return N_("Click inside the walls to floor them: the slab runs under their outline, its top at the level.");
        case Tool::Ceiling:   return N_("Click inside a room: a ceiling out to its walls' faces, as high as the Options Bar says.");
        case Tool::Room:      return N_("Click inside walls to place a room; the outline shows the room you'd get.");
        case Tool::Level:     return N_("Type the new level's elevation in the Options Bar, then Create.");
        case Tool::Grid:      return m_toolPts.empty() ? N_("Click to enter the grid's start point.") : N_("Click to enter the grid's end point.");
        case Tool::Section:   return m_toolPts.empty() ? N_("Click to enter the section's start point.")
                                                       : N_("Click to enter the section's end point; it looks to the left of the line.");
        case Tool::Move:      return m_toolPts.empty() ? N_("Click a base point to measure from.") : N_("Click where the base point goes. Esc cancels.");
        case Tool::Copy:      return m_toolPts.empty() ? N_("Click a base point to measure from.") : N_("Click where the copy's base point goes. Esc cancels.");
        case Tool::Rotate:    return m_placeCentre || !m_rotateCentre ? N_("Click the centre of rotation.")
                                   : m_toolPts.empty() ? N_("Click the start of the angle, or type an angle in the Options Bar.")
                                                       : N_("Click the end of the angle. Esc cancels.");
        case Tool::MirrorPick: return N_("Pick a wall or grid to mirror across.");
        case Tool::MirrorDraw: return m_toolPts.empty() ? N_("Click the start of the mirror axis.") : N_("Click the end of the mirror axis.");
        case Tool::Align:     return m_alignRef ? N_("Pick the element's highlighted edge to move and turn onto the reference.")
                                                : N_("Pick a reference edge or centre line; the line under the cursor is highlighted.");
        case Tool::Trim:      return m_trimFirst ? N_("Pick the second wall, on the part to keep.") : N_("Pick the first wall, on the part to keep.");
        case Tool::Split:     return N_("Click a wall where to split it.");
        case Tool::Offset:    return N_("Point to a wall: the offset goes to the cursor's side. Click to place it.");
        case Tool::SketchLine: return m_toolPts.empty() ? N_("Click to start a line.")
                                                        : N_("Click the line's end, or type a length and press Enter; back at the start closes the loop. Enter or Esc ends the chain.");
        case Tool::SketchRect: return m_toolPts.empty() ? N_("Click the rectangle's first corner.") : N_("Click the opposite corner.");
        case Tool::SketchPick: return N_("Pick walls: each adds a line along the wall's location line.");
        case Tool::Dimension: return m_toolRefs.size() < 2 ? N_("Pick parallel wall faces, wall ends, doors, windows or grid lines to dimension.")
                                                           : N_("Pick another reference, or click away from them where the dimension line goes. Esc starts over.");
        case Tool::Angular:   return m_toolRefs.size() < 2 ? N_("Pick two lines that meet: wall faces, wall centre lines or grid lines.")
                                                           : N_("Click where the arc goes. Esc starts over.");
        case Tool::Radial:    return m_toolRefs.empty() ? N_("Pick a round column's edge.") : N_("Click where the radius line goes. Esc starts over.");
        case Tool::Spot:      return N_("Click a floor, a roof, a stair, a wall or a column to read its top's elevation.");
        case Tool::Text:      return N_("Type the note in the Options Bar, then click where it goes.");
        case Tool::Tag:       return N_("Click an element to tag it; Tag All tags the doors and windows that have none.");
        case Tool::DetailLine: return m_toolPts.empty() ? N_("Click the line's start.")
                                                        : N_("Click the line's end, or type a length and press Enter. Enter or Esc ends the chain.");
        case Tool::Measure:   return m_toolPts.size() == 1 ? N_("Click the second point.") : N_("Click the first point to measure from.");
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
            { Tool::Window, N_("Modify | Place Window") },   { Tool::Column, N_("Modify | Place Column") },
            { Tool::Component, N_("Modify | Place Component") },
            { Tool::Roof, N_("Modify | Create Roof Footprint") }, { Tool::Floor, N_("Modify | Create Floor") },
            { Tool::Ceiling, N_("Modify | Place Ceiling") }, { Tool::Room, N_("Modify | Place Room") },
            { Tool::Level, N_("Modify | Place Level") },     { Tool::Grid, N_("Modify | Place Grid") },
            { Tool::Move, N_("Modify | Move") },             { Tool::Copy, N_("Modify | Copy") },
            { Tool::Rotate, N_("Modify | Rotate") },         { Tool::MirrorPick, N_("Modify | Mirror Pick Axis") },
            { Tool::MirrorDraw, N_("Modify | Mirror Draw Axis") }, { Tool::Align, N_("Modify | Align") },
            { Tool::Trim, N_("Modify | Trim/Extend to Corner") }, { Tool::Split, N_("Modify | Split Element") },
            { Tool::Offset, N_("Modify | Offset") },         { Tool::Mass, N_("Modify | Create Mass") },
            { Tool::Dimension, N_("Modify | Place Dimensions") }, { Tool::Angular, N_("Modify | Place Dimensions") },
            { Tool::Radial, N_("Modify | Place Dimensions") }, { Tool::Spot, N_("Modify | Spot Elevation") },
            { Tool::Text, N_("Modify | Place Text") },       { Tool::Tag, N_("Modify | Tag") },
            { Tool::DetailLine, N_("Modify | Place Detail Lines") }, { Tool::Measure, N_("Modify | Measure") },
            { Tool::Section, N_("Modify | Section") },
        };
        const std::string unit = Bim::UnitsOf::Name(m.units);
        // 高度：上面的某个标高（没选时上一层），或未连接时输入的高度（墙、柱）
        auto heightOptions = [&](std::optional<std::optional<Bim::Guid>>& top, std::string& height)
        {
            if (!m_planLevel)
                return;
            const double z = m.Elevation(*m_planLevel);
            std::vector<std::pair<Bim::Guid, std::string>> above;
            for (const auto& [id, l] : m.Levels())
                if (l->elevation > z)
                    above.emplace_back(m.GuidOf(id), l->name);
            const std::optional<Bim::Guid> now = top           ? *top
                                               : above.empty() ? std::nullopt
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
            combo(items, at, 130.0f, [this, above, &top](int i)
            {
                if (i < 0)
                    return;
                top = static_cast<size_t>(i) < above.size() ? std::optional<Bim::Guid>(above[static_cast<size_t>(i)].first) : std::nullopt;
                m_platform->Post([this] { BuildOptionsBar(); });
            });
            if (!now)
                field(height, 70.0f, unit);
        };
        // 库条目（线样式、文字类型……）的下拉框：选的记进 into，没选或已不在时显示 fallback
        auto named = [&](const std::vector<std::string>& names, std::string& into, const std::string* fallback, float width)
        {
            int at = 0;
            for (size_t i = 0; i < names.size(); ++i)
                if (names[i] == into || (fallback && names[i] == *fallback && std::find(names.begin(), names.end(), into) == names.end()))
                    at = static_cast<int>(i);
            combo(names, at, width, [names, &into](int i)
            {
                if (i >= 0)
                    into = names[static_cast<size_t>(i)];
            });
        };
        auto keys = [](const auto& map)
        {
            std::vector<std::string> out;
            for (const auto& [name, _] : map)
                out.push_back(name);
            return out;
        };
        // 草图模式：画的是什么、完成与取消；新建天花板、屋顶时还有它们的设置
        if (m_sketch)
        {
            static const char* const titles[] = { N_("Modify | Create Floor Boundary"), N_("Modify | Create Ceiling Boundary"),
                                                  N_("Modify | Create Roof Footprint"), N_("Modify | Edit Boundary"),
                                                  N_("Modify | Create Filled Region Boundary"), N_("Modify | Create Masking Region Boundary") };
            bar->AddChild<Label>(Tr(titles[static_cast<size_t>(m_sketch->of)]), 13.0f)->SetBold(true);
            bar->AddChild<Separator>(true);
            // 完成、取消会重建选项栏：等这个按钮的回调回去之后再做
            bar->AddChild<Button>(Tr("Finish Edit Mode"), [this] { m_platform->Post([this] { FinishSketch(); }); });
            bar->AddChild<Button>(Tr("Cancel Edit Mode"), [this] { m_platform->Post([this] { CancelSketch(); }); });
            if (m_sketch->of == SketchOf::Ceiling)
            {
                label(Tr("Height Offset:"));
                field(m_toolOpt.ceilingHeight, 70.0f, unit);
            }
            else if (m_sketch->of == SketchOf::Roof)
            {
                label(Tr("Slope:"));
                field(m_toolOpt.roofSlope, 50.0f, "°");
            }
            // 填充区域的类型（遮罩的除外）
            else if (m_sketch->of == SketchOf::Region)
            {
                std::vector<std::string> types;
                for (const auto& [name, t] : m.regionTypes)
                    if (!t.masking)
                        types.push_back(name);
                label(Tr("Type:"));
                named(types, m_toolOpt.regionType, Bim::Fills::DefaultRegionType(m.regionTypes, m.language), 150.0f);
            }
            return;
        }
        // 楼板、天花板、屋顶也可以画轮廓（草图模式）
        auto boundaryLine = [&](SketchOf of)
        {
            bar->AddChild<Button>(Tr("Boundary Line"), [this, of] { m_platform->Post([this, of] { StartSketch(of, 0, {}); }); });
        };
        for (const auto& [t, name] : names)
            if (t == m_tool && !(t == Tool::Component && FixtureOf(m_toolOf)))
            {
                bar->AddChild<Label>(Tr(name), 13.0f)->SetBold(true);
                bar->AddChild<Separator>(true);
            }
        const Fixture* fixture = m_tool == Tool::Component ? FixtureOf(m_toolOf) : nullptr;
        if (const char* title = fixture ? fixture->title : (m_tool == Tool::Stand || m_tool == Tool::Run) && m_toolOf ? PlaceTitle(m_toolOf) : nullptr)
        {
            bar->AddChild<Label>(Tr(title), 13.0f)->SetBold(true);
            bar->AddChild<Separator>(true);
        }
        switch (m_tool)
        {
        case Tool::Modify:
            bar->AddChild<Label>(Tr("Pick a tool on the Architecture tab to place walls, doors, windows, roofs and more."), 13.0f, Theme::TextDim);
            break;
        case Tool::Wall:
        {
            heightOptions(m_toolOpt.wallTop, m_toolOpt.wallHeight);
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
            boundaryLine(SketchOf::Roof);
            break;
        case Tool::Floor:
            boundaryLine(SketchOf::Floor);
            break;
        case Tool::Ceiling:
            label(Tr("Height Offset:"));
            field(m_toolOpt.ceilingHeight, 70.0f, unit);
            boundaryLine(SketchOf::Ceiling);
            break;
        case Tool::Column:
            heightOptions(m_toolOpt.columnTop, m_toolOpt.columnHeight);
            break;
        case Tool::Mass:
            heightOptions(m_toolOpt.massTop, m_toolOpt.massHeight);
            break;
        // 结构柱的高度；独立基础的厚度与顶的高度
        case Tool::Stand:
            if (m_toolOf && std::string_view(m_toolOf) == "structuralcolumn")
                heightOptions(m_toolOpt.columnTop, m_toolOpt.columnHeight);
            else
            {
                label(Tr("Thickness:"));
                field(m_toolOpt.footingThickness, 70.0f, unit);
                label(Tr("Top Offset:"));
                field(m_toolOpt.footingTop, 70.0f, unit);
            }
            break;
        // run 的高度在截面哪条线上、它在标高上多高；条形基础只有顶的高度
        case Tool::Run:
            if (m_toolOf && std::string_view(m_toolOf) != "stripfooting")
            {
                label(Tr("Justification:"));
                std::vector<std::string> items;
                int at = 0;
                for (const auto& [j, n] : Justifications)
                {
                    if (j == m_toolOpt.runJustify)
                        at = static_cast<int>(items.size());
                    items.push_back(Tr(n));
                }
                combo(items, at, 100.0f, [this](int i)
                {
                    if (i >= 0)
                        m_toolOpt.runJustify = Justifications[static_cast<size_t>(i)].first;
                });
                label(Tr("Offset:"));
                field(m_toolOpt.runOffset, 70.0f, unit);
            }
            else
            {
                label(Tr("Top Offset:"));
                field(m_toolOpt.footingTop, 70.0f, unit);
            }
            check(Tr("Chain"), m_toolOpt.chain);
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
        case Tool::Move:
            check(Tr("Constrain"), m_modifyOpt.constrain);
            break;
        case Tool::Copy:
            check(Tr("Constrain"), m_modifyOpt.constrain);
            check(Tr("Multiple"), m_modifyOpt.multiple);
            break;
        // 输入角度按 Enter 转；放中心；复制
        case Tool::Rotate:
        {
            label(Tr("Angle:"));
            auto* box = field(m_modifyOpt.rotateAngle, 70.0f, "°");
            box->SetOnSubmit([this](const std::string& t)
            {
                m_platform->Post([this, t]
                {
                    double a = 0.0;
                    if (!ReadNumber(t, a) || !m_rotateCentre)
                    {
                        MiniGUI::ShowMessageBox(*m_ui, Tr("Could not rotate"), Format(Tr("`{0}` is not an angle in degrees"), { t }));
                        return;
                    }
                    Bim::Json made;
                    if (RunCommand("rotate", Bim::Json{ { "ids", m_selection }, { "about", Bim::Json::array({ m_rotateCentre->x, m_rotateCentre->y }) },
                                                        { "angle", a }, { "copy", m_modifyOpt.rotateCopy } }, N_("Could not rotate"), &made))
                        Edited(made, m_modifyOpt.rotateCopy);
                });
            });
            bar->AddChild<Button>(Tr("Place Centre"), [this]
            {
                m_placeCentre = true;
                m_toolPts.clear();
                UpdateStatus();
            });
            check(Tr("Copy"), m_modifyOpt.rotateCopy);
            break;
        }
        case Tool::MirrorPick: case Tool::MirrorDraw:
            check(Tr("Copy"), m_modifyOpt.mirrorCopy);
            break;
        case Tool::Offset:
            label(Tr("Offset:"));
            field(m_modifyOpt.offset, 80.0f, unit);
            check(Tr("Copy"), m_modifyOpt.offsetCopy);
            break;
        // 文字：写什么、用哪个文字类型
        case Tool::Text:
            label(Tr("Text:"));
            field(m_toolOpt.noteText, 240.0f, Tr("the note"));
            label(Tr("Type:"));
            named(keys(m.textTypes), m_toolOpt.textType, Bim::TextTypes::Default(m.textTypes), 130.0f);
            break;
        case Tool::Tag:
            check(Tr("Leader"), m_toolOpt.tagLeader);
            bar->AddChild<Button>(Tr("Tag All"), [this] { m_platform->Post([this] { TagAll(); }); });
            break;
        case Tool::DetailLine:
            label(Tr("Line Style:"));
            named(keys(m.lineStyles), m_toolOpt.lineStyle, Bim::Lines::DefaultStyle(m.lineStyles, m.language), 150.0f);
            check(Tr("Chain"), m_toolOpt.chain);
            break;
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
        // 草图模式下还捕捉草图自己的线：直线的端点、中点与线，圆弧的端点与中点
        std::vector<Bim::Segment> lines;
        std::vector<std::pair<Bim::P2, Bim::Snap::Kind>> points;
        if (m_sketch)
            for (const Bim::SketchLine& l : m_sketch->lines)
            {
                if (l.arc == 0.0)
                    lines.emplace_back(l.start, l.end);
                else
                {
                    points.emplace_back(l.start, Bim::Snap::Kind::Endpoint);
                    points.emplace_back(l.end, Bim::Snap::Kind::Endpoint);
                    points.emplace_back(Bim::Sketch::Middle(l), Bim::Snap::Kind::Midpoint);
                }
            }
        return Bim::Snap::At(SeenModel(), *m_planLevel, v.ToMm({ pixel.x, pixel.y }), reach, from, square, m_snaps, lines, points);
    }

    Bim::Json MainFrame::Typed(const std::string& category, Bim::Json p) const
    {
        if (const auto f = m_toolOpt.openingFamilies.find(category); f != m_toolOpt.openingFamilies.end())
            p["family"] = f->second;
        if (const auto t = m_toolOpt.types.find(category); t != m_toolOpt.types.end())
            p["type"] = t->second;
        return p;
    }

    std::optional<Bim::Json> MainFrame::TopParams(const std::optional<std::optional<Bim::Guid>>& top, const std::string& height, std::string& error) const
    {
        const Bim::Model& m = m_session.GetModel();
        if (const std::optional<Bim::Guid> level = top ? *top : m.LevelAbove(*m_planLevel))
            return Bim::Json{ { "top_level", Bim::ToJson(*level) } };
        const auto h = Bim::UnitsOf::Parse(m.units, height);
        if (!h || *h <= 0.0)
        {
            error = Format(Tr("`{0}` is not a height"), { height });
            return std::nullopt;
        }
        return Bim::Json{ { "height", *h } };
    }

    std::optional<Bim::Json> MainFrame::RunParams(Bim::P2 a, Bim::P2 b, std::string& error) const
    {
        const Bim::Model& m = m_session.GetModel();
        const std::string category = m_toolOf ? m_toolOf : "beam";
        const bool strip = category == "stripfooting";
        const std::string& typed = strip ? m_toolOpt.footingTop : m_toolOpt.runOffset;
        const auto offset = Bim::UnitsOf::Parse(m.units, typed);
        if (!offset)
        {
            error = Format(Tr("`{0}` is not a height"), { typed });
            return std::nullopt;
        }
        Bim::Json p{ { "points", Bim::Json::array({ Pt(a), Pt(b) }) }, { "level", Bim::ToJson(*m_planLevel) }, { "offset", *offset } };
        if (!strip)
            p["justify"] = Bim::JustifyOf::Name(m_toolOpt.runJustify);
        return Typed(category, std::move(p));
    }

    std::optional<Bim::Json> MainFrame::StandParams(Bim::P2 at, std::string& error) const
    {
        const Bim::Model& m = m_session.GetModel();
        const std::string category = m_toolOf ? m_toolOf : "footing";
        Bim::Json p;
        if (category == "structuralcolumn")
        {
            const auto top = TopParams(m_toolOpt.columnTop, m_toolOpt.columnHeight, error);
            if (!top)
                return std::nullopt;
            p = *top;
        }
        else
        {
            const auto thickness = Bim::UnitsOf::Parse(m.units, m_toolOpt.footingThickness);
            const auto top = Bim::UnitsOf::Parse(m.units, m_toolOpt.footingTop);
            if (!thickness || !top)
            {
                error = Format(Tr("`{0}` is not a height"), { thickness ? m_toolOpt.footingTop : m_toolOpt.footingThickness });
                return std::nullopt;
            }
            p = Bim::Json{ { "thickness", *thickness }, { "offset", *top } };
        }
        p["at"]    = Pt(at);
        p["level"] = Bim::ToJson(*m_planLevel);
        return Typed(category, std::move(p));
    }

    std::optional<std::pair<std::string, std::string>> MainFrame::Placing() const
    {
        const Bim::Model& m = m_session.GetModel();
        if (m_toolOpt.component)
            if (const auto f = m.families.find(m_toolOpt.component->first);
                f != m.families.end() && PlacingFits(f->second.category) && f->second.types.contains(m_toolOpt.component->second))
                return m_toolOpt.component;
        for (const auto& [name, f] : m.families)
            if (PlacingFits(f.category) && !f.types.empty())
                return std::pair{ name, f.types.begin()->first };
        return std::nullopt;
    }

    bool MainFrame::PlacingFits(Bim::Fam::Category c) const
    {
        if (Bim::Fam::CategoryOf::Hosted(c))
            return false;
        const Fixture* f = m_tool == Tool::Component ? FixtureOf(m_toolOf) : nullptr;
        return !f || std::find(f->kinds.begin(), f->kinds.end(), c) != f->kinds.end();
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
            const auto top = TopParams(m_toolOpt.wallTop, m_toolOpt.wallHeight, error);
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
        // 体量：点一个个角点，回到第一点（或 Enter）时做出来
        case Tool::Mass:
            if (m_toolPts.size() >= 3 && m_toolPts.front().Dist(p) <= Bim::Tol)
                FinishMass();
            else if (m_toolPts.empty() || m_toolPts.back().Dist(p) > Bim::Tol)
                m_toolPts.push_back(p);
            break;
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
        case Tool::Section:
            if (m_toolPts.empty())
                m_toolPts.push_back(p);
            else if (m_toolPts.front().Dist(p) > Bim::Tol)
            {
                const Bim::P2 a = m_toolPts.front();
                m_toolPts.clear();
                RunCommand("section", Bim::Json{ { "start", Pt(a) }, { "end", Pt(p) } }, N_("Could not place the section"));
            }
            break;
        case Tool::Column:
        {
            std::string error;
            const auto top = TopParams(m_toolOpt.columnTop, m_toolOpt.columnHeight, error);
            if (!top)
            {
                MiniGUI::ShowMessageBox(*m_ui, Tr("Could not place the column"), error);
                break;
            }
            Bim::Json params = Typed("column", Bim::Json{ { "at", Pt(p) }, { "level", level } });
            params.update(*top);
            RunCommand("column", params, N_("Could not place the column"));
            break;
        }
        case Tool::Stand:
        {
            std::string error;
            const auto params = StandParams(p, error);
            const char* title = m_toolOf && std::string_view(m_toolOf) == "footing" ? N_("Could not place the footing") : N_("Could not place the column");
            if (!params)
                MiniGUI::ShowMessageBox(*m_ui, Tr(title), error);
            else
                RunCommand(Bim::CommandOf(m_toolOf ? m_toolOf : ""), *params, title);
            break;
        }
        // 第一下是起点，之后每一下放到这一点；连成链时从这一点接着放
        case Tool::Run:
        {
            if (m_toolPts.empty())
            {
                m_toolPts.push_back(p);
                break;
            }
            const Bim::P2 last = m_toolPts.back();
            if (last.Dist(p) <= Bim::Tol)
                break;
            std::string error;
            const auto params = RunParams(last, p, error);
            if (!params)
            {
                MiniGUI::ShowMessageBox(*m_ui, Tr("Could not place the run"), error);
                break;
            }
            if (RunCommand(Bim::CommandOf(m_toolOf ? m_toolOf : ""), *params, N_("Could not place the run")))
            {
                if (m_toolOpt.chain)
                    m_toolPts.push_back(p);
                else
                    m_toolPts.clear();
            }
            break;
        }
        case Tool::Component:
            if (const auto what = Placing())
                RunCommand("component", Bim::Json{ { "family", what->first }, { "type", what->second }, { "at", Pt(p) }, { "level", level },
                                                   { "angle", m_toolOpt.componentAngle } },
                           N_("Could not place the component"));
            break;
        case Tool::SketchLine:
            SketchPoint(p);
            break;
        // 详图线：同墙链；测量：两下一把尺子，再一下重量
        case Tool::DetailLine:
        {
            if (m_toolPts.empty())
            {
                m_toolPts.push_back(p);
                break;
            }
            const Bim::P2 last = m_toolPts.back();
            if (last.Dist(p) <= Bim::Tol)
                break;
            Bim::Json params{ { "start", Pt(last) }, { "end", Pt(p) }, { "level", level } };
            if (m.lineStyles.contains(m_toolOpt.lineStyle))
                params["style"] = m_toolOpt.lineStyle;
            if (RunCommand("detail_line", params, N_("Could not draw the detail line")))
            {
                if (m_toolOpt.chain)
                    m_toolPts.push_back(p);
                else
                    m_toolPts.clear();
            }
            break;
        }
        case Tool::Measure:
            if (m_toolPts.size() >= 2)
                m_toolPts.clear();
            if (m_toolPts.empty() || m_toolPts.back().Dist(p) > Bim::Tol)
                m_toolPts.push_back(p);
            break;
        default:
            break;
        }
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
        case Tool::Wall: case Tool::Grid: case Tool::Run: case Tool::Mass: case Tool::DetailLine: case Tool::Section:
            PlacePoint(SnapAt(pixel, m_toolPts, shift).at);
            return;
        // 测量的第二下从第一下量；量完的尺子不算起点
        case Tool::Measure:
            PlacePoint(SnapAt(pixel, m_toolPts.size() == 1 ? std::span<const Bim::P2>(m_toolPts) : std::span<const Bim::P2>(), shift).at);
            return;
        case Tool::Dimension: case Tool::Angular: case Tool::Radial: case Tool::Spot: case Tool::Text: case Tool::Tag:
            AnnotateClick(pixel, modifiers);
            return;
        case Tool::Column: case Tool::Component: case Tool::Stand:
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
            {
                p = Typed("floor", Bim::Json{ { "level", level } });
                if (m_toolOpt.floorStructural)
                    p["structural"] = true;
            }
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
        case Tool::SketchLine: case Tool::SketchRect: case Tool::SketchPick:
            SketchClick(pixel, modifiers);
            return;
        default:
            ModifyClick(pixel, modifiers);
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
        if (e.type != MiniGUI::KeyEventType::Down || !m_planLevel)
            return false;
        // Tab：光标下叠着的元素轮到下一个
        if (e.key == Key::Tab && !e.Ctrl())
        {
            ++m_tabIndex;
            m_viewport->RequestRender();
            return true;
        }
        if (m_tool == Tool::Modify)
            return false;
        switch (e.key)
        {
        case Key::Enter:
            if (!m_typedLength.empty())
                PlaceTyped();
            else if (m_tool == Tool::Wall || m_tool == Tool::SketchLine || m_tool == Tool::Run || m_tool == Tool::DetailLine)
                m_toolPts.clear();      // 结束墙链、草图线链、run 与详图线的链
            else if (m_tool == Tool::Mass && m_toolPts.size() >= 3)
                FinishMass();
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
        if (m_toolPts.empty() || (m_tool != Tool::Wall && m_tool != Tool::Grid && m_tool != Tool::SketchLine && m_tool != Tool::Run && m_tool != Tool::Mass &&
                                   m_tool != Tool::DetailLine && m_tool != Tool::Section) ||
            text == " " ||
            m_typedLength.size() > 40)
            return false;
        for (const char c : text)
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
        // 长度从数字开始（之后的英尺、英寸、单位都算它的）；先打的字母是两个字母的快捷键（MainFrame::OnShortcutText）
        if (m_typedLength.empty() && !std::isdigit(static_cast<unsigned char>(text[0])) && text[0] != '.')
            return false;
        m_typedLength += text;
        m_viewport->RequestRender();
        return true;
    }

    // =========================================================
    // 预览
    // =========================================================
    Bim::P2 MainFrame::DrawSnap(Bim::Render::Drawing& d, const Bim::Snap::Found& f, std::string& tip)
    {
        const Bim::Render::View& v = *m_planView;
        const float px = m_dpiScale;
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
    }

    void MainFrame::BuildPreview(Bim::Render::Drawing& d)
    {
        if (m_planTipBox)
            m_planTipBox->SetVisible(false);
        m_found.reset();
        if (m_tool == Tool::Level || !m_planLevel || !m_planView)
            return;
        std::string tip;
        // 读数跟着光标，在它右上方
        auto showTip = [&]
        {
            if (!m_planTipBox || tip.empty() || !m_hoverPixel)
                return;
            m_planTip->SetText(tip);
            MiniGUI::LayoutStyle& s = m_planTipBox->EditLayoutStyle();
            s.left = m_hoverPixel->x / m_dpiScale + 14.0f;
            s.top  = std::max(0.0f, m_hoverPixel->y / m_dpiScale - 30.0f);
            m_planTipBox->SetVisible(true);
        };
        // 草图模式：草图线与画草图线的工具（SketchMode.cpp）
        if (m_sketch)
        {
            BuildSketchPreview(d, tip);
            showTip();
            return;
        }
        // 注释工具：拾取的参照、会放下的注释（Annotate.cpp）
        if (AnnotateTool(m_tool))
        {
            BuildAnnotatePreview(d, tip);
            showTip();
            return;
        }
        // 修改与修改工具：夹点、框选、拖动、各工具的预览（Modify.cpp）
        if (m_tool == Tool::Modify || ToolCategory() == nullptr)
        {
            BuildModifyPreview(d, tip);
            showTip();
            return;
        }
        if (!m_hoverPixel)
            return;
        const Bim::Model& m = m_session.GetModel();
        const Bim::Render::View& v = *m_planView;
        const float px = m_dpiScale;
        const Bim::P2 mm = v.ToMm({ m_hoverPixel->x, m_hoverPixel->y });
        // 一个还不在模型里的元素：照平面会画的样子，用强调色
        auto sketched = [&](const Bim::Element& e)
        {
            Bim::Render::Drawing s = Bim::Render::Sketch(m, PlanKey(), v, e);
            for (Bim::Render::Prim& p : s.prims)
            {
                p.color = Accent;
                if (p.kind == Bim::Render::Prim::Kind::Fill)
                    continue;      // 填色会盖住下面的墙：只画线
                d.Push(std::move(p));
            }
        };
        // 捕捉到的点：记号与引导线
        auto marker = [&](const Bim::Snap::Found& f) { return DrawSnap(d, f, tip); };
        const Bim::Json level = Bim::ToJson(*m_planLevel);
        switch (m_tool)
        {
        // 体量：点过的角点连到光标，再虚接回第一点
        case Tool::Mass:
        {
            const Bim::P2 next = marker(SnapAt(*m_hoverPixel, m_toolPts, m_hoverShift));
            if (m_toolPts.empty())
                break;
            std::vector<Bim::Render::Px> path;
            for (const Bim::P2& q : m_toolPts)
                path.push_back(v.ToPx(q));
            path.push_back(v.ToPx(next));
            Stroke(d, path, 2.0f * px);
            if (m_toolPts.size() >= 2)
                Stroke(d, { v.ToPx(next), v.ToPx(m_toolPts.front()) }, 1.0f * px);
            const Bim::P2 last = m_toolPts.back();
            const std::string reading = Bim::UnitsOf::WithUnit(m.units, last.Dist(next)) + "  " + Degrees(Bim::Snap::Measure(last, next).angle);
            tip = !m_typedLength.empty() ? m_typedLength + " (Enter)" : (tip.empty() ? reading : tip + "    " + reading);
            break;
        }
        case Tool::Wall: case Tool::Grid: case Tool::Section:
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
        // 会放下的柱：选的类型，否则项目默认的
        case Tool::Column:
        {
            const Bim::P2 at = marker(SnapAt(*m_hoverPixel, {}, false));
            const auto chosen = m_toolOpt.types.find("column");
            const std::optional<std::string> type = chosen != m_toolOpt.types.end() ? std::optional<std::string>(chosen->second) : m.DefaultType("column");
            if (type)
            {
                Bim::Column c;
                c.typeName = *type;
                c.level    = *m_planLevel;
                c.at       = at;
                sketched(Bim::Element{ c });
            }
            break;
        }
        // 会放下的结构柱或独立基础，与 run：照命令会做出的样子（BIMCraft 的 stand_sketch、run_sketch）
        case Tool::Stand:
        {
            const Bim::P2 at = marker(SnapAt(*m_hoverPixel, {}, false));
            std::string error;
            Bim::Element e;
            if (const auto p = StandParams(at, error); p && Bim::StoodOf(m, *p, m_toolOf ? m_toolOf : "", e, error))
                sketched(e);
            break;
        }
        case Tool::Run:
        {
            const Bim::P2 next = marker(SnapAt(*m_hoverPixel, m_toolPts, m_hoverShift));
            if (m_toolPts.empty())
                break;
            const Bim::P2 last = m_toolPts.back();
            std::string error;
            std::vector<Bim::Element> made;
            if (const auto p = RunParams(last, next, error); p && last.Dist(next) > Bim::Tol && Bim::RunsOf(m, *p, m_toolOf ? m_toolOf : "", made, error))
                for (const Bim::Element& e : made)
                    sketched(e);
            else
                Stroke(d, { v.ToPx(last), v.ToPx(next) }, 2.0f * px);
            const std::string reading = Bim::UnitsOf::WithUnit(m.units, last.Dist(next)) + "  " + Degrees(Bim::Snap::Measure(last, next).angle);
            tip = !m_typedLength.empty() ? m_typedLength + " (Enter)" : (tip.empty() ? reading : tip + "    " + reading);
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
        showTip();
    }

    void MainFrame::FinishMass()
    {
        if (!m_planLevel || m_toolPts.size() < 3)
            return;
        std::string error;
        const auto top = TopParams(m_toolOpt.massTop, m_toolOpt.massHeight, error);
        if (!top)
        {
            MiniGUI::ShowMessageBox(*m_ui, Tr("Could not create the mass"), error);
            return;
        }
        Bim::Json boundary = Bim::Json::array();
        for (const Bim::P2& q : m_toolPts)
            boundary.push_back(Pt(q));
        Bim::Json params{ { "boundary", boundary }, { "level", Bim::ToJson(*m_planLevel) } };
        params.update(*top);
        if (RunCommand("mass", params, N_("Could not create the mass")))
            m_toolPts.clear();
    }

    std::vector<Bim::Id> MainFrame::SelectedMasses() const
    {
        const Bim::Model& m = m_session.GetModel();
        std::vector<Bim::Id> out;
        for (const Bim::Id id : m_selection)
            if (const Bim::Element* e = m.elements.Get(id); e && e->As<Bim::Mass>())
                out.push_back(id);
        return out;
    }

    void MainFrame::ByMass(const char* command, const char* title)
    {
        for (const Bim::Id id : SelectedMasses())
            if (!RunCommand(command, Bim::Json{ { "mass", id } }, title))
                break;
    }
}
