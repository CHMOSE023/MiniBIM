---
covers: docs/PLAN.md, THIRD_PARTY_NOTICES.md, src/Model/*, src/Commands/*
---

# 移植 BIMCraft

## 概述

BIMCraft（Rust，基准提交 `f3bbb33`，已同步 `0733fd4` 的部分修复，见 `docs/PLAN.md`；本机在 `D:\BIMCraft`）的核心 `crates/core` 没有界面，所有编辑都是命令：`commands.rs` 的命令表 `ALL`，
每条命令吃 JSON 参数、返回 JSON 结果或错误字符串，由 `engine.rs` 的 `Session` 执行（失败回滚、撤销）。界面、命令行、脚本都只调命令。
MiniBIM 照这个结构移植：先移植核心并带上它的测试，界面最后在 MiniGUI 上重写。顺序见 `docs/PLAN.md` 第 2 阶段；移植过来的代码在哪、
怎么用见 `.agents/features/model.md`。

## 动手之前

- 读 BIMCraft 根目录的 `CLAUDE.md`（模块地图）和 `crates/core/CLAUDE.md`（改动放在哪），以及要移植的模块对应的 `docs/areas/*.md`
- 一次移植一个模块（或一组紧密相关的），连同它的测试；编译、测试通过再做下一个
- 只看 BIMCraft 的 `crates/core`、`crates/ui-egui`、`crates/i18n` 与 `crates/rvt`（2026-10-31 起移植，见 `docs/PLAN.md` 的"Revit 文件"）
- `crates/rvt` 移植到 `src/Rvt/`，是 **Apache-2.0**：来源行写成
  `// 移植自 BIMCraft crates/rvt/src/<文件>.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE`；
  核心库与 `apps/Common` 不包含 `src/Rvt` 的头文件（宿主接上读取器）。只照 BIMCraft 仓库里 docs/rvt-provenance.md 列了来源的读法移植，不另外逆向；测试用合成的文件（`fixture.rs`、`project_fixture.rs`），
  不把 Autodesk 的文件放进仓库。已移植的部分、自己写的复合文档与 inflate、测试的合成样例见 `.agents/features/rvt.md`

## 来源标注（许可要求）

每个含移植代码的源文件开头一行：

```cpp
// 移植自 BIMCraft crates/core/src/walls.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
```

一个 C++ 文件来自几个 Rust 文件时都列上。之后从 BIMCraft 新版同步个别修复时来源行不改，改动记在 `docs/PLAN.md` 的"同步 BIMCraft 新版"，移植的测试注明新版的提交。BIMCraft 的版权行与 MIT 原文已在 `THIRD_PARTY_NOTICES.md`，不用逐个文件再贴。

## Rust → C++ 的对应写法

| Rust | C++（MiniBIM） | 说明 |
|---|---|---|
| `f64`，毫米 | `double`，毫米 | 单位不变；角度看原代码是度还是弧度 |
| `P2` | `Bim::P2`（`src/Model/Geom.h`） | 点与向量同一类型，运算照 BIMCraft（`Math::Point2` 区分点和向量，照搬的代码不好写） |
| `[f64; 3]`（三维点） | `Bim::P3`（`std::array<double, 3>`）；交给渲染、实体内核时换成 `Math::Point3` | |
| `Vec<T>`、`HashMap`、`BTreeMap` | `std::vector`、`std::unordered_map`、`std::map` | 要求按 id 有序遍历的地方（`BTreeMap`）用 `std::map`，不要换成无序的 |
| `Option<T>` | `std::optional<T>` | |
| `Result<T, String>` | 返回 `bool` / `std::optional`，错误放进 `std::string& error` | 不抛异常；BIMCraft 禁止 panic，C++ 里同样不让错误输入弄崩程序。`Session::Execute` 捕获漏网的异常当作命令失败 |
| `serde`（`#[derive(Serialize, Deserialize)]`） | `src/Model/ModelJson.cpp` 里手写的读写（nlohmann/json） | 字段名、省略默认值的规则照 serde 的标注，写出的文件 BIMCraft 能读 |
| `Arc<Model>` + `make_mut` | `shared_ptr<const Model>` + `Session::ModelMut` | 元素同样是共享的不可变对象，`Elements::GetMut` 写时复制 |
| 带数据的 `enum`（如 `Element`） | `std::variant` + `std::visit` | 新增一种元素时编译器能列出所有要补的分支，和 Rust 的 `match` 一样 |
| `trait` | 抽象基类或模板 | |
| 字段相同、只差类别的几种元素（`Door` / `Window`，`Column` / `StructuralColumn` / `Footing`，九类 run） | 派生自同一个结构体（`Door : Opening`、`StructuralColumn : Column`、`Beam : Run`……），各占 `Element::Variant` 的一种 | 共用的代码经 `Element::AsOpening`、`Element::AsColumn`、`Element::AsRun` 取基类 |
| `Arc<T>`（共享、不可变） | `std::shared_ptr<const T>` | |
| `u64` 元素 ID、`u128` GUID | `uint64_t`、两个 `uint64_t` 的结构体 | 引用别的元素一律存 GUID，不存 ID（BIMCraft 的规则） |
| `msg!` / `ui_msg!` 标记的原文 | `N_("…")` / `Tr("…")` | 英文原文照抄，译文以后从 BIMCraft 的 `.po` 合并（见 `docs/i18n.md`）。命令的报错不标记：照 BIMCraft 写英文 |
| `Language::word`（项目词典 `translations/project/*.po`） | `LanguageOf::Word`（`src/Model/Language.cpp` 的词表） | 只搬了用到的词；与界面语言无关 |
| `#[test] fn …` | `tests/<模块>/…Tests.cpp` 里的 `Check(…)` | 一条测试一个函数，名字照原来的意思写 |
| `memchr::memmem::find_iter` | `Bytes::ForEach`，要中途停下时用 `Bytes::Find` 从 `at + needle.size()` 接着找 | Revit 读取器（`src/Rvt`）：找到的位置**不重叠**，换成逐字节找会多出候选 |
| `String::from_utf16`、`char::is_control`、`str::trim().is_empty()` | `Bytes::CodePointsAt`、`Bytes::IsControl`、`Bytes::IsBlank` | 照 Rust 的规则：不成对的代理是错，控制字符只算 Cc，空白按 Unicode 的 White_Space |
| `buf.get(a..b) == Some(x)` | `Bytes::Matches` | 越界时为假，不读出界 |
| `sort_by` / `sort_by_key`（稳定） | `std::stable_sort` | `std::sort` 不稳定，排好后取第一个的地方结果会变 |
| `f64::total_cmp` | `Bytes::TotalLess`（`src/Rvt/Bytes.h`） | NaN 与 −0 也有位置，不能直接用 `<` |
| 迭代器的 `max_by_key` / `max_by`、`min_by_key` / `min_by` | 手写的循环 | 一样大时 `max_*` 取**最后一个**、`min_*` 取第一个；`std::max_element` 取第一个，结果会变 |
| `f64` 的 `{}`（`format!`、`to_string`） | `Bim::Text::Shortest` | 最短的、读回来不变的写法，`1.0` 写成 `1`；不要用 `std::to_string`（固定六位小数） |

## 已定的事（用户在第 2a 步定的）

- **JSON 库**：nlohmann/json 3.11.3（MIT，`3rd/nlohmann`，`ordered_json`）。没用 MiniGUI 的 `Data/Json.h`：它把数存成 `double`，项目的 `uid` 是完整的 64 位整数
- **项目文件**：内容与 BIMCraft 的 `.bimcraft` 相同（同一个 `format` 号，两边互相打开），扩展名 `.minibim`（不用 BIMCraft 的名称），也能打开 `.bimcraft`
- **撤销**：照 BIMCraft 存整个模型的快照（`Session`），不用 MiniCAD 的 `ICommand` / `CommandStack`

## 移植界面（第 3 阶段）

- `crates/ui-egui` 基于 egui（立即模式），不能逐行翻译：在 MiniGUI（保留模式的控件树）上重写，照它的行为、文字和布局（同 Revit）。
  分段见 `docs/PLAN.md` 第 3 阶段；来源行写 `crates/ui-egui/src/<文件>.rs`
- 立即模式每帧重画，MiniGUI 的面板要在模型、选择集、视图变了时重建（`MainFrame::RefreshPanels`）；控件回调里不拆掉控件自己，改模型的命令经 `AppPlatform::Post` 推后
- 不碰控件的逻辑（参数表、值的读写、搜索……）放进只依赖核心库的文件（如 `apps/Common/src/GUI/Parameters.cpp`），由 `AppTests` 测；BIMCraft 界面的测试（`crates/ui-egui/src/tests*`）
  多半驱动 egui，能移植的是这类纯逻辑的
- 界面文字照 BIMCraft 的英文原文（`msg!` 的那些）；中文译文可以先查 BIMCraft 的 `translations/ui/zh.po`，不合适的（如 Elevation 在标高上是"高程"）再改

## 测试与排查

- BIMCraft 的测试大多经 Session 的 execute 驱动命令（`crates/core/src/tests.rs`），移植后同样经 C++ 的 Session 驱动，这样命令、检查、回滚一起测到
- `hostile_params_never_panic_or_break_the_model`：用各种恶意参数扫一遍所有命令，模型不能坏、程序不能崩。命令表成形后移植它
- 结果对不上时，在 `D:\BIMCraft` 里 `cargo test -p bimcraft-core <名字>` 跑原来的测试，或用它的命令行 `bimcraft-cli` 跑同样的命令对照输出
