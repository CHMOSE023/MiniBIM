// 所有操作注册为 MiniGUI 命令；菜单、标题栏按钮经界面描述文件 ui/minibim_ui.json 引用命令 ID。
// 名称与提示写英文原文并用 N_ 标记（收集工具据此找到它们），UiLayout 每次应用时按当前语言翻译。
// 名称不写助记符 &：菜单上不显示下划线和 "(X)"
#include "GUI/MainFrame.h"
#include "I18n/Translator.h"

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
        add("file.exit", N_("Exit"), "", [this] { RequestExit(); });

        // ── 视图 ─────────────────────────────────────────────────
        add("view.home", N_("Home View"), N_("Southwest isometric view of the whole model"), [this] { HomeView(); });
        add("view.zoomFit", N_("Zoom to Fit"), N_("Fit the whole model in the view without changing the direction"), [this] { ZoomToFit(); });

        // ── 帮助 ─────────────────────────────────────────────────
        add("help.about", N_("About MiniBIM"), "", [this] { ShowAbout(); });
    }
}
