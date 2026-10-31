---
covers: src/Rvt/*, tests/Rvt/*, cmake/Rvt.cmake
---

# Revit 文件读取器

## 概述

移植自 BIMCraft `crates/rvt`（`f3bbb33`），在 `src/Rvt/`（目标 `MiniBIMRvt`，定义在 `cmake/Rvt.cmake`，命名空间 `MiniBIM::Rvt`），
开关 `MINIBIM_WITH_RVT`（默认开）。**这部分是 Apache-2.0，不是 MIT**：BIMCraft 的读取器有一部分又移植自 rvt-rs，许可原文与 NOTICE 在 `src/Rvt/`。
分段计划见 `docs/PLAN.md` 的"Revit 文件"；现在做完 R4（底层与记录层），导入（R5）、界面（R6）还没有：读得出项目里有哪些元素、标高与它们的数据，还不建成 MiniBIM 的模型。

| 代码 | 来自 BIMCraft | 内容 |
|---|---|---|
| `src/Rvt/Error.h` | `error.rs` | `Rvt::ErrorKind` 与 `Rvt::Error`（英文的说明，不经 `Tr`） |
| `src/Rvt/Bytes.h` | （自己写，代替 memchr 与 Rust 的 UTF-16） | 小端整数、子串查找、UTF-16 与 UTF-8 |
| `src/Rvt/Inflate.h` | （自己写，代替 flate2） | RFC 1951 的 raw deflate 解压 `Deflate::Inflate`（有输出上限，失败时输出不变）与 `Deflate::Crc32` |
| `src/Rvt/Cfb.h` | （自己写，代替 cfb） | MS-CFB 第 3、4 版复合文档的只读读取 `Cfb::File`：DIFAT、分配表、迷你流、目录树 |
| `src/Rvt/Gzip.h` | `gzip.rs` | 页（`Gzip::StripPages`）、流里的第一段或每一段 gzip（`Gzip::InflateFirst`、`Gzip::InflateMembers`） |
| `src/Rvt/Container.h` | `container.rs` | `Rvt::Revit`：打开文件（`Revit::Open`、`Revit::FromBytes`）、`Rvt::Limits`、读流与解压流 |
| `src/Rvt/Info.h` | `info.rs` | `BasicFileInfo` 流：版本年份、构建号、GUID、语言、保存路径与全部 `Key: value` 行 |
| `src/Rvt/Atom.h` | `atom.rs` | 族的 `PartAtom` 与项目的 `ProjectInformation`（一个条目的 ZIP）：标题、产品版本、OmniClass 与类别 |
| `src/Rvt/Preview.h` | `preview.rs` | `RevitPreview4.0` 里的 PNG，逐块查过大小与 CRC |
| `src/Rvt/Schema.h` | `schema.rs` | Formats/Latest 类表（`Rvt::Schema`、`Rvt::ReadSchema`）：类名到标签，标签随版本变 |
| `src/Rvt/ElemTable.h` | `elem_table.rs` | Global/ElemTable 声明的 ElementId（`Rvt::ReadElementIds`），两种记录大小靠找不靠猜 |
| `src/Rvt/Records.h` | `records.rs` | 分区里的元素记录（`Rvt::Record`，两种形式：到 2023 与 2024 起）、类型记录（`Rvt::TypeRecord`）、记录链，标记的标签取自类表（`Rvt::Tags`） |
| `src/Rvt/Inventory.h` | `inventory.rs` | 清单 `Rvt::ReadInventory`（按类别数放置的实例、标高与楼层设置），两遍读：`Rvt::Survey` 找记录，`Rvt::Gather` 再读要的元素数据、材质名、族名、柱底、房间 |
| `src/Rvt/Data.h` | `data.rs` | 元素的序列化数据（每个分区一份，`Data::Newest` 取最新的）、材质的名字、对象样式的类别材质、柱的几何节点、房间的编号与名称 |
| `src/Rvt/Names.h` | `names.rs` | 族与族类型的名字条目（`Names::Entries64`、`Names::Entries32`），`Names::Latest` 让最新的分区胜出 |
| `src/Rvt/Types.h` | `types.rs` | 类型的名字（`Types::FirstName`）、层（`Types::FindLayers`，布局按版本 `Types::LayoutOf`）、墙的朝向（`Types::Orientation`）、记录指的类型（`Types::UniqueType`） |
| `src/Rvt/Curves.h` | `curves.rs` | 元素数据里的有界直线与圆弧（墙的定位线） |
| `src/Rvt/Placement.h` | `placement.rs` | 族实例的放置（旋转与原点，`Placement::TransformOf`） |
| `src/Rvt/Sketch.h` | `sketch.rs` | 草图线连成的环（楼板、天花板、屋顶的边界）：`Sketch::FromEdges` 及容许散线、缺口、取整误差的几种做法 |

## 入口与数据流

`Revit::Open(path, limits, r, error)`（路径 UTF-8）只读复合文档的头、分配表与目录，流到用时才读：
`ReadStream` 给存着的字节（带页），`Inflate` 给第一段 gzip（`Global/*` 流、类表），`InflateAll` 把分区的每一段接起来；
容器分页存的流（`Gzip::IsPaged`）先去掉每 65,249 字节一页的最后 353 个字节再解。`ReadBasicFileInfo`、`DocumentAtom`、`ReadPreview` 在这之上。

记录层（R4）：`ReadSchema` 读类表，`Tags::Of` 按类名取出这个文件的标签（标记、数据头、层等）；`ReadElementIds` 读元素表；
`Survey` 把每个分区解开、按版本用 `Records::Records64` / `Records::Records32` 走一遍，得到清单与要的类别的记录（改过的元素在几个分区里各有一份）；
`Gather` 为这些记录再读一遍分区，收集元素数据与名字。各个解码器（`Types`、`Curves`、`Placement`、`Sketch`）只读一个元素的数据字节，
R5 的导入在这之上把墙、楼板等建成模型。

不用异常：函数返回 `bool`，失败时 `Rvt::Error` 说明是哪个流、哪一项上限、哪里坏了（报错照 BIMCraft 的原文写英文）。

## 约束与坑

- **许可的隔离**：核心库（`src/Model`、`src/Commands`、`src/Ifc` 等）不包含 `Rvt/` 下的头文件，`cmake/MiniBIM.cmake` 里没有 `src/Rvt`（`RvtTests` 查这两条）。
  每个文件开头写来源行（移植的写 `// 移植自 BIMCraft crates/rvt/src/<文件>.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE`，
  自己写的写"随 src/Rvt 以 Apache-2.0 发布"）；含 rvt-rs 代码的再用英文写 rvt-rs 的提交 `b6086d41b9758fd9b92f42f0a47b8345b9c58056`、文件、`Changed:` 与版权行，`RvtTests` 逐个文件查
- 只照 BIMCraft 的 docs/rvt-provenance.md 已列来源的读法移植，不自己逆向；Autodesk 的文件不进仓库，测试的文件都由 `tests/Rvt/Fixture.cpp` 现做
- 文件不可信：所有读法都有上限（`Rvt::Limits`：文件、流的个数、一个流、解开后的字节），链与目录树有环时报错，截短、随机改坏的文件不崩（`RvtTests` 有随机破坏）
- 自己的 inflate 不读 gzip 的头尾，`Gzip.cpp` 读；页的常数是对样例文件的观察（见 `src/Rvt/Gzip.h`），不要"顺手"改
- 标签不写死：类表的标签随版本变，一律经 `Tags::Of` 从文件自己的类表取；类表里缺必需的类时报错，缺可选的类（层、屋顶等）时那些读法不做
- 同一个元素在几个分区里各有一份（前一次保存后改过的）：名字与数据取最新的分区（`Names::Latest`、`Data::Newest`），同一个分区里两份不一致的不要
- 读法宁缺不错：值不在合理范围（不是单位向量、宽度超过十英尺、码点印不出来等）就当没有；草图的每个端点必须恰好由两条曲线共用，不猜顶点，
  曲线数上限 `Sketch::MaxEdges`，几种闭合做法对大量散线都要立刻拒绝（有计时测试）
- `Fixture::Deflate` 只写固定 Huffman 块或存储块：固定块几乎任何位序列都解得出，测"截短的段"时要用存储块（`Fixture::StoredGzip`），否则截短的段会把后面的垃圾解成数据

## 测试与排查

- `RvtTests`（要 `MINIBIM_WITH_RVT`），检查函数在 `tests/Rvt/Check.h`：
  - `tests/Rvt/RvtTests.cpp`：移植 BIMCraft 读取器的 src/tests/mod.rs 里的容器、gzip、恶意输入与 metadata.rs 里的 BasicFileInfo、Atom、预览图测试；
    另有自己写的 inflate（`tests/Rvt/DeflateVectors.inc` 是 zlib 压的动态 Huffman 块）与复合文档（第 4 版、DIFAT、环）的测试，以及许可的隔离
  - `tests/Rvt/RecordTests.cpp`：metadata.rs 里的类表、records.rs、inventory.rs 的合成文件测试（每个版本 2500 轮随机破坏，Debug 下每个版本几秒）
  - `tests/Rvt/DecoderTests.cpp`：decoders.rs（曲线、层、墙的朝向、名字、放置、名字条目、记录链）与 sketch.rs
  - 合成项目在 `tests/Rvt/ProjectFixture.cpp`（`ProjectFixture::Project`：类表、元素表与分区，两种记录形式），照 BIMCraft 的 project_fixture.rs
- BIMCraft 里要样例文件（`BIMCRAFT_RVT_SAMPLES`）的测试没有移植；walls.rs、floors.rs 等导入的测试在 R5 移植
