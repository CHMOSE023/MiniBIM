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
移植过来的代码与译文按 BIMCraft 的 MIT 选项使用，原文如下。每个含移植代码的源文件开头写明对应的 BIMCraft 文件与基准提交
（第 2 阶段起：`src/Model/`、`src/Commands/`、`src/View/ModelScene.*` 与对应的测试）。

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

## Noto Sans SC

Linux 版、网页版的界面字体 `assets/fonts/NotoSansSC-UI.ttf` 由 Noto Sans SC 生成（`tools/make_web_font.py`，复制自 MiniCAD：固定为 Regular 字重，
只保留 GB2312 字符、常用符号和源码、词典里出现的字符）。

- 来源：Google Noto Fonts（https://github.com/notofonts/noto-cjk）
- 许可证：SIL Open Font License 1.1（https://openfontlicense.org），字体文件的 name 表里附有版权与许可声明

## nlohmann/json

`3rd/nlohmann`：[nlohmann/json](https://github.com/nlohmann/json) 3.11.3 的单头文件 `json.hpp`，模型核心读写 JSON（命令参数、项目文件）用，
MIT 许可，全文另见 `3rd/nlohmann/LICENSE.MIT`；没有修改（见 `3rd/nlohmann/README.md`）。

```
MIT License

Copyright (c) 2013-2022 Niels Lohmann

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

## SDL3

Linux 版（`MiniBIMLinux`）的窗口、输入、剪贴板与文件对话框使用 SDL3，发布包里以静态库链入。

- 来源：https://github.com/libsdl-org/SDL（版本见 `.github/workflows/build.yml` 的 `SDL3_VERSION`）
- 许可证：zlib，原文如下

```
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## SMLib（NVIDIA USD BRep）

三维实体建模内核 `3rd/SMLib`，来自 NVIDIA USD BRep（usd-brep）1.0.1，经 MiniCAD 复制过来；本地修改见 `3rd/SMLib/README.md`。

- 版权：Copyright (c) 2020-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
- 许可证：Apache License 2.0，全文见 `3rd/SMLib/LICENSE`（发布包里为 `LICENSE-SMLib`）；各源文件头部保留原版权声明，上游没有 NOTICE 文件
- 改动：按 Apache-2.0 第 4(b) 条，改过的文件已在 `3rd/SMLib/README.md` 的“本地修改”里逐一说明

## stb

`3rd/stb` 下的 `stb_image.h`、`stb_truetype.h`（MiniGUI 的图像解码与界面字体），来自 [nothings/stb](https://github.com/nothings/stb)，
公有领域或 MIT 二选一（见各文件末尾），本地修改见 `3rd/stb/README.md`。
