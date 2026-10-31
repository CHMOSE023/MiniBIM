// 所有操作注册为 MiniGUI 命令；菜单、标题栏按钮经界面描述文件 ui/minibim_ui.json 引用命令 ID。
// 名称与提示写英文原文并用 N_ 标记（收集工具据此找到它们），UiLayout 每次应用时按当前语言翻译。
// 名称不写助记符 &：菜单上不显示下划线和 "(X)"
#include "GUI/MainFrame.h"
#include "Commands/Sample.h"
#include "Core/Log.h"
#include "Core/UIContext.h"
#include "I18n/Translator.h"
#include "Widgets/Button.h"
#include "Widgets/Dialog.h"
#include "Widgets/Label.h"
#include "Widgets/TextBox.h"
#include "Widgets/ViewportHost.h"
#include <algorithm>
#include <filesystem>

namespace MiniBIM
{
    void MainFrame::RegisterCommands()
    {
        using MiniGUI::Command;
        auto add = [this](const char* id, const char* label, const char* tooltip, std::function<void()> execute,
                          std::function<bool()> canExecute = {}, std::function<bool()> isChecked = {})
        {
            Command c;
            c.id         = id;
            c.label      = label;
            c.tooltip    = tooltip;
            c.execute    = std::move(execute);
            c.canExecute = std::move(canExecute);
            c.isChecked  = std::move(isChecked);
            m_commands.Register(std::move(c));
        };

        // ── 窗口（标题栏右侧的按钮执行这三个；平台没有这三个按钮时不注册，标题栏就不显示）───────
        if (m_platform->HasWindowControls())
        {
            add("window.minimize", N_("Minimize"), "", [this] { m_platform->MinimizeWindow(); });
            add("window.maximize", N_("Maximize"), "", [this] { m_platform->ToggleMaximizeWindow(); });
            add("window.close",    N_("Close"),    "", [this] { m_platform->CloseWindow(); });
        }

        // ── 文件 ─────────────────────────────────────────────────
        add("file.new", N_("New"), N_("Start an empty project with two levels"), [this] { Guard([this] { NewProject(); }); });
        add("file.open", N_("Open..."), m_revitReader ? N_("Open a MiniBIM project, a BIMCraft one, or an IFC or Revit file as a new project")
                                                      : N_("Open a MiniBIM project, a BIMCraft one, or an IFC file as a new project"),
            [this] { Guard([this] { OpenProject(); }); });
        add("file.revitReport", N_("Revit Import Report"), N_("What the last Revit file brought in, what it left out and why, with the element ids"),
            [this] { ShowRevitReport(); }, [this] { return HasRevitReport(); });
        add("file.save", N_("Save"), N_("Save the project"), [this] { SaveProject(false, {}); });
        add("file.saveAs", N_("Save As..."), N_("Save the project under another name"), [this] { SaveProject(true, {}); });
        add("file.exportIfc", N_("Export IFC..."), N_("Export the model as an IFC4 file for other BIM software"), [this] { ExportIfc(); });
        add("file.sample", N_("Sample House"), N_("Open the sample house, built by commands"), [this] { Guard([this] { OpenSample(); }); });
        add("file.start", N_("Start Page"), N_("The start page: new, open and the recent files; again to go back to the project"),
            [this] { ToggleStartPage(); }, {}, [this] { return m_startShown; });
        add("file.settings", N_("Settings..."), N_("The window's own settings: theme, language, palettes, autosave"), [this] { OpenSettings(); });
        add("file.shortcuts", N_("Keyboard Shortcuts..."), N_("The two-letter shortcuts typed over the view (WA wall, DR door…): see, change, load from a file"),
            [this] { OpenShortcuts(); });
        add("file.exit", N_("Exit"), "", [this] { RequestExit(); });

        // ── 编辑 ─────────────────────────────────────────────────
        // 草图模式里撤销与删除的是草图线（SketchMode.cpp），模型的撤销等完成或取消之后
        add("edit.undo", N_("Undo"), N_("Undo the last change"), [this] { if (m_sketch) UndoSketch(); else StepHistory(false); },
            [this] { return m_sketch ? !m_sketch->history.empty() : m_session.CanUndo(); });
        add("edit.redo", N_("Redo"), N_("Redo the change undone last"), [this] { StepHistory(true); }, [this] { return !m_sketch && m_session.CanRedo(); });
        add("edit.delete", N_("Delete"), N_("Delete the selected elements; a wall takes its doors and windows with it"),
            [this] { if (m_sketch) DeleteSketchLines(); else DeleteSelection(); },
            [this] { return m_sketch ? !m_sketch->picked.empty() : !m_selection.empty(); });
        add("edit.escape", N_("Cancel"), N_("Drop what the tool has drawn so far, then the tool, then the selection"), [this] { Escape(); });
        add("modify.select", N_("Modify"), N_("Select and change elements"), [this] { SetTool(Tool::Modify); }, {},
            [this] { return m_tool == Tool::Modify; });
        // 修改工具（Modify.cpp）：移动、复制、旋转、镜像要先选东西
        auto modifyTool = [&](const char* id, const char* label, const char* tooltip, Tool t)
        {
            add(id, label, tooltip, [this, t] { SetTool(m_tool == t ? Tool::Modify : t); },
                [this, t] { return !m_sketch && (!NeedsSelection(t) || !m_selection.empty()); }, [this, t] { return m_tool == t; });
        };
        modifyTool("modify.align", N_("Align"), N_("Align: pick a reference line, then what to line up with it"), Tool::Align);
        modifyTool("modify.offset", N_("Offset"), N_("Offset: pick a wall; a copy goes to the cursor's side"), Tool::Offset);
        modifyTool("modify.mirrorPick", N_("Mirror Pick Axis"), N_("Mirror, Pick Axis: pick a wall or grid to mirror across"), Tool::MirrorPick);
        modifyTool("modify.mirrorDraw", N_("Mirror Draw Axis"), N_("Mirror, Draw Axis: click the axis's two ends"), Tool::MirrorDraw);
        modifyTool("modify.split", N_("Split Element"), N_("Split Element: click a wall where to split it"), Tool::Split);
        modifyTool("modify.move", N_("Move"), N_("Move: click a base point, then where it goes"), Tool::Move);
        modifyTool("modify.copy", N_("Copy"), N_("Copy: click a base point, then where the copy goes"), Tool::Copy);
        modifyTool("modify.rotate", N_("Rotate"), N_("Rotate: click the angle's start, then its end"), Tool::Rotate);
        modifyTool("modify.trim", N_("Trim/Extend to Corner"), N_("Trim/Extend to Corner: pick two walls, on the parts to keep"), Tool::Trim);
        add("modify.typeProperties", N_("Type Properties"), N_("Edit the type of the selected elements, or of what the tool places"),
            [this] { EditType(); }, [this] { return CurrentTypeSubject().has_value(); });
        add("manage.materials", N_("Materials"), N_("The project's materials: their colours and patterns"), [this] { OpenMaterials(); });
        add("manage.parameters", N_("Project Parameters"), N_("Values each element of chosen categories can hold, shown in Properties and schedules"),
            [this] { OpenProjectParameters(); });
        add("manage.objectStyles", N_("Object Styles"), N_("The pen each model category draws in, seen and cut"), [this] { OpenObjectStyles(); });
        add("manage.lineWeights", N_("Line Weights"), N_("How wide each pen draws on paper, at each scale"), [this] { OpenLineWeights(); });
        add("manage.lineStyles", N_("Line Styles"), N_("The project's line styles: pen, colour and line pattern"), [this] { OpenLineStyles(); });
        add("manage.fillPatterns", N_("Fill Patterns"), N_("The project's fill patterns; import and export PAT files"), [this] { OpenFillPatterns(); });
        add("manage.purge", N_("Purge Unused"), N_("Take out types, families and the rest that nothing uses"), [this] { OpenPurge(); });
        // ── 视图（移植自 BIMCraft 的视图选项卡）：可见性 / 图形、复制视图、裁剪 ─────────
        add("view.visibility", N_("Visibility/Graphics"), N_("What the view shows: hide or show categories; a plan's scale and detail level"),
            [this] { OpenVisibilityGraphics(); });
        const auto planShown = [this] { return m_planLevel.has_value() && !m_sketch; };
        add("view.duplicate", N_("Duplicate View"), N_("A new plan view of this level, with this plan's view range, crop and graphics"),
            [this] { DuplicatePlan("duplicate"); }, planShown);
        add("view.detailing", N_("Duplicate with Detailing"), N_("A new plan view of this level, with copies of this plan's dimensions, tags and notes"),
            [this] { DuplicatePlan("detailing"); }, planShown);
        add("view.dependent", N_("Duplicate as Dependent"),
            N_("A dependent view: it follows this plan's view range and graphics, has a crop of its own and shows its annotation"),
            [this] { DuplicatePlan("dependent"); },
            [this]
            {
                const Bim::PlanView* v = m_planLevel ? m_session.GetModel().GetPlanView(PlanKey()) : nullptr;
                return m_planLevel.has_value() && !m_sketch && !(v && v->parent);
            });
        add("view.crop", N_("Crop View"), N_("Crop the plan to what it shows now, or uncrop it"), [this] { ToggleCrop(); }, planShown,
            [this] { return m_planLevel && m_session.GetModel().CropOf(PlanKey()).has_value(); });
        add("view.section", N_("Section"), N_("Section: click its two ends; it looks to the left of the line"),
            [this] { SetTool(m_tool == Tool::Section ? Tool::Modify : Tool::Section); }, [this] { return !m_sketch.has_value(); },
            [this] { return m_tool == Tool::Section; });
        add("view.elevation", N_("Elevations"), N_("Elevations of the building's four sides, those it doesn't have yet"), [this] { MakeElevations(); },
            [this] { return !m_sketch.has_value(); });
        add("view.sheet", N_("Sheet"), N_("A new sheet: paper with a title block, for views to go on"), [this] { NewSheet(); },
            [this] { return !m_sketch.has_value(); });
        add("view.placeView", N_("Place View"), N_("Put a view not on a sheet yet on the sheet shown"), [this] { OpenPlaceView(); },
            [this] { return m_sheetOf.has_value(); });
        add("view.schedule", N_("Schedule/Quantities"), N_("A new schedule: a table of the elements of a category, their fields as columns"),
            [this] { OpenNewSchedule(); });
        add("view.filters", N_("Filters"), N_("The project's view filters: the elements of some categories that meet rules, for views to hide or draw another way"),
            [this] { OpenViewFilters(); });
        add("view.template", N_("Create Template"), N_("A view template with this view's visibility/graphics, scale, detail level and view range"),
            [this] { OpenCreateTemplate(); }, [this] { return !m_sketch.has_value(); });
        add("modify.flip", N_("Flip Wall"), N_("Turn the selected walls round: their exterior sides go to the other side"), [this] { FlipSelection(); },
            [this]
            {
                const Bim::Model& m = m_session.GetModel();
                return std::any_of(m_selection.begin(), m_selection.end(), [&](Bim::Id id) { return m.GetWall(id) != nullptr; });
            });

        // ── 草图模式（SketchMode.cpp）：编辑边界，完成、取消，画草图线的工具 ─────────
        add("modify.editBoundary", N_("Edit Boundary"), N_("Edit the outline of the selected floor, ceiling, roof or filled region as sketch lines"),
            [this] { EditBoundary(); }, [this] { return CanEditBoundary(); });
        add("sketch.finish", N_("Finish Edit Mode"), N_("Finish the sketch: its lines must make one closed outline, with any openings inside it"),
            [this] { FinishSketch(); }, [this] { return m_sketch.has_value(); });
        add("sketch.cancel", N_("Cancel Edit Mode"), N_("Leave the sketch without changing the model"),
            [this] { CancelSketch(); }, [this] { return m_sketch.has_value(); });
        auto sketchTool = [&](const char* id, const char* label, const char* tooltip, Tool t)
        {
            add(id, label, tooltip, [this, t] { SetTool(m_tool == t ? Tool::Modify : t); }, [this] { return m_sketch.has_value(); },
                [this, t] { return m_tool == t; });
        };
        sketchTool("sketch.line", N_("Line"), N_("Line: click point to point; back at the start closes the loop"), Tool::SketchLine);
        sketchTool("sketch.rectangle", N_("Rectangle"), N_("Rectangle: click two opposite corners"), Tool::SketchRect);
        sketchTool("sketch.pickWalls", N_("Pick Walls"), N_("Pick Walls: each wall picked adds a line along it"), Tool::SketchPick);

        // ── 建筑：放东西的工具（Tools.cpp）─────────────────────────
        auto tool = [&](const char* id, const char* label, const char* tooltip, Tool t)
        {
            add(id, label, tooltip, [this, t] { SetTool(m_tool == t && t != Tool::Modify ? Tool::Modify : t); },
                [this, t] { return t == Tool::Modify || !m_sketch; }, [this, t] { return m_tool == t; });
        };
        tool("arch.wall", N_("Wall"), N_("Wall: each click builds a wall; Enter or Esc ends the chain"), Tool::Wall);
        tool("arch.door", N_("Door"), N_("Door: click a wall"), Tool::Door);
        tool("arch.window", N_("Window"), N_("Window: click a wall"), Tool::Window);
        tool("arch.column", N_("Column"), N_("Column: click where it stands"), Tool::Column);
        // 构件：任何可载入族；卫浴、电气等选项卡的设备按钮是同一个工具，只放某几类的族（m_toolOf）
        add("arch.component", N_("Component"), N_("Component: a family's type, where you click; Space turns it"),
            [this]
            {
                const bool same = m_tool == Tool::Component && !m_toolOf;
                m_toolOf = nullptr;
                SetTool(same ? Tool::Modify : Tool::Component);
            },
            [this] { return !m_sketch; }, [this] { return m_tool == Tool::Component && !m_toolOf; });
        tool("arch.roof", N_("Roof"), N_("Roof by Footprint: click inside the walls to roof them, on the level above"), Tool::Roof);
        // 建筑的楼板与结构选项卡的结构楼板是同一个工具，放的楼板是否承重不同
        auto floorTool = [&](const char* id, const char* label, const char* tooltip, bool structural)
        {
            add(id, label, tooltip,
                [this, structural]
                {
                    const bool same = m_tool == Tool::Floor && m_toolOpt.floorStructural == structural;
                    m_toolOpt.floorStructural = structural;
                    SetTool(same ? Tool::Modify : Tool::Floor);
                },
                [this] { return !m_sketch; }, [this, structural] { return m_tool == Tool::Floor && m_toolOpt.floorStructural == structural; });
        };
        floorTool("arch.floor", N_("Floor"), N_("Floor: click inside the walls to floor them"), false);
        tool("arch.ceiling", N_("Ceiling"), N_("Ceiling: click inside a room for a ceiling out to its walls"), Tool::Ceiling);
        tool("arch.room", N_("Room"), N_("Room: click inside walls"), Tool::Room);
        tool("arch.level", N_("Level"), N_("Level: a new level at an elevation"), Tool::Level);
        tool("arch.grid", N_("Grid"), N_("Grid: click its two ends"), Tool::Grid);

        // ── 结构：结构柱、梁、结构楼板、独立基础、条形基础（移植自 BIMCraft structure.rs）──────
        // 放一个类别的 Stand 或 Run 工具，或只放某几类族的构件工具（设备）
        auto toolOf = [&](const char* id, const char* label, const char* tooltip, Tool t, const char* category)
        {
            add(id, label, tooltip, [this, t, category] { SetToolOf(t, category); }, [this] { return !m_sketch; },
                [this, t, category] { return m_tool == t && m_toolOf && std::string_view(m_toolOf) == category; });
        };
        toolOf("struct.column", N_("Column"), N_("Structural Column: click where it stands"), Tool::Stand, "structuralcolumn");
        toolOf("struct.beam", N_("Beam"), N_("Beam (BM): click its start and its end; a chain of clicks makes a beam for each step"), Tool::Run, "beam");
        floorTool("struct.floor", N_("Floor"), N_("Floor: Structural (SB): click inside the walls to floor them with a slab that carries load"), true);
        toolOf("struct.footing", N_("Isolated"), N_("Isolated Footing (FI): click where the footing goes"), Tool::Stand, "footing");
        toolOf("struct.wallFooting", N_("Wall Footing"), N_("Wall Footing (FW): click its start and its end, or a chain of clicks, under a wall"), Tool::Run, "stripfooting");

        // ── 卫浴、电气、防雷、数据：run 与设备（移植自 BIMCraft mep.rs）──────────
        toolOf("mep.pipe", N_("Pipe"), N_("Pipe (PI): click its start and its end; a chain of clicks makes a pipe for each step"), Tool::Run, "pipe");
        toolOf("mep.plumbing", N_("Plumbing"), N_("Plumbing Fixture (PX): a basin, a WC, a sink, a shower or a drain, where you click; Space turns it"),
               Tool::Component, "plumbing");
        toolOf("mep.fitting", N_("Fitting"), N_("Pipe Fitting (PF): an elbow or a tee, where you click on a pipe's height; Space turns it"), Tool::Component, "fitting");
        toolOf("mep.accessory", N_("Accessory"), N_("Pipe Accessory (PA): a valve, where you click on a pipe's height; Space turns it"), Tool::Component, "accessory");
        toolOf("mep.equipment", N_("Equipment"), N_("Electrical Equipment (EE): a distribution board, where you click on the wall; Space turns it"),
               Tool::Component, "equipment");
        toolOf("mep.device", N_("Device"), N_("Device (DV): an outlet or a switch, where you click on the wall; Space turns it"), Tool::Component, "device");
        toolOf("mep.lighting", N_("Lighting"), N_("Lighting Fixture (LF): a luminaire, where you click, hung from the slab; Space turns it"), Tool::Component, "lighting");
        toolOf("mep.conduit", N_("Conduit"), N_("Conduit (CN): click its start and its end; a chain of clicks makes a conduit for each step"), Tool::Run, "conduit");
        toolOf("mep.cableTray", N_("Cable Tray"), N_("Cable Tray (CT): click its start and its end; a chain of clicks makes a tray for each step"), Tool::Run, "cabletray");
        toolOf("mep.wire", N_("Wire"), N_("Wire (EW): click its start and its end; a chain of clicks makes a wire for each step"), Tool::Run, "wire");
        toolOf("mep.airTerminal", N_("Air Terminal"), N_("Air Terminal (AT): a captor, where you click on the roof; Space turns it"), Tool::Component, "airterminal");
        toolOf("mep.downConductor", N_("Down Lead"), N_("Down Conductor (DC): click its start and its end; the same point twice and two heights stand it up a wall"),
               Tool::Run, "downconductor");
        toolOf("mep.earthing", N_("Electrode"), N_("Earthing Electrode (EG): a rod driven down from the level, where you click; Space turns it"), Tool::Component, "earthing");
        toolOf("mep.inspection", N_("Inspection"), N_("Inspection Box (IB): over an electrode, flush with the level, where you click; Space turns it"),
               Tool::Component, "inspection");
        toolOf("mep.groundConductor", N_("Conductor"), N_("Earthing Conductor (GC): click its start and its end; a chain of clicks makes the ring or the mesh, under the level"),
               Tool::Run, "groundconductor");
        toolOf("mep.dataOutlet", N_("Data"), N_("Data Outlet (DT): a data point, where you click on the wall; Space turns it"), Tool::Component, "dataoutlet");
        toolOf("mep.voiceOutlet", N_("Voice"), N_("Voice Outlet (VO): a telephone point, where you click on the wall; Space turns it"), Tool::Component, "voiceoutlet");
        toolOf("mep.rack", N_("Rack"), N_("Rack (RK): a floor rack, where you click; Space turns it"), Tool::Component, "rack");
        toolOf("mep.patchPanel", N_("Patch Panel"), N_("Patch Panel (PN): a panel on the wall, where you click; Space turns it"), Tool::Component, "patchpanel");
        toolOf("mep.cabling", N_("Cabling"), N_("Cabling (CB): click its start and its end; a chain of clicks makes a cable for each step"), Tool::Run, "cable");

        // ── 体量（移植自 BIMCraft 的体量选项卡）：体量工具；按面建模对选中的体量 ─────────
        tool("massing.mass", N_("Mass"), N_("Mass (MS): click the corners of the footprint; Enter, or the first corner again, finishes; Esc cancels"),
             Tool::Mass);
        auto byMass = [&](const char* id, const char* label, const char* tooltip, const char* command, const char* title)
        {
            add(id, label, tooltip, [this, command, title] { ByMass(command, title); }, [this] { return !m_sketch && !SelectedMasses().empty(); });
        };
        byMass("massing.floors", N_("Mass Floors"), N_("Mass Floors (MF): a floor at each level the selected masses cross"), "mass_floors",
               N_("Could not make the mass floors"));
        byMass("massing.wallByFace", N_("Wall by Face"), N_("Wall by Face (WB): a wall on each edge of the selected masses' footprints"), "wall_by_face",
               N_("Could not build the walls"));
        byMass("massing.floorByFace", N_("Floor by Face"), N_("Floor by Face (FB): a floor on the selected masses' top faces"), "floor_by_face",
               N_("Could not make the floor"));
        byMass("massing.roofByFace", N_("Roof by Face"), N_("Roof by Face (RB): a roof on the selected masses' top faces"), "roof_by_face",
               N_("Could not make the roof"));
        byMass("massing.updateByFace", N_("Update to Face"),
               N_("Update to Face (UF): the walls, floors and roofs made by face from the selected masses follow them again"), "update_by_face",
               N_("Could not update to the mass"));

        // ── 注释（移植自 BIMCraft 的注释选项卡）：尺寸、详图线、区域、文字、测量、标记 ─────────
        tool("annotate.aligned", N_("Aligned"),
             N_("Aligned Dimension (DI): pick parallel wall faces, wall ends, doors, windows or grids, then click where the dimension line goes"),
             Tool::Dimension);
        tool("annotate.angular", N_("Angular"), N_("Angular Dimension: pick two lines that meet, then click where the arc goes"), Tool::Angular);
        tool("annotate.radial", N_("Radial"), N_("Radial Dimension: pick a round column, then click where the radius goes"), Tool::Radial);
        tool("annotate.spot", N_("Spot Elevation"), N_("Spot Elevation (EL): click a floor, a roof or a stair"), Tool::Spot);
        tool("annotate.detailLine", N_("Detail Line"), N_("Detail Line (DL): click point to point; lines only this plan shows, in the Options Bar's line style"),
             Tool::DetailLine);
        auto region = [&](const char* id, const char* label, const char* tooltip, SketchOf of)
        {
            add(id, label, tooltip, [this, of] { StartSketch(of, 0, {}); }, [this] { return !m_sketch; });
        };
        region("annotate.filledRegion", N_("Filled Region"), N_("Filled Region (FR): sketch an area filled with a pattern, in this view"), SketchOf::Region);
        region("annotate.maskingRegion", N_("Masking Region"), N_("Masking Region: sketch an area that hides what's under it, in this view"), SketchOf::Masking);
        tool("annotate.text", N_("Text"), N_("Text (TX): type the note in the Options Bar, then click where it goes"), Tool::Text);
        tool("annotate.measure", N_("Measure"), N_("Measure (ME): click two points; their length and angle read beside the ruler between them"),
             Tool::Measure);
        tool("annotate.tag", N_("Tag by Category"),
             N_("Tag by Category (TG): click an element to tag it with its type, a door's or window's mark, or a room's name"), Tool::Tag);
        add("annotate.tagAll", N_("Tag All"), N_("Tag every door and window in this view that has no tag"), [this] { TagAll(); },
            [this] { return !m_sketch; });

        // ── 视图 ─────────────────────────────────────────────────
        add("view.3d", N_("3D View"), N_("Show the model in 3D"), [this] { ShowModel3D(); });
        add("view.plan", N_("Floor Plan"), N_("Show the plan of a level: the one shown last, or the lowest"), [this] { ShowPlan(std::nullopt); });
        add("view.levelUp", N_("Level Above"), N_("Show the plan of the level above"), [this] { StepLevel(1); });
        add("view.levelDown", N_("Level Below"), N_("Show the plan of the level below"), [this] { StepLevel(-1); });
        add("view.home", N_("Home View"), N_("Southwest isometric view of the whole model"), [this] { HomeView(); });
        add("view.zoomFit", N_("Zoom to Fit"), N_("Fit the whole model in the view without changing the direction"), [this] { ZoomToFit(); });
        add("view.properties", N_("Properties"), N_("Show or hide the Properties palette"), [this] { TogglePanel("properties"); }, {},
            [this] { return PanelShown("properties"); });
        add("view.browser", N_("Project Browser"), N_("Show or hide the Project Browser"), [this] { TogglePanel("browser"); }, {},
            [this] { return PanelShown("browser"); });

        // ── 帮助 ─────────────────────────────────────────────────
        add("help.about", N_("About MiniBIM"), "", [this] { ShowAbout(); });
        RegisterShortcuts();
    }

