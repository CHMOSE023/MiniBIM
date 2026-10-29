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
#include <filesystem>

namespace MiniBIM
{
    void MainFrame::RegisterCommands()
    {
        using MiniGUI::Command;
        auto add = [this](const char* id, const char* label, const char* tooltip, std::function<void()> execute)
        {
            Command c;
            c.id      = id;
            c.label   = label;
            c.tooltip = tooltip;
            c.execute = std::move(execute);
            m_commands.Register(std::move(c));
        };

        // ── 窗口（标题栏右侧的按钮执行这三个）───────────────────────
        add("window.minimize", N_("Minimize"), "", [this] { m_platform->MinimizeWindow(); });
        add("window.maximize", N_("Maximize"), "", [this] { m_platform->ToggleMaximizeWindow(); });
        add("window.close",    N_("Close"),    "", [this] { m_platform->CloseWindow(); });

        // ── 文件 ─────────────────────────────────────────────────
        add("file.new", N_("New"), N_("Start an empty project with two levels"), [this] { NewProject(); });
        add("file.open", N_("Open..."), N_("Open a MiniBIM project, or a BIMCraft one"), [this] { OpenProject(); });
        add("file.save", N_("Save"), N_("Save the project"), [this] { SaveProject(false); });
        add("file.saveAs", N_("Save As..."), N_("Save the project under another name"), [this] { SaveProject(true); });
        add("file.sample", N_("Sample House"), N_("Open the sample house, built by commands"), [this] { OpenSample(); });
        add("file.exit", N_("Exit"), "", [this] { RequestExit(); });

        // ── 视图 ─────────────────────────────────────────────────
        add("view.home", N_("Home View"), N_("Southwest isometric view of the whole model"), [this] { HomeView(); });
        add("view.zoomFit", N_("Zoom to Fit"), N_("Fit the whole model in the view without changing the direction"), [this] { ZoomToFit(); });

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
        return RunCommand("save", params, N_("Could not save the project"));
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
        // 换了项目（新建、打开、示例）：相机重新框住模型
        if (m_session.Resets() != m_sceneResets)
        {
            m_sceneResets  = m_session.Resets();
            m_cameraFitted = false;
        }
        UpdateTitle();
        UpdateStatus();
        if (m_viewport)
            m_viewport->RequestRender();
        m_platform->RequestRedraw();
    }
}
