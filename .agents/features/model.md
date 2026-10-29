---
covers: src/Model/*, src/Commands/*, 3rd/nlohmann/*, tests/Model/*, tests/Commands/*
---

# 模型核心

## 概述

移植自 BIMCraft `crates/core`（`f3bbb33`）的建筑模型、命令与撤销，命名空间 `MiniBIM::Bim`，在核心库里（不依赖界面，Linux CI 也编译）。
第 2a、2b 步移植了这些（进度与后续见 `docs/PLAN.md` 第 2 阶段）：

| 代码 | 来自 BIMCraft | 内容 |
|---|---|---|
| `src/Model/Geom.h` | `geom.rs`、`walls.rs` 的 `Plane` | 平面几何：`P2`、环、面积、裁剪、三角剖分、平面图的面、区域 |
| `src/Model/Units.h`、`src/Model/Language.h` | `units.rs`、`language.rs` | 项目单位（显示与输入）、项目语言（新名称用的词） |
| `src/Model/Ids.h` | `ids.rs` | `Id`、`Guid`、`Elements`（按 ID 或 GUID 找元素） |
| `src/Model/Model.h` | `model.rs` | 元素（标高、轴网、墙、楼板、天花板、门、窗、房间、房间分隔线、屋顶）、类型、材质、`Model::Check`；屋顶的坡面、房间的区域 |
| `src/Model/Skeleton.h` | `skeleton.rs` | 多边形（含天井）的直骨架：坡屋顶的斜脊、天沟、屋脊与各坡面 |
| `src/Model/Library.h` | `library.rs` | 库条目（类型、材质）与部件（类型的层）的 ID、GUID |
| `src/Model/ModelJson.h` | `model.rs`、`ids.rs`、`library.rs`、`lenient.rs` | 元素与项目文件的 JSON |
| `src/Model/Walls.h` | `walls.rs` 与它的 index 子模块 | 墙的定位线、墙面、斜接封口、轮廓、棱柱、加速索引 |
| `src/Commands/Session.h` | `engine.rs` | `Session`：执行命令、回滚、撤销 / 重做、脚本、事务 |
| `src/Commands/Commands.cpp` | `commands.rs` | 命令表 `Commands()` 与各命令（第 2b 步加了 `roof`、`ceiling`、`room`、`room_separator`、`room_area`） |
| `src/Commands/Sample.h` | `sample.rs` | 示例房屋脚本（已移植命令能做的部分） |

## 入口与数据流

- 一切编辑都是命令：`Bim::Session::Execute(id, params, result, error)`，参数与结果是 JSON（`Bim::Json` 即 `nlohmann::ordered_json`，`src/Model/Json.h`）。
  命令表在 `Commands()`（`Commands.cpp` 末尾），每条有 ID、快捷名（WA、DR……）、给 AI 代理读的参数说明、是否算撤销步骤。界面只调命令（`MainFrame::RunCommand`）
- 执行：命令读参数、构造元素放进 `Session::ModelMut()` → 之后 `Model::Resolve`（命令按 ID 给的引用换成 GUID）、`Model::SyncLibrary`、`Model::Check` 检查整个模型；
  命令失败、抛出异常或检查不通过时整个模型换回执行前的那一份，什么都不改
- 撤销照 BIMCraft 存整个模型的快照：`Session` 拿着 `shared_ptr<const Model>`，元素在 `Elements` 里也是 `shared_ptr<const Element>`，
  复制模型只复制指针，`Elements::GetMut` 改一个元素时才复制它（与快照共用时）。每条可撤销的命令改了模型就记一步（`Step` 按类别数做出与去掉的元素）
- 元素有 ID（六位数，从 `FirstId` 往上）和 GUID（生成一次、永不改变）；凡是指向元素的字段都存 GUID。命令里可以先写 ID（`Guid::OfId`），执行后解析成 GUID。
  库条目（类型、材质）也有 ID（从 `LastId` 往下）和 GUID；类型的层是部件，自己存着 ID 和 GUID
- 项目文件：`Model::ToFile` 写、`Model::Read` 读，格式与 BIMCraft 的 `.bimcraft` 相同（`FileFormat` 同为 9），扩展名用 `.minibim`，两边都能打开对方的文件。
  文件里引用库条目的名称（墙的 `type`、层的 `material`）一律写 GUID，读回时换回名称（`Model::NamesFromFile`）
- 读文件宽松、读命令严格：命令里拼错的键报错（`FromJson` 读元素时先查不认识的键）；读文件时本版本不认识的内容（BIMCraft 新加的字段、还没移植的元素、
  族、阶段、视图设置……）略去，略去的位置以 JSON 指针列在 `open` 结果的 `left_out`（`/elements/100004/finish`）。读完再清理：宿主没了的门窗也略去，
  贴着的东西不在了的墙松开。界面打开有略去内容的文件时弹窗说明，保存会丢掉它们
- 三维：`ModelScene`（`.agents/features/view3d.md`）从 `Bim::Model` 建实体
- 屋顶：迹线屋顶的坡面由 `Model::RoofFaces` 算出：平的、按 `Skeleton::Faces`（每条边以速度 1 向内移动，山墙边速度 0）的坡面，或拉伸屋顶的轮廓；
  `join` 插进的屋顶上方只留高过那个屋顶顶面的部分。`Model::RoofPieces` 再减去开洞。墙贴着屋顶时（`Wall::attachTop`）顶跟着它的底面（`Walls::TopAt`）。
  `roof` 不给轮廓时沿下一层的墙（`RoofFootprint`，伸出墙面 `overhang`），独立的一圈墙里没有房间时是天井
- 房间只存一个点，边界每次由点四周的线找（`Model::RoomRegion`）：墙中心线与房间分隔线，或在墙面量时（`Model::roomArea`）墙轮廓的边与分隔线。
  墙动了房间跟着变；`floor`、`roof` 沿墙铺时，里面有房间的独立的一圈墙算室内

## 约束与坑

- 照 BIMCraft 的规则：不让错误输入弄崩程序。参数里的数都查有限、在 `MaxCoord` 以内，列表有 `MaxPoints`、名称有 `MaxText` 的上限；
  `Session::Execute` 捕获异常当作命令失败（BIMCraft 的 `catch_unwind`）。`CommandTests` 的恶意参数扫描保证每条命令只拒绝、不出内部错误、不弄坏模型
- 新的检查规则加在 `Model::Check`，错误信息以"类别 ID: "开头、说明要改什么；命令不必自己撤销
- 核心的报错信息照 BIMCraft 写英文（给人和 AI 代理读），不经 `Tr`；界面弹窗的标题翻译。命令名称（撤销步骤的名称）用 `N_` 标记
- 新增一种元素：在 `Element::Variant` 里加一种，`std::visit` 的访问器（`Element::Category`、`Element::RefsMut`）会编译报错列出要补的分支；
  再补 `ModelJson.cpp` 的读写（`ReadElement`，宽松读时还没移植的类别整个略去）、`Model::Check`、`Model::LevelOf`、命令的 `Transformed` 等
- 新增库条目的种类：`Model::LibraryNames`、`Element::LibRefsMut` / `Model::LibRefsMut`（文件里名称换 GUID）；新的部件加进 `Model::PartsNow` 和 `Model::SyncParts`
- nlohmann/json：不用会抛异常的接口（`parse` 传 `allow_exceptions = false`，取值前查类型）；代码里构造的整数是有符号整数、解析出的是无符号整数，
  读非负整数用 `Bim::AsWhole`；不要在 range-for 里对临时对象调 `items()`（临时对象先析构，`Model::ToJson` 踩过）
- 数的读写不随系统区域设置：`Text::ParseNumber`（`from_chars`）、`Text::Shortest`（`to_chars`，照 Rust 的 `{}` 格式）
- 移植时还没有的部分在代码里注明是哪一步补上：族（带族的门窗）、贴图、填充样式、竖井（BIMCraft 的 `less_shafts`）。
  画轮廓的草图（`sketch`：楼板、天花板、屋顶的"编辑边界"，BIMCraft 的 `sketch.rs`、`curves.rs`）随平面视图的编辑（第 2d 步）移植，
  现在命令收到 `sketch` 就拒绝、读文件时略去
- 直骨架照 BIMCraft 用二叉堆排事件：同一时刻的事件顺序与 Rust 的堆可能不同，结果一样（`ModelTests` 的 2000 个随机迹线检查面积与平面性）

## 测试与排查

- `ModelTests`（`tests/Model/ModelTests.cpp`）：几何、单位、ID、项目语言、`Model::Check`、墙的接头与轮廓、加速索引、库、直骨架，移植自 BIMCraft 各模块的测试
- `CommandTests`（`tests/Commands/CommandTests.cpp`）：像前端一样经命令驱动（移植自 BIMCraft 的 `tests.rs`）：撤销、脚本、事务、建模、编辑、
  项目文件（含打开 BIMCraft 文件时略去的内容）、示例房屋、恶意参数扫描
- 用到还没移植的命令的 BIMCraft 测试（剖面、平面图、IFC……）略去了那些断言，移植那些命令时补回；BIMCraft 测试里的 `view3d::tops_at`
  （某点上各元素顶面的高度）还没移植，测试里由 `Model::RoofPieces`、`Model::CeilingSlabs`、`Walls::TopAt` 算
- 结果对不上时，在 `D:\BIMCraft` 里 `cargo test -p bimcraft-core <名字>` 跑原来的测试对照（`.agents/features/porting.md`）