    // =========================================================
    // 项目：都经模型命令（Bim::Session），失败时弹出原因
    // =========================================================
    namespace
    {
        std::filesystem::path ToPath(const std::string& utf8)
        {
            return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }

        std::string ToUtf8(const std::filesystem::path& p)
        {
            const std::u8string u8 = p.u8string();
            return std::string(u8.begin(), u8.end());
        }
    }

    bool MainFrame::RunCommand(const char* id, const Bim::Json& params, const char* title, Bim::Json* result)
    {
        Bim::Json out;
        std::string error;
        const bool ok = m_session.Execute(id, params, out, error);
        if (!ok)
        {
            // 核心的报错是英文（BIMCraft 的命令信息给人和 AI 代理读，不翻译）；对话框标题随界面语言
            LOG_WARN("命令 %s 失败：%s", id, error.c_str());
            MiniGUI::ShowMessageBox(*m_ui, Tr(title), error);
        }
        else if (result)
            *result = std::move(out);
        OnModelChanged();
        return ok;
    }

    void MainFrame::Guard(std::function<void()> then)
    {
        if (!Dirty())
        {
            then();
            return;
        }
        // 项目：保存过的显示文件名，否则显示项目名称
        std::string name = m_session.GetModel().name;
        if (const auto& path = m_session.Path())
            name = ToUtf8(ToPath(*path).filename());
        const std::string message = Format(Tr("Save changes to {0}?"), { name }) + "\n\n" +
                                    Tr("If you don't, the changes since it was last saved are lost.");
        MiniGUI::ShowMessageBox(*m_ui, "MiniBIM", message, { Tr("Save"), Tr("Don't Save"), Tr("Cancel") },
            [this, then = std::move(then)](int button)
            {
                // 对话框关掉以后再做（可能还要弹出文件对话框）
                if (button == 0)
                    m_platform->Post([this, then] { SaveProject(false, then); });
                else if (button == 1)
                    m_platform->Post(then);
            });
    }

