#include "GUI/MainFrame.h"
#include "Core/Log.h"
#include "Core/Translate.h"
#include "Core/UIContext.h"
#include "Data/Json.h"
#include "I18n/Translator.h"
#include "MiniBIMVersion.h"
#include "Render/IRenderer.h"
#include "Style/Theme.hpp"
#include "Widgets/Dialog.h"
#include "Widgets/Label.h"
#include "Widgets/Panel.h"
#include "Widgets/TitleBar.h"
#include "Widgets/UiLayout.h"
#include "Widgets/ViewportHost.h"
#include <algorithm>
#include <cmath>
#include <fstream>

namespace MiniBIM
{
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

        std::filesystem::file_time_type FileTime(const std::string& utf8Path)
        {
            std::error_code ec;
            const auto t = std::filesystem::last_write_time(ToPath(utf8Path), ec);
            return ec ? std::filesystem::file_time_type{} : t;
        }
    }

    MainFrame::MainFrame() = default;

    MainFrame::~MainFrame()
    {
        MiniGUI::SetTranslator({});
    }

    MiniGUI::TitleBar* MainFrame::GetTitleBar() const
    {
        return m_layout ? m_layout->GetTitleBar() : nullptr;
    }

    // =========================================================
    // 初始化
    // =========================================================
    bool MainFrame::Initialize(AppPlatform& platform, float width, float height, float pixelScale)
    {
        m_platform = &platform;
        m_dpiScale = pixelScale;

        // 多语言：词典在资源目录的 i18n/ 下；MiniGUI 自带的文字与界面描述文件也经同一个词典翻译
        I18n::Translator& translator = I18n::Translator::Instance();
        translator.SetCatalogDir(platform.GetResourceDir() + "/i18n");
        MiniGUI::SetTranslator([](std::string_view english) { return Tr(english); });
        LoadSettings();
        if (const std::string dir = platform.GetUserDataDir(); !dir.empty())
        {
            m_recentPath = dir + "/recent.json";
            m_recent     = RecentFiles::Load(m_recentPath);
        }
        const std::string code = !m_languageOverride.empty() ? m_languageOverride
                               : !m_languageSetting.empty()  ? m_languageSetting
                                                             : platform.GetSystemLanguage();
        LOG_INFO("语言：%s（设置 \"%s\"，系统 \"%s\"）", translator.SetLanguage(code).c_str(),
                 m_languageSetting.c_str(), platform.GetSystemLanguage().c_str());

        InitUI();
        OnResize(width, height, pixelScale);
        // 给了项目文件就打开它；给了 --run 的界面命令就在示例房屋上执行（看界面效果时用）；否则显示开始页
        const bool opened = !m_startupFile.empty() && OpenPath(m_startupFile);
        if (!opened && !m_startupCommands.empty())
            OpenSample();
        else if (!opened)
        {
            m_savedModel = m_session.Snapshot();    // 开始页后面的空项目算保存过的
            OnModelChanged();
            ShowStartPage(true, false);
        }
        // 打开了要打开的之后再开始：之前的换项目不算（启动时找的副本按这一份文档算）
        EnableAutosave();
        for (const std::string& id : m_startupCommands)
            if (!m_commands.Execute(id))
                LOG_WARN("启动参数 --run：没有界面命令 %s", id.c_str());
        return true;
    }

    void MainFrame::InitUI()
    {
        m_ui = std::make_unique<MiniGUI::UIContext>(&m_platform->GetUiBackend());
        ApplyTheme();
        if (!m_platform->LoadUiFonts(m_ui->GetTextSystem()))
            LOG_ERROR("MainFrame: 没有找到界面字体");
        AppPlatform* platform = m_platform;
        m_ui->SetRedrawCallback([platform] { platform->RequestRedraw(); });

        RegisterCommands();
        m_commands.BindShortcuts(m_ui->GetShortcuts());
        m_commands.AddListener([this] { UpdateStatus(); });

        // ── 宿主面板，交给界面描述文件摆放（标题写英文原文，UiLayout 应用时翻译）──
        m_layout = std::make_unique<MiniGUI::UiLayout>(m_commands, m_platform->GetResourceDir() + "/icons");
        m_layout->RegisterPanel("viewport",   CreateViewport(), N_("View"));
        m_layout->RegisterPanel("options",    CreateOptionsBar());
        m_layout->RegisterPanel("properties", CreateProperties(), N_("Properties"));
        m_layout->RegisterPanel("browser",    CreateBrowser(), N_("Project Browser"));
        m_layout->RegisterPanel("statusbar",  CreateStatusBar());
        m_layout->SetDynamicMenuItems("languages", [this] { return BuildLanguageMenu(); });
        m_layout->SetDynamicMenuItems("recent", [this] { return BuildRecentMenu(); });

        // 界面描述生成的部分放在 m_uiHost 里；开始页（StartPage.cpp）是它旁边绝对定位的一层（根节点把子节点都拉满，不排版）
        m_appHost = m_ui->GetRoot()->AddChild<MiniGUI::Node>();
        m_uiHost  = m_appHost->AddChild<MiniGUI::Node>();
        MiniGUI::LayoutStyle hs;
        hs.grow = 1.0f;
        m_uiHost->SetLayoutStyle(hs);
        if (m_uiPath.empty())
        {
#ifdef MINIBIM_UI_SOURCE
            // 开发时直接读源码目录里的文件（修改后立即生效），找不到时用输出目录里的副本
            std::error_code ec;
            if (std::filesystem::exists(ToPath(MINIBIM_UI_SOURCE), ec))
                m_uiPath = MINIBIM_UI_SOURCE;
#endif
            if (m_uiPath.empty())
                m_uiPath = m_platform->GetResourceDir() + "/ui/minibim_ui.json";
        }
        ReloadUi();
        BuildOptionsBar();
        CreateStartPage();
    }

    void MainFrame::ReloadUi()
    {
        m_uiFileTime = FileTime(m_uiPath);
        if (m_layout->ApplyFile(m_uiHost, m_uiPath))
        {
            m_uiError.clear();
            for (const std::string& w : m_layout->GetWarnings())
                LOG_WARN("界面描述文件：%s", w.c_str());
        }
        else
        {
            m_uiError = m_layout->GetError();
            LOG_ERROR("界面描述文件：%s", m_uiError.c_str());
        }
        if (MiniGUI::TitleBar* bar = GetTitleBar())
        {
            MiniGUI::TitleBar::WindowActions actions;
            actions.minimize       = [this] { m_platform->MinimizeWindow(); };
            actions.toggleMaximize = [this] { m_platform->ToggleMaximizeWindow(); };
            actions.close          = [this] { m_platform->CloseWindow(); };
            actions.isMaximized    = [this] { return m_platform->IsWindowMaximized(); };
            bar->SetWindowActions(std::move(actions));
        }
        UpdateTitle();
        UpdateStatus();
        m_platform->RequestRedraw();
    }

    void MainFrame::CheckUiFileChanged()
    {
        if (FileTime(m_uiPath) != m_uiFileTime)
            ReloadUi();
    }

    void MainFrame::OnResize(float width, float height, float pixelScale)
    {
        if (!m_ui)
            return;
        m_dpiScale = pixelScale;
        m_ui->SetDisplaySize({ width, height }, pixelScale);
        if (m_startShown && StartColumns() != m_startColumns)
            PostStartPage();
        m_ui->Update();
        m_commands.NotifyStateChanged();    // 最大化按钮切换"还原"图标
    }

    void MainFrame::RequestExit()
    {
        // 有没保存的改动时先问
        Guard([this]
        {
            // 保存过或说了不保存：这份文档的恢复副本不要了（工作线程在 MainFrame 销毁前做完）
            RetireAutosave();
            m_platform->Quit();
        });
    }

    // =========================================================
    // 语言
    // =========================================================
    std::vector<MiniGUI::MenuItem> MainFrame::BuildLanguageMenu()
    {
        using MiniGUI::MenuItem;
        std::vector<MenuItem> items;
        MenuItem system(Tr("System Default"),[this] { m_platform->Post([this] { SetLanguage({}, true); }); });
        system.checkedIf = [this] { return m_languageSetting.empty(); };
        items.push_back(std::move(system));
        items.push_back(MenuItem::Separator());
        // 各语言的名称用它自己的语言写（不翻译）：看不懂当前界面语言的人也找得到自己的语言
        for (const I18n::LanguageInfo& lang : I18n::Translator::Instance().GetLanguages())
        {
            MenuItem item(lang.name, [this, code = lang.code] { m_platform->Post([this, code] { SetLanguage(code, true); }); });
            item.checkedIf = [this, code = lang.code] { return !m_languageSetting.empty() && I18n::NormalizeLanguage(m_languageSetting) == code; };
            items.push_back(std::move(item));
        }
        return items;
    }

    void MainFrame::SetLanguage(const std::string& code, bool save)
    {
        m_languageSetting = code;
        m_languageOverride.clear();
        const std::string used = I18n::Translator::Instance().SetLanguage(code.empty() ? m_platform->GetSystemLanguage() : code);
        LOG_INFO("语言：切换到 %s", used.c_str());
        if (save)
            SaveSettings();
        RetranslateUi();
    }

    void MainFrame::RetranslateUi()
    {
        // 界面描述生成的部分（标题栏、菜单、命令名称）整体重建；宿主面板里的文字由 UpdateStatus、RefreshPanels 按当前语言重写
        ReloadUi();
        RefreshPanels();
        BuildOptionsBar();
        if (m_startShown)
            BuildStartPage();
        m_ui->Update();
    }

    // =========================================================
    // 用户设置：<用户数据目录>/settings.json
    // =========================================================
    std::string MainFrame::SettingsPath() const
    {
        const std::string dir = m_platform->GetUserDataDir();
        return dir.empty() ? std::string() : dir + "/settings.json";
    }

    void MainFrame::LoadSettings()
    {
        const std::string path = SettingsPath();
        std::string text;
        if (path.empty() || !MiniGUI::ReadTextFile(path, text))
            return;
        MiniGUI::JsonValue doc;
        MiniGUI::JsonError err;
        if (!MiniGUI::ParseJson(text, doc, err))
        {
            LOG_WARN("用户设置 %s 格式错误，%s", path.c_str(), err.ToString().c_str());
            return;
        }
        m_languageSetting = doc["language"].AsString("");
        if (const std::string theme = doc["theme"].AsString(""); theme == "light" || theme == "dark")
            m_themeSetting = theme;
        const double minutes = doc["autosave_minutes"].AsNumber(m_autosave.minutes);
        if (minutes >= 0 && minutes <= 120)
            m_autosave.minutes = static_cast<int>(minutes);
    }

    void MainFrame::SaveSettings() const
    {
        const std::string path = SettingsPath();
        if (path.empty())
            return;
        std::error_code ec;
        std::filesystem::create_directories(ToPath(path).parent_path(), ec);
        MiniGUI::JsonValue doc = MiniGUI::JsonValue::MakeObject();
        doc.Set("language", m_languageSetting);
        doc.Set("theme", m_themeSetting);
        doc.Set("autosave_minutes", static_cast<double>(m_autosave.minutes));
        std::ofstream out(ToPath(path), std::ios::binary | std::ios::trunc);
        out << doc.Dump(2) << '\n';
        if (!out)
            LOG_WARN("用户设置：写不进 %s", path.c_str());
    }

    // =========================================================
    // 标题、状态栏、关于
    // =========================================================
    void MainFrame::UpdateTitle()
    {
        // 项目：保存过的显示文件名，否则显示项目名称
        std::string project = m_session.GetModel().name;
        if (const auto& path = m_session.Path())
            project = ToUtf8(ToPath(*path).filename());
        // 有没保存的改动时项目名前加 *
        if (!project.empty() && Dirty())
            project = "*" + project;
        const std::string title = (project.empty() ? std::string() : project + " - ") + "MiniBIM " MINIBIM_VERSION_STRING;
        m_platform->SetTitle(title);
        if (MiniGUI::TitleBar* bar = GetTitleBar())
            bar->SetTitle(title);
    }

    std::unique_ptr<MiniGUI::Node> MainFrame::CreateStatusBar()
    {
        using namespace MiniGUI;
        auto bar = std::make_unique<Panel>(Theme::PanelAlt);
        LayoutStyle s;
        s.direction  = FlexDirection::Row;
        s.alignItems = Align::Center;
        s.height     = 26.0f;
        s.padding    = Edges::Make(10.0f, 0.0f, 10.0f, 0.0f);
        s.gap        = 16.0f;
        bar->SetLayoutStyle(s);

        m_statusHint = bar->AddChild<Label>(std::string(), 12.0f, Theme::TextDim);
        LayoutStyle grow;
        grow.grow = 1.0f;
        m_statusHint->SetLayoutStyle(grow);
        m_statusInfo = bar->AddChild<Label>(std::string(), 12.0f, Theme::TextDim);
        return bar;
    }

    void MainFrame::UpdateStatus()
    {
        if (!m_statusHint || !m_platform)
            return;
        if (!m_uiError.empty())
            m_statusHint->SetText(Tr("Interface description file:") + " " + m_uiError);
        else if (!m_sceneError.empty())
            m_statusHint->SetText(Tr(m_sceneError));
        else if (m_planLevel || m_tool != Tool::Modify)
            m_statusHint->SetText(Tr(ToolPrompt()));
        else if (m_scene.Empty())
            m_statusHint->SetText(Tr("The project has nothing to show in 3D yet; open a project or the sample house from the File menu"));
        else
            m_statusHint->SetText(Tr("Drag to orbit, middle- or right-drag to pan, scroll to zoom"));

        const ModelScene::Counts& c = m_scene.GetCounts();
        std::string info;
        if (m_planLevel)
            info = PlanStatus() + "    ";
        else if (!m_scene.Empty())
        {
            // 列表的分隔符也随语言（中文用"，"）
            const std::string walls   = Format(TrN("{0} wall", "{0} walls", c.walls), { std::to_string(c.walls) });
            const std::string floors  = Format(TrN("{0} floor", "{0} floors", c.floors), { std::to_string(c.floors) });
            const std::string doors   = Format(TrN("{0} door", "{0} doors", c.doors), { std::to_string(c.doors) });
            const std::string windows = Format(TrN("{0} window", "{0} windows", c.windows), { std::to_string(c.windows) });
            info = Format(TrC("list of four items", "{0}, {1}, {2}, {3}"), { walls, floors, doors, windows }) + "    ";
        }
        info += Format(Tr("Graphics: {0}"), { m_platform->GetGraphicsName() });
        info += "    v" MINIBIM_VERSION_STRING;
        m_statusInfo->SetText(info);
    }

    void MainFrame::ShowAbout()
    {
        const std::string text =
            Format(Tr("MiniBIM {0}, an open-source BIM application written in C++20."), { MINIBIM_VERSION_STRING }) + "\n\n" +
            Tr("Released under the MIT License. Third-party components and their licenses are listed in THIRD_PARTY_NOTICES.md.") + "\n\n" +
            Format(Tr("Graphics: {0}"), { m_platform->GetGraphicsName() }) + "\n" +
            Format(Tr("Language: {0}"), { I18n::Translator::Instance().GetLanguage() });
        MiniGUI::ShowMessageBox(*m_ui, Tr("About MiniBIM"), text);
    }

    // =========================================================
    // 三维视口
    // =========================================================
    void MainFrame::RebuildScene()
    {
        m_sceneError.clear();
        m_sceneModel = m_session.Snapshot();
        if (!m_scene.Build(*m_sceneModel, m_platform->GetSolidKernel(), m_sceneError))
            LOG_WARN("三维场景：%s", m_sceneError.c_str());
        ++m_sceneVersion;
    }

    std::unique_ptr<MiniGUI::Node> MainFrame::CreateViewport()
    {
        auto vp = std::make_unique<MiniGUI::ViewportHost>();
        vp->SetOnRender ([this](int w, int h) { RenderViewport(w, h); });
        vp->SetOnPointer([this](const MiniGUI::ViewportPointerEvent& e) { OnViewportPointer(e); });
        vp->SetOnKey    ([this](const MiniGUI::KeyEvent& e) { return OnPlanKey(e); });
        vp->SetOnText   ([this](std::string_view text) { return OnPlanText(text); });
        // 平面上跟着光标的读数（长度与角度、面积、捕捉到什么）：画在视口上面，不挡点击
        m_planTipBox = vp->AddChild<MiniGUI::Panel>(MiniGUI::Theme::TooltipBackground);
        MiniGUI::LayoutStyle tip;
        tip.position = MiniGUI::PositionType::Absolute;
        tip.padding  = MiniGUI::Edges::Make(6.0f, 2.0f, 6.0f, 2.0f);
        m_planTipBox->SetLayoutStyle(tip);
        m_planTipBox->SetHitTestVisible(false);
        m_planTipBox->SetVisible(false);
        m_planTip = m_planTipBox->AddChild<MiniGUI::Label>(std::string(), 12.0f, MiniGUI::Theme::Text);
        m_planTip->SetHitTestVisible(false);
        m_viewport = vp.get();
        return vp;
    }

    void MainFrame::HomeView()
    {
        // 主视图是三维的
        if (m_planLevel)
            ShowModel3D();
        if (!m_scene.Empty())
            m_camera.HomeView(m_scene.GetBounds());
        m_viewport->RequestRender();
    }

    void MainFrame::ZoomToFit()
    {
        if (m_planLevel)
            m_planView.reset();
        else if (!m_scene.Empty())
            m_camera.ZoomToFit(m_scene.GetBounds());
        m_viewport->RequestRender();
    }

    void MainFrame::OnViewportPointer(const MiniGUI::ViewportPointerEvent& e)
    {
        using namespace MiniGUI;
        if (m_planLevel)
        {
            OnPlanPointer(e);
            return;
        }
        switch (e.type)
        {
        case PointerEventType::Down:
            if (m_drag == Drag::None)
            {
                const bool shift = (e.modifiers & static_cast<uint8_t>(ModifierKey::Shift)) != 0;
                m_drag = (e.button == MouseButton::Left || shift) ? Drag::Orbit : Drag::Pan;
                m_lastPixel = e.pixel;
                m_downPixel = e.pixel;
            }
            break;
        case PointerEventType::Up:
            // 左键没拖动：拾取射线最先碰到的元素（Ctrl 加进或拿出），同平面上的单击
            if (e.button == MouseButton::Left && std::hypot(e.pixel.x - m_downPixel.x, e.pixel.y - m_downPixel.y) <= 4.0f * m_dpiScale &&
                m_camera.GetViewportWidth() > 0 && m_camera.GetViewportHeight() > 0)
            {
                const double x = 2.0 * e.pixel.x / m_camera.GetViewportWidth() - 1.0, y = 1.0 - 2.0 * e.pixel.y / m_camera.GetViewportHeight();
                const Math::Point3 origin = Math::Mat4::Inverse(m_camera.GetViewProj()).TransformPoint({ x, y, 0.0 });
                const std::optional<Bim::Id> hit = m_scene.Pick(origin, m_camera.GetForward());
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
            if (m_drag != Drag::None)
            {
                const Vec2 d{ e.pixel.x - m_lastPixel.x, e.pixel.y - m_lastPixel.y };
                m_lastPixel = e.pixel;
                if (m_drag == Drag::Orbit)
                    m_camera.Orbit(d.x, d.y);
                else
                    m_camera.Pan(d.x, d.y);
                m_viewport->RequestRender();
            }
            break;
        case PointerEventType::Wheel:
            if (e.wheelDelta.y != 0.0f)
            {
                m_camera.ZoomAt(e.pixel.x, e.pixel.y, std::pow(0.85, static_cast<double>(e.wheelDelta.y)));
                m_viewport->RequestRender();
            }
            break;
        default:
            break;
        }
    }

    void MainFrame::RenderViewport(int pixelWidth, int pixelHeight)
    {
        IRenderTarget& target = m_platform->GetViewportTarget();
        if (target.GetWidth() != pixelWidth || target.GetHeight() != pixelHeight)
            target.Resize(pixelWidth, pixelHeight);
        m_camera.SetViewportSize(pixelWidth, pixelHeight);
        if (!m_cameraFitted && !m_scene.Empty())
        {
            m_camera.HomeView(m_scene.GetBounds());
            m_cameraFitted = true;
        }

        IRenderer& renderer = m_platform->GetRenderer();
        renderer.BeginFrame(target, ViewportDesc{ .width = static_cast<float>(pixelWidth), .height = static_cast<float>(pixelHeight) });

        // 背景：上浅下深的渐变，跟随界面主题（直接给裁剪空间坐标，单位矩阵）
        const bool  dark   = m_ui->GetTheme().IsDark();
        const VertexColor top    = dark ? VertexColor(0.24f, 0.26f, 0.29f, 1.0f) : VertexColor(0.96f, 0.97f, 0.98f, 1.0f);
        const VertexColor bottom = dark ? VertexColor(0.13f, 0.14f, 0.16f, 1.0f) : VertexColor(0.80f, 0.83f, 0.87f, 1.0f);
        const Vertex_P3_C4 bg[6] = {
            { { -1.0f,  1.0f, 0.5f }, top },    { { 1.0f,  1.0f, 0.5f }, top },    { { 1.0f, -1.0f, 0.5f }, bottom },
            { { -1.0f,  1.0f, 0.5f }, top },    { { 1.0f, -1.0f, 0.5f }, bottom }, { { -1.0f, -1.0f, 0.5f }, bottom },
        };
        renderer.Submit(bg, Math::Mat4::Identity(), PrimitiveType::Triangle, false);

        if (m_planLevel)
            RenderPlan(renderer, pixelWidth, pixelHeight);
        else if (!m_scene.Empty())
        {
            const Math::Mat4 viewProj = m_camera.GetViewProj();
            renderer.SubmitCached(0, m_sceneVersion, m_scene.GetTriangles(), viewProj, PrimitiveType::Triangle, true);

            // 边：沿视线往相机挪一个像素对应的长度（正交投影下屏幕位置不变），免得与所在的面争深度
            const Math::Vec3 toward = m_camera.GetForward() * (-std::max(0.5, m_camera.GetViewHeight() / pixelHeight));
            const VertexColor edge  = dark ? VertexColor(0.08f, 0.08f, 0.09f, 1.0f) : VertexColor(0.18f, 0.19f, 0.21f, 1.0f);
            m_edgeVerts.clear();
            m_edgeVerts.reserve(m_scene.GetEdges().size());
            for (const Math::Point3& p : m_scene.GetEdges())
            {
                const Math::Point3 q = p + toward;
                m_edgeVerts.push_back({ { static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z) }, edge });
            }
            renderer.Submit(m_edgeVerts, viewProj, PrimitiveType::Line, true);

            // 选中的元素：它的三角形再画一遍，强调色，同样往相机挪一点
            m_edgeVerts.clear();
            const VertexColor picked(0.0f, 0.47f, 0.84f, 1.0f);
            for (const Bim::Id id : m_selection)
            {
                const auto [first, last] = m_scene.RangeOf(id);
                for (size_t i = first; i < last; ++i)
                {
                    const auto& p = m_scene.GetTriangles()[i].pos;
                    const Math::Point3 q = Math::Point3{ p.x, p.y, p.z } + toward;
                    m_edgeVerts.push_back({ { static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z) }, picked });
                }
            }
            if (!m_edgeVerts.empty())
                renderer.Submit(m_edgeVerts, viewProj, PrimitiveType::Triangle, true);
        }
        renderer.EndFrame();

        // 渲染目标重建后着色器资源会变，重新登记
        void* srv = target.GetNativeShaderResource();
        if (srv != m_viewSRV)
        {
            if (m_viewTex != MiniGUI::InvalidTextureId)
                m_platform->GetUiBackend().DestroyTexture(m_viewTex);
            m_viewTex = m_platform->RegisterViewportTexture(srv);
            m_viewSRV = srv;
            m_viewport->SetTexture(m_viewTex);
        }
    }

    // =========================================================
    // 每帧
    // =========================================================
    void MainFrame::RenderFrame()
    {
        if (!m_ui)
            return;
        // 1. 布局 → 2. 按最新尺寸和相机渲染视口 → 3. 界面（含视口纹理）画到窗口 → 呈现
        m_ui->Update();
        if (m_viewport && m_viewport->IsVisible())
            m_viewport->RenderContent();
        m_platform->BeginUiPass();
        m_ui->Render();
        m_platform->EndUiPass();
    }
}
