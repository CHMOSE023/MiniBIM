---
covers: cmake/ClShowIncludes.cpp, build.bat, run_tests.bat, CMakePresets.json, .gitignore, .github/workflows/build.yml
---

# 排错：按现象查

遇到报错或怪现象时，先在这里按现象找。每条：**现象** → 原因 → 做法。解决了不在表里的新问题，补一条。
跑测试用 skill `test`（编译环境、TEMP 等已在脚本里处理）。不少条目来自 MiniCAD（同一套构建和界面库）。

## 编译

### `fatal error C1083: Cannot open include file: "cmath"`
- 原因：MSVC 的 `INCLUDE` / `LIB` 来自开发者环境变量，普通 PowerShell / cmd 里没有
- 做法：用 `build.bat`（自己找 VS 并调用 `vcvars64.bat`），或用 `vcvars64` 包一层：
  ```powershell
  cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build out/debug --target MiniBIMWin 2>&1'
  ```

### `fatal error C1128: number of sections exceeded object file format limit`（`src/Commands/Commands.cpp`）
- 原因：命令表与各命令的模板实例太多，一个 .obj 超过 COFF 的 65 535 个节
- 做法：`cmake/MiniBIM.cmake` 给 MSVC 加了 `/bigobj`；别的大文件遇到时同样处理，不要把命令拆成更多零碎的文件来绕开

### 编译时打印 `'vswhere.exe' is not recognized as an internal or external command`
- 原因：VS 18 的 `vcvars64.bat` 自己的脚本找不到 vswhere（它不在 `PATH` 里）
- 做法：不用管，`cl` 照样配置好了，编译不受影响

### `build.bat` 里用 vswhere 找 VS 失败
- 原因：`%ProgramFiles(x86)%` 里有括号，放进 `if ( … )` 块就会拆坏；`for /f` 又会剥掉命令首尾的引号
- 做法：`build.bat` 里用 `goto` 代替 `if` 块，`for /f` 的命令前加 `call`（已经这样写，改的时候别改回去）

### 编译命令"成功"了，但 exe 时间戳没变 / 只打印了 cmd 的版权横幅
- 原因：在 Git Bash 里写 `cmd /c "\"...vcvars64.bat\" && ..."`，内层的 `\"` 被拆坏；`cmd //c "build.bat"` 也可能找不到当前目录里的 bat
- 做法：用 PowerShell，外层单引号（见上）；bat 写全路径；编完看一眼 exe 的时间戳

### 改了头文件却没重编，运行时莫名崩溃或行为是旧的
- 原因：中文版 cl 的 `/showIncludes` 前缀随控制台代码页变化，Ninja 会记不下头文件依赖。cl 经启动器 `cmake/ClShowIncludes.cpp` 运行，把前缀改写成 ASCII
- 做法：检查 `ninja -C out/debug -t deps`，除 SMLib（预编译头）和 `.rc.res` 外不应有 `#deps 0`；有的话重新配置 `out/debug`

### 用备份把文件还原后，编出的程序还是改过的那一版
- 原因：`Copy-Item`、`cp -p`、解压都保留备份文件的修改时间，还原后的源文件比对象文件旧，Ninja 认为不用重编
- 做法：还原后更新时间戳 `(Get-Item <文件>).LastWriteTime = Get-Date` 再编

### 编译到一半 exe 或 `rules.ninja` 不见了
- 原因：用户在 Visual Studio 里打开了项目，VS 正在重新配置 `out/debug`
- 做法：等它配置完再编，不要同时编译

### 第一次编译很慢
- 原因：要编整个 SMLib（约 98 万行）
- 做法：给足超时（约 5 分钟）或放到后台；之后只编改动的部分

## 运行与测试

### 测试往临时目录写文件失败
- 原因：MiniCAD 的经验是本机从项目目录启动的程序可能写不了系统 TEMP
- 做法：先把 TEMP 指到项目内：`$env:TEMP='D:\MiniBIM\out\tmp'; $env:TMP=$env:TEMP`（skill `test` 已经这样做）

### 测试程序输出停在一半，退出码 3（`abort`）
- 原因：`abort` 时 stdout 缓冲里的输出丢了，最后一行不是出错处；常见的是未捕获的异常（例如窄字符串构造中文路径时抛出的转换异常）
- 做法：重定向到文件；单独运行那一组；在可疑处加打印

### 中文路径打不开 / 建出乱码目录
- 原因：`std::filesystem::path` 用窄字符串构造、或用 `path::string()` 转回，都按系统代码页（GBK）转换
- 做法：接口间一律传 UTF-8；转成 path 用 `std::u8string`，转回用 `u8string()`

