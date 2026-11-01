#pragma once
#include <windows.h>
#include <map>
#include <memory>
#include <string>
#include "GUI/AppPlatform.h"
#include "GUI/MainFrame.h"
#include "IGraphicsDevice.h"
#include "Render/IRenderer.h"
#include "Solid/ISolidKernel.h"

namespace MiniGUI
{
    class Win32Input;
    class Win32Frame;
}

namespace MiniBIM
{
    // MiniBIM 桌面版（MiniBIMWin）的宿主，精简自 MiniCAD 的 Win32Window：无边框窗口 + 图形设备
    // （IGraphicsDevice：D3D12 / D3D11 / WARP）+ 消息循环，实现 AppPlatform。
    //   - 无边框窗口（Win32Frame）：标题栏由界面自绘，保留系统的贴靠、阴影、缩放边框
    //   - 按需重绘：界面需要重绘时 InvalidateRect，消息队列清空后在 WM_PAINT 里调用 MainFrame::RenderFrame
    //   - 监视界面描述文件所在目录，文件变化后让 MainFrame 重新加载
    // 浮动窗口、文件对话框等 MiniCAD 有的平台服务，用到时再从 MiniCAD 搬过来
    class Win32Window : public AppPlatform
    {
    public:
        Win32Window();
        ~Win32Window() override;

        Win32Window(const Win32Window&) = delete;
        Win32Window& operator=(const Win32Window&) = delete;

        MainFrame& GetFrame() { return m_main; }    // Initialize 之前可以设置界面描述文件、语言等选项
        HWND       GetHwnd() const { return m_hwnd; }

        // gpu：图形接口名称（d3d12、d3d11、warp），为空时按默认顺序
        bool Initialize(const wchar_t* title, int width, int height, const std::string& gpu = {});
        int  Run();

        // ── AppPlatform ─────────────────────────────────────────
        MiniGUI::IRenderBackend& GetUiBackend() override;
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
        bool IsWindowMaximized() const override;
        void MinimizeWindow() override;
        void ToggleMaximizeWindow() override;
        void CloseWindow() override;
        void Quit() override;

        std::string GetResourceDir() const override;
        std::string GetUserDataDir() const override;
        void PickFile(FileKind kind, std::function<void(const std::string&)> done) override;
        std::string ChooseSavePath(SaveKind kind, const std::string& suggestedName) override;
        std::string GetSystemLanguage() const override;
        bool OpenUrl(const std::string& url) override;
        void Post(std::function<void()> fn) override;
        void PostDelayed(unsigned ms, std::function<void()> fn) override;
        bool HasBackgroundThread() const override { return true; }

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT EventProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        bool InitWindow(const wchar_t* title, int width, int height);
        // 打开或保存的文件对话框：打开项目、保存项目、导出 IFC
        enum class FileDialog { Open, OpenRevit, SaveProject, SaveIfc, SavePat, Shortcuts, Pat, Dxf, PointCloud };
        std::string ShowProjectDialog(FileDialog kind, const std::string& suggestedName);
        void UpdateDisplaySize();
        void WatchUiFile();

    private:
        HWND   m_hwnd     = nullptr;
        float  m_dpiScale = 1.0f;
        HANDLE m_uiWatch  = nullptr;    // 界面描述文件所在目录的变化通知
        // PostDelayed 的系统定时器：ID → 到时执行的函数（ID 从 kFirstDelayTimer 起，避开 Win32Input 的）
        std::map<UINT_PTR, std::function<void()>> m_delayed;
        UINT_PTR m_nextDelayed = 0;

        // 声明顺序即构造顺序，销毁时相反：输入层 → 界面（MainFrame）→ 窗口框架 → 视口渲染目标 → 图形设备
        std::unique_ptr<IGraphicsDevice>       m_graphics;
        std::unique_ptr<IRenderTarget>         m_viewportRT;
        std::unique_ptr<MiniGUI::Win32Frame>   m_frame;
        MainFrame                              m_main;
        std::unique_ptr<MiniGUI::Win32Input>   m_input;
        std::unique_ptr<Solid::ISolidKernel>   m_solidKernel;   // MINIBIM_WITH_SOLID 时为 SMLib，第一次请求时创建
    };
}
