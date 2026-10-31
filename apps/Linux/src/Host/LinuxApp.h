#pragma once
#include "GUI/AppPlatform.h"
#include "GUI/MainFrame.h"
#include "IGraphicsDevice.h"
#include "Render/IRenderer.h"
#include "Solid/ISolidKernel.h"
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace MiniGUI
{
    class SDLInput;
}

namespace MiniBIM
{
    // MiniBIM Linux 版（MiniBIMLinux）的宿主，复制自 MiniCAD 的 LinuxApp：SDL3 窗口 + 图形设备
    // （IGraphicsDevice：默认 Vulkan，不行时 EGL / OpenGL ES 3）+ 事件循环，实现 AppPlatform，界面与桌面版共用 MainFrame。
    //   - 窗口用系统的标题栏和边框（HasWindowControls 为 false），X11 与 Wayland 由 SDL3 统一处理
    //   - 按需重绘：RequestRedraw 记下标志并推一个唤醒事件，事件队列清空后调用 MainFrame::RenderFrame；空闲时阻塞在 SDL_WaitEvent
    //   - Post、PostDelayed（SDL 定时器）与对话框结果经 SDL 的自定义事件交回界面线程
    //   - 文件对话框用 SDL3 的对话框接口（xdg-desktop-portal，没有时 zenity）；保存对话框在界面线程上等待结果
    class LinuxApp : public AppPlatform
    {
    public:
        LinuxApp();
        ~LinuxApp() override;

        LinuxApp(const LinuxApp&) = delete;
        LinuxApp& operator=(const LinuxApp&) = delete;

        MainFrame& GetFrame() { return m_main; }    // Initialize 之前可以设置界面描述文件、语言等选项

        // gpu：图形后端偏好（--gpu / MINIBIM_GPU：vulkan、gl），空串 = 默认
        bool Initialize(const char* title, int width, int height, const std::string& gpu);
        int  Run();

        // ── AppPlatform ─────────────────────────────────────────
        MiniGUI::IRenderBackend& GetUiBackend() override      { return m_graphics->GetUiBackend(); }
        IRenderer&               GetRenderer() override       { return m_graphics->GetRenderer(); }
        IRenderTarget&           GetViewportTarget() override { return *m_viewportRT; }
        MiniGUI::TextureId RegisterViewportTexture(void* nativeShaderResource) override;
        bool LoadUiFonts(MiniGUI::TextSystem& text) override;
        void RequestRedraw() override;
        void BeginUiPass() override;
        void EndUiPass() override;
        const char* GetGraphicsName() const override { return m_graphics->GetName(); }
        Solid::ISolidKernel* GetSolidKernel() override;

        void SetTitle(const std::string& utf8) override;
        bool HasWindowControls() const override { return false; }      // 标题栏和窗口按钮由系统（窗口管理器）提供
        bool IsWindowMaximized() const override;
        void MinimizeWindow() override;
        void ToggleMaximizeWindow() override;
        void CloseWindow() override;
        void Quit() override;

        std::string GetResourceDir() const override;
        std::string GetUserDataDir() const override;
        void PickFile(FileKind kind, std::function<void(const std::string& path)> done) override;
        std::string ChooseSavePath(SaveKind kind, const std::string& suggestedName) override;

        std::string GetSystemLanguage() const override;
        bool OpenUrl(const std::string& url) override;
        void Post(std::function<void()> fn) override;
        void PostDelayed(unsigned ms, std::function<void()> fn) override;
        bool HasBackgroundThread() const override { return true; }

    private:
        // SDL 窗口与 GL 上下文（用 Vulkan 时没有）：声明在最前，最后销毁（图形设备、渲染目标销毁时窗口与上下文还在）
        struct SdlWindow
        {
            SDL_Window*   window  = nullptr;
            SDL_GLContext context = nullptr;
            ~SdlWindow();
        };

        // api 为 SDL_WINDOW_VULKAN 或 SDL_WINDOW_OPENGL；窗口先隐藏，初始化完再显示
        bool  CreateSdlWindow(const char* title, int width, int height, SDL_WindowFlags api);
        bool  CreateGlContext();
        NativeWindow VulkanWindow() const;      // 实例扩展与建表面的回调都由 SDL3 提供
        int   PixelWidth() const;
        int   PixelHeight() const;
        void  HandleEvent(const SDL_Event& e);
        void  UpdateDisplaySize();
        float DisplayScale() const;     // 界面缩放：系统的显示缩放（SDL_GetWindowDisplayScale）
        void  ShowDialogError(const char* what);

    private:
        SdlWindow   m_sdl;
        uint32_t    m_uiEvent = 0;      // SDL_RegisterEvents：data1 为 new 出来的 std::function<void()>（可为空，只唤醒）
        std::atomic<bool> m_redraw{ true };
        bool        m_running = false;

        // 声明顺序即构造顺序，销毁时相反：输入层 → 界面（MainFrame）→ 内核 → 视口渲染目标 → 图形设备 → 窗口与 GL 上下文
        std::unique_ptr<IGraphicsDevice>       m_graphics;
        std::unique_ptr<IRenderTarget>         m_viewportRT;
        std::unique_ptr<Solid::ISolidKernel>   m_solidKernel;   // MINIBIM_WITH_SOLID 时为 SMLib，第一次请求时创建
        MainFrame                              m_main;
        std::unique_ptr<MiniGUI::SDLInput>     m_input;
    };
}