### 找不到程序的主窗口（`Process.MainWindowHandle` 是控制台，`FindWindow` 返回 0）
- 原因：调试版是控制台程序，进程的"主窗口"是控制台；`FindWindow` 在这台机器的工具环境里找不到窗口
- 做法：按进程枚举顶层窗口，找类名 `MiniBIMWin`（skill `ui-check` 的脚本）

### 截图里没有刚用按键打开的菜单 / 截到了桌面
- 原因：`PrintWindow` 取的是 D3D 交换链上一次呈现之前的画面；从屏幕上取时窗口要在前台，否则截到的是挡在上面的窗口和桌面。
  后台进程常常抢不到前台（用户正在用机器时尤其如此）
- 做法：用 skill `ui-check` 的脚本（只在确认前台时按键和从屏幕取，否则退回 `PrintWindow`）；用户在用机器时不要发按键

### 截图只有窗口左上的一块（字比平时大、状态栏不见了）
- 原因：屏幕有缩放（如 150%）时，不感知 DPI 的进程拿到的窗口矩形是缩小过的，`PrintWindow` 却按实际像素画
- 做法：截图的进程先调用 `SetProcessDPIAware`（skill `ui-check` 的脚本已经这样做）；截图尺寸随缩放变（150% 时 1920×1200）

### 截图整张是白的（或黑的、带系统的旧式边框），程序却没卡住
- 原因：第一帧画得慢（调试版里平面有几千个点云的点、或启动时 `--run` 改了模型要重建场景，一帧超过约 1 秒）时，`PrintWindow` 之后一直取不到画面；
  再画几帧也不行，换 D3D11 或 WARP 也一样。主线程其实空闲（cdb 抓栈在 `Win32Window::Run` 的等待里），不是程序的问题。
  3 个点的扫描正常、在平面绘制里人为睡 1.5 秒就白，可以这样区分
- 做法：用 `-Open` 打开存好的项目、只截画得快的视图；平面里点多时把 `Render::Plan` 的各项写出来离线画成图核对（`Drawing::prims` 的环与折线）。
  要先带进文件、文件对话框又挡着时，临时加环境变量钩子执行命令并 `save` 成项目，截完删掉钩子

### 三维视图里墙面、屋顶上有斜线
- 原因：网格经 `FromMesh` 转成的实体每个三角形是一个面，内核的 `Edges` 把三角剖分的对角线也当成边
- 做法：网格建成的实体自己给出边线（`ModelScene` 里斜棱柱的底圈、顶圈与竖边），不取内核的边

### 截图测试之后，用户选好的语言变了
- 原因：按键切换语言会写 `%LOCALAPPDATA%\MiniBIM\settings.json`
- 做法：运行前备份、结束后还原（skill `ui-check` 的脚本已经这样做）；只看某种语言用 `--lang`，它不写设置

### 程序启动后界面是英文，而系统是中文
- 原因：用户设置里存了 `en`，或 `assets/i18n` 没复制到 exe 旁边（`out/debug/MiniBIMWin/i18n/`）
- 做法：看控制台的"语言："日志（会写出设置和系统语言）；重新编译 `MiniBIMWin` 会复制资源

### 只改了词典（或图标），运行起来还是旧的译文
- 原因：`assets/` 由 `apps/Win32/CMakeLists.txt` 的 POST_BUILD 复制到 exe 旁边，只在 `MiniBIMWin` 重新链接时才执行；只改 `.po` 不触发
- 做法：`cp assets/i18n/*.po out/debug/MiniBIMWin/i18n/`（图标同理复制 `assets/icons/`），或改动任一源文件让它重新链接

### 界面上有一句一直是英文，`extract_messages.py --untranslated` 却没列出它
- 原因：`Tr` 的参数写成了几个相邻的字符串字面量（`Tr("…" "…")`，长句换行时容易这样写），`tools/extract_messages.py` 只认一个字面量，整句没进词典
- 做法：写成一个字面量，再长也不拆（`AGENTS.md`："参数只写一个字符串字面量"），重新运行 `python tools/extract_messages.py` 并补译文

### `ModuleNotFoundError: No module named 'PIL'`（运行 `tools/make_icons.py`）
- 原因：这台机器的 Pillow 装在用户目录的 site-packages 里，`python -I`（隔离模式）不看用户目录；或者确实没装
- 做法：运行项目自己的脚本不加 `-I`；没装就 `pip install pillow`。`--check` 不需要 Pillow

### Python 打印的中文是乱码
- 原因：输出被重定向时按系统代码页编码，读的一方按 UTF-8 解
- 做法：`tools/extract_messages.py` 重定向时已改用 UTF-8；别的脚本设 `PYTHONIOENCODING=utf-8`

