---
covers: src/Model/*, src/Commands/*, 3rd/nlohmann/*, tests/Model/*, tests/Commands/*
---

# 模型核心

## 概述

移植自 BIMCraft `crates/core`（`f3bbb33`）的建筑模型、命令与撤销，命名空间 `MiniBIM::Bim`，在核心库里（不依赖界面，Linux CI 也编译）。
第 2a–2d 步移植了这些（进度与后续见 `docs/PLAN.md` 第 2 阶段；第 2e 步的 IFC 交换在 `src/Ifc`，见 `.agents/features/ifc.md`）：

| 代码 | 来自 BIMCraft | 内容 |
|---|---|---|
| `src/Model/Geom.h` | `geom.rs`、`walls.rs` 的 `Plane` | 平面几何：`P2`、环、面积、裁剪、三角剖分、平面图的面、区域 |
| `src/Model/Units.h`、`src/Model/Language.h` | `units.rs`、`language.rs` | 项目单位（显示与输入）、项目语言（新名称用的词） |
| `src/Model/Ids.h` | `ids.rs` | `Id`、`Guid`、`Elements`（按 ID 或 GUID 找元素） |
| `src/Model/Model.h` | `model.rs` | 元素（标高、轴网、墙、楼板、天花板、门、窗、房间、房间分隔线、屋顶、构件、柱、结构柱、独立基础、九类 run、体量；注释：尺寸、文字、高程点、标记、详图线、填充区域）、类型、材质、`Model::Check`；屋顶的坡面、房间的区域、构件与带族门窗的实体 |
| `src/Model/Family.h` | `family.rs` | 族（`Bim::Family` 与 `Bim::Fam` 里的参数、公式、形体、符号）：公式求值、参数取值、检查、扫掠与旋转出三角形 |
| `src/Model/Csg.h` | `csg.rs` | 族的空心形体用的网格布尔运算（BSP，工作表不递归，`Csg::Meter` 限制花费） |
| `src/Model/FamilyLibrary.cpp` | `family.rs` 的 library、`fixtures.rs` | 新项目带的 34 个族：家具 10、门窗 6、各专业的器具 18 |
| `src/Model/Render.h` | `render.rs`、`pen.rs`、`styles.rs` 的平面部分 | 平面图的显示列表（像素里的填色环与折线）：墙与柱的剖切填充与边线、门窗、房间标签、轴网、屋顶、楼板、构件；线宽（笔）；拾取 |
| `src/Model/Align.h` | `align.rs`、`model.rs` 的 `reference_line` | 对齐：元素在平面上的直边与参照轴线（墙的中心线、两面、两端，门窗的中线与门框，轴网……），把一条线放到另一条上的刚体运动 |
| `src/Model/Snap.h` | `snap.rs` | 捕捉：平面上点下的点落在哪（端点、中点、交点、圆心、垂足、墙中心线、角度引导线、最近的 10 毫米），标尺的读数与刻度 |
| `src/Model/Sketch.h` | `sketch.rs` | 草图：零散的直线与圆弧（`SketchLine`）解成闭合的环、嵌套成轮廓与洞（`Sketch::Solve`、`Sketch::SolveOne`），解不出时列出每个问题（相交、重叠、分叉、开口、没有面积、几个外轮廓）与涉及的线 |
| `src/Model/Massing.h` | `massing.rs` | 体量做出的元素的记号（`Massing::FloorMark`、`Massing::Face`）、按面重建（`Massing::Follow`）、放样的轮廓与三角形、竖直的面、放样的检查 |
| `src/Model/Styles.h` | `lines.rs`、`fills.rs`、`model.rs` 的 `TextType` | 注释的库条目：线型图案、线样式、填充图案、填充区域类型、文字类型，各语言的默认条目与检查 |
| `src/Model/Curve.h` | `curves.rs` | 详图线的曲线：多段线（每段直线或圆弧）与圆；长度、映射、检查（椭圆与样条没有移植，读文件时略去那条线） |
| `src/Model/Annotate.cpp` | `model.rs`、`render.rs` 的注释部分 | 注释在哪个平面、标记写什么、尺寸量到的线与值、高程点读谁、元素在平面上的点（`Model::Anchor`） |
| `src/Model/Font.h` | `font.rs` | 平面图注释用的单线字（拉丁、西里尔大写，附加符号；没有的字画问号） |
| `src/Model/Skeleton.h` | `skeleton.rs` | 多边形（含天井）的直骨架：坡屋顶的斜脊、天沟、屋脊与各坡面 |
| `src/Model/Library.h` | `library.rs` | 库条目（类型、材质）与部件（类型的层）的 ID、GUID |
| `src/Model/ModelJson.h` | `model.rs`、`ids.rs`、`library.rs`、`lenient.rs` | 元素与项目文件的 JSON |
| `src/Model/Walls.h` | `walls.rs` 与它的 index 子模块 | 墙的定位线、墙面、斜接封口、轮廓、棱柱、加速索引 |
| `src/Commands/Session.h` | `engine.rs` | `Session`：执行命令、回滚、撤销 / 重做、脚本、事务 |
| `src/Commands/Commands.cpp` | `commands.rs` | 命令表 `Commands()` 与各命令（R1 加了 `column`（CL）、`structural_column`；第 3f 步加了 `autosave`、`recover`；第 3e 步加了 `edit_boundary`、`sketch`，`floor`、`ceiling`、`roof` 收 `sketch`；第 3d 步加了 `rename_type`、`delete_type`；第 3c 步加了 `align`、`rotate`、`mirror`、`corner`、`split`（只认墙；详图线的部分随详图线移植）；第 2b 步加了 `roof`、`ceiling`、`room`、`room_separator`、`room_area`，第 2c 步加了 `family`、`component`，门窗可带 `family`；第 2e 步 `open`、`save` 读写 `.ifc`） |
| `src/Commands/Autosave.h` | `autosave.rs` | 自动保存的工作线程 `Autosave::Worker`：按顺序写恢复副本（经 `autosave` 命令）、删、找（项目旁边的 `Autosave::BackupPath`，或临时文件夹里没保存过的），通知经回调叫界面线程来 `Worker::Poll`。没有 BIMCraft 的文件锁（两个窗口同时开着时可能都找到同一份） |
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
- 项目文件：`Model::ToFile` 写、`Model::Read` 读，格式与 BIMCraft 的 `.bimcraft` 相同（`FileFormat` 同为 11，BIMCraft `0733fd4`），扩展名用 `.minibim`，两边都能打开对方的文件。
  第 10 版的尺寸标注、第 11 版的项目语言 sv / cs / fr 还没移植：读入时略去并列出（语言按英语）
  文件里引用库条目的名称（墙的 `type`、层的 `material`）一律写 GUID，读回时换回名称（`Model::NamesFromFile`）
- 读文件宽松、读命令严格：命令里拼错的键报错（`FromJson` 读元素时先查不认识的键）；读文件时本版本不认识的内容（BIMCraft 新加的字段、还没移植的元素、
  族、阶段、视图设置……）略去，略去的位置以 JSON 指针列在 `open` 结果的 `left_out`（`/elements/100004/finish`）。读完再清理：宿主没了的门窗也略去，
  贴着的东西不在了的墙松开。界面打开有略去内容的文件时弹窗说明，保存会丢掉它们。`.ifc` 作为新项目导入、保存为 `.ifc` 是导出（`.agents/features/ifc.md`）
- 三维：`ModelScene`（`.agents/features/view3d.md`）从 `Bim::Model` 建实体
- 屋顶：迹线屋顶的坡面由 `Model::RoofFaces` 算出：平的、按 `Skeleton::Faces`（每条边以速度 1 向内移动，山墙边速度 0）的坡面，或拉伸屋顶的轮廓；
  `join` 插进的屋顶上方只留高过那个屋顶顶面的部分。`Model::RoofPieces` 再减去开洞。墙贴着屋顶时（`Wall::attachTop`）顶跟着它的底面（`Walls::TopAt`）。
  `roof` 不给轮廓时沿下一层的墙（`RoofFootprint`，伸出墙面 `overhang`），独立的一圈墙里没有房间时是天井
- 族：`Model::families` 按名称存族；构件（`Component`）和带族的门窗（`Opening::family`）放置族的一个类型，实例参数的值存在元素的 `params`。
  参数值由 `Family::Values` / `Family::ValuesIn`（门窗族读墙厚 `Wall Thickness`）算出，形体由 `Family::Pieces` 扫掠、旋转成朝外的三角形，
  空心形体用 `Csg::Subtract` 切掉列在它前面的实体。`Model::ComponentPieces`、`Model::OpeningPieces` 把它们放到模型坐标里。
  库里族、族类型（"族: 类型"）、族参数（"族: 参数"）各有 ID 和 GUID，族的形体与符号是部件；文件里构件的实例参数按族参数的 GUID 存（`Model::RekeyFamilyParams`）
- 平面图：`Render::Plan`（适配窗口）/ `Render::Draw`（给定视图）把一个标高画成 `Render::Drawing`：各项带画它的元素（拾取、高亮）、是元素的哪部分
  （剖到的、看到的、字）和笔；画完按对象样式把 `Pen::ByCategory` 换成类别的笔（`StyleOf`，门窗剖到的用中粗），再按 1 : n 的线宽表定宽。
  注释（轴网圆圈、房间标签）按 `AnnotationOf` 随比例放大。BIMCraft 平面图的视图图形覆盖、填充与线型图案、裁剪范围、尺寸与标记等注释、楼梯与设备、
  反射天花板平面还没移植。`Render::Sketch` 画还不在模型里的一个元素（工具预览）：放进模型的副本里画整个平面，只留它的几项
- 移动、复制、旋转、镜像、对齐都经 `Commands.cpp` 里的 `Motion`（点怎么变，平移时门窗沿墙滑多少，是不是镜像）与 `Relocate` / `Duplicate`：
  连着的墙伸缩保持相接，墙上的门窗随墙。镜像时墙反着走（外侧仍在镜像过去的那边）、楼板与天花板的轮廓反向、构件翻过来、门挂到另一侧门框；
  复制的房间、轴网、门窗取下一个编号
- 柱（R1，为读 Revit 文件先移植）：`Column` 站在一个标高上，到 `topLevel`（命令不给 `height` 时为上一个标高）或 `height` 那么高（`Model::ColumnSpan`），
  截面是类型的矩形或圆（`ColumnType`，圆柱的进深即直径，`Model::ColumnOutline`）。`StructuralColumn` 是同样的柱，类别是 structuralcolumn、
  类型另有一张表（`Types::structuralColumn`，文件里 `structural_column`，没有时不写）。`Footing`（独立基础）也是平放的柱，类型在 `Types::footing`，
  命令 `footing` 给厚度 `thickness`（默认 400）与顶的高度 `offset`，存成 `height` 与 `baseOffset`。
  平面图里剖到的柱与墙一起填色、描并集的边，剖切面以下看到的画轮廓（`Render.cpp` 的 `Stands`）；三维见 `.agents/features/view3d.md`、IFC 见 `.agents/features/ifc.md`；
  对齐给矩形柱的四边与十字轴线、捕捉给柱的中心。放柱、结构柱、基础的工具见 `.agents/features/main-window.md`
- run（第 5 阶段，移植自 `runs.rs`）：两点之间的直构件，`Run`（类型名、标高、起终点、`offset`、可选的 `endOffset`、`Justify` 对齐、`system`），
  九个类别各派生一个结构体（`MINIBIM_RUN`：`Beam`、`StripFooting`、`Pipe`、`Conduit`、`CableTray`、`Wire`、`DownConductor`、`GroundConductor`、`Cable`，
  列在 `RunCategories`），取基类用 `Element::AsRun`、由类别做元素用 `Element::OfRun`。类型 `RunType`（宽、高、圆、材质）按类别放在 `Types::runs`（文件里 `runs`）。
  几何都在 `src/Model/Runs.h`：`Runs::Geom`（`Model::RunGeom`：中心线两端，高度按对齐移到中心）给平面的占地、高度范围、三维的棱柱或圆管（`Runs::Geom::Tube`）；
  `Runs::Check` 拒绝太短、比 75° 陡（那是柱）的；`Runs::IfcTable` 是类别与 IFC 类、预定义类型的对照。命令 `beam`、`strip_footing`、`pipe`……（`RunsOf` 每段一个）
  默认的对齐见 `Runs::DefaultJustify`（梁与条形基础顶对齐）。平面：剖到的填色，标高下挂着的与上方的看得到（上方的虚线），不是竖着的画中心线；捕捉两端、中点与线
- 体量（5c，移植自 `massing.rs`）：`Mass`（名称、标高、逆时针的底面轮廓 `boundary`、`topLevel` 或 `height`、`baseOffset`、可选的顶部轮廓 `top`，与底面点点对应、同向）。
  `Model::MassSpan`（底与顶的高度）、`Model::MassCutBy`、`Model::MassOutline`（某高度处的轮廓，放样的在两个轮廓之间）、`Model::MassVolume`（拟柱体公式，按截面积精确）、
  `Model::MassSlices`（它跨过的标高）。由体量做的墙、楼板、屋顶在 `params` 的保留名下记着它：`mass_floors` 的楼板记 `{mass, level}`（`Massing::FloorMark`），
  按面做的记 `{mass, face}`（`Massing::Face`，墙为底面轮廓的边号，楼板与屋顶为 `"top"`）；`Massing::CheckValue` 检查记号，项目参数不能用这两个名字。
  复制、拆开的是普通元素（`Massing::Detach`），删掉体量时记号拿掉（`Massing::Forget`，要在删之前调，`Model::Same` 认元素要它还在），
  原地镜像的体量轮廓反着走、它的墙记着的边重新编号（`Massing::Mirrored`）。`update_by_face` 经 `Massing::Follow` 照体量现在的样子重建，面没了或斜了时整个拒绝。
  平面：剖切面穿过时那里的轮廓画虚线（`Render::MassColor`），捕捉它的角点，对齐认底面轮廓的边
- 注释（5d，移植自 `model.rs`、`lines.rs`、`fills.rs`、`render.rs`）：`Dimension`（对齐、角度、径向，`Reference` 为元素的 GUID 与 `Face`）、`Note`（文字）、
  `Spot`（高程点）、`Tag`（标记）、`DetailLine`（详图线）、`FilledRegion`（填充区域，草图可以是几个环，`Sketch::Solve`）；`Element::IsAnnotation`。
  MiniBIM 还没有平面视图元素（随 5e），注释的视图是一个标高的 GUID（它自己的平面，`Model::AnnotationPlan`、`Model::AnnotationIn`）：`Dimension::view` 与 `Tag::view`
  必须为空，读 BIMCraft 文件时在别的视图（平面视图、剖面、立面、图纸）里的注释、引用的东西已不在的标记与高程点略去，尺寸去掉不在的参照、不够时略去。
  库条目 `Model::linePatterns`、`lineStyles`、`fillPatterns`、`regionTypes`、`textTypes` 写进项目文件；项目语言不是英语而文件里的正是英文默认条目时换成那种语言的。
  文字的大小是文字类型的（`TextTypes::ForSize`：BIMCraft 旧文件里文字自带的 `size` 读入时换成同样大小的类型，没有时建一个 "<大小>mm"）。
  文字用单线字（`Font.h`）画；样式的命令（线样式、填充图案、文字类型以外的库条目）随 5f。命令 `dimension`（DI）、`tag`（TG，不给 `id` 时标记全部）、`text`（TX）、
  `text_type`、`spot`（EL）、`detail_line`（DL）、`filled_region`（FR）、`measure`（ME，不改模型）；`edit_boundary` 也编辑填充区域。
  移动、复制、镜像、旋转、翻转跟着改注释（`Transformed`、`TagHeads`、`PlaceTag`）；删除连带：标记随宿主，尺寸随标高、参照少于两条时，别的随它的平面。
  `ReferenceNear`、`ArcNear`（`Commands.h`）是 `dimension` 把点认成参照的办法，界面的尺寸工具也用
- 捕捉 `Snap::At`：场景里只有已移植的元素（墙、轴网、构件、柱、楼板、天花板、屋顶），草图模式里界面另给还没进模型的草图线（`extraLines`、`extraPoints`）；
  详图线的象限点与切点、点云、视图隐藏的元素随那些模块移植。放门窗：`OpeningOf`（`door` / `window` 命令做出的门窗，不放进模型）、`Model::OpeningSpan`（它所在的一段墙）、
  `Model::Centred`（离中点近时吸过去）
- 房间只存一个点，边界每次由点四周的线找（`Model::RoomRegion`）：墙中心线与房间分隔线，或在墙面量时（`Model::roomArea`）墙轮廓的边与分隔线。
  画到墙面的分隔线在中心线量时接着走到墙中心线（`Model::BoundingLines`，线本身不变）；在墙面量时点落在墙体里不成房间（`Model::WallHolding`，`room` 报错）
  墙动了房间跟着变；`floor`、`roof` 沿墙铺时，里面有房间的独立的一圈墙算室内

## 约束与坑

- 照 BIMCraft 的规则：不让错误输入弄崩程序。参数里的数都查有限、在 `MaxCoord` 以内，列表有 `MaxPoints`、名称有 `MaxText` 的上限；
  `Session::Execute` 捕获异常当作命令失败（BIMCraft 的 `catch_unwind`）。`CommandTests` 的恶意参数扫描保证每条命令只拒绝、不出内部错误、不弄坏模型
- 新的检查规则加在 `Model::Check`，错误信息以"类别 ID: "开头、说明要改什么；命令不必自己撤销。
  `Bim::Culprit` 从这样的报错取出元素的 ID：IFC 与 Revit 的导入据此逐个略去违反规则的元素，所以别的写法的报错会让整个导入失败
- 核心的报错信息照 BIMCraft 写英文（给人和 AI 代理读），不经 `Tr`；界面弹窗的标题翻译。命令名称（撤销步骤的名称）用 `N_` 标记
- 族的类型在 `Bim::Fam` 命名空间（`Fam::Kind` 是参数的种类，`Bim::Kind` 是库条目的种类；`Fam::Wall` 是墙厚参数名，`Bim::Wall` 是墙），
  在 `Bim` 里写族的代码时要带上 `Fam::`，不要 `using namespace Fam`
- `Model::Check` 每次都要查族，族的检查要把每个类型建一遍（含空心切割），Debug 下很慢：每个线程记住通过检查的族（不看形体与符号的 ID、GUID），
  同样的族在同样的材质名称下不再查；`FamilyLibrary` 每种语言只建一次
- 新增一种元素：在 `Element::Variant` 里加一种，`std::visit` 的访问器（`Element::Category`、`Element::RefsMut`）会编译报错列出要补的分支；
  再补 `ModelJson.cpp` 的读写（`ReadElement`，宽松读时还没移植的类别整个略去）、`Model::Check`、`Model::LevelOf`、命令的 `Transformed` 等
- 新增库条目的种类：`Model::LibraryNames`、`Element::LibRefsMut` / `Model::LibRefsMut`（文件里名称换 GUID）；新的部件加进 `Model::PartsNow`、`Model::SyncParts`
  和 `Model::ForgetParts`。`Model::SyncLibraryWith` 让调用的给出新条目与部件的 GUID（IFC 导入由文件内容得出，同一个文件每次一样）
- nlohmann/json：不用会抛异常的接口（`parse` 传 `allow_exceptions = false`，取值前查类型）；代码里构造的整数是有符号整数、解析出的是无符号整数，
  读非负整数用 `Bim::AsWhole`；不要在 range-for 里对临时对象调 `items()`（临时对象先析构，`Model::ToJson` 踩过）
- 数的读写不随系统区域设置：`Text::ParseNumber`（`from_chars`）、`Text::Shortest`（`to_chars`，照 Rust 的 `{}` 格式）
- 移植时还没有的部分在代码里注明是哪一步补上：贴图、填充样式、竖井（BIMCraft 的 `less_shafts`）、项目参数（元素 `params` 里不是族参数的值，
  读文件时略去）、收藏夹（`favorite`）、镜像与旋转命令（构件的 `mirrored` 已有）。
  草图（第 3e 步）：楼板、天花板、迹线屋顶记着画它们或最后编辑它们的草图线（`Floor::sketch` 等，`Element::SketchOf`），每条线是一个部件
  （"sketch line"，在 `Model::PartsNow`、`Model::SyncParts`、`Model::ForgetParts` 里），`Model::Check` 查线本身（`Sketch::Check`）；
  `edit_boundary` 不带 `sketch` 时给出现在的线（记着的线还画得出轮廓时用它们，否则轮廓的边当作新线），带 `sketch` 时解了换掉轮廓，线带 `id` 的保留 ID 与 GUID。
  只移植了直线与圆弧：`curves.rs` 的其他曲线（样条、椭圆……）读文件时整个草图略去（`left_out`），命令里拒绝
- 直骨架照 BIMCraft 用二叉堆排事件：同一时刻的事件顺序与 Rust 的堆可能不同，结果一样（`ModelTests` 的 2000 个随机迹线检查面积与平面性）

## 测试与排查

- `ModelTests`（`tests/Model/ModelTests.cpp`）：几何、单位、ID、项目语言、`Model::Check`、墙的接头与轮廓、加速索引、库、直骨架、族的公式与形体、CSG、单线字、捕捉（`snap.rs` 的测试）、门窗所在的一段与预览，
  移植自 BIMCraft 各模块的测试
- `CommandTests`（`tests/Commands/CommandTests.cpp`）：像前端一样经命令驱动（移植自 BIMCraft 的 `tests.rs`、`render.rs` 的测试）：撤销、脚本、事务、建模、编辑、平面图与拾取、
  项目文件（含打开 BIMCraft 文件时略去的内容）、示例房屋、恶意参数扫描，结构（`TestStructure`）与 MEP（`TestMep`：七类 run 的命令、
  被拒、类型、编辑、平面与捕捉，起步设备族在各语言里都有、每个类型的形体高度对）与体量（`TestMassing`：体量、体量楼层再跑与 prune、按面建模、
  `update_by_face`、放样）与注释（`TestAnnotations`：各注释命令、删除标高的连带、项目文件往返与平面、BIMCraft 旧文件的文字大小与别的视图里的注释、测量）
- 用到还没移植的命令的 BIMCraft 测试（剖面、平面图、IFC……）略去了那些断言，移植那些命令时补回；BIMCraft 测试里的 `view3d::tops_at`
  （某点上各元素顶面的高度）还没移植，测试里由 `Model::RoofPieces`、`Model::CeilingSlabs`、`Walls::TopAt` 算
- 结果对不上时，在 `D:\BIMCraft` 里 `cargo test -p bimcraft-core <名字>` 跑原来的测试对照（`.agents/features/porting.md`）
