// 所有操作注册为 MiniGUI 命令；菜单、标题栏按钮经界面描述文件 ui/minibim_ui.json 引用命令 ID。
// 名称与提示写英文原文并用 N_ 标记（收集工具据此找到它们），UiLayout 每次应用时按当前语言翻译。
// 名称不写助记符 &：菜单上不显示下划线和 "(X)"
#include "GUI/MainFrame.h"
#include "Commands/Sample.h"
#include "Core/Log.h"
#include "Core/UIContext.h"
#include "I18n/Translator.h"
#include "Widgets/Dialog.h"
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

        // ── 窗口（标题栏右侧的按钮执行这三个）───────────────────────
        add("window.minimize", N_("Minimize"), "", [this] { m_platform->MinimizeWindow(); });
        add("window.maximize", N_("Maximize"), "", [this] { m_platform->ToggleMaximizeWindow(); });
        add("window.close",    N_("Close"),    "", [this] { m_platform->CloseWindow(); });

        // ── 文件 ─────────────────────────────────────────────────
        add("file.new", N_("New"), N_("Start an empty project with two levels"), [this] { Guard([this] { NewProject(); }); });
        add("file.open", N_("Open..."), N_("Open a MiniBIM project, a BIMCraft one, or an IFC file as a new project"),
            [this] { Guard([this] { OpenProject(); }); });
        add("file.save", N_("Save"), N_("Save the project"), [this] { SaveProject(false); });
        add("file.saveAs", N_("Save As..."), N_("Save the project under another name"), [this] { SaveProject(true); });
        add("file.exportIfc", N_("Export IFC..."), N_("Export the model as an IFC4 file for other BIM software"), [this] { ExportIfc(); });
        add("file.sample", N_("Sample House"), N_("Open the sample house, built by commands"), [this] { Guard([this] { OpenSample(); }); });
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
        add("modify.flip", N_("Flip Wall"), N_("Turn the selected walls round: their exterior sides go to the other side"), [this] { FlipSelection(); },
            [this]
            {
                const Bim::Model& m = m_session.GetModel();
                return std::any_of(m_selection.begin(), m_selection.end(), [&](Bim::Id id) { return m.GetWall(id) != nullptr; });
            });

        // ── 草图模式（SketchMode.cpp）：编辑边界，完成、取消，画草图线的工具 ─────────
        add("modify.editBoundary", N_("Edit Boundary"), N_("Edit the outline of the selected floor, ceiling or roof as sketch lines"),
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
        tool("arch.component", N_("Component"), N_("Component: a family's type, where you click; Space turns it"), Tool::Component);
        tool("arch.roof", N_("Roof"), N_("Roof by Footprint: click inside the walls to roof them, on the level above"), Tool::Roof);
        tool("arch.floor", N_("Floor"), N_("Floor: click inside the walls to floor them"), Tool::Floor);
        tool("arch.ceiling", N_("Ceiling"), N_("Ceiling: click inside a room for a ceiling out to its walls"), Tool::Ceiling);
        tool("arch.room", N_("Room"), N_("Room: click inside walls"), Tool::Room);
        tool("arch.level", N_("Level"), N_("Level: a new level at an elevation"), Tool::Level);
        tool("arch.grid", N_("Grid"), N_("Grid: click its two ends"), Tool::Grid);

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
                    m_platform->Post([this, then] { if (SaveProject(false)) then(); });
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
        const std::string path = m_platform->PickProjectFile();
        if (path.empty())
            return;
        Bim::Json r;
        if (!RunCommand("open", Bim::Json{ { "path", path } }, N_("Could not open the project"), &r))
            return;
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
    }

    bool MainFrame::SaveProject(bool saveAs)
    {
        // 只往 .minibim 里直接保存：打开的是 BIMCraft 的文件时也另存，免得把略去了内容的项目写回原文件
        const auto& current = m_session.Path();
        const bool own = current && ToPath(*current).extension() == ".minibim";
        std::string path;
        if (saveAs || !own)
        {
            const std::string stem = current ? ToUtf8(ToPath(*current).stem()) : m_session.GetModel().name;
            path = m_platform->ChooseProjectSavePath((stem.empty() ? std::string("Project") : stem) + ".minibim");
            if (path.empty())
                return false;
        }
        const Bim::Json params = path.empty() ? Bim::Json::object() : Bim::Json{ { "path", path } };
        if (!RunCommand("save", params, N_("Could not save the project")))
            return false;
        m_savedModel = m_session.Snapshot();
        UpdateTitle();
        return true;
    }

    void MainFrame::ExportIfc()
    {
        const auto& current = m_session.Path();
        const std::string stem = current ? ToUtf8(ToPath(*current).stem()) : m_session.GetModel().name;
        const std::string path = m_platform->ChooseIfcSavePath((stem.empty() ? std::string("Project") : stem) + ".ifc");
        if (path.empty())
            return;
        // save 写 .ifc 时只导出，不改项目自己的路径；项目仍然算没保存
        RunCommand("save", Bim::Json{ { "path", path } }, N_("Could not export IFC"));
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
