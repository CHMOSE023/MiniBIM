---
covers: docs/PLAN.md, THIRD_PARTY_NOTICES.md
---

# 移植 BIMCraft

## 概述

BIMCraft（Rust，基准提交 `f3bbb33`，本机在 `D:\BIMCraft`）的核心 `crates/core` 没有界面，所有编辑都是命令：`commands.rs` 的命令表 `ALL`，
每条命令吃 JSON 参数、返回 JSON 结果或错误字符串，由 `engine.rs` 的 `Session` 执行（失败回滚、撤销）。界面、命令行、脚本都只调命令。
MiniBIM 照这个结构移植：先移植核心并带上它的测试，界面最后在 MiniGUI 上重写。顺序见 `docs/PLAN.md` 第 2 阶段。

## 动手之前

- 读 BIMCraft 根目录的 `CLAUDE.md`（模块地图）和 `crates/core/CLAUDE.md`（改动放在哪），以及要移植的模块对应的 `docs/areas/*.md`
- 一次移植一个模块（或一组紧密相关的），连同它的测试；编译、测试通过再做下一个
- 只看 BIMCraft 的 `crates/core`、`crates/ui-egui`、`crates/i18n`；不看、不移植 `crates/rvt`（只有 Apache-2.0，且依赖对 Revit 格式的逆向）

## 来源标注（许可要求）

每个含移植代码的源文件开头一行：

```cpp
// 移植自 BIMCraft crates/core/src/walls.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
```

一个 C++ 文件来自几个 Rust 文件时都列上。BIMCraft 的版权行与 MIT 原文已在 `THIRD_PARTY_NOTICES.md`，不用逐个文件再贴。

## Rust → C++ 的对应写法

| Rust | C++（MiniBIM） | 说明 |
|---|---|---|
| `f64`，毫米 | `double`，毫米 | 单位不变；角度看原代码是度还是弧度 |
| `P2` / 三维点 | `Math::Point2` / `Math::Point3` | `src/Core/Math`，行向量约定 |
| `Vec<T>`、`HashMap`、`BTreeMap` | `std::vector`、`std::unordered_map`、`std::map` | 要求按 id 有序遍历的地方（`BTreeMap`）用 `std::map`，不要换成无序的 |
| `Option<T>` | `std::optional<T>` | |
| `Result<T, String>` | 返回 `bool` / `std::optional`，错误放进 `std::string& error` | 不抛异常；BIMCraft 禁止 panic，C++ 里同样不让错误输入弄崩程序 |
| 带数据的 `enum`（如 `Element`） | `std::variant` + `std::visit` | 新增一种元素时编译器能列出所有要补的分支，和 Rust 的 `match` 一样 |
| `trait` | 抽象基类或模板 | |
| `Arc<T>`（共享、不可变） | `std::shared_ptr<const T>` | |
| `u64` 元素 ID、`u128` GUID | `uint64_t`、两个 `uint64_t` 的结构体 | 引用别的元素一律存 GUID，不存 ID（BIMCraft 的规则） |
| `msg!` / `ui_msg!` 标记的原文 | `N_("…")` / `Tr("…")` | 英文原文照抄，译文以后从 BIMCraft 的 `.po` 合并（见 `docs/i18n.md`） |
| `#[test] fn …` | `tests/<模块>/…Tests.cpp` 里的 `Check(…)` | 一条测试一个函数，名字照原来的意思写 |

## 还没定的事（移植第一个用到它的模块前先问用户）

- **JSON 库**：命令的参数与结果、项目文件（`.bimcraft` 是 JSON）都要用。可选：把 MiniGUI 的 `Data/Json.h` 移进核心库（核心库不能依赖界面库），
  或引入 nlohmann/json（MIT）。前者没有新依赖，后者功能全
- **项目文件格式**：沿用 BIMCraft 的 `.bimcraft`（能互相打开）还是另定扩展名
- **撤销**：BIMCraft 的 `Session` 按整个模型做快照与回滚；照搬，还是用 MiniCAD 的 `ICommand` / `CommandStack`（逐条命令的 Execute / Undo）

## 测试与排查

- BIMCraft 的测试大多经 Session 的 execute 驱动命令（`crates/core/src/tests.rs`），移植后同样经 C++ 的 Session 驱动，这样命令、检查、回滚一起测到
- `hostile_params_never_panic_or_break_the_model`：用各种恶意参数扫一遍所有命令，模型不能坏、程序不能崩。命令表成形后移植它
- 结果对不上时，在 `D:\BIMCraft` 里 `cargo test -p bimcraft-core <名字>` 跑原来的测试，或用它的命令行 `bimcraft-cli` 跑同样的命令对照输出
