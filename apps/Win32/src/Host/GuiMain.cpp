// MiniBIMWin：MiniBIM 桌面版入口（界面由 MiniGUI 绘制）
// 用法：MiniBIMWin.exe [项目文件] [--ui 界面描述文件.json] [--gpu d3d12|d3d11|warp] [--lang 语言代码] [--run 界面命令 ID]…
// 图形接口也可以用环境变量 MINIBIM_GPU 指定（命令行优先）；指定的不可用时按默认顺序回退。
// 给了项目文件（.minibim、.bimcraft、.ifc）就打开它，否则启动后显示开始页。
// --lang（如 zh_CN、en）只对这一次启动有效，不改用户设置；--run（如 view.plan，可给多个）在打开示例房屋后依次执行
// 以 USE_WIN32 编译时为窗口程序（没有控制台，日志不可见）
#include "Host/Win32Window.h"
#include <shellapi.h>
#include <iterator>
#include <string>

using namespace MiniBIM;

namespace
{
    std::string ToUtf8(const wchar_t* w)
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    int RunApp()
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        Win32Window window;
        MainFrame&  frame = window.GetFrame();
        std::string gpu;

        // 命令行参数按 UTF-8 取（--ui 的路径可能含中文）
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = ToUtf8(argv[i]);
            if (a == "--ui" && i + 1 < argc)
                frame.SetUiFile(ToUtf8(argv[++i]));
            else if (a == "--gpu" && i + 1 < argc)
                gpu = ToUtf8(argv[++i]);
            else if (a == "--lang" && i + 1 < argc)
                frame.SetLanguageOverride(ToUtf8(argv[++i]));
            else if (a == "--run" && i + 1 < argc)
                frame.AddStartupCommand(ToUtf8(argv[++i]));
            else if (a.rfind("--", 0) != 0)
                frame.SetStartupFile(a);
        }
        LocalFree(argv);
        if (gpu.empty())
        {
            wchar_t env[64] = {};
            if (GetEnvironmentVariableW(L"MINIBIM_GPU", env, static_cast<DWORD>(std::size(env))) > 0)
                gpu = ToUtf8(env);
        }

        if (!window.Initialize(L"MiniBIM", 1280, 800, gpu))
            return 2;
        return window.Run();
    }
}

#ifdef USE_WIN32
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return RunApp();
}
#else
int main()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    return RunApp();
}
#endif
