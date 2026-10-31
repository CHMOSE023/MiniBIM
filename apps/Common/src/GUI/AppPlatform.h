#pragma once
#include "Render/IRenderBackend.h"
#include "Render/IRenderTarget.h"
#include <functional>
#include <string>
#include <vector>

namespace MiniGUI
{
    class TextSystem;
}

namespace MiniBIM
{
    class IRenderer;
    class IRenderTarget;
    namespace Solid { class ISolidKernel; }

    // 选来读的文件（决定筛选的扩展名）
    enum class FileKind
    {
        Project,    // .minibim / .bimcraft / .ifc
        Shortcuts,  // 快捷键文件：.json / .xml（Revit 导出的）/ .txt / .md
    };

    // 要写的文件
    enum class SaveKind
    {
        Project,    // .minibim
        Ifc,        // .ifc（导出 IFC4）
    };

    // 主窗口（MainFrame）依赖的平台服务，结构同 MiniCAD 的 AppPlatform。Win32 版（apps/Win32/src/Host/Win32Window）、
    // Linux 版与网页版各实现一份，MainFrame 本身不含平台代码。
    // 需要新的平台能力时在这里加虚函数，不要在 apps/Common 里写 #ifdef
    class AppPlatform
    {
    public:
        virtual ~AppPlatform() = default;

        // ── 图形 ─────────────────────────────────────────────────
        virtual MiniGUI::IRenderBackend& GetUiBackend()      = 0;   // 界面渲染后端（D3D12Backend / D3D11Backend）
        virtual IRenderer&               GetRenderer()       = 0;   // 三维视口渲染器
        virtual IRenderTarget&           GetViewportTarget() = 0;   // 主窗口视口的离屏渲染目标
        // 把视口渲染目标的着色器资源（IRenderTarget::GetNativeShaderResource）登记为界面纹理
        virtual MiniGUI::TextureId RegisterViewportTexture(void* nativeShaderResource) = 0;
        virtual bool LoadUiFonts(MiniGUI::TextSystem& text) = 0;
        virtual void RequestRedraw() = 0;       // 安排一次重绘（Win32：InvalidateRect）
        virtual void BeginUiPass()   = 0;       // 绑定窗口的帧缓冲并清屏
        virtual void EndUiPass()     = 0;       // 呈现，并同步输入法位置等平台输入状态
        virtual const char* GetGraphicsName() const = 0;    // 关于对话框显示，例如 "D3D12"

        // ── 窗口 ─────────────────────────────────────────────────
        virtual void SetTitle(const std::string& utf8) = 0;
        virtual bool IsWindowMaximized() const { return false; }
        virtual void MinimizeWindow()       {}
        virtual void ToggleMaximizeWindow() {}
        virtual void CloseWindow() = 0;         // 相当于点了关闭按钮：由 MainFrame::RequestExit 决定是否退出
        virtual void Quit()        = 0;         // 确认后退出

        // ── 文件与目录 ───────────────────────────────────────────
        virtual std::string GetResourceDir() const = 0;     // ui/ i18n/ 所在目录（UTF-8）
        virtual std::string GetUserDataDir() const = 0;     // 用户设置（settings.json）的目录（UTF-8）；空串 = 不保存
        // 选一个文件来读；done 收到 UTF-8 路径（取消时不调用）。选择可以是异步的（Linux 的门户对话框、网页版的 <input>），
        // done 在界面线程上调用；平台没有文件对话框时什么都不做
        virtual void PickFile(FileKind kind, std::function<void(const std::string& path)> done) { (void)kind; (void)done; }
        // 存到哪里：返回 UTF-8 路径，空串 = 取消。
        // 有系统对话框时（HasSaveDialog）弹出它，suggestedName 为默认文件名（带扩展名）；
        // 没有时 MainFrame 先用自己的对话框问文件名，再把带扩展名的文件名传进来，平台直接给出路径（网页版：内存文件系统里）
        virtual std::string ChooseSavePath(SaveKind kind, const std::string& suggestedName) { (void)kind; (void)suggestedName; return {}; }
        virtual bool HasSaveDialog() const { return true; }
        // 写完一个文件（保存、另存为、导出 IFC）之后：网页版把它下载到本机；桌面版什么都不做
        virtual void OnDocumentSaved(const std::string& path) { (void)path; }

        // ── 系统 ─────────────────────────────────────────────────
        // 系统界面语言（BCP 47，如 "zh-CN"、"en-US"）；取不到时返回空串
        virtual std::string GetSystemLanguage() const { return {}; }
        // 用系统默认程序打开链接；平台不支持时返回 false
        virtual bool OpenUrl(const std::string& url) { (void)url; return false; }
        // 在界面线程上稍后执行 fn：当前消息（包括正在进行的绘制）处理完之后
        virtual void Post(std::function<void()> fn) { fn(); }
        // ms 毫秒以后在界面线程上执行 fn（自动保存的定时）；平台没有定时器时不执行
        virtual void PostDelayed(unsigned ms, std::function<void()> fn) { (void)ms; (void)fn; }
        // 能开后台线程（桌面版）。为 false 时（网页版没有 pthread）不开自动保存：它在工作线程上写文件
        virtual bool HasBackgroundThread() const { return false; }

        // ── 三维实体内核（SMLib）─────────────────────────────────
        // 构建时关掉内核（MINIBIM_WITH_SOLID=OFF）返回 nullptr
        virtual Solid::ISolidKernel* GetSolidKernel() { return nullptr; }
    };
}
