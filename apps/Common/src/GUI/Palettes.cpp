// 移植自 BIMCraft crates/ui-egui/src/dock.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Core/Log.h"
#include "Core/UIContext.h"
#include "I18n/Translator.h"
#include "Widgets/DockSpace.h"
#include "Widgets/UiLayout.h"
#include <algorithm>

// 面板的停靠与浮动，照 BIMCraft（同 Revit）。拖标签停靠、拖出或双击浮出、拖浮动窗口的标题栏停回去由 MiniGUI 的停靠区做；这里是
//   - 右键面板的标签（或浮动窗口的标题栏）弹出的菜单：靠左、靠右、靠下停靠，浮动，关闭（BIMCraft 面板标题上的 Dock 菜单与 ×）。
//     停到一侧时那一侧已有面板就排在它们后面（左右两侧上下排，底部左右排），没有就在整个停靠区的那一侧开一栏
//   - 布局存进 settings.json 的 "dock"（MiniGUI::DockSpace::SaveLayout 的格式），每次改动后保存、启动时恢复；
//     设置里"重置面板布局"回到界面描述文件里的布局（特性在上、项目浏览器在下，靠左）
namespace MiniBIM
{
    namespace
    {
        using MiniGUI::DockSide;
        using MiniGUI::DockSpace;
        using MiniGUI::Rect;
        using MiniGUI::Vec2;

        constexpr const char* kView = "viewport";       // 视图：不能关、不能浮出，是判断停在哪一侧的参照
        constexpr Vec2 kFloatSize{ 300.0f, 420.0f };    // 浮出的窗口一开始的大小（BIMCraft 的 FLOAT_SIZE）

        // 一点在视图的哪一侧（停靠区里视图之外的地方）
        std::optional<DockSide> SideOf(Vec2 p, const Rect& view)
        {
            if (view.IsEmpty())
                return std::nullopt;
            if (p.x < view.min.x)
                return DockSide::Left;
            if (p.x > view.max.x)
                return DockSide::Right;
            if (p.y > view.max.y)
                return DockSide::Bottom;
            if (p.y < view.min.y)
                return DockSide::Top;
            return std::nullopt;
        }
    }

    MiniGUI::DockSpace* MainFrame::MainDock() const
    {
        return m_layout ? m_layout->GetDock("main") : nullptr;
    }

    void MainFrame::ShowPanelMenu(const std::string& panel, MiniGUI::Vec2 at)
    {
        using MiniGUI::MenuItem;
        DockSpace* dock = MainDock();
        if (!dock || panel == kView)
            return;
        const bool floating = dock->IsPanelFloating(panel);
        // 右键的是标签或浮动窗口的标题栏，就在面板自己身上：按点在视图的哪一侧算它停在哪里
        const std::optional<DockSide> side = floating ? std::nullopt : SideOf(at, dock->GetPanelRect(kView));
        std::vector<MenuItem> items;
        for (const auto& [s, text] : { std::pair{ DockSide::Left, N_("Dock Left") }, std::pair{ DockSide::Right, N_("Dock Right") },
                                       std::pair{ DockSide::Bottom, N_("Dock Bottom") } })
        {
            MenuItem item(Tr(text), [this, panel, s] { m_platform->Post([this, panel, s] { DockPanelTo(panel, s); }); });
            item.Enabled(side != s);        // 已经停在那一侧
            items.push_back(std::move(item));
        }
        MenuItem flt(Tr("Float"), [this, panel, at] { m_platform->Post([this, panel, at] { FloatPanelAt(panel, at); }); });
        flt.Enabled(!(floating && dock->GetFloatPanels(panel).size() == 1));     // 已经独占一个浮动窗口
        items.push_back(std::move(flt));
        items.push_back(MenuItem::Separator());
        items.emplace_back(Tr("Close"), [this, panel] { m_platform->Post([this, panel] { TogglePanel(panel.c_str()); }); });
        ShowContextMenu(*m_ui, std::move(items), at);
    }