    void MainFrame::NewProject()
    {
        RunCommand("new", Bim::Json::object(), N_("Could not create a project"));
    }

    void MainFrame::OpenProject()
    {
        m_platform->PickFile(m_revitReader ? FileKind::ProjectOrRevit : FileKind::Project, [this](const std::string& path)
        {
            if (!path.empty())
                OpenPath(path);
        });
    }

    void MainFrame::OpenFiles(const std::vector<std::string>& utf8Paths)
    {
        m_opens.Add(utf8Paths);
        if (!m_platform || m_opensPosted || !m_opens.Waiting())
            return;
        m_opensPosted = true;
        m_platform->Post([this] { TakeOpens(); });
    }

    void MainFrame::TakeOpens()
    {
        m_opensPosted = false;
        const bool busy = ImportingRevit() || (m_ui && m_ui->HasModal());
        if (const auto path = m_opens.Take(busy))
            Guard([this, path = *path] { OpenPath(path); });
    }

    bool MainFrame::OpenPath(const std::string& path)
    {
        // Revit 文件在后台读：读进来时才换掉项目；不进最近打开
        if (RevitReport::IsRevit(path))
            return OpenRevit(path);
        Bim::Json r;
        if (!RunCommand("open", Bim::Json{ { "path", path } }, N_("Could not open the project"), &r))
        {
            // 打不开、而且已经不在了：从最近打开里去掉
            std::error_code ec;
            if (!m_recentPath.empty() && !std::filesystem::exists(ToPath(path), ec))
            {
                m_recent.Remove(path);
                KeepRecent();
            }
            return false;
        }
        NoteRecent(path);
        // IFC 里 MiniBIM 还没有的元素、违反规则的略去了：说一声有几个（列表在命令的结果里）
        if (r.contains("skipped") && r["skipped"].is_array() && !r["skipped"].empty())
        {
            const size_t n = r["skipped"].size();
            MiniGUI::ShowMessageBox(*m_ui, Tr("Opened with parts left out"),
                Format(TrN("{0} item of this IFC file has no MiniBIM element yet, or breaks one of its rules, and was left out.",
                           "{0} items of this IFC file have no MiniBIM element yet, or break one of its rules, and were left out.",
                           static_cast<long long>(n)), { std::to_string(n) }));
        }
        // 这个版本还不认识的内容（BIMCraft 有、还没移植的元素与设置）略去了：说一声，保存时会丢掉
        const auto left = r.contains("left_out_count") ? Bim::AsWhole(r["left_out_count"]) : std::nullopt;
        if (left && *left > 0)
        {
            const std::string n = std::to_string(*left);
            MiniGUI::ShowMessageBox(*m_ui, Tr("Opened with parts left out"),
                Format(TrN("{0} item of this file isn't supported by this version yet and was left out. Saving the project drops it.",
                           "{0} items of this file aren't supported by this version yet and were left out. Saving the project drops them.",
                           static_cast<long long>(*left)), { n }));
        }
        return true;
    }

