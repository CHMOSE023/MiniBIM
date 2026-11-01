<p align="center">
  <img src="assets/icons/app.ico" width="96" height="96" alt="MiniBIM" />
</p>

<h1 align="center">MiniBIM</h1>

<p align="center">
  <strong>An open-source BIM application written in C++20.</strong><br />
  Runs on Windows, Linux and in the browser, models with the SMLib solid kernel, ports its BIM features from BIMCraft, and speaks several languages.
</p>

<p align="center">
  <a href="https://github.com/CHMOSE023/MiniBIM/releases">Download</a> (Windows, Linux, web) ·
  <a href="https://minibim.bimhub.top">Try it in the browser</a> ·
  <a href="README_CN.md">简体中文</a>
</p>

<p align="center">
  <a href="https://github.com/CHMOSE023/MiniBIM/actions/workflows/build.yml"><img src="https://github.com/CHMOSE023/MiniBIM/actions/workflows/build.yml/badge.svg" alt="CI" /></a>
  <a href="https://github.com/CHMOSE023/MiniBIM/releases/latest"><img src="https://img.shields.io/github/v/release/CHMOSE023/MiniBIM?label=release" alt="Latest release" /></a>
  <a href="https://github.com/CHMOSE023/MiniBIM/releases"><img src="https://img.shields.io/github/downloads/CHMOSE023/MiniBIM/total?label=downloads" alt="Downloads" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/CHMOSE023/MiniBIM" alt="License" /></a>
  <a href="https://qm.qq.com/q/O08yWfRvmE"><img src="https://img.shields.io/badge/QQ群-加入-12B7F5?logo=tencentqq&logoColor=white" alt="QQ group" /></a>
  <a href="https://github.com/CHMOSE023/MiniBIM/stargazers"><img src="https://img.shields.io/github/stars/CHMOSE023/MiniBIM?style=flat" alt="Stars" /></a>
</p>

![MiniBIM](docs/images/screenshot.png)

