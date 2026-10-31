// ── MiniBIM 桌面版宿主：窗口、图形设备、消息循环与平台服务（精简自 MiniCAD 的 Win32Window）──
#include "Host/Win32Window.h"
#include "resource.h"
#ifdef MINIBIM_WITH_SOLID
#include "Solid/SMLib/SMLibKernel.h"
#endif
#include "Core/Log.h"
#include "Core/UIContext.h"
#include "Platform/Win32/Win32Fonts.h"
#include "Platform/Win32/Win32Frame.h"
#include "Platform/Win32/Win32Input.h"
#include "Widgets/TitleBar.h"
#include "I18n/Translator.h"
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <filesystem>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace MiniBIM
{
    namespace
    {
        constexpr UINT kMsgPosted = WM_APP + 1;     // lParam：new 出来的 std::function<void()>（Post）
        constexpr UINT_PTR kFirstDelayTimer = 0x4D420000;   // PostDelayed 的定时器 ID 从这里起

        std::wstring ToWide(const std::string& utf8)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
            std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
            if (n > 1)
                MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), n);
            return w;
        }

        std::string ToUtf8(const wchar_t* w)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
            if (n > 1)
                WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
            return s;
        }
    }

    Win32Window::Win32Window() = default;

    Win32Window::~Win32Window()
    {
        if (m_uiWatch)
            FindCloseChangeNotification(m_uiWatch);
        // 成员按声明的相反顺序销毁：输入层、界面先于界面后端和设备
    }

    Solid::ISolidKernel* Win32Window::GetSolidKernel()
    {
#ifdef MINIBIM_WITH_SOLID
        if (!m_solidKernel)
            m_solidKernel = Solid::CreateSMLibKernel();
#endif
        return m_solidKernel.get();
    }

    // =========================================================
    // 初始化
    // =========================================================
    bool Win32Window::Initialize(const wchar_t* title, int width, int height, const std::string& gpu)
    {
        if (!InitWindow(title, width, height))
            return false;

        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        m_graphics = CreateGraphicsDevice(NativeWindow{ m_hwnd }, w, h, gpu);
        if (!m_graphics)
            return false;
        m_viewportRT = m_graphics->CreateRenderTarget();
        m_viewportRT->Create(w, h);

        m_dpiScale = static_cast<float>(GetDpiForWindow(m_hwnd)) / 96.0f;
        if (!m_main.Initialize(*this, static_cast<float>(w) / m_dpiScale, static_cast<float>(h) / m_dpiScale, m_dpiScale))
            return false;
        m_input = std::make_unique<MiniGUI::Win32Input>(&m_main.GetUI(), m_hwnd);

        // ── 无边框窗口：标题栏的空白处可以拖动窗口 ───────────────────
        m_frame->SetCaptionTest([this](int x, int y)
        {
            MiniGUI::TitleBar* bar = m_main.GetTitleBar();
            return bar && bar->IsCaptionAt({ static_cast<float>(x) / m_dpiScale, static_cast<float>(y) / m_dpiScale });
        });
        m_frame->SetOnActiveChanged([this](bool active)
        {
            if (MiniGUI::TitleBar* bar = m_main.GetTitleBar())
                bar->SetWindowActive(active);
        });

        WatchUiFile();
        ShowWindow(m_hwnd, SW_SHOW);
        return true;
    }

    bool Win32Window::InitWindow(const wchar_t* title, int width, int height)
    {
        HINSTANCE hInstance = GetModuleHandleW(nullptr);

        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DBLCLKS;
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInstance;
        wc.hCursor       = nullptr;     // 光标由 MiniGUI 在 WM_SETCURSOR 设置（边框上由系统设置）
        wc.lpszClassName = L"MiniBIMWin";
        // 程序图标编在 exe 的资源里（MiniBIM.rc），不依赖启动时的当前目录
        wc.hIcon         = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        wc.hIconSm       = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_ICON), IMAGE_ICON,
                                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        RegisterClassExW(&wc);

        // 无边框：窗口矩形就是客户区。按系统缩放放大初始尺寸，居中到主显示器的工作区
        const float scale = static_cast<float>(GetDpiForSystem()) / 96.0f;
        const int   w     = static_cast<int>(width * scale);
        const int   h     = static_cast<int>(height * scale);
        MONITORINFO mi    = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
        const int x = mi.rcWork.left + std::max(0, static_cast<int>(mi.rcWork.right - mi.rcWork.left - w) / 2);
        const int y = mi.rcWork.top  + std::max(0, static_cast<int>(mi.rcWork.bottom - mi.rcWork.top - h) / 2);

        m_hwnd = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                                 x, y, w, h, nullptr, nullptr, hInstance, this);
        if (!m_hwnd)
            return false;
        m_frame = std::make_unique<MiniGUI::Win32Frame>(m_hwnd);
        m_frame->Apply();       // 先保存再应用：Apply 触发的 WM_NCCALCSIZE 要经过 m_frame
        return true;
    }

    void Win32Window::UpdateDisplaySize()
    {
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        m_dpiScale = static_cast<float>(GetDpiForWindow(m_hwnd)) / 96.0f;
        const float w = static_cast<float>(rc.right - rc.left);
        const float h = static_cast<float>(rc.bottom - rc.top);
        m_main.OnResize(w / m_dpiScale, h / m_dpiScale, m_dpiScale);
    }

    void Win32Window::WatchUiFile()
    {
        const std::string& path = m_main.GetUiPath();
        const std::wstring dir  = std::filesystem::path(std::u8string(path.begin(), path.end())).parent_path().wstring();
        m_uiWatch = FindFirstChangeNotificationW(dir.c_str(), FALSE,
                                                 FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
        if (m_uiWatch == INVALID_HANDLE_VALUE)
        {
            m_uiWatch = nullptr;
            LOG_WARN("无法监视界面描述文件所在目录，修改后要重新启动才生效");
        }
    }

    // =========================================================
    // 消息
    // =========================================================
    LRESULT Win32Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        Win32Window* self = nullptr;
        if (msg == WM_NCCREATE)
        {
            self = static_cast<Win32Window*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        else
        {
            self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->EventProc(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT Win32Window::EventProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        LRESULT result = 0;
        // 无边框窗口的非客户区消息先处理（命中测试要在 MiniGUI 之前）
        if (m_frame && m_frame->HandleMessage(hwnd, msg, wParam, lParam, result))
            return result;
        if (m_input && m_input->HandleMessage(hwnd, msg, wParam, lParam, result))
            return result;

        switch (msg)
        {
        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            if (m_graphics && wParam != SIZE_MINIMIZED)
            {
                m_graphics->Resize(LOWORD(lParam), HIWORD(lParam));
                if (m_input)        // 界面初始化完成之后
                    UpdateDisplaySize();
            }
            return 0;

        case WM_GETMINMAXINFO:
        {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            const float scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
            mmi->ptMinTrackSize = { static_cast<LONG>(640 * scale), static_cast<LONG>(420 * scale) };
            return 0;
        }

        case WM_DPICHANGED:
        {
            const RECT* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (m_input)
                UpdateDisplaySize();
            return 0;
        }

        case WM_PAINT:
            // 消息队列里没有其他消息时才会收到 WM_PAINT：此时本帧之前的输入已全部处理
            if (m_input)
                m_main.RenderFrame();
            ValidateRect(hwnd, nullptr);
            return 0;

        case WM_CLOSE:
            m_main.RequestExit();
            return 0;

        case kMsgPosted:
        {
            std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lParam));
            if (*fn)
                (*fn)();
            return 0;
        }

        case WM_TIMER:
        {
            const auto it = m_delayed.find(wParam);
            if (it == m_delayed.end())
                break;
            KillTimer(hwnd, wParam);
            const std::function<void()> fn = std::move(it->second);
            m_delayed.erase(it);
            if (fn)
                fn();
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    int Win32Window::Run()
    {
        // 空闲时阻塞等待（消息或界面描述文件所在目录的变化），CPU 占用为 0；需要重绘时 UIContext 回调 InvalidateRect
        while (true)
        {
            const DWORD count = m_uiWatch ? 1 : 0;
            const DWORD r = MsgWaitForMultipleObjectsEx(count, &m_uiWatch, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (count && r == WAIT_OBJECT_0)
            {
                FindNextChangeNotification(m_uiWatch);
                m_main.CheckUiFileChanged();
            }

            MSG msg = {};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                    return static_cast<int>(msg.wParam);
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    // =========================================================
    // AppPlatform：图形
    // =========================================================
    MiniGUI::IRenderBackend& Win32Window::GetUiBackend()
    {
        return m_graphics->GetUiBackend();
    }

    MiniGUI::TextureId Win32Window::RegisterViewportTexture(void* nativeShaderResource)
    {
        return m_graphics->RegisterViewportTexture(nativeShaderResource);
    }

    bool Win32Window::LoadUiFonts(MiniGUI::TextSystem& text)
    {
        return MiniGUI::LoadSystemUIFonts(text);    // 微软雅黑 UI + Segoe UI Symbol
    }

    void Win32Window::RequestRedraw()
    {
        InvalidateRect(m_hwnd, nullptr, FALSE);     // 线程安全
    }

    void Win32Window::Post(std::function<void()> fn)
    {
        auto* pending = new std::function<void()>(std::move(fn));
        if (!PostMessageW(m_hwnd, kMsgPosted, 0, reinterpret_cast<LPARAM>(pending)))
            delete pending;
    }

    void Win32Window::PostDelayed(unsigned ms, std::function<void()> fn)
    {
        const UINT_PTR id = kFirstDelayTimer + (m_nextDelayed++ & 0xFFFF);
        if (SetTimer(m_hwnd, id, std::max<UINT>(ms, USER_TIMER_MINIMUM), nullptr))
            m_delayed[id] = std::move(fn);
    }

    void Win32Window::BeginUiPass()
    {
        m_graphics->BeginUiPass();
    }

    void Win32Window::EndUiPass()
    {
        m_graphics->Present();
        if (m_input)
            m_input->Sync();        // 按焦点开关输入法，候选窗跟随光标
    }

    // =========================================================
    // AppPlatform：窗口
    // =========================================================
    void Win32Window::SetTitle(const std::string& utf8)
    {
        SetWindowTextW(m_hwnd, ToWide(utf8).c_str());
    }

    bool Win32Window::IsWindowMaximized() const { return m_frame && m_frame->IsWindowMaximized(); }
    void Win32Window::MinimizeWindow()          { m_frame->Minimize(); }
    void Win32Window::ToggleMaximizeWindow()    { m_frame->ToggleMaximize(); }
    void Win32Window::CloseWindow()             { PostMessageW(m_hwnd, WM_CLOSE, 0, 0); }
    void Win32Window::Quit()                    { DestroyWindow(m_hwnd); }

    // =========================================================
    // AppPlatform：目录与系统
    // =========================================================
    std::string Win32Window::GetResourceDir() const
    {
        wchar_t buf[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        const std::u8string u8 = std::filesystem::path(buf).parent_path().u8string();
        return std::string(u8.begin(), u8.end());
    }

    std::string Win32Window::GetUserDataDir() const
    {
        // %LOCALAPPDATA%\MiniBIM
        wchar_t buf[MAX_PATH] = L"";
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return {};
        const std::u8string u8 = (std::filesystem::path(buf) / L"MiniBIM").u8string();
        return std::string(u8.begin(), u8.end());
    }

    // 打开 / 保存的文件对话框（系统对话框是模态的：选完才返回）。筛选器的文字随界面语言
    void Win32Window::PickFile(FileKind kind, std::function<void(const std::string&)> done)
    {
        const std::string path = ShowProjectDialog(kind == FileKind::Shortcuts      ? FileDialog::Shortcuts
                                                 : kind == FileKind::ProjectOrRevit ? FileDialog::OpenRevit
                                                                                    : FileDialog::Open, {});
        if (!path.empty() && done)
            done(path);
    }

    std::string Win32Window::ChooseSavePath(SaveKind kind, const std::string& suggestedName)
    {
        return ShowProjectDialog(kind == SaveKind::Ifc ? FileDialog::SaveIfc : FileDialog::SaveProject, suggestedName);
    }

    std::string Win32Window::ShowProjectDialog(FileDialog kind, const std::string& suggestedName)
    {
        const bool save = kind == FileDialog::SaveProject || kind == FileDialog::SaveIfc;
        // 筛选器：名称\0模式\0……\0\0；名称经 Tr 翻译
        std::wstring filter;
        auto add = [&](const std::string& label, const wchar_t* pattern)
        {
            filter += ToWide(label);
            filter += L'\0';
            filter += pattern;
            filter += L'\0';
        };
        if (kind == FileDialog::SaveProject)
            add(Tr("MiniBIM project") + " (*.minibim)", L"*.minibim");
        else if (kind == FileDialog::SaveIfc)
            add(Tr("IFC file") + " (*.ifc)", L"*.ifc");
        else if (kind == FileDialog::Shortcuts)
            add(Tr("Shortcuts file") + " (*.json;*.xml;*.txt;*.md)", L"*.json;*.xml;*.txt;*.md");
        else if (kind == FileDialog::OpenRevit)
        {
            add(Tr("All projects") + " (*.minibim;*.bimcraft;*.ifc;*.rvt)", L"*.minibim;*.bimcraft;*.ifc;*.rvt");
            add(Tr("MiniBIM project") + " (*.minibim)", L"*.minibim");
            add(Tr("BIMCraft project") + " (*.bimcraft)", L"*.bimcraft");
            add(Tr("IFC file") + " (*.ifc)", L"*.ifc");
            add(Tr("Revit project") + " (*.rvt)", L"*.rvt");
        }
        else
        {
            add(Tr("All projects") + " (*.minibim;*.bimcraft;*.ifc)", L"*.minibim;*.bimcraft;*.ifc");
            add(Tr("MiniBIM project") + " (*.minibim)", L"*.minibim");
            add(Tr("BIMCraft project") + " (*.bimcraft)", L"*.bimcraft");
            add(Tr("IFC file") + " (*.ifc)", L"*.ifc");
        }
        add(Tr("All files") + " (*.*)", L"*.*");
        filter += L'\0';

        std::vector<wchar_t> buf(32768, L'\0');
        if (save && !suggestedName.empty())
        {
            const std::wstring name = ToWide(suggestedName);
            std::copy_n(name.begin(), std::min(name.size(), buf.size() - 1), buf.begin());
        }
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = m_hwnd;
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrFile   = buf.data();
        ofn.nMaxFile    = static_cast<DWORD>(buf.size());
        ofn.lpstrDefExt = kind == FileDialog::SaveIfc ? L"ifc" : L"minibim";
        ofn.Flags       = OFN_NOCHANGEDIR | OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
        if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)))
            return {};
        return ToUtf8(buf.data());
    }

    std::string Win32Window::GetSystemLanguage() const
    {
        // 用户的界面语言（"控制面板 > 语言"里排第一的显示语言），取不到时用区域设置
        ULONG count = 0, size = 0;
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size > 0)
        {
            std::vector<wchar_t> buf(size);
            if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &size) && buf[0])
                return ToUtf8(buf.data());
        }
        wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"";
        if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0)
            return ToUtf8(name);
        return {};
    }

    bool Win32Window::OpenUrl(const std::string& url)
    {
        const HINSTANCE r = ShellExecuteW(m_hwnd, L"open", ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(r) > 32;
    }
}