    void MainFrame::SaveProject(bool saveAs, std::function<void()> then)
    {
        // 只往 .minibim 里直接保存：打开的是 BIMCraft 的文件时也另存，免得把略去了内容的项目写回原文件
        const auto& current = m_session.Path();
        const bool own = current && ToPath(*current).extension() == ".minibim";
        auto save = [this, then = std::move(then)](const std::string& path)
        {
            const Bim::Json params = path.empty() ? Bim::Json::object() : Bim::Json{ { "path", path } };
            if (!RunCommand("save", params, N_("Could not save the project")))
                return;
            m_savedModel = m_session.Snapshot();
            if (const auto& saved = m_session.Path())
            {
                NoteRecent(*saved);
                m_platform->OnDocumentSaved(*saved);
            }
            AutosaveSaved();
            UpdateTitle();
            if (then)
                then();
        };
        if (!saveAs && own)
        {
            save({});
            return;
        }
        AskSavePath(SaveKind::Project, ProjectStem() + ".minibim", std::move(save));
    }

    std::string MainFrame::ProjectStem() const
    {
        const auto& current = m_session.Path();
        const std::string stem = current ? ToUtf8(ToPath(*current).stem()) : m_session.GetModel().name;
        return stem.empty() ? std::string("Project") : stem;
    }

