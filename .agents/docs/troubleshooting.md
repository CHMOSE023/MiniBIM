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

### `ModuleNotFoundError: No module named 'PIL'`（运行 `tools/make_icons.py`）
- 原因：这台机器的 Pillow 装在用户目录的 site-packages 里，`python -I`（隔离模式）不看用户目录；或者确实没装
- 做法：运行项目自己的脚本不加 `-I`；没装就 `pip install pillow`。`--check` 不需要 Pillow

### Python 打印的中文是乱码
- 原因：输出被重定向时按系统代码页编码，读的一方按 UTF-8 解
- 做法：`tools/extract_messages.py` 重定向时已改用 UTF-8；别的脚本设 `PYTHONIOENCODING=utf-8`

## 改文件

### 经 Bash 写进文件的内容里，`\n` 变成了真换行、`\v` 变成了控制字符
- 原因：Bash 工具里的 heredoc / `printf` 会处理反斜杠。实例：生成 `.po` 时文件头的 `"Language: zh_CN\n"` 被拆成两行，收集工具读不出文件头，重写时把它丢了
- 做法：往文件里写带转义的内容时用 Edit / Write 工具，或先把脚本写成文件再运行；改完核对

## CI（`.github/workflows/build.yml`）

### Windows 通过，Linux 任务编译失败
- 原因：MSVC 会顺带包含很多标准库头文件（`<cstdlib>`、`<cstring>`……），GCC 不会；或用了 MSVC 独有的函数（`fopen_s`、`_dupenv_s`）
- 做法：把用到的头文件写全；Windows 专有的调用放进 `#ifdef _WIN32`。Linux 任务只编核心库和测试，界面与渲染不编。
  lambda 里不要捕获结构化绑定（`auto [a, b] = …` 得到的名字）：较老的 GCC / Clang 不认，先复制成普通变量

### Linux 任务 ctest 报找不到测试程序（Could not find executable）
- 原因：新的测试程序登记在 `tests/CMakeLists.txt`（ctest 会跑），但没加进 `build.yml` Linux 任务的 `--target` 列表，没有编出来
- 做法：把它加进那一行的目标列表（skill `test` 的说明里也提了）

### 词典检查失败（`Assets` 任务的 Check catalogs）
- 原因：改了界面文字但没运行 `python tools/extract_messages.py`
- 做法：按 skill `i18n` 更新词典并补译文，一起提交

### 图标检查失败（`Assets` 任务的 Check icons）
- 原因：界面描述文件引用了脚本里没有定义的图标，改了脚本没重新生成，或往 `assets/icons` 里放了别处来的图片
- 做法：按 `.agents/features/icons.md` 在 `tools/make_icons.py` 里加图标、运行它，把生成的文件一起提交
