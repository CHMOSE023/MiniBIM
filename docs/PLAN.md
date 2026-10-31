# MiniBIM 开发计划

MiniBIM 把 [BIMCraft](https://github.com/IPedrax/BIMCraft)（Rust，约 13.7 万行）移植为 C++20，界面、渲染、构建、代码风格沿用
[MiniCAD](https://github.com/CHMOSE023/MiniCAD)，几何内核用 SMLib。许可与来源见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。

基准：BIMCraft `f3bbb33`（2026-10-28，已同步 `0733fd4` 的部分修复，见下面"同步 BIMCraft 新版"），MiniCAD `0bd1687`（2026-10-29）。

## 第 1 阶段：骨架（已完成）

- 构建：CMake + Ninja + MSVC，预设 `debug` / `release`，`build.bat`、`run_tests.bat`；静态运行库，复制自 MiniCAD 的 `cmake/`
- 界面：MiniGUI（`src/UI`），界面描述文件 `assets/ui/minibim_ui.json`，自绘标题栏 + 菜单栏 + 三维视口 + 状态栏
- 渲染：Windows 的 D3D12 / D3D11（WARP 回退），`apps/Render`（第 4 阶段加上 Linux 的 Vulkan / GLES3、网页的 WebGPU / WebGL2）
- 几何：SMLib（`3rd/SMLib`）经 `Solid::ISolidKernel`；临时的演示场景检验内核与视口（第 2a 步换成了模型的三维场景 `ModelScene`）
- 多语言：PO 词典 + `Tr` / `TrN` / `TrC`，MiniGUI 与界面描述文件同样翻译，运行时切换（[i18n.md](i18n.md)）；简体中文、英文、西班牙语、葡萄牙语
- 图标：`tools/make_icons.py` 生成全部图标（照 MiniCAD 的同名脚本）：程序图标 `app.ico` / `app.png`，现有命令的图标
- 测试：`I18nTests`、`ViewTests`、`SMLibSmokeTests`
- CI 与发布：`.github/workflows/build.yml`（Windows 编译、测试、打包；Linux 编译核心库与测试；词典与图标检查；推送 `v*` 标签时发布；第 4 阶段加上 Linux 版与网页版）
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
  留到后面的：画轮廓的草图（3e 已移植直线与圆弧）、老虎窗 `dormer` 与按体量面建屋顶（`roof_by_face`，要体量）、
  竖井（`shaft`），以及 `view3d::tops_at`（某点上各元素顶面的高度，随 2d 的高程点等）
- **2c（已完成）族与构件**：`family.rs`（公式、参数、形体的扫掠与旋转、检查）、`csg.rs`（空心形体的网格布尔运算）、`fixtures.rs` 与新项目带的
  34 个族；构件（命令 `component`）、带族的门窗（`door`、`window` 的 `family`）、命令 `family`（新建、修改、改名、删除、从文件读、存成文件）；
  库里的族、族类型、族参数与族的部件，文件里族参数按 GUID；示例房屋用族的门窗并放上十一件家具；三维场景画族的形体。
  留到后面的：收藏夹（`favorite`）、镜像与旋转命令
- **2d（已完成）平面视图**：`render.rs` 平面图的核心（墙的剖切填充与并集边线、复合墙的层线、门窗与族的平面线、房间标签、轴网、屋顶的脊线与上层屋顶的虚线、
  楼板与坡度箭头、构件的俯视轮廓）、拾取，`pen.rs` 的线宽表、`styles.rs` 的默认对象样式，`font.rs` 的单线字；界面"视图"菜单切换三维与各标高的平面
  （`view.plan`、`view.levelUp`、`view.levelDown`），平面里平移、缩放、单击选中。
  留到后面的：视图的图形覆盖（`graphics.rs`）、填充与线型图案（`fills.rs`、`lines.rs`）、裁剪范围、注释（尺寸、标记、文字、高程点）、剖面与立面、
  反射天花板平面、图纸与出图（PDF / SVG / PNG）、草图编辑（编辑边界，3e 已做）
- **2e（已完成）交换**：`step.rs`（STEP 文件的读法）、`ifc.rs`（IFC4 导出：空间结构、墙楼板天花板房间屋顶、构件与族门窗的形体与材质、洞口、
  材质层组、基本数量）、`import.rs`（IFC2x3 / IFC4 导入：按几何量出墙、门窗、楼板、天花板、屋顶、房间、轴网、房间分隔线，家具器具成为族的构件，
  层与材质颜色，GlobalId 成为 GUID）；命令 `open`、`save` 读写 `.ifc`，界面"打开"可选 IFC、文件菜单"导出 IFC…"。说明见 `.agents/features/ifc.md`。
  留到后面的：柱、楼梯、坡道、栏杆、管井、体量、管线与系统的导出导入（随这些元素移植），阶段、设计选项、项目参数的属性集
- 再往后：结构、设备、明细表、出图、DXF 导入等

每个含移植代码的源文件开头写一行来源，例如：

```cpp
// 移植自 BIMCraft crates/core/src/walls.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
```

不移植：`crates/update`（自动更新，以后按 MiniCAD 的发布方式另做）。

`crates/rvt`（Revit 读取器）起初定为不移植，2026-10-31 改为移植（见下面"Revit 文件"）。

## 第 3 阶段：界面（移植 `crates/ui-egui`）

`ui-egui`（约 4.5 万行）基于 egui，不能逐行翻译，要在 MiniGUI 上重写：功能区（`Ribbon`）、特性面板（`PropertyGrid`）、
项目浏览器、平面画布与工具、剖面与图纸。MiniCAD 的 `apps/Common` 有现成的写法可参考（停靠面板、命令行、视图方块）。

界面要用的图标（功能区、工具栏、项目浏览器、属性面板……）**全部由 `tools/make_icons.py` 自动生成**：移植一个命令就在脚本里给它画一个，
不用 BIMCraft 的图标，也不手画或拷图片（做法见 `.agents/features/icons.md`）。

BIMCraft 的界面译文（`translations/ui/zh.po`、`es.po`、`pt.po`）在界面移植之后合并进 `assets/i18n`：两边都以英文原文为键，
同样的原文可以直接沿用译文。

`ui-egui` 里很多界面属于核心还没移植的功能（结构、设备、体量、注释、剖面立面、图纸、明细表、阶段、视图图形、链接、点云、CAD 导入……），
随那些功能一起做。照第 2 阶段的做法分段，每段都能编译、测试、单独提交：

- **3a 外壳（已完成）**：功能区（`ribbon.rs`）与停靠区（`dock.rs` 的特性、项目浏览器两块面板），快速访问工具栏（保存、撤销、重做），
  多选的选择集（单击、Ctrl 单击、Esc）、删除、翻转墙；项目浏览器（`browser.rs`：视图——各标高的平面与三维视图，族——系统族的类型、
  可载入族的类型，搜索）；特性面板（`properties.rs`：类型选择器、按类别分组的实例参数，多选时取值相同的才显示，改动经 `modify` 命令；
  没选东西时显示平面视图对应的标高）；有未保存的改动时新建、打开、退出先问
- **3b 平面上放东西（已完成）**：工具（`Tool`）与选项栏，墙链、门、窗、构件、屋顶、楼板、天花板、房间、标高、轴网；
  捕捉（核心的 `Snap`：端点、中点、交点、圆心、垂足、墙中心线、角度引导线，Shift 保持方正）、预览（会放下的东西、引导线、长度与角度、面积）、
  输入长度按 Enter；没选东西时特性面板显示"新建 …"与它的类型；功能区"建筑"选项卡。
  留到后面的：墙的其他画法（矩形、多边形、圆、弧、拾取线）、房间分隔线工具（BIMCraft 的界面也没有）、门窗与构件的临时尺寸、捕捉设置、拉伸屋顶、老虎窗
- **3c 改东西（已完成）**：框选（窗选、交叉选）、Tab 切换、拖动移动、墙端点的夹点；移动、复制、旋转、镜像（拾取轴、绘制轴）、对齐、修剪 / 延伸为角、拆分、偏移；
  核心补了命令 `align`、`rotate`、`mirror`、`corner`、`split`（`align.rs` 的对齐线）；三维视图里单击拾取并高亮。
  留到后面的：修剪 / 延伸单个与多个元素、添加与删除顶点、详图线的偏移与拆分（随详图线）；门窗与构件的临时尺寸（随尺寸标注）
- **3d 对话框（已完成）**：类型属性（`typeprops.rs`：系统族的类型参数，墙、楼板、屋顶、天花板的层；复制、重命名）、族类型（族的类型参数）、材质
  （新建、删除、改名、颜色、截面与表面图案）；核心补了命令 `rename_type`、`delete_type`；项目浏览器里双击类型打开类型属性，右键删除类型或族。
  留到后面的：材质的贴图（核心还没移植 `texture.rs`）、Purge Unused（`purge`）
- **3e 草图模式（已完成）**：楼板、天花板、屋顶（迹线屋顶）的"编辑边界"与新建时的"边界线"。核心移植 `sketch.rs`（`Model/Sketch.h`：
  焊接端点、找交叉与重叠、走成环、嵌套成轮廓与洞，解不出时列出每个问题与涉及的线）；草图线是元素记着的部件（ID 与 GUID，文件格式同 BIMCraft），
  命令 `edit_boundary`、`sketch`，`floor`、`ceiling`、`roof` 收 `sketch`；界面（`SketchMode.cpp`）：画直线、矩形、拾取墙，
  选线、框选、拖端点、删除、在草图里撤销，完成时解不出就说哪里不对并把线标红，草图模式里不离开它的平面。
  留到后面的：直线与圆弧以外的曲线（`curves.rs` 的样条、椭圆等）、在界面里画圆弧（现在只有拾取弧墙得到圆弧）、草图线的圆角、修剪、拆分、偏移与对齐、
  竖井与填充区域的草图、命令的 `check_edits`（简单轮廓的检查）
- **3f 程序（已完成）**：开始页（`start.rs`：新建、打开、示例房屋、最近的卡片，Ctrl+D 叫出与回去）、最近打开（`recent.rs`，`recent.json`）、
  自动保存与恢复（`autosave.rs`：核心 `Bim::Autosave` 的后台线程，命令 `autosave`、`recover`；启动与打开项目时问恢复 / 放弃 / 稍后）、
  设置（`settings.rs`：主题、布局浮动 / 紧凑、语言、面板、自动保存间隔）、两个字母的快捷键（`shortcuts.rs`：WA、DR、ZF……，键盘快捷键对话框 KS，从文件读入）；
  合并 BIMCraft 的界面译文：新增西班牙语、葡萄牙语（原文相同的沿用 BIMCraft 的译文，其余新译，`Cut` 意思不同不沿用）。
  留到后面的：开始页卡片的项目缩略图、自动保存的文件锁（两个程序开同一个项目）、视口与平面视图跟着主题换颜色、BIMCraft 设置里的性能 / 图形 / 更新页、
  快捷键里 MiniBIM 还没有的动作（捕捉替代 SE、SM……，可见性 VV、临时隐藏 HH……）随那些功能再加；西语、葡语的新译文请母语者校对
- **3g 面板浮动（已完成，3a 漏移植的 `dock.rs` 部分）**：拖出浮动、拖回停靠沿用 MiniGUI 的停靠区；右键标签或浮动窗口标题栏的菜单
  （停靠在左侧 / 右侧 / 底部、浮动、关闭），布局存进 settings.json 并在启动时恢复，设置里"重置面板布局"。
  留到后面的：视图标签栏与视图浮动（`dock.rs` 的 `FloatView`：同时开几个视图、拖出成窗口、拖回标签栏，要先让视口能有多个）

## Revit 文件（移植 `crates/rvt`）

2026-10-31 定：照 BIMCraft 的隔离移植它的 Revit 读取器（`.rvt`、`.rfa`），并先把柱移植进核心，读进来的柱才有地方放。

- **许可**：`crates/rvt` 只有 Apache-2.0（含 rvt-rs 的代码），移植过来的部分也是 Apache-2.0。放在 `src/Rvt/`（目标 `MiniBIMRvt`），
  核心库不依赖它；开关 `MINIBIM_WITH_RVT` 默认开，桌面版、Linux 版、网页版的发行包都编入，并附 `src/Rvt/LICENSE`（发布包里为 `LICENSE-Rvt`）与 `src/Rvt/NOTICE`（`NOTICE-Rvt`）
- **风险**：Revit 格式没有公开规范，读法来自 rvt-rs 与对样例文件的观察。BIMCraft 的 `docs/rvt-provenance.md` 写明没核实 rvt-rs 是否净室实现、
  不对合法性下结论；这个风险由项目所有者承担。只移植那份文档已列来源的读法，不自己逆向，Autodesk 的文件不进仓库（测试用合成的文件）
- **分段**：
  - R1 ✅ 柱（核心）：`Column`、`StructuralColumn` 与它们的类型、命令 `column`、`structural_column`、检查、项目文件、对齐与捕捉（BIMCraft 的柱测试已移植进 `CommandTests`）
  - R2 ✅ 柱的三维、平面与 IFC 导出导入（放柱的界面工具与图标在 R6 做了）
  - R3 ✅ rvt 底层：复合文档（MS-CFB，自己写）、gzip 与有界的 inflate（自己写）、`BasicFileInfo`、`ProjectInformation` / `PartAtom`、预览图（BIMCraft 的容器、gzip 与元数据测试已移植进 `RvtTests`，见 `.agents/features/rvt.md`）
  - R4 ✅ rvt 记录：`Formats/Latest` 的类表、`ElemTable`、分区里的元素记录与类型记录、标高、元素数据、名称、类型与层、曲线、放置、草图（BIMCraft 的 records.rs、inventory.rs、decoders.rs、sketch.rs 与类表的测试已移植，合成项目见 `tests/Rvt/ProjectFixture.cpp`）
  - R5 ✅ rvt 导入：墙、楼板与天花板、屋顶（迹线与拉伸）、门窗、房间、柱，读不了的列出原因（`Rvt::Import`；BIMCraft 的 import.rs、walls.rs、floors.rs、roofs.rs、openings.rs、rooms.rs、columns.rs 里不要样例文件的测试已移植）
  - R6 ✅ 界面打开 `.rvt`：宿主接上读取器（主窗口不依赖它），后台线程读、提示框盖住窗口，读进来的是新的没保存的项目，导入报告（按类别、未导入与原因、复制报告），
    文件 > Revit 导入报告；Linux 版与网页版编入（网页版没有线程，提示框画出来后在界面线程上读）；发布包附 `LICENSE-Rvt`、`NOTICE-Rvt`；
    放柱的工具（功能区"建筑 > 柱"，CL，高度同墙）与图标，柱与结构柱的参数、类型（项目浏览器、类型属性）。BIMCraft `ui-egui` 的 `tests/revit.rs` 里不驱动 egui 的
    （数略去了几个、拆 id、报告的文字）移植进 `AppTests`。结构柱的工具留到"结构"选项卡（BIMCraft 的梁、结构楼板等一起）
  - R7 ✅ 拖进窗口打开文件（桌面版 `WM_DROPFILES`、Linux 版 SDL 的拖放事件、网页版读进内存文件系统），同"打开"先问未保存的改动；
    读 Revit 文件或有对话框时拖进来的排队，之后只开最后一个（`OpenQueue`，同 BIMCraft 的 `opens`，测试移植进 `AppTests`）

## 第 4 阶段：平台

- 4a ✅ 平台接口：文件对话框改成异步（`PickFile` 选好时回调，`ChooseSavePath` + `HasSaveDialog`，`OnDocumentSaved`），
  `HasBackgroundThread` 为 false 时不开自动保存，`HasWindowControls` 为 false 时标题栏不显示窗口按钮
- 4b ✅ Linux 版（`apps/Linux`，SDL3，Vulkan / GLES3）：从 MiniCAD 复制宿主 `LinuxApp`、`apps/Render` 的 `Vulkan/`、`GLES3/` 与 `cmake/Render.cmake` 的 Linux 段落；
  界面字体是自带的 Noto Sans SC 子集（`tools/make_web_font.py` 生成 `assets/fonts/NotoSansSC-UI.ttf`），自动保存的定时用 SDL 定时器
- 4c ✅ 网页版（`apps/Web`，Emscripten，WebGPU / WebGL2，`build_wasm.bat`）：从 MiniCAD 复制宿主 `WebApp`、`apps/Render/WebGPU` 与 `cmake/Render.cmake` 的段落；
  打开读进内存文件系统，保存与导出 IFC 先问文件名再下载；核心库在网页上不链接 pthread，不自动保存，不记住设置与最近打开；SMLib 编入（8 MB 栈）
- 4d ✅ CI：照 MiniCAD 的 `build.yml` 加上 Linux 版（SDL3 静态编译并缓存，`Linux (Vulkan / GLES3)`，也跑全部测试）与网页版（`Web (WebAssembly)`）的编译、打包，
  标签构建时一起发布（`linux-x64.tar.gz`、`web.zip`）。网页版还没有部署到网站（要部署时照 MiniCAD 加 Cloudflare Pages 或 GitHub Pages 的一步）

## 同步 BIMCraft 新版

- `0733fd4`（2026-10-29）：文件格式升到 11（第 10 版的尺寸标注、第 11 版的项目语言 sv / cs / fr 读入时略去并列出，语言按英语）；
  画到墙面的房间分隔线在中心线量时也分开房间（BIMCraft #149）；在墙面量时房间的点不能在墙体里，`inspect` 报出 `room_area`；
  材质的透明度（`Material::transparency`，新项目的玻璃 70，`material` 命令、检查、材质对话框、IFC 导出与读回；三维还不画半透明）；
  单线字的 Æ；IFC 导入里不属于楼梯的平台板读成楼板。
  没同步的：新的项目语言与界面语言（暂不移植）、IFC 导入的标记与别的程序的属性集（要项目参数）、尺寸标注、三维透视、对齐辅助线、
  技术标准、钢筋与配筋、天花板平面里的梁、巴西规范的计算；DWG 导入以后用 MiniCAD 的 MiniDWG（移植自 ACadSharp，MIT），不用 BIMCraft 的 acadrust

## 与 MiniCAD 的差异

复制过来的代码在 MiniBIM 里做过这些改动，以后从 MiniCAD 同步时留意：

- 命名空间 `MiniCAD` → `MiniBIM`，宏 `MINICAD_*` → `MINIBIM_*`（MiniGUI 的命名空间不变）
- MiniGUI：新增 `Core/Translate.h`（`MiniGUI::Tr`、`SetTranslator`）；`UiLayout` 每次应用时翻译界面描述文件里的文字与命令名称、
  提示、面板标题；`Binding`、`NumberBox`、`ColorPicker`、`CommandConsole`、`CommandUI`、`Dialog`、`Ribbon`、`TabView`、`TextBox`、
  `TitleBar` 里写死的中文改成英文原文 + `Tr`（菜单文字去掉了助记符 `&`，不显示下划线和 `(X)`）；`Win32Fonts` 加了韩文回退字体
- MiniGUI：`UIContext::SetUnhandledTextHandler`（没有节点要的文字交给宿主，两个字母的快捷键用）
- MiniGUI：`DockSpace::SetOnPanelMenu`、`DockSpace::PanelMenuOfFloat`、`FloatingPanel::SetOnMenu`（右键标签或浮动窗口的标题栏，宿主弹出面板菜单）
- MiniGUI：`ScrollView::OnLayout` 里内容超出视口不到 1 像素时不出滚动条（按内容定高的视图在 150% 缩放下，边对齐到设备像素后会矮一点）
- MiniGUI：界面描述文件的节点属性 `card`（row / column / 宿主面板画成圆角带边框的卡片，功能区 false 时方角贴边）与 `variants`（`UiLayout::SetVariant` 选的变体覆盖节点属性），
  `Ribbon::SetCard`（MiniCAD 的功能区总是卡片）：设置里的布局浮动 / 紧凑用
- MiniGUI：`UIContext::UpdateHover` 在关掉提示之后才取树的版本号（修复：提示显示着时指针直接跳到相邻控件，新控件收不到 Enter、点了不触发；MiniCAD 也有这个问题）
- `cmake/Render.cmake` 照 MiniCAD 分 Windows、Linux、网页三段；`apps/Win32` 的宿主精简（没有浮动窗口、自测）；`AppPlatform` 精简并加了 `GetSystemLanguage`、`PostDelayed`，
  文件对话框同 MiniCAD 是异步的 `PickFile` 与 `ChooseSavePath`，但 `FileKind` / `SaveKind` 换成项目、IFC、快捷键文件（没有 DWG 的格式与版本）
- 没有复制：MiniDWG、节点图、天正、CAD 实体 / 场景 / 编辑器（`src/Core/Entity`、`src/Scene`、`src/Document`、`src/Editor`、`src/Viewport`）
