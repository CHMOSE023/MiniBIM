#pragma once
#include "GUI/AppPlatform.h"
#include "GUI/DemoScene.h"
#include "View/OrbitCamera.h"
#include "Data/CommandRegistry.h"
#include "Render/IRenderBackend.h"
#include "Widgets/Menu.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace MiniGUI
{
    class UIContext;
    class Label;
    class Node;
    class TitleBar;
    class UiLayout;
    class ViewportHost;
    struct ViewportPointerEvent;
}

namespace MiniBIM
{
    // MiniBIM 主窗口的界面与逻辑（与平台无关，平台服务经 AppPlatform）。结构沿用 MiniCAD 的 MainFrame：
    //   - 整个界面由 MiniGUI 绘制：标题栏自绘，菜单栏在标题栏里；菜单与布局由界面描述文件 ui/minibim_ui.json 描述（UiLayout），
    //     保存后自动重新加载
    //   - 三维视口（面板 "viewport"）：OrbitCamera，拖动左键环绕、拖动中键或右键平移、Shift + 拖动环绕、滚轮缩放
    //   - 状态栏（面板 "statusbar"）：操作提示、模型统计、语言、图形接口、版本
    //   - 多语言：界面文字经 MiniGUI::Tr / MiniBIM::Tr 翻译（I18n/Translator.h），"文件 > 语言"切换后整个界面按新语言重建；
    //     选择保存在 AppPlatform::GetUserDataDir() 下的 settings.json，没有保存过时跟随系统语言
    //
    // 帧顺序（同 MiniCAD）：输入到达时立即更新相机并请求重绘 → 宿主调用 RenderFrame：
    // UIContext::Update 布局 → ViewportHost::RenderContent 按最新尺寸和相机渲染视口 → UIContext::Render 画界面 → 呈现
    class MainFrame
    {
    public:
        MainFrame();
        ~MainFrame();

        // 界面描述文件（UTF-8 路径）；不设置时优先用源码目录里的文件，其次是可执行文件旁的 ui/minibim_ui.json
        void SetUiFile(std::string utf8Path) { m_uiPath = std::move(utf8Path); }
        // 启动参数指定的语言（--lang），优先于用户设置，不保存
        void SetLanguageOverride(std::string code) { m_languageOverride = std::move(code); }

        // 平台对象必须比 MainFrame 活得久。尺寸为逻辑像素，pixelScale = 物理像素 / 逻辑像素
        bool Initialize(AppPlatform& platform, float width, float height, float pixelScale);

        // ── 由宿主调用 ───────────────────────────────────────────
        void OnResize(float width, float height, float pixelScale);
        void RenderFrame();
        void RequestExit();
        void CheckUiFileChanged();              // 界面描述文件变了就重新加载（宿主监视文件所在目录）

        const std::string&  GetUiPath() const { return m_uiPath; }
        MiniGUI::UIContext& GetUI() const { return *m_ui; }
        MiniGUI::TitleBar*  GetTitleBar() const;

        // 语言：code 为空 = 跟随系统。save 时写进用户设置
        void               SetLanguage(const std::string& code, bool save);
        const std::string& GetLanguageSetting() const { return m_languageSetting; }

    private:
        void InitUI();
        void RegisterCommands();                                  // MainCommands.cpp
        std::unique_ptr<MiniGUI::Node> CreateViewport();
        std::unique_ptr<MiniGUI::Node> CreateStatusBar();
        std::vector<MiniGUI::MenuItem> BuildLanguageMenu();
        void ReloadUi();
        void RetranslateUi();                                     // 切换语言后：重建界面描述生成的部分，刷新宿主自己的文字
        void UpdateTitle();
        void UpdateStatus();
        void ShowAbout();
        void BuildDemoScene();

        void RenderViewport(int pixelWidth, int pixelHeight);
        void OnViewportPointer(const MiniGUI::ViewportPointerEvent& e);
        void HomeView();
        void ZoomToFit();

        void LoadSettings();
        void SaveSettings() const;
        std::string SettingsPath() const;

        AppPlatform* m_platform = nullptr;
        float        m_dpiScale = 1.0f;

        std::unique_ptr<MiniGUI::UIContext> m_ui;
        MiniGUI::CommandRegistry            m_commands;
        std::unique_ptr<MiniGUI::UiLayout>  m_layout;
        MiniGUI::Node*                      m_uiHost     = nullptr;
        MiniGUI::ViewportHost*              m_viewport   = nullptr;
        MiniGUI::Label*                     m_statusHint = nullptr;
        MiniGUI::Label*                     m_statusInfo = nullptr;

        std::string                     m_uiPath;
        std::filesystem::file_time_type m_uiFileTime{};
        std::string                     m_uiError;            // 界面描述文件读不出或写错时的原因（状态栏显示）

        std::string m_languageSetting;                        // 用户选的语言，空 = 跟随系统
        std::string m_languageOverride;

        OrbitCamera m_camera;
        bool        m_cameraFitted = false;
        DemoScene   m_scene;
        std::string m_sceneError;
        std::vector<Vertex_P3_C4> m_edgeVerts;                // 每帧按视线方向把边往相机挪一点再画（免得与面争深度）

        MiniGUI::TextureId m_viewTex = MiniGUI::InvalidTextureId;
        void*              m_viewSRV = nullptr;

        enum class Drag { None, Orbit, Pan };
        Drag         m_drag = Drag::None;
        MiniGUI::Vec2 m_lastPixel;
    };
}
