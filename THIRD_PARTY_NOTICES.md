# 第三方声明

MiniBIM 本身按 [MIT 许可证](LICENSE) 发布。下面是它包含或移植自的第三方代码与资源，以及各自的许可证。
发布包里随可执行文件附带 `LICENSE`、本文件和 `LICENSE-SMLib`。

## MiniCAD

界面库 MiniGUI（`src/UI/`）、渲染后端（`apps/Render/`）、数学与渲染接口（`src/Core/`、`src/Render/`）、实体内核接口及其 SMLib 适配层（`src/Solid/`）、
构建脚本（`cmake/`）、桌面版宿主的骨架（`apps/Win32/`）复制自 [MiniCAD](https://github.com/CHMOSE023/MiniCAD)，
与 MiniBIM 同一作者、同为 MIT 许可证。

- 复制基准提交：`0bd1687df2f45b10dff06df86bfa41d59792482b`（2026-10-29）
- 复制之后的改动见 `docs/PLAN.md` 的“与 MiniCAD 的差异”

## BIMCraft

MiniBIM 计划把 [BIMCraft](https://github.com/IPedrax/BIMCraft)（Rust）的模型、命令与界面移植为 C++（见 `docs/PLAN.md`）。
移植过来的代码与译文按 BIMCraft 的 MIT 选项使用，原文如下。移植开始后，每个含移植代码的源文件开头写明对应的 BIMCraft 文件与基准提交。

- 移植基准提交：`f3bbb33f3b8b6cc6f80b3bd6611e33c330cb448f`（2026-10-28）
- 不移植 `crates/rvt`（Revit 文件读取器）：它只有 Apache-2.0 许可，含 rvt-rs 的代码，并且依赖对 Revit 格式的逆向分析
- 不使用 BIMCraft 的名称、标志与图标

```
MIT License

Copyright (c) 2026 IPedrax and the BIMCraft contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## SMLib（NVIDIA USD BRep）

三维实体建模内核 `3rd/SMLib`，来自 NVIDIA USD BRep（usd-brep）1.0.1，经 MiniCAD 复制过来；本地修改见 `3rd/SMLib/README.md`。

- 版权：Copyright (c) 2020-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
- 许可证：Apache License 2.0，全文见 `3rd/SMLib/LICENSE`（发布包里为 `LICENSE-SMLib`）；各源文件头部保留原版权声明，上游没有 NOTICE 文件
- 改动：按 Apache-2.0 第 4(b) 条，改过的文件已在 `3rd/SMLib/README.md` 的“本地修改”里逐一说明

## stb

`3rd/stb` 下的 `stb_image.h`、`stb_truetype.h`（MiniGUI 的图像解码与界面字体），来自 [nothings/stb](https://github.com/nothings/stb)，
公有领域或 MIT 二选一（见各文件末尾），本地修改见 `3rd/stb/README.md`。