    void MainFrame::AskSavePath(SaveKind kind, const std::string& suggestedName, std::function<void(const std::string&)> done)
    {
        if (m_platform->HasSaveDialog())
        {
            if (const std::string path = m_platform->ChooseSavePath(kind, suggestedName); !path.empty())
                done(path);
            return;
        }

        // 平台没有系统的另存为对话框（网页版）：自己问文件名，扩展名固定
        using namespace MiniGUI;
        const std::string ext = ToUtf8(ToPath(suggestedName).extension());
        const std::string base = ToUtf8(ToPath(suggestedName).stem());
        auto dialog = std::make_unique<Dialog>(kind == SaveKind::Ifc            ? Tr("Export IFC")
                                               : kind == SaveKind::FillPatterns ? Tr("Export Fill Patterns")
                                                                                : Tr("Save As"));
        Dialog* dlg = dialog.get();
        Node* body = dlg->GetBody();
        LayoutStyle& bs = body->EditLayoutStyle();     // 留着对话框给内容区的边距
        bs.gap = 6.0f;
        body->AddChild<Label>(Tr("File name"), 13.0f, Theme::Text);
        auto* row = body->AddChild<Node>();
        LayoutStyle rs;
        rs.direction  = FlexDirection::Row;
        rs.alignItems = Align::Center;
        rs.gap        = 6.0f;
        row->SetLayoutStyle(rs);
        auto* name = row->AddChild<TextBox>(base);
        name->EditLayoutStyle().width = 280.0f;
        row->AddChild<Label>(ext, 13.0f, Theme::TextDim);
        body->AddChild<Label>(Tr("Your browser downloads the file when it is written."), 12.0f, Theme::TextDim);

        auto trimmed = [](std::string t)
        {
            const auto first = t.find_first_not_of(" \t");
            if (first == std::string::npos)
                return std::string();
            t.erase(t.find_last_not_of(" \t") + 1);
            return t.substr(first);
        };
        Button* ok = dlg->AddButton(kind == SaveKind::Project ? Tr("Save") : Tr("Export"), [this, dlg, name, kind, ext, trimmed, done = std::move(done)]
        {
            std::string n = trimmed(name->GetText());
            dlg->Close();
            if (n.empty())
                return;
            if (!(n.size() > ext.size() && ToUtf8(ToPath(n).extension()) == ext))
                n += ext;
            // 对话框关掉以后再写（写文件时可能还要弹出报错）
            m_platform->Post([this, kind, n, done]
            {
                if (const std::string path = m_platform->ChooseSavePath(kind, n); !path.empty())
                    done(path);
            });
        }, DialogButtonRole::Default);
        dlg->AddButton(Tr("Cancel"), [dlg] { dlg->Close(); }, DialogButtonRole::Cancel);
        name->SetOnChanged([ok, trimmed](const std::string& t) { ok->SetEnabled(!trimmed(t).empty()); });
        ok->SetEnabled(!trimmed(base).empty());
        dlg->SetInitialFocus(name);
        m_ui->OpenPopup(std::move(dialog));
        name->SelectAll();
    }

