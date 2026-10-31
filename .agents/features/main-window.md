---
covers: assets/ui/minibim_ui.json, apps/Common/src/GUI/MainFrame.*, apps/Common/src/GUI/MainCommands.cpp, apps/Common/src/GUI/PlanView.cpp, apps/Common/src/GUI/Panels.cpp, apps/Common/src/GUI/Palettes.cpp, apps/Common/src/GUI/Tools.cpp, apps/Common/src/GUI/Annotate.cpp, apps/Common/src/GUI/Modify.cpp, apps/Common/src/GUI/SketchMode.cpp, apps/Common/src/GUI/Dialogs.cpp, apps/Common/src/GUI/Schedules.cpp, apps/Common/src/GUI/Parameters.*, apps/Common/src/GUI/Recent.*, apps/Common/src/GUI/StartPage.cpp, apps/Common/src/GUI/Autosave.cpp, apps/Common/src/GUI/Settings.cpp, apps/Common/src/GUI/Shortcuts.*, apps/Common/src/GUI/ShortcutsDialog.cpp, apps/Common/src/GUI/Revit.cpp, apps/Common/src/GUI/RevitReport.*, apps/Common/src/GUI/OpenQueue.h, tests/App/*, apps/Common/src/GUI/AppPlatform.h, apps/Win32/src/Host/*, apps/Linux/src/Host/*, apps/Web/src/Host/*, src/UI/Widgets/UiLayout.*
---

# 主窗口

## 概述

整个界面由 MiniGUI 绘制（`src/UI`，复制自 MiniCAD），主窗口的逻辑在 `MiniBIM::MainFrame`（`apps/Common/src/GUI/MainFrame.h`），与平台无关；
平台服务经 `AppPlatform`，现在只有 Win32 宿主 `Win32Window`（`apps/Win32/src/Host/Win32Window.h`）。结构照 MiniCAD 的主窗口，但精简了很多：
没有文档标签、浮动窗口。界面按 `docs/PLAN.md` 第 3 阶段从 BIMCraft 的 `crates/ui-egui` 移植（在 MiniGUI 上重写），现在做到 3d：
标题栏（菜单栏 + 快速访问工具栏：打开、保存、撤销、重做）、功能区（"建筑"、"修改"、"视图"三个选项卡）、选项栏、停靠区（左上特性、左下项目浏览器，同 Revit）、视图、状态栏。

## 入口与数据流

- 启动：`GuiMain.cpp` 解析参数（项目文件、`--ui`、`--gpu`、`--lang`、`--run`）→ `Win32Window::Initialize` 建无边框窗口和图形设备（D3D12 → D3D11 → WARP）→ `MainFrame::Initialize`：
  选语言（`--lang` > 用户设置 > `AppPlatform::GetSystemLanguage`）、建 `UIContext`、注册命令、注册宿主面板、读界面描述文件；给了项目文件（`MainFrame::SetStartupFile`）就打开它，给了 `--run` 的界面命令（`MainFrame::AddStartupCommand`；看界面效果时用，
  如 `--run view.plan`）就打开示例房屋（`MainFrame::OpenSample`）后依次执行，都没有时显示开始页
- 界面描述文件 `assets/ui/minibim_ui.json`（格式见 `src/UI/Widgets/UiLayout.h`）：菜单、快速访问工具栏、功能区与布局；宿主面板 `canvas`（视图，`MiniGUI::ViewportHost`）、
  `viewport`（停靠区里视图的位置，`MainFrame::CreateViewArea`）、`options`（选项栏，`Tools.cpp`）、`properties`、`browser`（`Panels.cpp`）、`statusbar`。
  停靠区的布局写在文件的 `dock` 里，用户改过的布局存进 settings.json（见下面"面板的停靠与浮动"）
- 视图铺满（照 MiniCAD，BIMCraft 也是视图在下、其余浮在上面）：`canvas` 铺满标题栏以下，功能区、选项栏、停靠区、状态栏放在叠在它上面的一层（`"passThrough"`，空白处点击穿透到视图）；
  停靠区是浮动的（`MiniGUI::DockSpace::SetFloating`），视图的位置 `viewport` 是没有标题栏的组，成了透明、点击穿透的"洞"，范围就是视图没被挡住的部分。
  框住模型、整个平面时放进这块区域、以它的中心为中心：`MainFrame::UpdateFitInsets` 每次渲染前按"洞"与视口的差算出留边，交给 `OrbitCamera::SetFitInsets`
  与平面的 `Bim::Render::Fit`（`MainFrame::RenderPlan`）；面板停在视图的哪一侧也按"洞"判断。留边变了（拖动面板）画面不动，下次框住时才用
- 布局（移植自 BIMCraft 的 Layout 设置）：默认"浮动"，功能区、选项栏、停靠面板、状态栏是叠在视图上的圆角卡片，四周与之间留 6 像素的缝露出视图（三维的渐变底、平面的白纸）
  （界面描述文件里 `"card": true`、停靠区 `"gap": 6`，功能区本来就是卡片）；"紧凑"时 `MainFrame::ReloadUi` 让 `MiniGUI::UiLayout::SetVariant` 选用变体 `compact`，
  各节点 `"variants": { "compact": { … } }` 去掉缝、方角贴边（`MiniGUI::Ribbon::SetCard`，停靠区 gap 为 0 时是紧贴的方角面板）。BIMCraft 卡片的阴影没有做（同 MiniCAD 的外观）
- 面板的停靠与浮动（`Palettes.cpp`，移植自 `dock.rs`）：拖标签停到别处、拖出停靠区或双击标签浮出成窗口、拖浮动窗口的标题栏停回去、双击标题栏回原处，
  都是 MiniGUI 的 `MiniGUI::DockSpace` 做的（浮动窗口是同一窗口里的 `MiniGUI::FloatingPanel` 弹层，不是独立的系统窗口）。右键面板的标签或浮动窗口的标题栏
  （`MiniGUI::DockSpace::SetOnPanelMenu`）弹出菜单（`MainFrame::ShowPanelMenu`）：停靠在左侧 / 右侧 / 底部（`MainFrame::DockPanelTo`：那一侧已有面板就排在最后一个后面，
  按右键处在视图的哪一侧判断面板现在停在哪里，已在那一侧的一项变灰）、浮动（`MainFrame::FloatPanelAt`，300×420）、关闭。
  布局每次改动（`MiniGUI::DockSpace::SetOnLayoutChanged`、菜单、显示 / 隐藏面板）后由 `MainFrame::SavePanelLayout` 写进 settings.json 的 `dock`（`MiniGUI::DockSpace::SaveLayout` 的格式）
  与 `dock_panels`（当时有的面板）；界面描述文件应用之后 `MainFrame::AttachDock` 记下文件里的布局，换语言重建时恢复用户的布局（`MainFrame::RestorePanelLayout`），
  界面描述文件改了重新加载时用文件的。保存后新增了面板、或保存的布局里没有视图时回到文件的布局。设置里"重置面板布局"（`MainFrame::ResetPanelLayout`）回到文件的布局。
  还没有：视图标签栏与视图浮动（BIMCraft 能同时开几个视图、把视图拖出成窗口），浮出成独立的系统窗口（MiniCAD 的 `FloatingPanelHost`，没复制）
  开发时读源码目录里的文件（宏 `MINIBIM_UI_SOURCE`），宿主监视它所在的目录，保存后 `MainFrame::CheckUiFileChanged` 重新加载；写错时保留原界面，状态栏显示原因
- 命令：`MainCommands.cpp` 里注册为 `MiniGUI::Command`，名称和提示写英文原文并用 `N_` 标记，`MiniGUI::UiLayout` 每次应用时翻译；菜单只引用命令 ID。
  标题栏右侧的按钮执行 `window.minimize` / `window.maximize` / `window.close`，这三个命令必须有
- 项目：`MainFrame` 拥有一个 `Bim::Session`（模型与撤销历史，`.agents/features/model.md`），界面只经模型命令改它：
  文件菜单的新建 / 打开 / 保存 / 另存为 / 导出 IFC / 示例房屋（`MainFrame::NewProject`、`MainFrame::OpenProject`、`MainFrame::SaveProject`、
  `MainFrame::ExportIfc`、`MainFrame::OpenSample`）都走
  `MainFrame::RunCommand`（命令失败时弹出对话框，标题翻译、核心给的原因是英文），之后 `MainFrame::OnModelChanged` 重建三维场景、刷新标题与状态栏，
  换了项目时相机重新框住模型、清空选择集。标题栏显示保存过的文件名，否则显示项目名称；有没保存的改动时前面加 `*`
- 开始页（`StartPage.cpp`，移植自 `start.rs`）：盖在标题栏下面的一层（`MainFrame::m_appHost` 里 `m_uiHost` 旁边绝对定位的面板；
  `UIContext::GetRoot` 把子节点都拉满、不排版，所以多套了一层），菜单与快速访问工具栏照常能用。左边"开始"：新建项目、打开、示例房屋，叫到项目上面时还有
  "回到 …"；右边"最近"：最近打开的文件排成卡片（文件名、文件夹 · 多久以前改的 `RecentFiles::Age`，文件不在了写"找不到文件"；单击打开，右键打开或从列表移除），
  一行几张按窗口宽度（`MainFrame::StartColumns`，变了时重建）；底栏是 GitHub 上的源代码与版本。`file.start`（快速访问工具栏第一个，Ctrl+D）
  在项目上叫出、再按回去（`MainFrame::ToggleStartPage`）；换了项目（新建、打开、示例房屋）时让给项目。卡片还没有项目的缩略图
- 自动保存与恢复（`Autosave.cpp`，移植自界面的 `autosave.rs`；写文件的是核心的 `Bim::Autosave::Worker`）：有没保存的改动时每隔 `AutosaveState::minutes` 分钟
  （默认 2，0 不保存，存在 settings.json 的 `autosave_minutes`）写一份恢复副本——保存过的项目写在它旁边（`<项目>.autosave.minibim`），没保存过的写在临时文件夹的
  `MiniBIM/untitled-<GUID>.autosave.minibim`。定时经 `AppPlatform::PostDelayed`（Win32 用系统定时器）。保存成功后（`MainFrame::AutosaveSaved`）、
  换项目或不保存就退出时（`MainFrame::RetireAutosave`）删掉。启动时（`MainFrame::EnableAutosave`，在打开启动的项目之后）找临时文件夹里的副本，打开项目时找它旁边的，
  找到了就问"恢复 / 放弃 / 稍后"（`MainFrame::ShowRecovery`）：恢复经 `recover` 命令打开成没保存的工作（`MainFrame::RecoverBackup`），之后接着写那份副本；
  还没决定的副本不覆盖、不删
- 设置（`Settings.cpp`，移植自 `settings.rs`）：文件 > 设置（`file.settings`）弹出的对话框，左边各页：界面（主题深色 / 浅色 `MainFrame::ApplyTheme`、布局浮动 / 紧凑
  `MainFrame::SetCompactLayout`（同换语言重建界面）、语言、两块面板的显示与重置面板布局）、
  文件（自动保存开关与间隔）。改了马上生效并写 settings.json。主题只换 MiniGUI 的颜色，视口与平面视图不跟着换。BIMCraft 设置里的性能、图形、更新等页随那些功能再加
- 两个字母的快捷键（`Shortcuts.h` 的 `Shortcuts` 管键表、不碰控件；`ShortcutsDialog.cpp`；移植自 `shortcuts.rs` 与 `lib.rs` 的 `SHORTCUTS`）：
  在视图上（或没有焦点时）打字母，最后两个凑成一个快捷键就执行它的界面命令（WA 墙、DR 门、ZF 缩放匹配……）。字母经
  `UIContext::SetUnhandledTextHandler` 到 `MainFrame::OnShortcutText`：没有节点要、没有弹层、不在输入法组合中、不在开始页上时才算；
  画了起点之后先打数字的是长度（`MainFrame::OnPlanText`）。默认的表（`MainFrame::RegisterShortcuts`）是桌面自己的几个加上模型命令的快捷名
  （`Bim::Command` 的 `aliases`）中有界面命令的；改过的存在 settings.json 的 `shortcuts`（键 → 界面命令 ID，空 = 关掉默认的）。
  文件 > 键盘快捷键（`file.shortcuts`，KS）：搜索、删除、指定、恢复默认，从文件读入（`AppPlatform::PickFile` 的 `FileKind::Shortcuts`；JSON、Revit 导出的 XML、
  "WA - Wall" 这样的行，说明按命令名称或 ID 找，`MainFrame::ShortcutCommand`）
- 最近打开（`Recent.h` 的 `RecentFiles`，移植自 `recent.rs`）：文件菜单"最近打开"（动态子菜单 `recent`，`MainFrame::BuildRecentMenu`）列最后打开或保存的
  10 个项目与 IFC 文件，新的在前，同名的后面加文件夹；"清除最近打开的文件"。打开成功（`MainFrame::OpenPath`）、保存成功时 `MainFrame::NoteRecent`；
  打不开且文件已经不在了时从列表去掉。存在用户设置目录的 `recent.json`（`AppPlatform::GetUserDataDir`，空时不记），读不出、坏了、过大都当作空列表
- 没保存的改动：`MainFrame::Dirty` 比较当前模型与最后新建、打开、保存的那一份（快照指针，同 BIMCraft；撤销回到那一份时又算保存过的；导出 IFC 不算保存）。
  新建、打开、示例房屋、退出（含标题栏的关闭）都经 `MainFrame::Guard`：有改动时问保存、不保存、取消
- 编辑：`edit.undo`（Ctrl+Z）、`edit.redo`（Ctrl+Y）走模型命令 `undo` / `redo`；`edit.delete`（Delete）删选择集，`modify.flip` 翻转其中的墙；
  `edit.escape`（Esc，`MainFrame::Escape`）先丢掉输入的长度、画了一半的，再退出工具，最后清空选择集。命令的 `canExecute` 按撤销历史与选择集，模型或选择集变了时 `CommandRegistry::NotifyStateChanged`
- 选择集 `MainFrame::SetSelection`：平面上单击选中、Ctrl + 单击加进或拿出；模型变了时删掉的元素离开选择集（`MainFrame::OnPlanModelChanged`）。
  三维视图里左键单击（没拖动）经 `ModelScene::Pick` 选中，选中的元素画成强调色
- 修改（`Modify.cpp`，移植自 `canvas.rs`、`lib.rs` 的选择与修改部分）：光标下的元素高亮（`MainFrame::Hovered`，几个叠在一起时 Tab 轮换）；
  左键在空白处拖是框选（从左往右窗选、只选整个在框里的，从右往左交叉选，按各元素画出来的外框算），在元素上拖是移动选择集（没选中的先选中，取整到 10 毫米），
  选中一面墙时两端是夹点（`MainFrame::BeginPlanDrag` → `MainFrame::FinishPlanDrag`，拖夹点是 `move` 的 `end`）。别的工具下左键拖还是平移。
  功能区"修改"的修改工具：对齐（先参照线再要对上去的线，`MainFrame::HoveredLine` 在光标附近各元素的 `Align::Lines` 里取最近的）、偏移（墙复制或移到光标一侧，
  距离在选项栏）、镜像（拾取轴 / 绘制轴，默认复制）、拆分（单击墙）、移动、复制（两点，"约束""多个"）、旋转（中心默认在选择集外框的中点，"放置中心"另点；
  两点定角度，或选项栏输入角度按 Enter；"复制"）、修剪 / 延伸为角（两面墙，点在要保留的部分上）。移动、复制、旋转、镜像要先选东西（`MainFrame::NeedsSelection`）。
  预览：选择集画出来的线按要去的地方搬过去（虚影），镜像的轴是点划线，对齐、修剪拾到的线加粗
- 特性面板（`MainFrame::BuildProperties`，移植自 `properties.rs`）：类型选择器（这一类的类型；门窗另有门窗族的"族: 类型"，构件只有族的），"类别（个数）"，
  按组列出参数（行表 `Props::RowsOf`、楼层平面的 `Props::PlanRows`，在 `Parameters.cpp`，只依赖核心库，`tests/App/AppTests.cpp` 测）。
  选中的是一类元素时，取值都一样的才显示；没选东西时是楼层平面（读写它的标高：名称、高程、视图范围），三维视图没有参数。
  改动都是 `modify` 命令（`MainFrame::EditParameter`，`Props::Nested` 按点号嵌套，长度按项目单位读写 `Props::Parse` / `Props::Shown`）：
  输入框按 Enter 或离开时改一次，下拉列表与复选框选了就改；失败时弹窗并把面板重建回原值
- 项目浏览器（`MainFrame::BuildBrowser`，移植自 `browser.rs`）：视图（全部）→ 楼层平面（各标高）、三维视图；族 → 系统族的类型、可载入族的类型。
  双击视图打开它，正在显示的视图选中着；搜索框按词筛（`Props::Words`、`Props::Matches`：不分大小写，`*` 任意一串、`?` 任意一个，组名配上的词里面的项不必再配）。
  重建时各分支照旧展开或收起（按路径记在 `MainFrame` 里）
- 工具（`Tools.cpp`，移植自 `canvas.rs`、`lib.rs` 的放置部分）：`MainFrame::Tool`，功能区"建筑"的 `arch.*` 命令切换（再点一次回到修改），
  在三维视图里选工具先打开平面（标高除外），回三维时退出工具。平面上：单击 `MainFrame::ToolClick`，光标移动时记下位置、`MainFrame::BuildPreview`
  在模型之上画会放下的东西（门窗、构件经 `Render::Sketch`，房间、天花板、楼板、屋顶画轮廓）、捕捉的记号与引导线，读数（长度与角度、面积、门窗离那一段两端）
  是视口上的一个 Label。点经 `MainFrame::SnapAt`（`Snap::At`，范围 10 像素到点、6 像素到线）。墙：单击连成墙链，墙的高度（上面的标高或未连接的高度）、
  定位线、"链"在选项栏；门窗：单击墙，离那一段中点 10 像素内放正中；柱（`arch.column`，CL）：单击处立一根，高度同墙的选项栏（上一层或未连接时的高度，
  `MainFrame::TopParams`），预览经 `Render::Sketch`；构件：空格转 90°；屋顶、楼板建好后选中并回到修改；标高：选项栏输入高程"创建"。
  "结构"选项卡（移植自 `structure.rs`）：结构柱与独立基础是 `MainFrame::Tool::Stand`、梁与条形基础是 `MainFrame::Tool::Run`，类别在 `MainFrame::m_toolOf`
  （`MainFrame::SetToolOf`）；结构柱的高度同柱，独立基础的厚度与顶部偏移在选项栏；run 单击两端、"链"时接着放，
  选项栏的对正与偏移每个 run 工具各记各的（`MainFrame::KeepRunOptions`），条形基础只有顶部偏移；预览经 `StoodOf`、`RunsOf` 做出命令会放的元素再 `Render::Sketch`。
  "楼板：结构"（`struct.floor`，SB）是楼板工具放 `structural` 的楼板（`ToolOptions::floorStructural`）。run 的特性有 Z 轴对正（`Props::Kind::Justification`）、
  终点偏移（空时同起点）、系统；类型属性对话框对 run 的类型另有"圆形"与材质。
  "卫浴""电气""防雷""数据"选项卡（移植自 BIMCraft 界面的 `mep.rs`，命令 `mep.*`）：管道、线管、电缆桥架、导线、引下线、接地导体、综合布线是 Run 工具；
  设备（卫浴装置、管件、管道附件、电气设备、装置、照明、接闪器、接地极、检查井、数据 / 语音插座、机柜、配线架）是构件工具，`m_toolOf` 为设备的键：
  `MainFrame::PlacingFits` 只让它放那几类族（`Tools.cpp` 的 `Fixtures` 表，类型选择器与 `MainFrame::Placing` 都照它），选项栏的标题是设备的；
  建筑的"构件"（`arch.component`）清掉 `m_toolOf`，放任何可载入族。设备与 run 的两字母快捷键在 `ShortcutsDialog.cpp` 的表里
  "体量"选项卡（移植自界面的 `massing_tab`，命令 `massing.*`）：体量工具 `MainFrame::Tool::Mass` 单击底面轮廓的角点（`m_toolPts`），回到第一点或 Enter 时
  `MainFrame::FinishMass` 按选项栏的顶部（`ToolOptions::massTop`、`massHeight`，同墙）执行 `mass`；按面建模的五个按钮（体量楼层、面墙、面楼板、面屋顶、更新到面）
  只在选中了体量时可用（`MainFrame::SelectedMasses`），对每个选中的体量执行一次命令（`MainFrame::ByMass`）。显示体量随视图的图形覆盖移植
  "注释"选项卡（移植自界面的 `annotate_tab`，命令 `annotate.*`，工具在 `Annotate.cpp`，`MainFrame::AnnotateTool`）：对齐、角度、径向尺寸单击参照
  （`MainFrame::PickReference` 经 `ReferenceNear` / `ArcNear`，拾取的记在 `m_toolRefs`，虚线显示），够了之后在空处单击放尺寸线（`MainFrame::DimensionAt` 做预览）；
  高程点、文字（选项栏输入文字、选文字类型）、按类别标记（单击有点可指的元素，选项栏可加引线，"全部标记"即 `MainFrame::TagAll`）；
  详图线同墙链（选项栏选线样式），测量两下一把尺子、读数在光标旁。填充区域与遮罩区域是草图模式（`SketchOf::Region`、`SketchOf::Masking`），
  完成时执行 `filled_region`（几个环用 `Sketch::Solve`），遮罩的用有 `masking` 的类型、不画边界线。注释的特性面板只读出它的平面与值，文字与标记的引线可改
  "视图"选项卡（命令 `view.*`）："图形"面板的可见性 / 图形（`MainFrame::OpenVisibilityGraphics`，`Dialogs.cpp`：跟随的视图样板、类别的勾选、平面的比例与详细程度、
  项目过滤器的套用与可见，样板控制的灰掉，改了立即执行命令）、视图过滤器（`MainFrame::OpenViewFilters`）、从当前视图创建样板（`MainFrame::OpenCreateTemplate`）
  与裁剪视图（`MainFrame::ToggleCrop`）；"创建"面板的复制视图、带细节复制、复制为相关视图（`MainFrame::DuplicatePlan`，之后显示新的平面视图）
  与明细表/数量（`MainFrame::OpenNewSchedule`，之后 `MainFrame::OpenSchedule` 显示表格，点行首选中它列出的元素；这三个对话框在 `Schedules.cpp`）。
  项目浏览器的"明细表/数量"列出六张内置的与项目建的明细表；界面还不能导出 CSV（要新的保存对话框种类，`schedule` 命令的 `path` 可以）。
  显示的平面是 `MainFrame::PlanKey`：标高的平面，或 `m_planOf` 指的平面视图（`MainFrame::ShowPlanView`）；绘制、注释工具与填充区域都用它。
  项目浏览器的楼层平面下列出各标高的平面视图，相关视图挂在父视图下；平面视图的特性面板是 `Props::PlanViewRows`
  剖面与立面："创建"面板的剖面（`MainFrame::Tool::Section`，点两下执行 `section`，可输入长度）与建筑立面（`view.elevation`，`MainFrame::MakeElevations`）；
  项目浏览器的"立面""剖面"双击打开（`MainFrame::ShowSection`，`m_sectionOf`）。剖面视口 `MainFrame::RenderSection`：模型变了时重算 `ModelScene::Cut`，
  白色的面与往相机挪一点的边画成消隐线，上面盖 `Render::DrawSection` 的显示列表（剖到的、标高、轴网、名称）；`MainFrame::OnSectionPointer` 拖动平移、
  滚轮缩放、单击剖到的选中它。可见性 / 图形对话框对剖面也有比例与详细程度。看到的形体由 `MainFrame::SubmitSectionFaces` 画（图纸上的剖面也用它）
  图纸："图纸组合"面板的图纸（`view.sheet`，`MainFrame::NewSheet` 执行 `sheet` 后显示它）与放置视图（`view.placeView`，显示图纸时才可用，
  `MainFrame::OpenPlaceView` 列出还没放上图纸的视图，在 `Schedules.cpp`）；项目浏览器的"图纸（全部）"按编号列出图纸，下面是它上面的视图。
  图纸视口（`PlanView.cpp` 末尾）：`MainFrame::ShowSheet`（`m_sheetOf`），`MainFrame::RenderSheet` 先画白纸，再按 `Render::Frames` 把剖面看到的形体
  （`m_sheetCuts`，场景变了时重算）与三维视图（场景的三角形，按视图的方位角与仰角正交投影到框里）画进各框，最后盖 `Render::DrawSheet` 的显示列表；
  `MainFrame::OnSheetPointer` 拖动平移、滚轮缩放、单击选中视口（`MainFrame::SheetPick`：显示列表里归它的或在它的框里），拖动选中的视口执行 `move`。
  切换视图的 `Show*` 都调 `m_commands.NotifyStateChanged`，按视图可用的命令（裁剪视图、放置视图）跟着变
  画了起点后在视图里直接输入长度按 Enter（`MainFrame::PlaceTyped`），视口的按键经 `ViewportHost::SetOnKey` / `SetOnText`。
  没选东西时特性面板显示工具要放的"新建 …"，类型选择器选的是工具接下来放的类型（`ToolOptions::types`、门窗的族、构件的族与类型）
- 草图模式（`SketchMode.cpp`，移植自界面的 `sketch.rs`）：`MainFrame::m_sketch` 有值时在草图里。入口是"修改 > 编辑边界"（`modify.editBoundary`，
  选中一块楼板、天花板、迹线屋顶或填充区域时，`MainFrame::EditBoundary` 经 `edit_boundary` 取线）与楼板、天花板、屋顶工具选项栏上的"边界线"（新建）。
  `MainFrame::StartSketch` 换到功能区"修改"（按译文标题 `Ribbon::FindTab`）与直线工具；草图线（品红）只在 `MainFrame` 里，完成（`sketch.finish`，
  `MainFrame::FinishSketch`）时先用 `Sketch::SolveOne` 解，解不出就弹窗逐条说问题（`MainFrame::FriendlyProblems`）并把涉及的线标红，
  解得出才执行 `edit_boundary`、`floor`、`ceiling`、`roof` 的 `sketch`（保留的线带 `id`），然后选中结果回到修改；取消（`sketch.cancel`）丢掉草图。
  画线工具 `Tool::SketchLine`（逐点，回到起点闭合，可输入长度）、`Tool::SketchRect`、`Tool::SketchPick`（沿墙的定位线，弧墙为圆弧），点经捕捉，
  也捕捉草图自己的线（`Snap::At` 的 `extraLines`、`extraPoints`）。修改工具下单击选线（Ctrl 加减）、空白处拖框选、拖选中线的端点（那一点上的线端一起动）；
  `edit.delete`、`edit.undo` 在草图里删线、退回草图的上一步（`MainFrame::UndoSketch`），重做与放东西、修改的工具灰掉；不能离开草图的平面
  （`MainFrame::LeavingSketch`），换项目时丢掉草图
- 选项栏（`MainFrame::BuildOptionsBar`）在换工具、换语言时重建（草图模式里是标题、完成与取消，新建天花板、屋顶时还有高度偏移、坡度），输入框里的文字存进 `MainFrame` 的 `ToolOptions`，用时再读（同 BIMCraft）
- 对话框（`Dialogs.cpp`，移植自 `typeprops.rs` 与 `properties.rs` 的对话框）：都是 `MiniGUI::Dialog`，状态放在一个 `shared_ptr` 里，
  内容按状态整个重建（经 `AppPlatform::Post`，不在控件自己的回调里拆掉它）；命令经 `MainFrame::RunQuiet` 执行，失败时原因显示在对话框里而不弹窗。
  类型属性（`MainFrame::OpenTypeDialog`）：族（系统族的类别）与类型，复制…、重命名…；类型参数（宽度、高度、窗台、板块尺寸……）；墙、楼板、屋顶、天花板的层
  （功能、材质、厚度，向外 / 向内或向上 / 向下、删除、插入层，改了层时厚度由层定）；确定、取消、应用，都经 `type`、`rename_type`。
  族类型（`MainFrame::OpenFamilyDialog`）：族的类型参数，公式算出的只显示；经 `family`、`rename_type`。确定后选中的元素换成显示的类型，没选东西时工具改放它。
  打开哪个由 `MainFrame::CurrentTypeSubject` 定：选中的一类元素的同一个类型，或工具要放的类型；入口是"修改 > 类型属性"、特性面板的"编辑类型"、
  项目浏览器里双击类型（右键还有删除，经 `delete_type`）。材质（`MainFrame::OpenMaterials`，"管理 > 材质"）：颜色、透明度（三维视图还不画半透明）、截面与表面图案，每个改动是一条 `material` 命令。
  "管理"选项卡的其余对话框在 `ManageDialogs.cpp`，每个改动是一条模型命令、失败原因写在对话框里：项目参数（`MainFrame::OpenProjectParameters`：名称、种类、类别）、
  对象样式（`MainFrame::OpenObjectStyles`：各类别看到的与剖到的笔）、线样式（`MainFrame::OpenLineStyles`：笔、颜色、线型图案）、线宽（`MainFrame::OpenLineWeights`：
  选一个比例看模型的 16 支笔、注释的 16 支笔，加删比例）、填充图案（`MainFrame::OpenFillPatterns`：制图或模型、各族线的角度与间距，导入导出 PAT 经
  `FileKind::FillPatterns` / `SaveKind::FillPatterns`）、清除未使用（`MainFrame::OpenPurge`：勾选种类，`dry_run` 列出，再清）。
  特性面板在选中的元素下列出它类别的项目参数（"项目参数"组：是 / 否为复选框，其余写着改，清空即去掉值），改时 `modify {ids, params: {名称: 值}}`
- 两块面板在模型、选择集、视图、语言变了时重建（`MainFrame::RefreshPanels` 经 `AppPlatform::Post` 推到当前消息之后，合并成一次；不在控件自己的回调里拆掉它）。
  控件回调里要改模型的，也经 `AppPlatform::Post` 再执行命令
- 文件对话框经 `AppPlatform::PickFile`（异步：选好时回调，取消时不回调）与 `AppPlatform::ChooseSavePath`
  （Win32 用 `GetOpenFileNameW` / `GetSaveFileNameW`，筛选器文字经 `Tr`）。所以 `MainFrame::SaveProject` 不返回结果，保存成功后做传进来的 `then`
  （`MainFrame::Guard` 的"保存"之后接着新建 / 打开 / 退出）；`MainFrame::AskSavePath` 在平台没有系统另存为对话框（`AppPlatform::HasSaveDialog`，网页版）时
  自己问文件名。每写完一个文件（保存、导出 IFC）调用 `AppPlatform::OnDocumentSaved`（网页版下载到本机）。
  `AppPlatform::HasBackgroundThread` 为 false 的平台不开自动保存（恢复副本在工作线程上写）。
- 宿主：Windows 为 `Win32Window`（`apps/Win32`，无边框窗口、自绘标题栏）；Linux 为 `LinuxApp`（`apps/Linux`，SDL3，复制自 MiniCAD）：
  系统的标题栏和边框（`AppPlatform::HasWindowControls` 为 false，不注册 `window.*` 命令，标题栏不显示那三个按钮），
  文件对话框经 SDL3（xdg-desktop-portal / zenity，打开是异步的），`PostDelayed` 用 SDL 定时器，界面字体用自带的 `assets/fonts/NotoSansSC-UI.ttf`。
  网页版为 `WebApp`（`apps/Web`，Emscripten，复制自 MiniCAD）：没有窗口按钮，打开时浏览器的文件选择框把文件读进内存文件系统 `/upload`，
  `AppPlatform::HasSaveDialog` 为 false（`MainFrame::AskSavePath` 问文件名），写到 `/download` 后 `AppPlatform::OnDocumentSaved` 下载；
  没有线程（不自动保存），`AppPlatform::GetUserDataDir` 为空（不记住设置与最近打开）；有未保存的改动时关闭标签页前浏览器询问（`MainFrame::HasUnsavedChanges`）。
  本机看效果：`build_wasm.bat serve`，或在内置浏览器里打开构建目录的 `index.html` 所在的本地服务器。
  新加平台服务时三个宿主都要实现（Linux 版没有显示器时在 Xvfb 里运行、截图，见 `.agents/docs/troubleshooting.md`）打开可选 `.minibim`、`.bimcraft`、`.ifc`（IFC 作为新项目，没有路径），有 Revit 读取器时还有 `.rvt`（`FileKind::ProjectOrRevit`），这些文件也可以拖进窗口（`MainFrame::OpenFiles`）；
  保存只直接写回 `.minibim`：打开的是 BIMCraft 的 `.bimcraft` 或 IFC 时"保存"也走另存为，免得把略去了内容的项目写回原文件；
  打开时有略去的内容（项目文件的 `left_out`、IFC 的 `skipped`）会弹窗说几个。"导出 IFC…"（`file.exportIfc`）是 `save` 到 `.ifc`，不改项目的路径
- Revit 文件（`Revit.cpp`，移植自 `revit.rs`；不碰控件的部分在 `RevitReport.h`，`AppTests` 测）：读取器由宿主接上（`MainFrame::SetRevitReader`，
  见 `.agents/features/rvt.md`），有它时打开对话框是 `FileKind::ProjectOrRevit`（多一个 `.rvt`）。`MainFrame::OpenPath` 遇到 `.rvt` 转给 `MainFrame::OpenRevit`：
  有后台线程时在分离的 `std::thread` 上读（结果放进 `MainFrame::RevitRead`，读完经 `AppPlatform::Post` 叫 `MainFrame::FinishRevit`；退出时 `MainFrame::AbandonRevit`
  不等它），网页版没有线程，提示框画出来之后（`MainFrame::RenderFrame` 末尾）在界面线程上读（`MainFrame::ReadRevit`）。读的时候一个模态弹层盖住窗口
  （"正在打开 Revit 文件"，Esc 不关；模态弹层也挡住快捷键），一次读一个。读取器抛出的异常当作读不了。读进来的换掉项目（`Session::Reset`，没有路径、不进最近打开，
  保存时问存成哪个 `.minibim`），状态栏说"已从 Revit 2024 打开：N 个图元，未导入 M 个"（`RevitReport::StatusLine`，模型还是那一份、没拿工具时）；
  导入有说明时弹出导入报告（`MainFrame::ShowRevitReport`：按类别进来了多少，未导入的与备注，元素 id 在下面，"复制报告"复制成纯文本 `RevitReport::Text`），
  之后在"文件 > Revit 导入报告"（`file.revitReport`，项目换了就灰掉）。启动参数给的 `.rvt` 先显示开始页，再在提示框下读
- 拖进窗口的文件（R7，移植自 BIMCraft `lib.rs` 的 `opens`）：宿主收到后交给 `MainFrame::OpenFiles`（桌面版 `WM_DROPFILES`，Linux 版 SDL 的
  `SDL_EVENT_DROP_FILE`，网页版在页面上放下时读进内存文件系统 `/upload/drop-<次数>/`）。文件排进 `OpenQueue`（`OpenQueue.h`，`AppTests` 测）：
  没在读 Revit 文件、没有模态对话框时 `MainFrame::TakeOpens` 取最后一个，其余作废，经 `MainFrame::Guard` 先问未保存的改动再 `MainFrame::OpenPath`；
  忙的时候等着，`MainFrame::RenderFrame` 末尾见到空下来了再安排。认不出的文件同"打开"一样报错
- 动态菜单：`{ "title": "Language", "dynamic": "languages" }` 由 `MainFrame::BuildLanguageMenu` 生成（跟随系统 + 词典目录里的每种语言，名称用那种语言自己写）
- 帧顺序（同 MiniCAD）：输入到达时立即改状态并 `RequestRender` → 消息队列空了收到 `WM_PAINT` → `MainFrame::RenderFrame`：`UIContext::Update` 布局 →
  `ViewportHost::RenderContent` 按最新尺寸渲染视口 → `AppPlatform::BeginUiPass` → `UIContext::Render` → `AppPlatform::EndUiPass`（呈现）
- 用户设置：`%LOCALAPPDATA%\MiniBIM\settings.json`（`AppPlatform::GetUserDataDir`），有 `language`（空 = 跟随系统）、`theme`（`dark` / `light`）、`layout`（`floating` / `compact`）、`autosave_minutes`、`shortcuts`、`dock` 与 `dock_panels`（面板布局）；`MainFrame::LoadSettings` / `MainFrame::SaveSettings`

## 约束与坑

- `apps/Common` 里不写 `#ifdef`：需要新的平台能力就在 `AppPlatform` 加虚函数，在 `Win32Window` 里实现（以后的 Web、Linux 宿主各实现一份）
- 切换语言（布局同样）要重建界面：`MainFrame::SetLanguage` → `MainFrame::RetranslateUi` → `MainFrame::ReloadUi`（界面描述生成的部分整体重建，命令名称重新翻译），
  宿主面板里的文字由 `MainFrame::UpdateStatus` 之类按当前语言重写。新加的宿主面板如果显示文字，也要在 `RetranslateUi` 里刷新
- 菜单项的回调里不要直接重建界面：经 `AppPlatform::Post` 放到当前消息之后（语言菜单就是这样做的）
- 状态栏、对话框里的句子用 `Format(Tr(…))`，标点也随语言（`TrC("list of four items", "{0}, {1}, {2}, {3}")`）
- 视口有两种：三维（`ModelScene`）与一个标高的平面（`PlanView.cpp`）。"视图"菜单的 `view.3d`、`view.plan`、`view.levelUp`（PageUp）、
  `view.levelDown`（PageDown）切换；平面由核心库的 `Bim::Render::Draw` 按 `MainFrame` 存的视图（比例、中心）画成显示列表，填色的环剖成三角形、
  折线（含虚线）展开成带宽度的四边形，按画的先后一起交给 `IRenderer::Submit`（后画的盖住先画的：门的缺口盖住墙的填色）。平面画在白纸上，
  不随界面主题；拖动平移（左键拖出几个像素、中键、右键）、滚轮以指针处为中心缩放（都要重画显示列表）、单击左键拾取（`Drawing::Pick`），
  选择集里的元素用强调色，状态栏写平面名与选中的元素（几个时写个数）。模型变了（`OnModelChanged` → `MainFrame::OnPlanModelChanged`）下次画时重画；显示的标高删了回到最低的
- BIMCraft 的修剪 / 延伸单个与多个元素、添加与删除顶点、详图线的偏移与拆分随详图线移植；墙工具的其他画法（矩形、多边形、圆、弧、拾取线）、两个字母的快捷键（WA、DR，3e）、
  门窗与构件的临时尺寸、捕捉设置对话框、拉伸屋顶、老虎窗等还没移植
- 调试版是控制台程序：`Process.MainWindowHandle` 拿到的是控制台窗口，找主窗口要按类名 `MiniBIMWin`（skill `ui-check` 的脚本）
- 图标：程序图标编在 exe 里（`MiniBIM.rc` 的 `IDI_ICON`，`Win32Window::InitWindow` 从资源加载，不依赖当前目录），标题栏图标是界面描述文件 titlebar 的 `"icon": "app.png"`，
  命令图标写在 `commands` 里；全部由 `tools/make_icons.py` 生成（`.agents/features/icons.md`）。菜单项不显示图标

## 测试与排查

- 界面效果：skill `ui-check`，中英文各截一张。对话框可以用 `-Run` 直接打开（如 `-Run arch.wall,modify.typeProperties`、`-Run manage.materials`、`-Run manage.objectStyles`）。截图脚本不能点视图，选中元素后的特性面板、工具的预览要靠手动看
  （或临时加几行在第一次画平面后选上元素、选工具、调 `MainFrame::ToolClick` 再截，截完删掉）
- `AppTests`：参数表、值的写法与读法、搜索，以及示例房屋里每个元素的每个可写参数照面板的写法交给 `modify` 都被接受；
  Revit 导入报告（`RevitReport`：认出 `.rvt`、数略去了几个、拆出 id、按类别计数、状态栏那一句、复制的纯文本）；拖进窗口的文件排队（`OpenQueue`）
- 界面描述文件写错：状态栏和控制台日志（`LOG_ERROR`）给出行列
- 初始化失败（返回 2）：看控制台日志里的图形接口一行；`--gpu warp` 用软件设备排除显卡问题
