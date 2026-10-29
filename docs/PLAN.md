# MiniBIM 开发计划

MiniBIM 把 [BIMCraft](https://github.com/IPedrax/BIMCraft)（Rust，约 13.7 万行）移植为 C++20，界面、渲染、构建、代码风格沿用
[MiniCAD](https://github.com/CHMOSE023/MiniCAD)，几何内核用 SMLib。许可与来源见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。

基准：BIMCraft `f3bbb33`（2026-10-28），MiniCAD `0bd1687`（2026-10-29）。

## 第 1 阶段：骨架（已完成）

- 构建：CMake + Ninja + MSVC，预设 `debug` / `release`，`build.bat`、`run_tests.bat`；静态运行库，复制自 MiniCAD 的 `cmake/`
- 界面：MiniGUI（`src/UI`），界面描述文件 `assets/ui/minibim_ui.json`，自绘标题栏 + 菜单栏 + 三维视口 + 状态栏
- 渲染：Windows 的 D3D12 / D3D11（WARP 回退），`apps/Render`
- 几何：SMLib（`3rd/SMLib`）经 `Solid::ISolidKernel`；临时的演示场景检验内核与视口（第 2a 步换成了模型的三维场景 `ModelScene`）
- 多语言：PO 词典 + `Tr` / `TrN` / `TrC`，MiniGUI 与界面描述文件同样翻译，运行时切换（[i18n.md](i18n.md)）；简体中文、英文
- 图标：`tools/make_icons.py` 生成全部图标（照 MiniCAD 的同名脚本）：程序图标 `app.ico` / `app.png`，现有命令的图标
- 测试：`I18nTests`、`ViewTests`、`SMLibSmokeTests`
- CI 与发布：`.github/workflows/build.yml`（Windows 编译、测试、打包；Linux 编译核心库与测试；词典与图标检查；推送 `v*` 标签时发布）
- AI 编程助手：`AGENTS.md` 与 `.agents/`（skill `test`、`ui-check`、`i18n`，功能指南，排错表），结构照 MiniCAD

## 第 2 阶段：模型核心（移植 `crates/core`）

BIMCraft 的核心与界面分得很清楚：所有编辑都是 `commands.rs` 里的命令，由 `engine.rs` 的 `Session` 执行（回滚、撤销），
界面、命令行、脚本都只调命令。照这个结构移植，先做不依赖界面的部分，每一步都带上对应的测试（BIMCraft 的 `*_tests.rs`）。

建议顺序（括号里是 BIMCraft 的行数，供估计工作量）：

1. 基础：`units.rs`（317）、`ids.rs`（495，元素 ID 与 GUID）、`geom.rs`（493，平面几何）
2. 模型：`model.rs`（5432，元素、类型、`Model::check`）、`library.rs`（1531，类型、材质、族）
3. 墙：`walls.rs`（836）与 `walls/`：墙轴线、墙面、斜接、轮廓
4. 命令与撤销：`commands.rs`（8740）、`engine.rs`（346）。C++ 里对应 MiniCAD 的 `ICommand` / `CommandStack` 思路，但以 BIMCraft 的命令表为准
5. 文件：项目文件的保存 / 打开；`sample.rs` 的示例房屋（替换掉第 1 阶段的演示场景）
6. 三维：`view3d.rs`（2694）的场景生成改为经 SMLib 出网格、经 `IRenderer` 绘制（BIMCraft 自带的软件光栅化不移植）
7. 平面视图：`render.rs`（4080，显示列表、拾取）→ 视口里的二维绘制
8. 交换：`ifc.rs`（1795，导出）、`import.rs`（3911，导入）、`step.rs`（408）；再往后是明细表、出图、DXF 导入等

### 进度

上面 1–8 一起移植是几万行 C++（`model.rs` 的元素引用了族、视图图形、阶段、贴图等十几个模块，示例房屋几乎用到全部命令），
所以按元素种类分段做，每段都能编译、测试、单独提交：

- **2a（已完成）先接上一部分模型，替换演示场景**：`units`、`ids`、`geom`；模型子集（标高、轴网、墙、楼板、不带族的门窗，类型与材质，
  `Model::Check` 里与它们有关的规则）；`library` 的 ID 与 GUID（类型、材质、类型的层）；`walls` 与 `walls/index`；`Session`（快照撤销、回滚、脚本、事务）
  与命令 `new`、`open`、`save`、`undo`、`redo`、`level`、`grid`、`wall`、`door`、`window`、`floor`、`type`、`material`、`flip`、`modify`、`move`、`copy`、`delete`、
  `units`、`inspect`、`elements`；项目文件（`.minibim`，与 `.bimcraft` 同一格式，不认识的内容略去并列出）；示例房屋（已移植命令能做的部分）；
  三维场景经 SMLib 建实体（`ModelScene`）；界面的文件菜单（新建、打开、保存、另存为、示例房屋）。说明见 `.agents/features/model.md`
- **2b（已完成）屋顶与房间**：`skeleton.rs` 直骨架；屋顶（迹线屋顶：平的、四坡、山墙、天井，拉伸屋顶，开洞，插进别的屋顶）与命令 `roof`，
  墙顶贴屋顶；`ceiling`；`room`（由四周的墙与分隔线找区域、面积、周长）、`room_separator`、`room_area`（在墙中心线或墙面量）；
  `floor`、`roof` 沿墙铺时里面有房间的一圈墙算室内；示例房屋加上屋顶、四个房间与天花板；三维场景加屋顶与天花板。
  留到后面的：画轮廓的草图（`sketch.rs`、`curves.rs`，即"编辑边界"，随 2d 的平面编辑）、老虎窗 `dormer` 与按体量面建屋顶（`roof_by_face`，要体量）、
  竖井（`shaft`），以及 `view3d::tops_at`（某点上各元素顶面的高度，随 2d 的高程点等）
- **2c（已完成）族与构件**：`family.rs`（公式、参数、形体的扫掠与旋转、检查）、`csg.rs`（空心形体的网格布尔运算）、`fixtures.rs` 与新项目带的
  34 个族；构件（命令 `component`）、带族的门窗（`door`、`window` 的 `family`）、命令 `family`（新建、修改、改名、删除、从文件读、存成文件）；
  库里的族、族类型、族参数与族的部件，文件里族参数按 GUID；示例房屋用族的门窗并放上十一件家具；三维场景画族的形体。
  留到后面的：收藏夹（`favorite`）、镜像与旋转命令
- **2d（已完成）平面视图**：`render.rs` 平面图的核心（墙的剖切填充与并集边线、复合墙的层线、门窗与族的平面线、房间标签、轴网、屋顶的脊线与上层屋顶的虚线、
  楼板与坡度箭头、构件的俯视轮廓）、拾取，`pen.rs` 的线宽表、`styles.rs` 的默认对象样式，`font.rs` 的单线字；界面"视图"菜单切换三维与各标高的平面
  （`view.plan`、`view.levelUp`、`view.levelDown`），平面里平移、缩放、单击选中。
  留到后面的：视图的图形覆盖（`graphics.rs`）、填充与线型图案（`fills.rs`、`lines.rs`）、裁剪范围、注释（尺寸、标记、文字、高程点）、剖面与立面、
  反射天花板平面、图纸与出图（PDF / SVG / PNG）、草图编辑（编辑边界）
- **2e（已完成）交换**：`step.rs`（STEP 文件的读法）、`ifc.rs`（IFC4 导出：空间结构、墙楼板天花板房间屋顶、构件与族门窗的形体与材质、洞口、
  材质层组、基本数量）、`import.rs`（IFC2x3 / IFC4 导入：按几何量出墙、门窗、楼板、天花板、屋顶、房间、轴网、房间分隔线，家具器具成为族的构件，
  层与材质颜色，GlobalId 成为 GUID）；命令 `open`、`save` 读写 `.ifc`，界面"打开"可选 IFC、文件菜单"导出 IFC…"。说明见 `.agents/features/ifc.md`。
  留到后面的：柱、楼梯、坡道、栏杆、管井、体量、管线与系统的导出导入（随这些元素移植），阶段、设计选项、项目参数的属性集
- 再往后：结构、设备、明细表、出图、DXF 导入等

每个含移植代码的源文件开头写一行来源，例如：

```cpp
// 移植自 BIMCraft crates/core/src/walls.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
```

不移植：`crates/rvt`（Revit 读取器，只有 Apache-2.0，且依赖对 Revit 格式的逆向）、`crates/update`（自动更新，以后按 MiniCAD 的发布方式另做）。

## 第 3 阶段：界面（移植 `crates/ui-egui`）

`ui-egui`（约 4.5 万行）基于 egui，不能逐行翻译，要在 MiniGUI 上重写：功能区（`Ribbon`）、特性面板（`PropertyGrid`）、
项目浏览器、平面画布与工具、剖面与图纸。MiniCAD 的 `apps/Common` 有现成的写法可参考（停靠面板、命令行、视图方块）。

界面要用的图标（功能区、工具栏、项目浏览器、属性面板……）**全部由 `tools/make_icons.py` 自动生成**：移植一个命令就在脚本里给它画一个，
不用 BIMCraft 的图标，也不手画或拷图片（做法见 `.agents/features/icons.md`）。

BIMCraft 的界面译文（`translations/ui/zh.po`、`es.po`、`pt.po`）在界面移植之后合并进 `assets/i18n`：两边都以英文原文为键，
同样的原文可以直接沿用译文。

`ui-egui` 里很多界面属于核心还没移植的功能（结构、设备、体量、注释、剖面立面、图纸、明细表、阶段、视图图形、链接、点云、CAD 导入……），
随那些功能一起做。照第 2 阶段的做法分段，每段都能编译、测试、单独提交：

- **3a 外壳**：功能区（`ribbon.rs`）与停靠区（`dock.rs` 的特性、项目浏览器两块面板），快速访问工具栏（保存、撤销、重做），
  多选的选择集（单击、Ctrl 单击、Esc）、删除、翻转墙；项目浏览器（`browser.rs`：视图——各标高的平面与三维视图，族——系统族的类型、
  可载入族的类型，搜索）；特性面板（`properties.rs`：类型选择器、按类别分组的实例参数，多选时取值相同的才显示，改动经 `modify` 命令；
  没选东西时显示平面视图对应的标高）；有未保存的改动时新建、打开、退出先问
- **3b 平面上放东西**：工具状态机（`Tool`）与选项栏，墙链、门、窗、房间、标高、轴网、楼板、屋顶、天花板、构件、房间分隔线；
  捕捉（`snap.rs` 端点、中点、交点、垂足）、临时尺寸、橡皮筋预览；功能区"建筑"选项卡
- **3c 改东西**：框选、Tab 切换；移动、复制、旋转、镜像、对齐、修剪 / 延伸、拆分、偏移；墙端点与门窗的夹点拖动；三维视图里拾取
- **3d 对话框**：类型属性（`typeprops.rs`，复合墙与楼板的层）、材质、族类型；草图模式的"编辑边界"（随核心的 `sketch.rs`）
- **3e 程序**：开始页（`start.rs`）、最近打开（`recent.rs`）、自动保存（`autosave.rs`）、设置（`settings.rs`）、
  两个字母的快捷键（`shortcuts.rs`）；合并 BIMCraft 的界面译文

## 第 4 阶段：平台

- 网页版（Emscripten，WebGPU / WebGL2）与 Linux 版（SDL3，Vulkan / GLES3）：从 MiniCAD 复制 `apps/Web`、`apps/Linux`、
  `apps/Render` 里对应的后端与 `cmake/Render.cmake` 的段落，`AppPlatform` 各实现一份
- CI：照 MiniCAD 的 `build.yml` 加上网页版（Emscripten）与 Linux 版（SDL3）的编译、打包与发布，网页版部署

## 与 MiniCAD 的差异

复制过来的代码在 MiniBIM 里做过这些改动，以后从 MiniCAD 同步时留意：

- 命名空间 `MiniCAD` → `MiniBIM`，宏 `MINICAD_*` → `MINIBIM_*`（MiniGUI 的命名空间不变）
- MiniGUI：新增 `Core/Translate.h`（`MiniGUI::Tr`、`SetTranslator`）；`UiLayout` 每次应用时翻译界面描述文件里的文字与命令名称、
  提示、面板标题；`Binding`、`NumberBox`、`ColorPicker`、`CommandConsole`、`CommandUI`、`Dialog`、`Ribbon`、`TabView`、`TextBox`、
  `TitleBar` 里写死的中文改成英文原文 + `Tr`（菜单文字去掉了助记符 `&`，不显示下划线和 `(X)`）；`Win32Fonts` 加了韩文回退字体
- `cmake/Render.cmake` 只保留 Windows；`apps/Win32` 的宿主精简（没有浮动窗口、自测）；`AppPlatform` 精简并加了 `GetSystemLanguage`，
  文件对话框换成项目文件用的 `PickProjectFile`、`ChooseProjectSavePath`
- 没有复制：MiniDWG、节点图、天正、CAD 实体 / 场景 / 编辑器（`src/Core/Entity`、`src/Scene`、`src/Document`、`src/Editor`、`src/Viewport`）
