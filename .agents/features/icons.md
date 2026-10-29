---
covers: tools/make_icons.py, assets/icons/*, apps/Win32/src/MiniBIM.rc, apps/Win32/src/resource.h
---

# 图标

## 概述

**MiniBIM 用到的图标一律由 `tools/make_icons.py` 生成**，不手画、不从别处拷图片（包括 MiniCAD、BIMCraft 的图标与标志）。
以后功能区、工具栏、项目浏览器、属性面板要的图标，都是移植或新增那个命令时在脚本里加一个函数生成出来的。
脚本照 MiniCAD 的 `tools/make_icons.py` 写：同样的绘图工具类和风格，界面看起来与 MiniCAD 一家。

## 两类图标

| 类 | 文件 | 用在哪 | 风格 |
|---|---|---|---|
| 命令图标 | `assets/icons/<名称>.png`，64×64 RGBA | 界面描述文件 `commands` 的 `"icon"`，功能区 / 工具栏按钮（菜单项不显示图标） | 灰色线条 (179,179,179) + 青色强调 (0,192,232)，不透明度 224，线宽 4（64 网格），圆头；4 倍尺寸绘制后缩小抗锯齿 |
| 程序图标 | `assets/icons/app.ico`（16、20、24、32、40、48、64、128、256）、`assets/icons/app.png`（64×64） | `app.ico` 编进 exe（`MiniBIM.rc` 的 `IDI_ICON`）：资源管理器、任务栏、窗口；`app.png`：标题栏左上角（界面描述文件 titlebar 的 `"icon"`） | **透明背景**上一座**绿色**的等轴测楼（`APP_GREEN` = (88,226,19)，与 MiniCAD 的程序图标同色；平涂、不用渐变、没有底板）：顶面浅、右侧面深，侧面白色横线是楼层，楼填满画布；不画成平面的小房子（太像"首页"按钮）；按 1024 绘制，逐个尺寸缩小 |

## 新增一个命令图标

1. 在 `tools/make_icons.py` 的“命令图标”一节加一个函数：`@icon("名称")`，名称用帕斯卡命名（`WallTool`、`LevelNew`），就是文件名；
   注释写它给哪个命令用。在 64×64 网格上画，用 `Icon` 的 `line`、`poly`、`rect`、`circle`、`arc`、`arrow`、`dashed`、`dot` 和共用图形（`cube` 等）。
   主体用灰色 `G`，要强调的部分（新建的东西、动作的方向）用青色 `A` / `AF`。BIM 构件尽量用等轴测，与 `cube` 的角度一致
2. 运行（在仓库根目录）：

   ```powershell
   python tools/make_icons.py --preview out/tmp/icons.png
   ```

   生成全部图标，并拼一张预览图（深、浅两种底色）；用 Read 工具看预览，两种底色下都要看得清
3. 在 `assets/ui/minibim_ui.json` 的 `commands` 里给命令写 `"icon": "<名称>.png"`
4. `python tools/make_icons.py --check`（skill `test` 改了图标会自动跑）：引用的图标都有定义、都已生成，`assets/icons` 里没有脚本以外来的文件

改了程序图标（`app_icon`）后要重新编译 `MiniBIMWin`：`MiniBIM.rc` 经 `OBJECT_DEPENDS` 依赖 `app.ico`，会重编资源。

## 约束与坑

- 生成要 Pillow（`pip install pillow`）。这台机器的 Pillow 装在用户目录的 site-packages 里：`python -I` 会屏蔽它，运行脚本不要加 `-I`
- `--check` 不需要 Pillow，CI 的 `Assets` 任务跑它；不在 CI 上重新生成比较像素（不同版本的 Pillow 缩小时结果略有差别）
- 生成的 PNG 和 ICO 要提交进仓库（程序运行时直接读它们）；改了脚本就重新生成，一起提交
- 不要用别的软件的图标或标志当参考临摹；Revit 的图标、BIMCraft 的标志都不行（见 `THIRD_PARTY_NOTICES.md`）
- 程序图标在 16×16 下也要认得出：改设计时看预览图最下面一排的小尺寸
- 颜色分工（用户定的）：程序图标是绿色，其他图标（命令图标等）用蓝色系（青色强调 `A`），不要混用