    // =========================================================
    // 最近打开
    // =========================================================
    void MainFrame::NoteRecent(const std::string& path)
    {
        if (m_recentPath.empty())
            return;
        m_recent.Push(path);
        KeepRecent();
    }

    void MainFrame::KeepRecent()
    {
        std::string error;
        if (!m_recentPath.empty() && !m_recent.Save(m_recentPath, error))
            LOG_WARN("最近打开：%s", error.c_str());
        if (m_startShown)
            PostStartPage();
    }

    std::vector<MiniGUI::MenuItem> MainFrame::BuildRecentMenu()
    {
        using MiniGUI::MenuItem;
        std::vector<MenuItem> items;
        if (m_recent.Paths().empty())
            return items;   // 空列表：菜单项显示为禁用
        const std::vector<std::string> labels = m_recent.Labels();
        for (size_t i = 0; i < labels.size(); ++i)
        {
            const std::string path = m_recent.Paths()[i];
            items.emplace_back(labels[i], [this, path] { m_platform->Post([this, path] { Guard([this, path] { OpenPath(path); }); }); });
        }
        items.push_back(MenuItem::Separator());
        items.emplace_back(Tr("Clear Recent Files"), [this] { m_recent.Clear(); KeepRecent(); });
        return items;
    }

    void MainFrame::ExportIfc()
    {
        AskSavePath(SaveKind::Ifc, ProjectStem() + ".ifc", [this](const std::string& path)
        {
            // save 写 .ifc 时只导出，不改项目自己的路径；项目仍然算没保存
            if (RunCommand("save", Bim::Json{ { "path", path } }, N_("Could not export IFC")))
                m_platform->OnDocumentSaved(path);
        });
    }

