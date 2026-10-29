# AGENTS.md

本文件为 AI 编程助手在本仓库中工作时提供指引。构建命令见 `README.md`，开发计划见 `docs/PLAN.md`，多语言做法见 `docs/i18n.md`。

## 按需阅读（`.agents/`）

下面的文件不会自动加载，做到相应的事时再读（结构照 MiniCAD 的 `.agents/`）：

- **验证改动** → skill [`.agents/skills/test`](.agents/skills/test/SKILL.md)：按改动编译并运行相关测试、检查词典，只返回汇总
- **看界面效果**（布局、菜单、视口、各语言的文字）→ skill [`.agents/skills/ui-check`](.agents/skills/ui-check/SKILL.md)：运行桌面版并截下主窗口
- **改界面文字、补译文、加语言** → skill [`.agents/skills/i18n`](.agents/skills/i18n/SKILL.md)
- **改某一块功能之前** → [`.agents/features/INDEX.md`](.agents/features/INDEX.md)：按功能（主窗口、三维视口、多语言、移植 BIMCraft）分的实现说明
- **编译、运行或改文件时报错** → [`.agents/docs/troubleshooting.md`](.agents/docs/troubleshooting.md)：按报错现象查原因和做法；遇到不在表里的新问题，解决后补一条

### 文档跟着代码走

- 本文件、`.agents/` 和 `docs/` 里的说法与代码冲突时，**以代码为准**，并在同一次改动里改正文档（不要另开一次）
- 改了行为、接口、约束或文件位置时，同时更新覆盖它的文档：每份功能指南与 skill 开头的 `covers` 列出了它覆盖的代码
- 核对：`pwsh -NoProfile -ExecutionPolicy Bypass -File .agents/scripts/check-docs.ps1`（测试 skill 每次跑完会自动运行）。它报出文档里已经找不到的路径、`类::成员`、`MINIBIM_*` 环境变量与宏，以及"改了 covers 里的代码而文档没改"的提醒；`-Paths <文件>` 查改某个文件要看哪些文档
- 写文档时引用代码用反引号，写完整路径或 `类::成员`，脚本才查得到；占位写成 `<名称>`

## 项目概览

C++20 的开源 BIM 软件（MIT）。界面、渲染、构建、代码风格沿用 MiniCAD（复制自 MiniCAD 提交 `0bd1687`），几何内核 `3rd/SMLib`，
建模功能按 `docs/PLAN.md` 从 BIMCraft（Rust）移植。第一阶段只有 Windows 桌面版。

### 目录

- `src/`：核心库（目标 `MiniBIM`，定义在 `cmake/MiniBIM.cmake`，命名空间 `MiniBIM`）。`I18n/` 多语言，`View/` 三维相机，
  `Core/`（数学、日志）与 `Render/`（`IRenderer` 等接口）复制自 MiniCAD
- `src/Solid/`：三维实体内核接口 `ISolidKernel.h`（只用 MiniBIM 类型）；SMLib 实现在 `src/Solid/SMLib/`（目标 `MiniBIMSolid`，唯一包含 SMLib 头文件的地方）
- `src/UI/`：MiniGUI（目标 `MiniGUI`，定义在 `cmake/MiniGUI.cmake`，命名空间 `MiniGUI`），复制自 MiniCAD；与 MiniCAD 的差异记在 `docs/PLAN.md`
- `apps/Common/`：主窗口的界面与逻辑（目标 `MiniBIMApp`），平台服务经 `AppPlatform`（`apps/Common/src/GUI/AppPlatform.h`）
- `apps/Render/`：渲染后端（`cmake/Render.cmake`），Windows 为 D3D12 / D3D11；宿主经 `IGraphicsDevice` 使用
- `apps/Win32/`：桌面版宿主 `Win32Window`（目标 `MiniBIMWin`）
- `assets/`：界面描述文件 `ui/minibim_ui.json`、词典 `i18n/*.po`、图标 `icons/`（全部由 `tools/make_icons.py` 生成）；构建后复制到可执行文件旁
- `3rd/`：SMLib（Apache-2.0，本地修改见 `3rd/SMLib/README.md`，开关 `MINIBIM_WITH_SOLID`）、stb
- `tools/extract_messages.py`：收集界面原文、更新词典；`tools/make_icons.py`：生成全部图标；`tests/`：测试

### include 根目录（重要）

MiniBIM → `src`，MiniGUI → `src/UI`，渲染后端 → `apps/Render`，共用应用 → `apps/Common/src`，桌面版 → `apps/Win32/src`。
`src` 和 `src/UI` 下都有 `Core/`、`Render/`：不要让同一个相对路径（如 `Core/<文件>.h`）同时存在于两个根下——重名时编译器按搜索顺序悄悄选中其中一个。

### 源文件登记