    void MainFrame::DockPanelTo(const std::string& panel, MiniGUI::DockSide side)
    {
        DockSpace* dock = MainDock();
        if (!dock)
            return;
        // 那一侧已有的面板里最后一个（左右两侧最下面的、底部最右边的）；不在当前标签上的面板量不到位置，它所在的组由当前标签代表
        const Rect  view = dock->GetPanelRect(kView);
        std::string last;
        float       far  = 0.0f;
        for (const std::string& id : dock->GetPanelIds())
        {
            if (id == panel || id == kView || !dock->IsPanelActive(id) || dock->IsPanelFloating(id))
                continue;
            const Rect r = dock->GetPanelRect(id);
            if (r.IsEmpty() || SideOf(r.Center(), view) != side)
                continue;
            const float d = side == DockSide::Bottom ? r.Center().x : r.Center().y;
            if (last.empty() || d > far)
                last = id, far = d;
        }
        if (last.empty())
            dock->Dock(panel, {}, side, side == DockSide::Bottom ? 240.0f : 290.0f);
        else
            dock->Dock(panel, last, side == DockSide::Bottom ? DockSide::Right : DockSide::Bottom);
        dock->ActivatePanel(panel);
        SavePanelLayout();
        m_commands.NotifyStateChanged();
    }

    void MainFrame::FloatPanelAt(const std::string& panel, MiniGUI::Vec2 at)
    {
        DockSpace* dock = MainDock();
        if (!dock)
            return;
        // 窗口的左上角在右键处往里一点；弹层自己会挪进窗口可见的范围
        const Vec2 corner = at + Vec2{ 40.0f, 40.0f };
        if (dock->FloatPanel(panel, Rect{ corner, corner + kFloatSize }))
            SavePanelLayout();
        m_commands.NotifyStateChanged();
    }

    void MainFrame::ResetPanelLayout()
    {
        DockSpace* dock = MainDock();
        if (!dock || !m_dockDefault.IsObject())
            return;
        dock->LoadLayout(m_dockDefault);
        SavePanelLayout();
        m_commands.NotifyStateChanged();
    }

    void MainFrame::SavePanelLayout()
    {
        DockSpace* dock = MainDock();
        if (!dock)
            return;
        m_dockLayout = dock->SaveLayout();
        m_dockPanels = dock->GetPanelIds();
        SaveSettings();
    }

    void MainFrame::AttachDock(bool restore)
    {
        DockSpace* dock = MainDock();
        if (!dock)
            return;
        m_dockDefault = dock->SaveLayout();     // 界面描述文件里的布局：重置时回到它
        if (restore)
            RestorePanelLayout();
        else
            m_dockLayout = m_dockDefault;       // 界面描述文件改了：用它的布局，下次改动时再存
        dock->SetOnLayoutChanged([this] { SavePanelLayout(); });
        dock->SetOnPanelMenu([this](const std::string& id, Vec2 at) { ShowPanelMenu(id, at); });
    }

    void MainFrame::RestorePanelLayout()
    {
        DockSpace* dock = MainDock();
        if (!dock || !m_dockLayout.IsObject())
            return;     // 还没保存过
        // 保存之后界面描述文件新增了面板：旧布局里没有它，用默认布局，免得新面板一直看不到
        for (const std::string& id : dock->GetPanelIds())
        {
            if (std::find(m_dockPanels.begin(), m_dockPanels.end(), id) == m_dockPanels.end())
            {
                LOG_INFO("面板布局：界面新增了面板 %s，使用默认布局", id.c_str());
                m_dockLayout = m_dockDefault;
                return;
            }
        }
        std::vector<std::string> warnings;
        // 视图必须在布局里；文件写坏了或界面描述换了面板时回到默认布局
        if (!dock->LoadLayout(m_dockLayout, &warnings) || !dock->IsPanelVisible(kView) || dock->IsPanelFloating(kView))
        {
            LOG_WARN("面板布局：保存的布局与当前界面不符，使用默认布局");
            dock->LoadLayout(m_dockDefault);
            m_dockLayout = m_dockDefault;
            return;
        }
        for (const std::string& w : warnings)
            LOG_WARN("面板布局：%s", w.c_str());
    }
}
