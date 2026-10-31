---
covers: src/Ifc/*, tests/Ifc/*
---

# IFC 交换

## 概述

移植自 BIMCraft `crates/core`（`f3bbb33`）的 `step.rs`、`ifc.rs`、`import.rs`（第 2e 步），在核心库的 `src/Ifc/`，命名空间 `MiniBIM::Bim::Ifc`：

| 代码 | 来自 BIMCraft | 内容 |
|---|---|---|
| `src/Ifc/Step.h` | `step.rs` | ISO 10303-21（STEP 物理文件）的读法：头部的模式、编号的实体与参数（`Ifc::Step::Parse`），字符串转义的还原（`Ifc::Step::Decode`） |
| `src/Ifc/Ifc.h`、`src/Ifc/Export.cpp` | `ifc.rs` | 模型写成 IFC4（`Ifc::Export`），GlobalId 与 GUID 互换（`Ifc::Compress`、`Ifc::Expand`） |
| `src/Ifc/Import.cpp` | `import.rs` | IFC2x3 / IFC4 读成模型（`Ifc::Import`），按几何量出每个元素 |

命令：`open` 路径以 `.ifc` 结尾时作为新项目导入（没有路径，保存时再问；结果有 `format`、`elements`（各类别几个）、`skipped`、`moved`）；
`save` 路径以 `.ifc` 结尾时导出 IFC4，不改项目自己的路径。界面：文件菜单"打开"可选 `.ifc`，"导出 IFC…"（`file.exportIfc`）问存到哪里再 `save`；
导入有略去的东西时弹窗说几个（`MainFrame::OpenProject`）。

## 入口与数据流

- 导出（`Ifc::Export`）：项目、场地、建筑、楼层（每个标高一个）的空间结构；墙是轮廓（与平面图一样在相接处斜接）的拉伸体，贴着屋顶或楼板的墙是每个棱柱一个闭合壳；
  平的楼板拉伸、带坡度的楼板与屋顶是板块的闭合壳；天花板是 `IFCCOVERING` 拉伸；房间是区域拉伸到上一层的 `IFCSPACE`；构件按族的类别
  （`Fam::CategoryOf::Ifc`）写成元素，各形体是着材质颜色的三角面集；门窗填在开在墙上的 `IFCOPENINGELEMENT` 里，族的门窗带形体；
  柱与结构柱是 `IFCCOLUMN`（类型的矩形或圆的截面从底拉伸到顶）；
  房间分隔线是对象类型 `RoomSeparation`（`Ifc::Separation`）的 `IFCVIRTUALELEMENT`。复合类型的层写成材质层组与用法，材质带颜色与透明度（表面样式着色的第二个参数，导入时读回）；
  墙、楼板、门窗、房间带 `Qto_*BaseQuantities`；结构楼板的 `Pset_SlabCommon`、结构柱的 `Pset_ColumnCommon` 说 `LoadBearing`
- GlobalId：元素与楼层的是它们自己的 GUID（`Ifc::Compress`），别的（洞口、关系、属性集）由元素的 GUID 与用途得出，所以每次导出都一样；没有元素的由模型的 `uid` 得出
- 导入（`Ifc::Import`）：`Ifc::Step::Parse` 读整个文件 → 单位（长度换成毫米、平面角换成弧度）→ 关系（在哪个楼层、是什么的一部分、洞口开在哪、门窗填在哪、类型、材质）
  → 楼层成为标高 → 墙（有定位线时沿它，弯的按圆弧；没有时沿墙自己的 x；厚度是实体横过它的伸展；差一点相接的端点移到另一面墙上）→ 楼板（平的或斜的顶面、洞）
  → 天花板（不属于楼梯的平台板也读成楼板）→ 柱（`IFCCOLUMN`：截面与最长的水平边对齐，角点多且离中心一样远的是圆柱，承重的是结构柱）→ 屋顶（平的；四周一个坡度的按直骨架核对；否则横着扫过的一个轮廓）→ 顶部切到屋顶的墙贴着它 → 家具与器具等成为构件（文件里每个名称一个族、
  每个形状一个类型，镜像的是同一类型的镜像）→ 房间分隔线 → 门窗（在洞口切开墙的地方，开启方式从门或它的类型读）→ 房间（轮廓里的一点）→ 轴网
- 读出的元素先用 ID 指标高和墙，建好后每个元素的 GUID 换成文件里的 GlobalId（`Elements::Regiven`），库的 GUID 由文件内容得出（`Model::SyncLibraryWith`），
  所以同一个文件每次读出一样的模型；最后逐个去掉 `Model::Check` 不通过的元素（墙带着它的门窗），连同原因列进 `skipped`
- 模型放在很远处（场地在地图坐标上）时整体移到原点附近，移了多远在 `moved`

## 约束与坑

- 还没移植的部分：基础、梁与管线、楼梯、坡道、栏杆、管井、体量、系统，以及 BIMCraft 存阶段、设计选项、项目参数的属性集。
  导出不写它们；导入时它们按"MiniBIM has no element for …"列进 `skipped`。BIMCraft 把穿过楼板、天花板、屋顶的洞口元素读作管井，
  这里只读实体里本来就有的洞（作为它们的开洞），实体里没有、只由洞口元素切出的洞列进 `skipped`
- STEP 解析不信任文件：列表最多套 32 层，放置链最多走 64 层，曲线、轮廓、表示项最多套 16 层；圆盘扫掠超过 `MaxPoints` 段、构件超过 `Fam::MaxTriangles`
  个三角形、一个族超过 `Fam::MaxForms` 个形体的略去；坏文件报错（带行号），不改打开着的项目
- 报错与 `skipped` 的文字照 BIMCraft 写英文，把 "BIMCraft" 换成 "MiniBIM"（不使用 BIMCraft 的名称）
- 写 STEP 的数用 `to_chars`（不随区域设置），实数总带小数点；字符串里可打印 ASCII 以外的写 `\X2\…\X0\` 或 `\X4\…\X0\`
- `Bim::Step` 是撤销的一步（`Session.h`），STEP 解析在 `Bim::Ifc::Step`
- 往返测试比较的是元素 JSON 里到毫米的值（`tests/Ifc/IfcTests.cpp` 的 `Describe`）：改了导出或导入的几何，跑 `IfcTests` 看墙、门窗、楼板、房间、屋顶、天花板回来时是否一样

## 测试与排查

- `IfcTests`（`tests/Ifc/IfcTests.cpp`）：STEP 的读法与坏文件、GlobalId、导出的结构与引用（每个引用都指向只写了一次的实体）、构件与族门窗的形体与材质、
  复合墙的层组、导入网格与映射项、地图坐标、楼板天花板屋顶的洞、层的叠放方向与膜、构件往返、代理的朝向与族的形体上限、同一文件读出一样、
  示例房屋加各种屋顶楼板的往返、别的软件写的 IFC2x3 办公室、门窗编号、坏文件、材质颜色与透明度、推拉门、房间分隔线
- 结果对不上时，在 `D:\BIMCraft` 用 `cargo test -p bimcraft-core ifc` / `import` 跑原来的测试；导出的文件可以用 IfcOpenShell 或别的 BIM 软件打开看