源文件逐个列出（不用通配），新增 `.cpp` 要加到对应位置：核心库 `cmake/MiniBIM.cmake`，MiniGUI `cmake/MiniGUI.cmake`，
共用应用 `apps/Common/CMakeLists.txt`，渲染后端 `cmake/Render.cmake`，宿主 `apps/Win32/CMakeLists.txt`，测试 `tests/CMakeLists.txt`。
例外：`3rd/SMLib` 在 `cmake/SMLib.cmake` 里按目录通配。

## 架构

- `MainFrame`（`apps/Common/src/GUI/MainFrame.h`）：整个界面由 MiniGUI 绘制；菜单与布局来自界面描述文件（`UiLayout`，保存后自动重新加载），
  宿主面板 `viewport`、`statusbar` 由代码提供；所有操作注册为命令（`MainCommands.cpp`）
- `apps/Common` 必须保持平台无关：平台相关的一律经 `AppPlatform`，不要写 `#ifdef`；需要新的平台能力就在 `AppPlatform` 加虚函数，在各宿主里实现
- 帧顺序（同 MiniCAD）：输入到达时立即更新状态并请求重绘 → 宿主在 `WM_PAINT` 调 `MainFrame::RenderFrame`：布局 → 渲染视口 → 画界面 → 呈现
- 三维视口：`OrbitCamera`（正交、Z 向上、单位毫米）+ `IRenderer`；`Mat4` 为行向量约定（P' = P · M）
- 几何：只经 `Solid::ISolidKernel`（`AppPlatform::GetSolidKernel`）；SMLib 不能并发使用（适配层内部加锁）
- 现在的模型是临时的 `DemoScene`，第 2 阶段移植 BIMCraft 的模型后删掉

## 约定

### 多语言（详见 `docs/i18n.md`）
- 给人看的文字写**英文原文**并经 `Tr("…")` / `TrN` / `TrC`（核心库与 `apps/`）或 `MiniGUI::Tr`（`src/UI`）；参数只写一个字符串字面量
- 先存起来之后才翻译的（命令名称等）用 `N_("…")` 标记；带数值的句子用 `Format(Tr("{0} …"), { … })`，不要拼接译文片段
- 界面描述文件里的 `title`、`label`、`tooltip`、`text` 也写英文原文
- 改了界面文字后运行 `python tools/extract_messages.py`，再补 `assets/i18n/zh_CN.po` 的译文
- 日志与开发者才看的报错不翻译，写中文

### 图标（详见 `.agents/features/icons.md`）
- 项目用到的图标**一律由 `tools/make_icons.py` 生成**，不手画、不从别处拷图片：新命令要图标时在脚本里加一个 `@icon` 函数，
  运行 `python tools/make_icons.py`，再在界面描述文件 `commands` 里写 `"icon": "<名称>.png"`；生成的 PNG 一起提交
- 命令图标照 MiniCAD 的风格（灰色线条 + 青色强调，64×64）；程序图标 `app.ico` / `app.png` 也由脚本画

### 移植 BIMCraft
- 移植一个有界面按钮的命令时，同时在 `tools/make_icons.py` 里给它画图标（不用 BIMCraft 的图标）
- 每个含移植代码的源文件开头写一行来源：`// 移植自 BIMCraft crates/core/src/<文件>.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md`
- 不移植 `crates/rvt`（只有 Apache-2.0，依赖对 Revit 格式的逆向）；不使用 BIMCraft 的名称、标志、图标
- 不抄 Autodesk 的内容（族、样板、图标、帮助文字）

### 路径与编码
路径在接口间一律传 UTF-8 `std::string`；转成 `std::filesystem::path` 用 `std::u8string`，转回用 `u8string()`。
不要用 `path::string()` 或窄字符串构造 path（按系统代码页转换，英文系统遇到中文会抛异常）。

### 日志
`Core/Log.h` 提供 `LOG_INFO`、`LOG_WARN`、`LOG_ERROR`（桌面版输出到控制台）。

### 文本格式
仓库里文本统一存为 LF（`.gitattributes`），`.bat` 为 CRLF。代码注释与提交信息用简体中文；标识符、命令、路径保持英文。

## 验证改动

用 `.agents/skills/test` 只编译、运行与改动相关的测试，不要每次跑 `run_tests.bat` 全量；全量测试交给 CI
（`.github/workflows/build.yml`：到 main 的 PR 跑 `Windows (D3D12 / D3D11)`、`Linux (core + tests)`、`Assets`（词典与图标）三个任务；
Linux 任务只编核心库和测试，移植来的核心代码要能在 GCC 上编译）。改了界面要实际运行看（skill `ui-check`，中英文都看）。

## 发布

推送 `v*` 标签后 CI 编译、测试、打包 `MiniBIM-<标签>-windows-x64.zip`，创建 GitHub Release。版本号只改根 `CMakeLists.txt` 的 `MINIBIM_VERSION`（三段或四段）。
发布说明取自 `docs/release-notes/<标签>.md`，打标签前先写好（没有时只写一行默认说明）。
