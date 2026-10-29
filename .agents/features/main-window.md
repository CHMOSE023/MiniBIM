---
covers: assets/ui/minibim_ui.json, apps/Common/src/GUI/MainFrame.*, apps/Common/src/GUI/MainCommands.cpp, apps/Common/src/GUI/PlanView.cpp, apps/Common/src/GUI/Panels.cpp, apps/Common/src/GUI/Parameters.*, tests/App/*, apps/Common/src/GUI/AppPlatform.h, apps/Win32/src/Host/*, src/UI/Widgets/UiLayout.*
---

# 主窗口

## 概述

整个界面由 MiniGUI 绘制（`src/UI`，复制自 MiniCAD），主窗口的逻辑在 `MiniBIM::MainFrame`（`apps/Common/src/GUI/MainFrame.h`），与平台无关；
平台服务经 `AppPlatform`，现在只有 Win32 宿主 `Win32Window`（`apps/Win32/src/Host/Win32Window.h`）。结构照 MiniCAD 的主窗口，但精简了很多：
没有文档标签、浮动窗口、起始页。界面按 `docs/PLAN.md` 第 3 阶段从 BIMCraft 的 `crates/ui-egui` 移植（在 MiniGUI 上重写），现在做到 3a：
标题栏（菜单栏 + 快速访问工具栏：打开、保存、撤销、重做）、功能区（"修改"、"视图"两个选项卡）、停靠区（左上特性、左下项目浏览器，同 Revit）、视图、状态栏。

## 入口与数据流

- 启动：`GuiMain.cpp` 解析参数（`--ui`、`--gpu`、`--lang`）→ `Win32Window::Initialize` 建无边框窗口和图形设备（D3D12 → D3D11 → WARP）→ `MainFrame::Initialize`：
  选语言（`--lang` > 用户设置 > `AppPlatform::GetSystemLanguage`）、建 `UIContext`、注册命令、注册宿主面板、读界面描述文件、打开示例房屋（`MainFrame::OpenSample`），
  再依次执行启动参数 `--run` 给的界面命令（`MainFrame::AddStartupCommand`；看界面效果时用，如 `--run view.plan`）
- 界面描述文件 `assets/ui/minibim_ui.json`（格式见 `src/UI/Widgets/UiLayout.h`）：菜单、快速访问工具栏、功能区与布局；宿主面板 `viewport`（`MiniGUI::ViewportHost`）、
  `properties`、`browser`（`Panels.cpp`）、`statusbar`。停靠区的布局写在文件的 `dock` 里，用户拖动后的布局还不保存（3e 的设置再做）
  开发时读源码目录里的文件（宏 `MINIBIM_UI_SOURCE`），宿主监视它所在的目录，保存后 `MainFrame::CheckUiFileChanged` 重新加载；写错时保留原界面，状态栏显示原因
- 命令：`MainCommands.cpp` 里注册为 `MiniGUI::Command`，名称和提示写英文原文并用 `N_` 标记，`MiniGUI::UiLayout` 每次应用时翻译；菜单只引用命令 ID。
  标题栏右侧的按钮执行 `window.minimize` / `window.maximize` / `window.close`，这三个命令必须有
- 项目：`MainFrame` 拥有一个 `Bim::Session`（模型与撤销历史，`.agents/features/model.md`），界面只经模型命令改它：
  文件菜单的新建 / 打开 / 保存 / 另存为 / 导出 IFC / 示例房屋（`MainFrame::NewProject`、`MainFrame::OpenProject`、`MainFrame::SaveProject`、
  `MainFrame::ExportIfc`、`MainFrame::OpenSample`）都走
  `MainFrame::RunCommand`（命令失败时弹出对话框，标题翻译、核心给的原因是英文），之后 `MainFrame::OnModelChanged` 重建三维场景、刷新标题与状态栏，
  换了项目时相机重新框住模型、清空选择集。标题栏显示保存过的文件名，否则显示项目名称；有没保存的改动时前面加 `*`
- 没保存的改动：`MainFrame::Dirty` 比较当前模型与最后新建、打开、保存的那一份（快照指针，同 BIMCraft；撤销回到那一份时又算保存过的；导出 IFC 不算保存）。
  新建、打开、示例房屋、退出（含标题栏的关闭）都经 `MainFrame::Guard`：有改动时问保存、不保存、取消
- 编辑：`edit.undo`（Ctrl+Z）、`edit.redo`（Ctrl+Y）走模型命令 `undo` / `redo`；`edit.delete`（Delete）删选择集，`modify.flip` 翻转其中的墙；
  `edit.selectNone`（Esc）清空选择集。命令的 `canExecute` 按撤销历史与选择集，模型或选择集变了时 `CommandRegistry::NotifyStateChanged`
- 选择集 `MainFrame::SetSelection`：平面上单击选中、Ctrl + 单击加进或拿出；模型变了时删掉的元素离开选择集（`MainFrame::OnPlanModelChanged`）。三维视图里还不能选（3c）
- 特性面板（`MainFrame::BuildProperties`，移植自 `properties.rs`）：类型选择器（这一类的类型；门窗另有门窗族的"族: 类型"，构件只有族的），"类别（个数）"，
  按组列出参数（行表 `Props::RowsOf`、楼层平面的 `Props::PlanRows`，在 `Parameters.cpp`，只依赖核心库，`tests/App/AppTests.cpp` 测）。
  选中的是一类元素时，取值都一样的才显示；没选东西时是楼层平面（读写它的标高：名称、高程、视图范围），三维视图没有参数。
  改动都是 `modify` 命令（`MainFrame::EditParameter`，`Props::Nested` 按点号嵌套，长度按项目单位读写 `Props::Parse` / `Props::Shown`）：
  输入框按 Enter 或离开时改一次，下拉列表与复选框选了就改；失败时弹窗并把面板重建回原值
- 项目浏览器（`MainFrame::BuildBrowser`，移植自 `browser.rs`）：视图（全部）→ 楼层平面（各标高）、三维视图；族 → 系统族的类型、可载入族的类型。
  双击视图打开它，正在显示的视图选中着；搜索框按词筛（`Props::Words`、`Props::Matches`：不分大小写，`*` 任意一串、`?` 任意一个，组名配上的词里面的项不必再配）。
  重建时各分支照旧展开或收起（按路径记在 `MainFrame` 里）
- 两块面板在模型、选择集、视图、语言变了时重建（`MainFrame::RefreshPanels` 经 `AppPlatform::Post` 推到当前消息之后，合并成一次；不在控件自己的回调里拆掉它）。
  控件回调里要改模型的，也经 `AppPlatform::Post` 再执行命令
- 文件对话框经 `AppPlatform::PickProjectFile`、`AppPlatform::ChooseProjectSavePath`、`AppPlatform::ChooseIfcSavePath`
  （Win32 用 `GetOpenFileNameW` / `GetSaveFileNameW`，筛选器文字经 `Tr`）。打开可选 `.minibim`、`.bimcraft`、`.ifc`（IFC 作为新项目，没有路径）；
  保存只直接写回 `.minibim`：打开的是 BIMCraft 的 `.bimcraft` 或 IFC 时"保存"也走另存为，免得把略去了内容的项目写回原文件；
  打开时有略去的内容（项目文件的 `left_out`、IFC 的 `skipped`）会弹窗说几个。"导出 IFC…"（`file.exportIfc`）是 `save` 到 `.ifc`，不改项目的路径
- 动态菜单：`{ "title": "Language", "dynamic": "languages" }` 由 `MainFrame::BuildLanguageMenu` 生成（跟随系统 + 词典目录里的每种语言，名称用那种语言自己写）
- 帧顺序（同 MiniCAD）：输入到达时立即改状态并 `RequestRender` → 消息队列空了收到 `WM_PAINT` → `MainFrame::RenderFrame`：`UIContext::Update` 布局 →
  `ViewportHost::RenderContent` 按最新尺寸渲染视口 → `AppPlatform::BeginUiPass` → `UIContext::Render` → `AppPlatform::EndUiPass`（呈现）
- 用户设置：`%LOCALAPPDATA%\MiniBIM\settings.json`（`AppPlatform::GetUserDataDir`），现在只有 `language`（空 = 跟随系统）；`MainFrame::LoadSettings` / `MainFrame::SaveSettings`

## 约束与坑

- `apps/Common` 里不写 `#ifdef`：需要新的平台能力就在 `AppPlatform` 加虚函数，在 `Win32Window` 里实现（以后的 Web、Linux 宿主各实现一份）
- 切换语言要重建界面：`MainFrame::SetLanguage` → `MainFrame::RetranslateUi` → `MainFrame::ReloadUi`（界面描述生成的部分整体重建，命令名称重新翻译），
  宿主面板里的文字由 `MainFrame::UpdateStatus` 之类按当前语言重写。新加的宿主面板如果显示文字，也要在 `RetranslateUi` 里刷新
- 菜单项的回调里不要直接重建界面：经 `AppPlatform::Post` 放到当前消息之后（语言菜单就是这样做的）
- 状态栏、对话框里的句子用 `Format(Tr(…))`，标点也随语言（`TrC("list of four items", "{0}, {1}, {2}, {3}")`）
- 视口有两种：三维（`ModelScene`）与一个标高的平面（`PlanView.cpp`）。"视图"菜单的 `view.3d`、`view.plan`、`view.levelUp`（PageUp）、
  `view.levelDown`（PageDown）切换；平面由核心库的 `Bim::Render::Draw` 按 `MainFrame` 存的视图（比例、中心）画成显示列表，填色的环剖成三角形、
  折线（含虚线）展开成带宽度的四边形，按画的先后一起交给 `IRenderer::Submit`（后画的盖住先画的：门的缺口盖住墙的填色）。平面画在白纸上，
  不随界面主题；拖动平移（左键拖出几个像素、中键、右键）、滚轮以指针处为中心缩放（都要重画显示列表）、单击左键拾取（`Drawing::Pick`），
  选择集里的元素用强调色，状态栏写平面名与选中的元素（几个时写个数）。模型变了（`OnModelChanged` → `MainFrame::OnPlanModelChanged`）下次画时重画；显示的标高删了回到最低的
- 平面上还不能画东西、移动东西（第 3 阶段的 3b、3c），现在能改模型的是特性面板、删除、翻转、撤销重做
- 调试版是控制台程序：`Process.MainWindowHandle` 拿到的是控制台窗口，找主窗口要按类名 `MiniBIMWin`（skill `ui-check` 的脚本）
- 图标：程序图标编在 exe 里（`MiniBIM.rc` 的 `IDI_ICON`，`Win32Window::InitWindow` 从资源加载，不依赖当前目录），标题栏图标是界面描述文件 titlebar 的 `"icon": "app.png"`，
  命令图标写在 `commands` 里；全部由 `tools/make_icons.py` 生成（`.agents/features/icons.md`）。菜单项不显示图标

## 测试与排查

- 界面效果：skill `ui-check`，中英文各截一张。截图脚本不能点视图，选中元素后的特性面板要靠手动看（或临时在启动时选上再截，截完改回）
- `AppTests`：参数表、值的写法与读法、搜索，以及示例房屋里每个元素的每个可写参数照面板的写法交给 `modify` 都被接受
- 界面描述文件写错：状态栏和控制台日志（`LOG_ERROR`）给出行列
- 初始化失败（返回 2）：看控制台日志里的图形接口一行；`--gpu warp` 用软件设备排除显卡问题