### Linux 上没有 `MiniBIMLinux` 目标（只编出了核心库和测试）
- 原因：配置时没找到 SDL3 或 OpenGL ES 3（cmake 输出"MiniBIMLinux：缺少 SDL3 或 OpenGL ES 3，跳过"）。SDL3 自己编译安装在 `~/sdl3` 时要告诉 CMake
- 做法：`cmake -S . -B out/linux -DCMAKE_PREFIX_PATH=$HOME/sdl3`；OpenGL ES 3 装 `libgles-dev libegl-dev`（见 README 的 Linux 编译）。
  没有显示器的机器上看界面：`xvfb-run -a -s '-screen 0 1400x900x24'` 里运行，用 ImageMagick 的 `import -window root` 截屏；
  llvmpipe 的 Vulkan 与 OpenGL ES 3 都能画，`--gpu gl` 换后端

### 测试时对时错，或数据像是乱的：`for (… : SeenOf(s)->Levels())`
- 原因：C++20 的范围 for 只延长最后那个临时对象（`Levels()` 返回的 vector）的寿命，`SeenOf(s)` 返回的 `shared_ptr` 在循环开始前就析构了，
  vector 里是指向已释放模型的指针。实例：链接的测试里找 "Shell: Roof deck" 找不到
- 做法：先存成局部变量（`const auto seen = SeenOf(s);`）再遍历；返回值类型的（`WallsOf` 复制出来的）不受影响

### 测试断言项目文件里按 GUID 记的东西，Windows 与 Linux 都失败
- 原因：`Model::ToJson()` 是模型的样子，库里的引用仍是名称；只有写成文件时（`Model::ToFile`）才经 `Element::LibRefsMut` 换成 GUID。
  实例：CAD 测试找 "plan" 图纸的 GUID 作为图层可见性的键，在 `ToJson().dump()` 里找不到
- 做法：要看文件里写了什么就用 `Model::ToFile(text, error)`；新加的按库名引用的字段（如视图图形的图层键）也要在 `Element::LibRefsMut` 里映射

### PowerShell 里 `cmd /c "build_wasm.bat"` 报 "is not recognized"
- 原因：这台机器的 cmd 不在当前目录里找批处理（`NoDefaultCurrentDirectoryInExePath`）
- 做法：写成 `cmd /c ".\build_wasm.bat"`（`build.bat`、`run_tests.bat` 同理）

### 网页版保存、导出时想验证写出的文件，又不想真的下载
- 做法：在浏览器的控制台里先替换 `HTMLAnchorElement.prototype.click`（记下 `download` 文件名）和 `URL.createObjectURL`（记下 `Blob` 大小），
  再点保存；`WebApp` 的下载就是建一个 `<a download>` 再点它

## 改文件

### 经 Bash 写进文件的内容里，`\n` 变成了真换行、`\v` 变成了控制字符
- 原因：Bash 工具里的 heredoc / `printf` 会处理反斜杠。实例：生成 `.po` 时文件头的 `"Language: zh_CN\n"` 被拆成两行，收集工具读不出文件头，重写时把它丢了
- 做法：往文件里写带转义的内容时用 Edit / Write 工具，或先把脚本写成文件再运行；改完核对

## CI（`.github/workflows/build.yml`）

### Windows 通过，Linux 任务编译失败
- 原因：MSVC 会顺带包含很多标准库头文件（`<cstdlib>`、`<cstring>`……），GCC 不会；或用了 MSVC 独有的函数（`fopen_s`、`_dupenv_s`）
- 做法：把用到的头文件写全；Windows 专有的调用放进 `#ifdef _WIN32`。Linux 任务连同界面（`apps/Common`）与 Linux 宿主一起编。
  lambda 里不要捕获结构化绑定（`auto [a, b] = …` 得到的名字）：较老的 GCC / Clang 不认，先复制成普通变量

### 网页任务编译失败，桌面版和 Linux 都通过
- 原因：Emscripten 没有线程（核心库在网页上不链接 pthread），或用了网页上没有的系统调用；`emdawnwebgpu` 端口包第一次要从 GitHub 下载
- 做法：本机 `build_wasm.bat` 复现（emsdk 版本与 `build.yml` 的 `EMSDK_VERSION` 一致）；要线程的功能看 `AppPlatform::HasBackgroundThread`

### 词典检查失败（`Assets` 任务的 Check catalogs）
- 原因：改了界面文字但没运行 `python tools/extract_messages.py`
- 做法：按 skill `i18n` 更新词典并补译文，一起提交

### 图标检查失败（`Assets` 任务的 Check icons）
- 原因：界面描述文件引用了脚本里没有定义的图标，改了脚本没重新生成，或往 `assets/icons` 里放了别处来的图片
- 做法：按 `.agents/features/icons.md` 在 `tools/make_icons.py` 里加图标、运行它，把生成的文件一起提交
