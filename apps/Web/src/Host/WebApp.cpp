// ── MiniBIM 网页版宿主：图形设备、浏览器文件选择与下载、页面关闭提示（复制自 MiniCAD 的 WebApp）─────────
#include "Host/WebApp.h"
#include "Core/Log.h"
#include "Core/UIContext.h"
#include "I18n/Translator.h"
#include "Platform/Web/WebFonts.h"
#include "Platform/Web/WebInput.h"
#include "Widgets/Dialog.h"
#ifdef MINIBIM_WITH_SOLID
#include "Solid/SMLib/SMLibKernel.h"
#endif
#include <emscripten.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace MiniBIM
{
    namespace
    {
        WebApp* g_app = nullptr;
    }

    // =========================================================
    // JavaScript：文件选择、下载、页面关闭提示
    // =========================================================
    EM_JS_DEPS(MiniBIMWebApp, "$stringToNewUTF8,$UTF8ToString,$FS");

    // 选一个文件读进内存文件系统 /upload/<请求号>/<文件名>，完成后回调路径；取消时路径为空串
    EM_JS(void, MiniBIMWeb_PickFile, (int requestId, const char* acceptPtr), {
        const input = document.createElement("input");
        input.type = "file";
        input.accept = UTF8ToString(acceptPtr);
        input.style.display = "none";
        let settled = false;
        const finish = (path, error) => {
            const p = stringToNewUTF8(path);
            const e = stringToNewUTF8(error || "");
            _MiniBIMWeb_OnFilePicked(requestId, p, e);
            _free(p);
            _free(e);
        };
        input.addEventListener("cancel", () => {
            if (settled) return;
            settled = true;
            input.remove();
            finish("", "");
        });
        input.addEventListener("change", async () => {
            if (settled) return;
            settled = true;
            const file = (input.files || [])[0];
            input.remove();
            if (!file) { finish("", ""); return; }
            try {
                const buf = await file.arrayBuffer();
                const directory = "/upload/" + requestId;
                FS.mkdirTree(directory);
                const path = directory + "/" + file.name.replace(/[\\/]/g, "_");
                FS.writeFile(path, new Uint8Array(buf));
                finish(path, "");
            } catch (err) {
                finish("", file.name + ": " + String(err));
            }
        });
        document.body.appendChild(input);
        input.click();
    });

    // 把虚拟文件系统里的文件下载到本机
    EM_JS(void, MiniBIMWeb_Download, (const char* pathPtr), {
        const path = UTF8ToString(pathPtr);
        let data;
        try { data = FS.readFile(path); } catch (e) { console.error("MiniBIM: 找不到要下载的文件 " + path); return; }
        const blob = new Blob([data], { type: "application/octet-stream" });
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = path.substring(path.lastIndexOf("/") + 1);
        document.body.appendChild(a);
        a.click();
        a.remove();
        setTimeout(() => URL.revokeObjectURL(url), 10000);
    });

    // 网址带 ?webgl（或 ?webgl=1）时强制用 WebGL2：排查问题、对比两个后端
    EM_JS(int, MiniBIMWeb_PreferWebGL, (), {
        try { return new URLSearchParams(location.search).has("webgl") ? 1 : 0; } catch (e) { return 0; }
    });

    // 浏览器的界面语言（BCP 47）；调用方 free
    EM_JS(char*, MiniBIMWeb_Language, (), {
        return stringToNewUTF8(navigator.language || "");
    });

    // 设备异步就绪时初始化失败：加载画面还在就显示原因（与 Module.onAbort 相同）
    EM_JS(void, MiniBIMWeb_ReportFailure, (const char* what), {
        if (Module["onAbort"]) Module["onAbort"](UTF8ToString(what));
    });

    EM_JS(void, MiniBIMWeb_SetTitle, (const char* title), {
        document.title = UTF8ToString(title);
    });

    // 有未保存的改动时，关闭或刷新页面前让浏览器询问
    EM_JS(void, MiniBIMWeb_InstallUnloadGuard, (), {
        window.addEventListener("beforeunload", (e) => {
            if (_MiniBIMWeb_HasUnsaved()) { e.preventDefault(); e.returnValue = ""; }
        });
    });

    extern "C"
    {
        EMSCRIPTEN_KEEPALIVE void MiniBIMWeb_OnFilePicked(int requestId, const char* path, const char* error)
        {
            if (g_app)
                g_app->OnFilePicked(requestId, path, error);
        }

        EMSCRIPTEN_KEEPALIVE int MiniBIMWeb_HasUnsaved()
        {
            return g_app && g_app->HasUnsavedChanges() ? 1 : 0;
        }
    }

    // =========================================================
    // 初始化
    // =========================================================
    WebApp::WebApp()
    {
        g_app = this;
    }

    WebApp::~WebApp()
    {
        if (g_app == this)
            g_app = nullptr;
    }

    bool WebApp::Initialize()
    {
        if (!m_host.Initialize())
            return false;
        if (MiniBIMWeb_PreferWebGL())
        {
            LOG_INFO("MiniBIM 网页版：网址带 ?webgl，使用 WebGL2");
            return InitializeWebGL();
        }

        // 先试 WebGPU：取适配器、设备都是异步的，回调前不建任何上下文、不画帧
        const NativeWindow window{ const_cast<char*>(m_host.GetCanvasSelector().c_str()) };
        CreateWebGPUGraphicsDeviceAsync(window, m_host.GetFramebufferWidth(), m_host.GetFramebufferHeight(),
            [this](std::unique_ptr<IGraphicsDevice> device)
            {
                bool ok = false;
                if (device)
                {
                    m_graphics = std::move(device);
                    ok = OnGraphicsReady();
                }
                else
                {
                    LOG_INFO("MiniBIM 网页版：WebGPU 不可用，改用 WebGL2");
                    ok = InitializeWebGL();
                }
                if (!ok)
                    ReportFailure("初始化失败（浏览器需要支持 WebGPU 或 WebGL2）");
            });
        return true;
    }

    bool WebApp::InitializeWebGL()
    {
        if (!m_host.CreateWebGLContext())
            return false;
        NativeWindow window{ const_cast<char*>(m_host.GetCanvasSelector().c_str()) };
        window.glContext = true;        // WebHost 刚建好 WebGL2 上下文并设为当前
        m_graphics = CreateGraphicsDevice(window, m_host.GetFramebufferWidth(), m_host.GetFramebufferHeight(), "webgl2");
        return m_graphics && OnGraphicsReady();
    }

    bool WebApp::OnGraphicsReady()
    {
        LOG_INFO("MiniBIM 网页版：图形接口 %s", m_graphics->GetName());
        // 异步等待期间画布尺寸可能变了：按当前尺寸
        m_graphics->Resize(m_host.GetFramebufferWidth(), m_host.GetFramebufferHeight());
        m_viewportRT = m_graphics->CreateRenderTarget();
        m_viewportRT->Create(m_host.GetFramebufferWidth(), m_host.GetFramebufferHeight());

        if (!m_main.Initialize(*this, m_host.GetLogicalWidth(), m_host.GetLogicalHeight(), m_host.GetPixelScale()))
            return false;
        m_input = std::make_unique<MiniGUI::WebInput>(&m_main.GetUI(), m_host.GetCanvasSelector().c_str());

        m_host.SetOnResize([this](float w, float h, float scale)
        {
            m_graphics->Resize(m_host.GetFramebufferWidth(), m_host.GetFramebufferHeight());
            m_main.OnResize(w, h, scale);
        });
        m_host.SetOnFrame([this] { m_main.RenderFrame(); });
        MiniBIMWeb_InstallUnloadGuard();
        m_host.RequestFrame();
        return true;
    }

    void WebApp::ReportFailure(const char* what)
    {
        std::printf("MiniBIM：%s\n", what);
        MiniBIMWeb_ReportFailure(what);
    }

    bool WebApp::HasUnsavedChanges() const
    {
        return m_main.HasUnsavedChanges();
    }

    // =========================================================
    // AppPlatform：图形
    // =========================================================
    MiniGUI::IRenderBackend& WebApp::GetUiBackend() { return m_graphics->GetUiBackend(); }
    IRenderer&               WebApp::GetRenderer()  { return m_graphics->GetRenderer(); }
    IRenderTarget&           WebApp::GetViewportTarget() { return *m_viewportRT; }

    MiniGUI::TextureId WebApp::RegisterViewportTexture(void* nativeShaderResource)
    {
        return m_graphics->RegisterViewportTexture(nativeShaderResource);
    }

    Solid::ISolidKernel* WebApp::GetSolidKernel()
    {
#ifdef MINIBIM_WITH_SOLID
        // 与桌面版相同：第一次请求时创建，之后一直持有。网页版没有后台线程，调用天然是串行的
        if (!m_solidKernel)
            m_solidKernel = Solid::CreateSMLibKernel();
#endif
        return m_solidKernel.get();
    }

    bool WebApp::LoadUiFonts(MiniGUI::TextSystem& text)
    {
        return MiniGUI::LoadWebUIFonts(text);   // 打包进来的 /fonts/NotoSansSC-UI.ttf
    }

    void WebApp::BeginUiPass()
    {
        m_graphics->BeginUiPass();
    }

    void WebApp::EndUiPass()
    {
        m_graphics->Present();      // 浏览器在本次回调返回后自动呈现画布，这里什么也不做
        if (m_input)
            m_input->Sync();        // 文字焦点变化时切换 textarea，候选窗跟随光标
    }

    // =========================================================
    // AppPlatform：窗口
    // =========================================================
    void WebApp::SetTitle(const std::string& utf8)
    {
        MiniBIMWeb_SetTitle(utf8.c_str());
    }

    void WebApp::CloseWindow()
    {
        m_main.RequestExit();
    }

    void WebApp::Quit()
    {
        // 网页不能自己关闭标签页：未保存的改动已经处理完，什么也不做
        LOG_INFO("MiniBIM 网页版：关闭浏览器标签页即可退出");
    }

    // =========================================================
    // AppPlatform：文件
    // =========================================================
    void WebApp::PickFile(FileKind kind, std::function<void(const std::string& path)> done)
    {
        const int id = m_nextPickId++;
        m_pickRequests[id] = std::move(done);
        MiniBIMWeb_PickFile(id, kind == FileKind::Shortcuts ? ".json,.xml,.txt,.md" : ".minibim,.bimcraft,.ifc");
    }

    void WebApp::OnFilePicked(int requestId, const char* path, const char* error)
    {
        auto it = m_pickRequests.find(requestId);
        if (it == m_pickRequests.end())
            return;
        auto done = std::move(it->second);
        m_pickRequests.erase(it);
        if (error && *error)
            MiniGUI::ShowMessageBox(m_main.GetUI(), Tr("Could not read the file"), error, { Tr("OK") }, [](int) {});
        else if (done && path && *path)
            done(path);
        m_host.RequestFrame();
    }

    std::string WebApp::ChooseSavePath(SaveKind, const std::string& suggestedName)
    {
        // 浏览器没有"另存为"对话框：MainFrame 已经问过文件名（带扩展名），写到内存文件系统，写完（OnDocumentSaved）下载
        std::string name = suggestedName.empty() ? std::string("Project.minibim") : suggestedName;
        std::replace(name.begin(), name.end(), '/', '_');
        EM_ASM({ try { FS.mkdir("/download"); } catch (e) {} });
        return "/download/" + name;
    }

    void WebApp::OnDocumentSaved(const std::string& path)
    {
        MiniBIMWeb_Download(path.c_str());
    }

    // =========================================================
    // AppPlatform：系统
    // =========================================================
    std::string WebApp::GetSystemLanguage() const
    {
        char* lang = MiniBIMWeb_Language();
        std::string code = lang ? lang : "";
        std::free(lang);
        return code;
    }

    bool WebApp::OpenUrl(const std::string& url)
    {
        if (url.empty())
            return false;
        // 在用户点击的回调里调用，浏览器不会把它当作弹出窗口拦截
        EM_ASM({
            window.open(UTF8ToString($0), "_blank", "noopener,noreferrer");
        }, url.c_str());
        return true;
    }

    void WebApp::Post(std::function<void()> fn)
    {
        PostDelayed(0, std::move(fn));
    }

    void WebApp::PostDelayed(unsigned ms, std::function<void()> fn)
    {
        auto* task = new std::function<void()>(std::move(fn));
        emscripten_async_call([](void* p)
        {
            std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()>*>(p));
            (*f)();
        }, task, static_cast<int>(ms));
    }
}
