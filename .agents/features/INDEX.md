---
covers: .agents/features/*.md
---

# 功能指南索引

改某一块功能之前先读对应的指南：入口在哪、数据怎么流、有哪些已知的约束和坑。整体的开发计划在 `docs/PLAN.md`。

| 指南 | 内容 | 主要代码 | 测试 |
|---|---|---|---|
| [main-window](main-window.md) | 主窗口：界面描述文件、命令、功能区与停靠区（面板的停靠、浮动与布局保存）、特性面板、项目浏览器、选择集、放东西的工具（含柱与"结构"选项卡的结构柱、梁、基础）与选项栏、打开 Revit 文件与导入报告、拖进窗口打开文件、框选与修改工具、类型属性与材质对话框、未保存提示、帧顺序、平台接口、Win32 / Linux / 网页宿主、用户设置（含布局浮动 / 紧凑）、平面视图 | `apps/Common/src/GUI`、`apps/Win32`、`apps/Linux`、`apps/Web`、`MiniGUI::UiLayout` | skill `ui-check`、`AppTests` |
| [view3d](view3d.md) | 三维视口：相机、渲染、边线、实体内核与模型的三维场景 | `src/View/OrbitCamera.h`、`src/View/ModelScene.h`、`apps/Render`、`src/Solid` | `ViewTests`、`SMLibSmokeTests`、`ModelSceneTests` |
| [model](model.md) | 模型核心（移植自 BIMCraft）：元素与检查、墙、柱与 run（梁、基础、管线）、族、库与文件、命令与撤销、平面图、示例房屋 | `src/Model`、`src/Commands` | `ModelTests`、`CommandTests` |
| [ifc](ifc.md) | IFC 交换（移植自 BIMCraft）：STEP 的读法、IFC4 导出、IFC2x3 / IFC4 导入 | `src/Ifc` | `IfcTests` |
| [rvt](rvt.md) | Revit 文件读取器（移植自 BIMCraft，Apache-2.0）：复合文档、gzip 与页、BasicFileInfo、Atom 条目、预览图；类表、元素表、记录与清单、元素数据的读法、草图；导入成模型（墙、楼板、天花板、屋顶、门窗、房间、柱）；宿主怎样接上读取器；许可的隔离与发布包里的许可文件 | `src/Rvt` | `RvtTests` |
| [多语言](../../docs/i18n.md) | 词典、`Tr` 系列、界面库的翻译钩子、收集工具、字体 | `src/I18n`、`src/UI/Core/Translate.h` | `I18nTests`、skill `i18n` |
| [porting](porting.md) | 移植 BIMCraft：顺序、来源标注（含 Apache-2.0 的 Revit 读取器）、Rust → C++ 的对应写法、已定的事、界面的移植 | `docs/PLAN.md` | 随模块移植 BIMCraft 的测试 |
| [icons](icons.md) | 图标：全部由脚本生成，命令图标的风格与新增步骤，程序图标 | `tools/make_icons.py`、`assets/icons` | `make_icons.py --check`（skill `test`） |

新增功能指南时照现有的结构写（概述 → 入口与数据流 → 约束与坑 → 测试与排查），开头用 `covers:` 列出覆盖的代码（逗号分隔，`*` 通配），并在上表里加一行。写完运行 `.agents/scripts/check-docs.ps1` 核对引用。
