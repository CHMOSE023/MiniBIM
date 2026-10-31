// ── MiniBIM Linux 版宿主：SDL3 窗口、图形设备（Vulkan 或 EGL / GLES3）、事件循环与平台服务（复制自 MiniCAD 的 LinuxApp）──
#include "Host/LinuxApp.h"
#ifdef MINIBIM_WITH_SOLID
#include "Solid/SMLib/SMLibKernel.h"
#endif
#include "Core/Log.h"
#include "Core/UIContext.h"
#include "I18n/Translator.h"
#include "Paint/Image.h"
#include "Platform/SDL/SDLFonts.h"
#include "Platform/SDL/SDLInput.h"
#include "Widgets/Dialog.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace MiniBIM
{
    namespace
    {
        // 推一个界面线程事件；失败（SDL 已退出、队列满）时就地释放，不再调用
        void PushUiEvent(uint32_t type, std::function<void()>* fn)
        {
            SDL_Event e{};
            e.type       = type;
            e.user.data1 = fn;
            if (!SDL_PushEvent(&e))
                delete fn;
        }

        std::string GetEnv(const char* name)
        {
            const char* v = std::getenv(name);
            return v ? v : "";
        }

        // ── 文件对话框（SDL3：xdg-desktop-portal，没有时 zenity）────────────
        // 筛选器的文字随界面语言；SDL 要求筛选器一直有效到回调，所以和请求放在一起
        struct DialogFilters
        {
            std::vector<std::string>          labels;
            std::vector<std::string>          patterns;
            std::vector<SDL_DialogFileFilter> items;

            void Add(std::string label, std::string pattern)
            {
                labels.push_back(std::move(label));
                patterns.push_back(std::move(pattern));
                items.clear();
                for (size_t i = 0; i < labels.size(); ++i)
                    items.push_back({ labels[i].c_str(), patterns[i].c_str() });
            }
            const SDL_DialogFileFilter* Data() const { return items.data(); }
            int Count() const { return static_cast<int>(items.size()); }
        };

        void FillOpenFilters(FileKind kind, DialogFilters& f)
        {
            if (kind == FileKind::Shortcuts)
                f.Add(Tr("Shortcuts file") + " (*.json;*.xml;*.txt;*.md)", "json;xml;txt;md");
            else if (kind == FileKind::ProjectOrRevit)
            {
                f.Add(Tr("All projects") + " (*.minibim;*.bimcraft;*.ifc;*.rvt)", "minibim;bimcraft;ifc;rvt");
                f.Add(Tr("MiniBIM project") + " (*.minibim)", "minibim");
                f.Add(Tr("BIMCraft project") + " (*.bimcraft)", "bimcraft");
                f.Add(Tr("IFC file") + " (*.ifc)", "ifc");
                f.Add(Tr("Revit project") + " (*.rvt)", "rvt");
            }
            else
            {
                f.Add(Tr("All projects") + " (*.minibim;*.bimcraft;*.ifc)", "minibim;bimcraft;ifc");
                f.Add(Tr("MiniBIM project") + " (*.minibim)", "minibim");
                f.Add(Tr("BIMCraft project") + " (*.bimcraft)", "bimcraft");
                f.Add(Tr("IFC file") + " (*.ifc)", "ifc");
            }
            f.Add(Tr("All files") + " (*.*)", "*");
        }

        // 对话框结果：paths 为空 = 取消；error 不为空 = 对话框打不开（没有 portal / zenity 等）
        using DialogResult = std::function<void(std::vector<std::string> paths, int filter, std::string error)>;

        struct DialogRequest
        {
            uint32_t      uiEvent = 0;
            DialogResult  result;
            DialogFilters filters;
        };

        // SDL 的回调可能在其他线程上：整理好结果后交回界面线程
        void SDLCALL OnDialogDone(void* userdata, const char* const* filelist, int filter)
        {
            std::unique_ptr<DialogRequest> request(static_cast<DialogRequest*>(userdata));
            std::vector<std::string> paths;
            std::string error;
            if (!filelist)
            {
                const char* e = SDL_GetError();
                error = e && *e ? e : "未知错误";
            }
            else
            {
                for (const char* const* p = filelist; *p; ++p)
                    paths.emplace_back(*p);
            }
            PushUiEvent(request->uiEvent, new std::function<void()>(
                [result = std::move(request->result), paths = std::move(paths), filter, error = std::move(error)]() mutable
                {
                    result(std::move(paths), filter, std::move(error));
                }));
        }

        // 同步等待的对话框（存到哪里：AppPlatform 要求当场返回路径）
        struct SyncDialog
        {
            std::atomic<bool> done{ false };
            std::string       path;
            int               filter = -1;
            std::string       error;
        };

        void SDLCALL OnSyncDialogDone(void* userdata, const char* const* filelist, int filter)
        {
            auto* d = static_cast<SyncDialog*>(userdata);
            if (!filelist)
            {
                const char* e = SDL_GetError();
                d->error = e && *e ? e : "未知错误";
            }
            else if (*filelist)
            {
                d->path = *filelist;
            }
            d->filter = filter;
            d->done   = true;
        }

        bool HasDot(const std::string& path)
        {
            const size_t slash = path.find_last_of('/');
            return path.find('.', slash == std::string::npos ? 0 : slash + 1) != std::string::npos;
        }

        // PostDelayed：SDL 的定时器回调在定时器线程上，到点后把 fn 推回界面线程
        struct DelayedCall
        {
            uint32_t               uiEvent = 0;
            std::function<void()>* fn      = nullptr;
        };

        Uint32 SDLCALL OnDelayed(void* userdata, SDL_TimerID, Uint32)
        {
            std::unique_ptr<DelayedCall> call(static_cast<DelayedCall*>(userdata));
            PushUiEvent(call->uiEvent, call->fn);
            return 0;   // 只执行一次
        }
    }

    LinuxApp::SdlWindow::~SdlWindow()
    {
        if (context)
            SDL_GL_DestroyContext(context);
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
    }

    LinuxApp::LinuxApp() = default;

    LinuxApp::~LinuxApp()
    {
        // 成员按声明的相反顺序销毁：输入层、界面先于图形设备，GL 上下文最后（SdlWindow）
    }

    // =========================================================
    // 初始化
    // =========================================================
    bool LinuxApp::Initialize(const char* title, int width, int height, const std::string& gpu)
    {
        SDL_SetAppMetadata("MiniBIM", nullptr, "io.github.minibim.MiniBIM");
        SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");                 // X11 上也用 EGL 建上下文（与 Wayland 相同）
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");       // 关闭按钮只发 CLOSE_REQUESTED，由 MainFrame 询问未保存的改动
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            LOG_ERROR("SDL_Init 失败：%s", SDL_GetError());
            return false;
        }
        m_uiEvent = SDL_RegisterEvents(1);

        // 按系统缩放放大初始尺寸
        const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        const float s     = scale > 0.0f ? scale : 1.0f;
        const int   ww    = static_cast<int>(width * s);
        const int   wh    = static_cast<int>(height * s);

        // 图形后端：默认先试 Vulkan，不行再用 OpenGL ES 3（--gpu gl 直接用它）。
        // 窗口要按后端带 SDL_WINDOW_VULKAN 或 SDL_WINDOW_OPENGL，二者不能兼得：换后端就重建窗口（还没显示，看不出来）
        if (gpu.empty() || gpu == "vulkan")
        {
            if (CreateSdlWindow(title, ww, wh, SDL_WINDOW_VULKAN))
                m_graphics = CreateGraphicsDevice(VulkanWindow(), PixelWidth(), PixelHeight(), "vulkan");
            if (!m_graphics)
            {
                LOG_WARN("Vulkan 不可用，改用 OpenGL ES 3");
                SDL_DestroyWindow(m_sdl.window);
                m_sdl.window = nullptr;
            }
        }
        if (!m_graphics)
        {
            if (!CreateSdlWindow(title, ww, wh, SDL_WINDOW_OPENGL) || !CreateGlContext())
                return false;
            NativeWindow window;
            window.glContext = true;
            m_graphics = CreateGraphicsDevice(window, PixelWidth(), PixelHeight(), "gl");
        }
        if (!m_graphics)
            return false;
        SDL_SetWindowMinimumSize(m_sdl.window, static_cast<int>(640 * s), static_cast<int>(420 * s));
        LOG_INFO("图形后端：%s", m_graphics->GetName());

        const int pw = PixelWidth();
        const int ph = PixelHeight();
        m_viewportRT = m_graphics->CreateRenderTarget();
        m_viewportRT->Create(pw, ph);

        const float ds = DisplayScale();
        if (!m_main.Initialize(*this, static_cast<float>(pw) / ds, static_cast<float>(ph) / ds, ds))
            return false;
        m_input = std::make_unique<MiniGUI::SDLInput>(&m_main.GetUI(), m_sdl.window);

        // 窗口图标（任务栏、Alt+Tab）
        MiniGUI::Image icon;
        if (MiniGUI::LoadImageFromFile(GetResourceDir() + "/icons/app.png", icon) && !icon.Empty())
            if (SDL_Surface* surface = SDL_CreateSurfaceFrom(icon.width, icon.height, SDL_PIXELFORMAT_RGBA32, icon.pixels.data(), icon.width * 4))
            {
                SDL_SetWindowIcon(m_sdl.window, surface);
                SDL_DestroySurface(surface);
            }

        SDL_ShowWindow(m_sdl.window);
        m_running = true;
        return true;
    }

    bool LinuxApp::CreateSdlWindow(const char* title, int width, int height, SDL_WindowFlags api)
    {
        m_sdl.window = SDL_CreateWindow(title, width, height, api | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
        if (!m_sdl.window)
        {
            if (api == SDL_WINDOW_VULKAN)
                LOG_WARN("创建 Vulkan 窗口失败：%s", SDL_GetError());      // 之后改用 OpenGL ES 3
            else
                LOG_ERROR("创建窗口失败：%s", SDL_GetError());
        }
        return m_sdl.window != nullptr;
    }

    bool LinuxApp::CreateGlContext()
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);      // 窗口只画界面；三维视口画在自己的离屏目标里（带深度）
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
        m_sdl.context = SDL_GL_CreateContext(m_sdl.window);
        if (!m_sdl.context)
        {
            LOG_ERROR("创建 OpenGL ES 3 上下文失败：%s", SDL_GetError());
            return false;
        }
        SDL_GL_MakeCurrent(m_sdl.window, m_sdl.context);
        SDL_GL_SetSwapInterval(1);
        return true;
    }

    NativeWindow LinuxApp::VulkanWindow() const
    {
        // 实例扩展与建表面都交给 SDL3：图形设备（apps/Render）不依赖窗口库
        NativeWindow window;
        Uint32 count = 0;
        if (const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&count))
            window.vulkanInstanceExtensions.assign(exts, exts + count);
        window.createVulkanSurface = [w = m_sdl.window](void* instance, uint64_t* surface)
        {
            VkSurfaceKHR s{};   // SDL_vulkan.h 自带句柄类型的定义，没有 vulkan.h 的 VK_NULL_HANDLE
            if (!SDL_Vulkan_CreateSurface(w, static_cast<VkInstance>(instance), nullptr, &s))
            {
                LOG_WARN("SDL_Vulkan_CreateSurface 失败：%s", SDL_GetError());
                return false;
            }
            *surface = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(s));
            return true;
        };
        return window;
    }

    int LinuxApp::PixelWidth() const
    {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(m_sdl.window, &pw, &ph);
        return pw;
    }

    int LinuxApp::PixelHeight() const
    {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(m_sdl.window, &pw, &ph);
        return ph;
    }

    float LinuxApp::DisplayScale() const
    {
        const float s = SDL_GetWindowDisplayScale(m_sdl.window);
        return s > 0.0f ? s : 1.0f;
    }

    void LinuxApp::UpdateDisplaySize()
    {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(m_sdl.window, &pw, &ph);
        if (pw <= 0 || ph <= 0)
            return;
        m_graphics->Resize(pw, ph);
        const float ds = DisplayScale();
        m_main.OnResize(static_cast<float>(pw) / ds, static_cast<float>(ph) / ds, ds);
    }

    // =========================================================
    // 事件循环
    // =========================================================
    int LinuxApp::Run()
    {
        // 空闲时阻塞等待事件，CPU 占用为 0；有重绘请求时取完队列里的事件再画一帧（本帧之前的输入已全部交给界面）
        while (m_running)
        {
            SDL_Event e;
            const bool got = m_redraw ? SDL_PollEvent(&e) : SDL_WaitEvent(&e);
            if (got)
            {
                HandleEvent(e);
                while (m_running && SDL_PollEvent(&e))
                    HandleEvent(e);
            }
            if (m_running && m_redraw.exchange(false))
                m_main.RenderFrame();       // 绘制期间提出的重绘请求留到下一轮（与网页版相同）
        }

        // 退出前释放还在队列里的界面线程任务（不再执行）
        SDL_Event e;
        while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, m_uiEvent, m_uiEvent) > 0)
            delete static_cast<std::function<void()>*>(e.user.data1);
        return 0;
    }

    void LinuxApp::HandleEvent(const SDL_Event& e)
    {
        if (e.type == m_uiEvent)
        {
            // Post、PostDelayed、对话框结果；data1 为空时只是唤醒（RequestRedraw）
            std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(e.user.data1));
            if (fn && *fn)
                (*fn)();
            return;
        }
        if (m_input)
            m_input->HandleEvent(e);

        switch (e.type)
        {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            UpdateDisplaySize();
            RequestRedraw();
            break;

        case SDL_EVENT_WINDOW_EXPOSED:
            RequestRedraw();
            break;

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            // 没有监视目录：切回窗口时检查界面描述文件是否改过（在编辑器里改完切回来即可看到）
            m_main.CheckUiFileChanged();
            RequestRedraw();
            break;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        case SDL_EVENT_QUIT:                // 终端里 Ctrl+C、kill
            m_main.RequestExit();           // 有未保存的改动时先询问
            break;

        case SDL_EVENT_DROP_BEGIN:
            m_drops.clear();
            break;

        case SDL_EVENT_DROP_FILE:
            if (e.drop.data)
                m_drops.emplace_back(e.drop.data);
            break;

        case SDL_EVENT_DROP_COMPLETE:
            if (!m_drops.empty())
                m_main.OpenFiles(std::exchange(m_drops, {}));
            break;

        default:
            break;
        }
    }

    // =========================================================
    // AppPlatform：图形
    // =========================================================
    MiniGUI::TextureId LinuxApp::RegisterViewportTexture(void* nativeShaderResource)
    {
        return m_graphics->RegisterViewportTexture(nativeShaderResource);
    }

    bool LinuxApp::LoadUiFonts(MiniGUI::TextSystem& text)
    {
        // 自带的 Noto Sans SC 子集（tools/make_web_font.py），不依赖系统装了哪些中文字体
        return MiniGUI::LoadSDLUIFonts(text, GetResourceDir() + "/fonts/NotoSansSC-UI.ttf");
    }

    void LinuxApp::RequestRedraw()
    {
        // 线程安全：其他线程上也可调用。已经有未处理的请求时不再推事件
        if (!m_redraw.exchange(true) && m_uiEvent)
            PushUiEvent(m_uiEvent, nullptr);
    }

    void LinuxApp::BeginUiPass()
    {
        m_graphics->BeginUiPass();
    }

    void LinuxApp::EndUiPass()
    {
        m_graphics->Present();          // Vulkan 在这里呈现
        if (m_sdl.context)
            SDL_GL_SwapWindow(m_sdl.window);    // OpenGL ES 3 交换缓冲
        if (m_input)
            m_input->Sync();        // 按焦点开关文字输入，候选窗跟随光标
    }

    Solid::ISolidKernel* LinuxApp::GetSolidKernel()
    {
#ifdef MINIBIM_WITH_SOLID
        if (!m_solidKernel)
            m_solidKernel = Solid::CreateSMLibKernel();
#endif
        return m_solidKernel.get();
    }

    // =========================================================
    // AppPlatform：窗口
    // =========================================================
    void LinuxApp::SetTitle(const std::string& utf8)
    {
        SDL_SetWindowTitle(m_sdl.window, utf8.c_str());
    }

    bool LinuxApp::IsWindowMaximized() const { return (SDL_GetWindowFlags(m_sdl.window) & SDL_WINDOW_MAXIMIZED) != 0; }
    void LinuxApp::MinimizeWindow()          { SDL_MinimizeWindow(m_sdl.window); }

    void LinuxApp::ToggleMaximizeWindow()
    {
        if (IsWindowMaximized())
            SDL_RestoreWindow(m_sdl.window);
        else
            SDL_MaximizeWindow(m_sdl.window);
    }

    void LinuxApp::CloseWindow()
    {
        Post([this] { m_main.RequestExit(); });     // 与 Win32 的 WM_CLOSE 相同：当前事件处理完再决定
    }

    void LinuxApp::Quit()
    {
        m_running = false;
    }

    // =========================================================
    // AppPlatform：文件
    // =========================================================
    void LinuxApp::ShowDialogError(const char* what)
    {
        LOG_ERROR("文件对话框打不开：%s", what);
        MiniGUI::ShowMessageBox(m_main.GetUI(), Tr("Could not show the file dialog"),
                                Format(Tr("{0}\n\nThe file dialog needs xdg-desktop-portal or zenity. You can also give a project file on the command line."),
                                       { what }),
                                { Tr("OK") }, [](int) {});
        RequestRedraw();
    }

    void LinuxApp::PickFile(FileKind kind, std::function<void(const std::string& path)> done)
    {
        auto* request = new DialogRequest{ m_uiEvent, [this, done = std::move(done)](std::vector<std::string> paths, int, std::string error)
        {
            if (!error.empty())
                ShowDialogError(error.c_str());
            else if (!paths.empty() && done)
                done(paths.front());
        }, {} };
        FillOpenFilters(kind, request->filters);
        SDL_ShowOpenFileDialog(OnDialogDone, request, m_sdl.window, request->filters.Data(), request->filters.Count(), nullptr, false);
    }

    std::string LinuxApp::ChooseSavePath(SaveKind kind, const std::string& suggestedName)
    {
        // AppPlatform 要求当场返回路径：在界面线程上等对话框关闭（期间只取系统事件，不画界面）
        DialogFilters filters;
        const char* ext = kind == SaveKind::Ifc ? ".ifc" : ".minibim";
        if (kind == SaveKind::Ifc)
            filters.Add(Tr("IFC file") + " (*.ifc)", "ifc");
        else
            filters.Add(Tr("MiniBIM project") + " (*.minibim)", "minibim");
        SyncDialog dialog;
        SDL_ShowSaveFileDialog(OnSyncDialogDone, &dialog, m_sdl.window, filters.Data(), filters.Count(),
                               suggestedName.empty() ? nullptr : suggestedName.c_str());
        while (!dialog.done)
        {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_MOUSE_REMOVED);   // 对话框打开期间在主窗口上的按键、点击作废
        if (!dialog.error.empty())
        {
            ShowDialogError(dialog.error.c_str());
            return {};
        }
        std::string path = dialog.path;
        if (!path.empty() && !HasDot(path))
            path += ext;        // 没写扩展名时补上（不是每个对话框都按筛选器补）
        return path;
    }

    std::string LinuxApp::GetResourceDir() const
    {
        // 可执行文件所在目录（资源在构建后复制到它旁边）；SDL 返回的路径以 / 结尾
        std::string dir = SDL_GetBasePath() ? SDL_GetBasePath() : "";
        while (dir.size() > 1 && dir.back() == '/')
            dir.pop_back();
        return dir;
    }

    std::string LinuxApp::GetUserDataDir() const
    {
        // $XDG_DATA_HOME/MiniBIM，默认 ~/.local/share/MiniBIM
        std::string base = GetEnv("XDG_DATA_HOME");
        if (base.empty())
        {
            const std::string home = GetEnv("HOME");
            if (home.empty())
                return {};
            base = home + "/.local/share";
        }
        return base + "/MiniBIM";
    }

    // =========================================================
    // AppPlatform：系统
    // =========================================================
    std::string LinuxApp::GetSystemLanguage() const
    {
        // SDL 按用户的偏好排好（LANGUAGE、LC_ALL、LANG 等），取第一个："zh" + "CN" → "zh-CN"
        std::string code;
        int count = 0;
        if (SDL_Locale** locales = SDL_GetPreferredLocales(&count))
        {
            if (count > 0 && locales[0] && locales[0]->language)
            {
                code = locales[0]->language;
                if (locales[0]->country && *locales[0]->country)
                    code += std::string("-") + locales[0]->country;
            }
            SDL_free(locales);
        }
        return code;
    }

    bool LinuxApp::OpenUrl(const std::string& url)
    {
        return !url.empty() && SDL_OpenURL(url.c_str());   // xdg-open
    }

    void LinuxApp::Post(std::function<void()> fn)
    {
        PushUiEvent(m_uiEvent, new std::function<void()>(std::move(fn)));
    }

    void LinuxApp::PostDelayed(unsigned ms, std::function<void()> fn)
    {
        auto* call = new DelayedCall{ m_uiEvent, new std::function<void()>(std::move(fn)) };
        if (!SDL_AddTimer(ms, OnDelayed, call))
        {
            delete call->fn;
            delete call;
        }
    }
}
