// MiniBIMLinux：MiniBIM Linux 版入口（界面由 MiniGUI 绘制，SDL3 窗口 + Vulkan，不行时 OpenGL ES 3）
// 用法：MiniBIMLinux [项目文件] [--ui 界面描述文件.json] [--gpu vulkan|gl] [--lang 语言代码] [--run 界面命令 ID]…
// 参数与桌面版 MiniBIMWin 相同（见 apps/Win32/src/Host/GuiMain.cpp）；图形后端也可以用环境变量 MINIBIM_GPU 指定（命令行优先）
#include "Host/LinuxApp.h"
#include <cstdlib>
#include <string>

using namespace MiniBIM;

int main(int argc, char** argv)
{
    LinuxApp   app;
    MainFrame& frame = app.GetFrame();

    const char* envGpu = std::getenv("MINIBIM_GPU");
    std::string gpu    = envGpu ? envGpu : "";

    // Linux 的命令行参数就是 UTF-8 字节
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--ui" && i + 1 < argc)
            frame.SetUiFile(argv[++i]);
        else if (a == "--gpu" && i + 1 < argc)
            gpu = argv[++i];
        else if (a == "--lang" && i + 1 < argc)
            frame.SetLanguageOverride(argv[++i]);
        else if (a == "--run" && i + 1 < argc)
            frame.AddStartupCommand(argv[++i]);
        else if (a.rfind("--", 0) != 0)
            frame.SetStartupFile(a);
    }

    if (!app.Initialize("MiniBIM", 1280, 800, gpu))
        return 2;
    return app.Run();
}
