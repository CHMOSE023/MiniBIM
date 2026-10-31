---
name: test
description: 按改动编译并运行 MiniBIM 的相关测试，只返回汇总（每个测试程序通过 / 失败项数、失败的那几行，以及词典是否需要更新、图标是否齐全）。改完代码要验证时用它，不要自己拼编译命令或跑 run_tests.bat 全量；可指定测试名或 all。不用于设计测试用例、排查某个测试为什么失败（看输出与代码）或 CI 上的失败（先看 CI 日志）；看界面效果用 ui-check。
covers: tests/CMakeLists.txt, run_tests.bat, .agents/skills/test/run.ps1
---

# 按需测试

运行脚本（PowerShell 工具）：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .agents/skills/test/run.ps1                 # 按工作区改动挑测试
pwsh -NoProfile -ExecutionPolicy Bypass -File .agents/skills/test/run.ps1 I18nTests       # 指定测试，可多个
pwsh -NoProfile -ExecutionPolicy Bypass -File .agents/skills/test/run.ps1 all             # 全部 ctest + 编译桌面版 + 词典与图标检查
```

选项：`-Preset release`；`-Base <提交>`（改动按与该提交的差异算，默认 `HEAD` 即没提交的改动；分支上已提交的改动用 `-Base main`）。
第一次编译要编整个 SMLib（Debug 约 5 分钟），给足超时或用 `run_in_background`。

脚本做的事：

1. 用 `git diff` 与未跟踪文件找出改动，按 `run.ps1` 顶部的 `$rules` 对照出测试目标：改了 `apps/` 只编译 `MiniBIMWin`；
   改了界面文字可能在的地方（`apps/Common`、`assets/ui`、`src/UI`、`src/I18n`、`assets/i18n`）再检查词典；改了 CMake 跑全部
2. 用 `vcvars64` 包好环境（VS 装在别处时设 `MINIBIM_VCVARS`，否则用 vswhere 找），`out/<preset>` 还没配置时先 `cmake --preset`，只编译这些目标；编译失败时列出错误行后退出
3. 把 TEMP 指到 `out/tmp`，逐个运行，完整输出写进 `out/tmp/test/<目标>.txt`
4. 词典检查：`python tools/extract_messages.py --check`，代码或界面描述文件里的原文和 `assets/i18n` 对不上时算失败；
   图标检查（改了 `assets/ui`、`assets/icons`、`tools/make_icons.py` 时）：`python tools/make_icons.py --check`，引用了没定义的图标、
   改了脚本没重新生成、`assets/icons` 里有别处来的图片时算失败（做法见 `.agents/features/icons.md`）
5. 输出表格：每项的结果与用时，失败时附上 `[FAIL]` 行（最多 15 行）；退出码 = 失败的项数
6. 最后运行 `.agents/scripts/check-docs.ps1`：报出文档里找不到的引用，以及改了代码而对应文档没改的提醒（不影响退出码）。有提醒时看一眼那份文档，行为、接口或约束变了就在同一次改动里更新

## 看结果之后

- 失败：先看表格里的失败行，需要上下文再读 `out/tmp/test/<目标>.txt`，不要重跑全部
- "崩溃"：程序异常退出，stdout 缓冲里的输出可能丢了，最后一行不一定是出错处；单独运行那个程序或在测试里加打印定位
- "找不到程序"：目标名写错，或该测试没启用（`SMLibSmokeTests`、`ModelSceneTests` 要 `MINIBIM_WITH_SOLID`）
- 词典需要更新：按 skill `i18n` 做（运行收集工具、补译文）
- 界面改动（`MiniBIMWin` 只编译）：用 skill `ui-check` 实际运行并截图，中英文都看

## 新增测试或测试程序

- 测试程序是普通的可执行文件：打印 `[PASS]` / `[FAIL]` 行，有失败时返回非 0（照 `tests/I18n/I18nTests.cpp` 的 `Check` 写）；登记在 `tests/CMakeLists.txt`，输出到构建目录根
- 新增测试程序时在 `run.ps1` 的 `$rules` 里加一条对照，并加进 `.github/workflows/build.yml` 的 Linux 任务编译的目标（那里只编列出的目标，ctest 却跑全部登记的）
- 现有的测试程序：`I18nTests`、`ViewTests`、`ModelTests`、`CommandTests`（模型核心，见 `.agents/features/model.md`）、`IfcTests`（IFC 交换，见 `.agents/features/ifc.md`），
  `AppTests`（主窗口里不碰界面控件的部分，直接编进 `apps/Common/src/GUI/Parameters.cpp`、`apps/Common/src/GUI/Recent.cpp`、`apps/Common/src/GUI/Shortcuts.cpp`，见 `.agents/features/main-window.md`），`SMLibSmokeTests`、`ModelSceneTests`（要 SMLib）
- 测试也要能在 Linux（GCC）上编译运行（CI 的 Linux 任务编译核心库和测试）：Windows 专有的调用放进 `#ifdef _WIN32`；不用 `fopen_s` 这类 MSVC 独有的函数；用到的标准库头文件写全（MSVC 会顺带包含，GCC 不会）
- 不依赖本机文件：样例在临时目录里现做；中文路径按 UTF-8 构造 `std::filesystem::path`（`std::u8string`）
- 移植 BIMCraft 的模块时，把它的 `*_tests.rs` 一起移植（见 `.agents/features/porting.md`）
