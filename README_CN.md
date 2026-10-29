<p align="center">
  <img src="assets/icons/app.ico" width="96" height="96" alt="MiniBIM" />
</p>

<h1 align="center">MiniBIM</h1>

<p align="center">
  <strong>开源 BIM 软件，使用 C++20 开发。</strong><br />
  在 Windows 上运行，几何内核使用 SMLib，建模功能移植自 BIMCraft，界面支持多语言。
</p>

<p align="center">
  <a href="https://github.com/CHMOSE023/MiniBIM/releases">桌面版下载</a> ·
  <a href="README.md">English</a>
</p>

<p align="center">
  <a href="https://github.com/CHMOSE023/MiniBIM/actions/workflows/build.yml"><img src="https://github.com/CHMOSE023/MiniBIM/actions/workflows/build.yml/badge.svg" alt="CI" /></a>
  <a href="https://github.com/CHMOSE023/MiniBIM/releases/latest"><img src="https://img.shields.io/github/v/release/CHMOSE023/MiniBIM?label=release" alt="Latest release" /></a>
  <a href="https://github.com/CHMOSE023/MiniBIM/releases"><img src="https://img.shields.io/github/downloads/CHMOSE023/MiniBIM/total?label=downloads" alt="Downloads" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/CHMOSE023/MiniBIM" alt="Licence" /></a>
  <a href="https://qm.qq.com/q/O08yWfRvmE"><img src="https://img.shields.io/badge/QQ群-加入-12B7F5?logo=tencentqq&logoColor=white" alt="QQ群" /></a>
  <a href="https://github.com/CHMOSE023/MiniBIM/stargazers"><img src="https://img.shields.io/github/stars/CHMOSE023/MiniBIM?style=flat" alt="Stars" /></a>
</p>

![MiniBIM](docs/images/screenshot_cn.png)

MiniBIM 的界面与渲染沿用 [MiniCAD](https://github.com/CHMOSE023/MiniCAD)，几何内核使用 SMLib，
建模功能移植自 [BIMCraft](https://github.com/IPedrax/BIMCraft)，界面支持多语言。

> 目前处于第 2 阶段（移植 BIMCraft 的模型）：标高、轴网、墙、楼板、天花板、屋顶、房间、门窗、族与构件及其类型与材质，带撤销的命令，项目文件（`.minibim`，
> 与 BIMCraft 的 `.bimcraft` 同一格式）已经移植。程序启动时打开用命令搭起来的示例房屋，三维经 SMLib 建模。
> 后面的移植计划见 [docs/PLAN.md](docs/PLAN.md)。

### Windows 版编译

需要 CMake 3.20+、Ninja、Visual Studio（MSVC，C++20）。

```bat
build.bat
out\debug\MiniBIMWin\MiniBIMWin.exe
```

`build.bat` 不在 VS 开发环境里时会自己找到 VS 并调用 `vcvars64.bat`；已经在 "Developer Command Prompt for VS" 里时也可以直接：

```bat
cmake --preset debug
cmake --build --preset debug
```

发布版用 `build.bat release`（窗口程序，没有控制台）。

启动参数：`--lang zh_CN`（只对这一次启动指定语言）、`--gpu d3d12|d3d11|warp`（也可以用环境变量 `MINIBIM_GPU`）、`--ui <界面描述文件.json>`。

### 测试

```bat
run_tests.bat
```

编译后运行 `ctest`（多语言、相机、SMLib 冒烟测试），再检查词典是否需要更新（`tools/extract_messages.py --check`）、
界面用到的图标是否都已生成（`tools/make_icons.py --check`）。
GitHub Actions 在每个到 `main` 的 PR 上编译 Windows 版并运行测试，推送 `v*` 标签时打包并发布（`.github/workflows/build.yml`）。

### 多语言

界面文字在代码里写英文原文，运行时按词典（`assets/i18n/*.po`，gettext 格式）翻译；"文件 > 语言"里切换，或跟随系统语言。
现在有简体中文和英文；新增一种语言只需要加一个 `.po` 文件，不用改代码，欢迎提交译文。做法见 [docs/i18n.md](docs/i18n.md)。

### 目录

| 目录 | 内容 |
|---|---|
| `src/` | 核心库 `MiniBIM`：多语言（`I18n/`）、相机与模型的三维场景（`View/`）、数学与渲染接口（`Core/`、`Render/`）、实体内核接口（`Solid/`） |
| `src/Model/`、`src/Commands/` | 建筑模型、命令与撤销（移植自 BIMCraft） |
| `src/UI/` | 界面库 MiniGUI（复制自 MiniCAD） |
| `apps/Common/` | 主窗口的界面与逻辑（与平台无关） |
| `apps/Render/` | 渲染后端（D3D12、D3D11） |
| `apps/Win32/` | Windows 桌面版宿主 |
| `assets/` | 界面描述文件 `ui/minibim_ui.json`、词典 `i18n/`、图标 `icons/` |
| `3rd/` | SMLib（NURBS BRep 实体内核，Apache-2.0）、stb、nlohmann/json（MIT） |
| `tools/` | `extract_messages.py`：收集界面原文、更新词典；`make_icons.py`：生成 MiniBIM 用到的全部图标 |
| `tests/` | 测试 |
| `.agents/` | AI 编程助手的说明、功能指南与 skill（入口是 [AGENTS.md](AGENTS.md)） |

### 许可

MiniBIM 按 [MIT 许可证](LICENSE) 发布。第三方组件（SMLib 为 Apache-2.0）与移植来源见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
 
---

<sub>Autodesk 与 Revit 是 Autodesk, Inc. 的商标。MiniBIM 是独立的开源项目，与 Autodesk 没有关联。IFC 是 buildingSMART International 的开放标准（ISO 16739）。</sub>