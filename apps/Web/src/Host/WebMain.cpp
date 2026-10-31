// MiniBIM 网页版入口：界面与桌面版相同（MiniGUI 绘制），经 WebGPU 渲染，浏览器不支持时用 WebGL2
#include "Host/WebApp.h"
#ifdef MINIBIM_WITH_RVT
#include "Rvt/Import.h"
#endif
#include <cstdio>

int main()
{
    // 页面关闭前一直存在：main 返回后运行时继续由浏览器事件驱动（-sEXIT_RUNTIME=0）
    auto* app = new MiniBIM::WebApp();
#ifdef MINIBIM_WITH_RVT
    // Revit 文件的读取器（src/Rvt，Apache-2.0）：主窗口不依赖它，由宿主接上。网页上没有后台线程：提示框画出来后在界面线程上读
    app->GetFrame().SetRevitReader(&MiniBIM::Rvt::ImportPath);
#endif
    if (!app->Initialize())
    {
        std::printf("MiniBIM：初始化失败（浏览器需要支持 WebGPU 或 WebGL2）\n");
        return 1;
    }
    return 0;
}
