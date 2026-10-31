#pragma once
#include "GUI/AppPlatform.h"
#include "GUI/MainFrame.h"
#include "Host/WebHost.h"
#include "IGraphicsDevice.h"
#include "Render/IRenderer.h"
#include "Solid/ISolidKernel.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace MiniGUI
{
    class WebInput;
}

namespace MiniBIM
{
    // MiniBIM 网页版的宿主，复制自 MiniCAD 的 WebApp：画布（WebHost）+ 图形设备（IGraphicsDevice：WebGPU，不支持时 WebGL2）
    // + 浏览器事件，实现 AppPlatform，界面与桌面版共用 MainFrame。
    //   - 图形接口：先异步试 WebGPU（浏览器有 navigator.gpu 且取到适配器和设备），否则或网址带 ?webgl 时用 WebGL2；
    //     同一个画布只能有一种上下文，所以确定不用 WebGPU 后才建 WebGL2 上下文。设备就绪之前不建界面、不画帧
    //   - 打开：浏览器的文件选择框，选中的文件读进内存文件系统 /upload 后打开
    //   - 保存、导出 IFC：MainFrame 先问文件名（HasSaveDialog 为 false），写到内存文件系统 /download 后由浏览器下载到本机
    //   - 没有线程（不开自动保存）、不保存用户设置与最近打开（GetUserDataDir 为空）
    //   - 资源（界面描述文件、词典、图标、字体）打包在虚拟文件系统根目录下
    //   - 有未保存的改动时，关闭或刷新页面前浏览器会询问
    class WebApp : public AppPlatform
    {
    public:
        WebApp();
        ~WebApp() override;

        WebApp(const WebApp&) = delete;
        WebApp& operator=(const WebApp&) = delete;

        bool Initialize();          // 开始创建图形设备（可能是异步的），设备就绪后才初始化界面
        MainFrame& GetFrame() { return m_main; }

        // 由 JavaScript 回调（内部使用）
        void OnFilePicked(int requestId, const char* path, const char* error);
        bool HasUnsavedChanges() const;

        // ── AppPlatform ─────────────────────────────────────────
        MiniGUI::IRenderBackend& GetUiBackend() override;
        IRenderer&               GetRenderer() override;
        IRenderTarget&           GetViewportTarget() override;
        MiniGUI::TextureId RegisterViewportTexture(void* nativeShaderResource) override;
        bool LoadUiFonts(MiniGUI::TextSystem& text) override;
        void RequestRedraw() override { m_host.RequestFrame(); }
        void BeginUiPass() override;
        void EndUiPass() override;
        const char* GetGraphicsName() const override { return m_graphics ? m_graphics->GetName() : ""; }
        Solid::ISolidKernel* GetSolidKernel() override;   // MINIBIM_WITH_SOLID 时为 SMLib，第一次请求时创建

        void SetTitle(const std::string& utf8) override;
        bool HasWindowControls() const override { return false; }
        void CloseWindow() override;
        void Quit() override;

        std::string GetResourceDir() const override { return ""; }     // 资源在虚拟文件系统根目录：/ui、/i18n、/icons、/fonts
        std::string GetUserDataDir() const override { return ""; }     // 不保存用户设置与最近打开
        void PickFile(FileKind kind, std::function<void(const std::string& path)> done) override;
        std::string ChooseSavePath(SaveKind kind, const std::string& suggestedName) override;
        bool HasSaveDialog() const override { return false; }          // 文件名由 MainFrame 的对话框询问
        void OnDocumentSaved(const std::string& path) override;

        std::string GetSystemLanguage() const override;
        bool OpenUrl(const std::string& url) override;
        void Post(std::function<void()> fn) override;
        void PostDelayed(unsigned ms, std::function<void()> fn) override;

    private:
        bool InitializeWebGL();     // 建 WebGL2 上下文与 GLES3 图形设备
        bool OnGraphicsReady();     // 图形设备就绪：建视口渲染目标、界面、输入，开始画帧
        void ReportFailure(const char* what);

    private:
        // 声明顺序即构造顺序，销毁时相反：输入层 → 界面（MainFrame）→ 内核 → 视口渲染目标 → 图形设备 → 画布（WebGL 上下文）
        WebHost                                 m_host;
        std::unique_ptr<IGraphicsDevice>        m_graphics;
        std::unique_ptr<IRenderTarget>          m_viewportRT;
        std::unique_ptr<Solid::ISolidKernel>    m_solidKernel;   // 声明在 MainFrame 之前，销毁时晚于它
        MainFrame                               m_main;
        std::unique_ptr<MiniGUI::WebInput>      m_input;

        std::unordered_map<int, std::function<void(const std::string&)>> m_pickRequests;
        int m_nextPickId = 1;
    };
}