    void MainFrame::StepHistory(bool redo)
    {
        RunCommand(redo ? "redo" : "undo", Bim::Json::object(), redo ? N_("Could not redo") : N_("Could not undo"));
    }

    void MainFrame::DeleteSelection()
    {
        if (!m_selection.empty())
            RunCommand("delete", Bim::Json{ { "ids", m_selection } }, N_("Could not delete"));
    }

    void MainFrame::FlipSelection()
    {
        const Bim::Model& m = m_session.GetModel();
        std::vector<Bim::Id> walls;
        for (const Bim::Id id : m_selection)
            if (m.GetWall(id))
                walls.push_back(id);
        if (!walls.empty())
            RunCommand("flip", Bim::Json{ { "ids", walls } }, N_("Could not flip the walls"));
    }

    void MainFrame::OpenSample()
    {
        std::vector<Bim::Json> out;
        std::string error;
        if (!m_session.Script(Bim::SampleScript, out, error))
        {
            LOG_WARN("示例房屋：%s", error.c_str());
            MiniGUI::ShowMessageBox(*m_ui, Tr("Could not build the sample house"), error);
        }
        OnModelChanged();
    }

    void MainFrame::OnModelChanged()
    {
        if (m_session.Snapshot() != m_sceneModel)
            RebuildScene();
        // 换了项目（新建、打开、示例）：相机重新框住模型，选择集清空，这一份算保存过的
        if (m_session.Resets() != m_sceneResets)
        {
            m_sceneResets  = m_session.Resets();
            m_cameraFitted = false;
            m_selection.clear();
            // 草图是上一个项目的
            if (m_sketch)
            {
                m_sketch.reset();
                SetTool(Tool::Modify);
            }
            m_savedModel   = m_session.Snapshot();
            // 新建、打开了项目：开始页让给它；恢复副本跟着换
            if (m_startShown)
                ShowStartPage(false, false);
            AutosaveDocument();
        }
        OnPlanModelChanged();
        UpdateTitle();
        UpdateStatus();
        RefreshPanels();
        m_commands.NotifyStateChanged();
        if (m_viewport)
            m_viewport->RequestRender();
        m_platform->RequestRedraw();
    }
}