MiniBIM reuses the user interface and renderer of [MiniCAD](https://github.com/CHMOSE023/MiniCAD), models with the SMLib
geometry kernel, and is porting its BIM features from [BIMCraft](https://github.com/IPedrax/BIMCraft). The interface is multilingual.

Version 0.2.0 has BIMCraft's model, its interface and the rest of its ribbon tabs:

- **Model**: levels, grids, walls, floors, ceilings, roofs, rooms, doors and windows, families and components with their types and materials,
  columns; every edit is a command with undo; project files (`.minibim`, the same format as BIMCraft's `.bimcraft`)
- **Views**: 3D modelled with SMLib, plans of each level, sections and elevations, schedules, sheets, visibility / graphics overrides, view templates and filters
- **Tabs**: Architecture, Structure, Plumbing, Electrical, Lightning Protection, Data, Insert, Annotate, Massing, Modify, View and Manage
- **Exchange**: IFC (IFC2x3 / IFC4 in, IFC4 out), Revit projects (`.rvt`, read-only, with a report of what was left out), linked models,
  DXF import and link, point clouds (PTS, XYZ, PLY, LAS, E57)

What's in each release is in [docs/release-notes](docs/release-notes); what comes next is in [docs/PLAN.md](docs/PLAN.md) (in Chinese):
stairs, ramps and railings, groups, phases and design options, among others.

### Building on Windows

You need CMake 3.20+, Ninja and Visual Studio (MSVC, C++20).

```bat
build.bat
out\debug\MiniBIMWin\MiniBIMWin.exe
```

Outside a Visual Studio developer environment, `build.bat` finds Visual Studio and runs `vcvars64.bat` itself. Inside a
"Developer Command Prompt for VS" you can also run:

```bat
cmake --preset debug
cmake --build --preset debug
```

`build.bat release` builds the release version (a windowed program without a console).

Command-line options: `--lang zh_CN` (the language for this run only), `--gpu d3d12|d3d11|warp` (or the environment variable
`MINIBIM_GPU`), `--ui <interface description file.json>`.

### Building on Linux

You need CMake 3.20+, Ninja, GCC 13+ or Clang (C++20), the OpenGL ES 3 / EGL development packages and
[SDL3](https://github.com/libsdl-org/SDL). On Ubuntu / Debian:

```bash
sudo apt install build-essential cmake ninja-build pkg-config libegl-dev libgles-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libdbus-1-dev libudev-dev
```

The Vulkan renderer (optional; used first when present, with OpenGL ES 3 as the fallback) also needs the headers and the shader compiler:

```bash
sudo apt install libvulkan-dev glslang-tools mesa-vulkan-drivers
```

SDL3 is `sudo apt install libsdl3-dev` on Ubuntu 25.04 / Debian 13 and later; on older releases build it from source, for example into `~/sdl3`:

```bash
cmake -S SDL3-3.4.18 -B sdl3-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/sdl3 -DSDL_TESTS=OFF
cmake --build sdl3-build && cmake --install sdl3-build
```

Then build MiniBIM (leave out `CMAKE_PREFIX_PATH` when SDL3 is installed system-wide):

```bash
cmake -S . -B out/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$HOME/sdl3
cmake --build out/linux
out/linux/MiniBIMLinux/MiniBIMLinux [project file]
```

Without SDL3 or OpenGL ES 3 only the core library and the tests are built (`ctest --test-dir out/linux`); without the Vulkan
packages only OpenGL ES 3 is used. The command-line options are the Windows ones, with `--gpu vulkan|gl`. The open and save
dialogs need xdg-desktop-portal or zenity.

### Building for the web

You need the [Emscripten SDK](https://emscripten.org) (`build_wasm.bat` expects it in `D:\dev\emsdk`; change `EMSDK_ENV` at its top).

```bat
build_wasm.bat serve
```

This builds `out/build/wasm/MiniBIMWeb/` and serves it at http://localhost:8080/MiniBIMWeb/. The web version uses the browser's
WebGPU and falls back to WebGL2 (add `?webgl` to the address to force it). Opening reads the chosen file into the page; saving and
exporting IFC ask for a file name and download the file. It doesn't autosave or keep settings. The first build downloads the WebGPU
bindings (the emdawnwebgpu port) from GitHub.

### Tests

```bat
run_tests.bat
```

It builds, runs `ctest` (the model, the commands, IFC, the Revit reader, the 3D scene, the main window, translations, the camera and an SMLib smoke test) and checks that the translation catalogs are up to date
(`tools/extract_messages.py --check`) and that every icon the interface uses is generated (`tools/make_icons.py --check`). GitHub Actions builds the Windows, Linux and web versions and runs the tests on every pull request to `main`, and
packages and publishes a release when a `v*` tag is pushed (`.github/workflows/build.yml`), then deploys the web version to
https://minibim.bimhub.top (Cloudflare Pages, `.github/workflows/pages.yml`).

### Languages

Interface text is written in English in the code and translated at run time from gettext catalogs (`assets/i18n/*.po`). Pick a
language under File > Language, or follow the system's. Simplified Chinese, English, Spanish and Portuguese are available today. A new language is
one `.po` file, with no code changes, and translations are welcome. See [docs/i18n.md](docs/i18n.md) (in Chinese) for how it
works.

### Layout

| Directory | Contents |
|---|---|
| `src/` | The `MiniBIM` core library: translations (`I18n/`), the camera and the model's 3D scene (`View/`), math and renderer interfaces (`Core/`, `Render/`), the solid kernel interface (`Solid/`) |
| `src/Model/`, `src/Commands/` | The building model, its commands and undo, ported from BIMCraft |
| `src/Ifc/` | IFC exchange: STEP reading, IFC4 export, IFC2x3 / IFC4 import |
| `src/Rvt/` | The Revit file reader (Apache-2.0, see License) |
| `src/UI/` | The MiniGUI interface library (copied from MiniCAD) |
| `apps/Common/` | The main window's interface and logic (platform-independent) |
| `apps/Render/` | Renderers (D3D12 and D3D11 on Windows, Vulkan and OpenGL ES 3 on Linux, WebGPU and WebGL2 in the browser) |
| `apps/Win32/`, `apps/Linux/`, `apps/Web/` | The Windows desktop host; the Linux host (SDL3); the web host (Emscripten) |
| `assets/` | The interface description `ui/minibim_ui.json`, the catalogs in `i18n/`, the icons in `icons/` and the interface font for Linux and the web in `fonts/` |
| `3rd/` | SMLib (NURBS B-rep solid kernel, Apache-2.0), stb and nlohmann/json (MIT) |
| `tools/` | `extract_messages.py`: collects interface text and updates the catalogs; `make_icons.py`: draws every icon MiniBIM uses; `make_web_font.py`: makes the interface font for Linux and the web |
| `tests/` | Tests |
| `.agents/` | Notes, feature guides and skills for AI coding assistants (start at [AGENTS.md](AGENTS.md)) |

### License

MiniBIM is released under the [MIT License](LICENSE). Third-party components (SMLib is Apache-2.0) and the code MiniBIM is
ported from are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The Revit file reader in `src/Rvt/` (ported from
BIMCraft, which ported parts of it from rvt-rs) is Apache-2.0 only, not MIT; builds that include it (the default, `MINIBIM_WITH_RVT`)
ship its licence and notice as `LICENSE-Rvt` and `NOTICE-Rvt`.

---

<sub>Autodesk and Revit are trademarks of Autodesk, Inc. MiniBIM is an independent open-source project and is not affiliated with Autodesk. IFC is an open standard of buildingSMART International (ISO 16739).</sub>
