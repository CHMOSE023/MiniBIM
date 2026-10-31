---
name: ui-check
description: 运行 MiniBIM 桌面版并截下主窗口，核对界面改动的实际效果：布局、菜单、三维视口、状态栏，以及中文 / 英文等各语言下的文字（截断、遗漏翻译、标点）。改了 apps/Common、assets/ui、src/UI 或词典之后用它看一眼。不用于跑回归测试（用 test），也不截整个屏幕。
covers: .agents/skills/ui-check/*, apps/Win32/src/Host/GuiMain.cpp
---

# 看界面效果

先编好 `MiniBIMWin`（skill `test` 改了界面会顺带编译），再运行（PowerShell 工具）：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .agents/skills/ui-check/scripts/shot.ps1 -Lang zh_CN
pwsh -NoProfile -ExecutionPolicy Bypass -File .agents/skills/ui-check/scripts/shot.ps1 -Lang en
pwsh -NoProfile -ExecutionPolicy Bypass -File .agents/skills/ui-check/scripts/shot.ps1 -Lang zh_CN -Keys "{F10}{DOWN}{RIGHT}" -Name lang-menu   # 文件 > 语言 子菜单
pwsh -NoProfile -ExecutionPolicy Bypass -File .agents/skills/ui-check/scripts/shot.ps1 -Lang zh_CN -Run "view.plan,view.levelUp" -Name plan    # 启动后执行界面命令：Level 2 的平面
pwsh -NoProfile -ExecutionPolicy Bypass -File .agents/skills/ui-check/scripts/shot.ps1 -Open out/tmp/x.minibim -Run view.plan -Name x   # 打开一个项目（如用 CommandTests 的命令做出来的）再执行
```

截图在 `out/tmp/shots/<Name>.png`，用 Read 工具看。不带 `-Run` 时截到的是开始页（启动时没给项目）。选项：`-Run`（启动参数 `--run`：打开示例房屋后依次执行的界面命令 ID，逗号分隔；
不用按键、不抢焦点，能用命令到达的界面优先用它）、`-Keys`（SendKeys 语法：`{F10}` 激活菜单栏并停在第一个菜单，`{RIGHT}` / `{LEFT}` 换菜单，
`{DOWN}` 展开、在菜单里往下，`{RIGHT}` 打开子菜单，`{ENTER}`、`{ESC}`；菜单没有助记符，Alt+字母打不开）、
`-Open`（启动时打开的项目文件；给了它时 `-Run` 在它上面执行，不打开示例房屋）、`-Wait`（窗口出现后等多久再截，默认 3000 毫秒）、`-Name`、`-Release`。

## 要知道的

- 窗口会真的出现在用户桌面上，截完立即关闭（`WM_CLOSE`）。不带 `-Keys` 时用 `PrintWindow` 只截这个窗口，不需要前台、不受遮挡，随时可以用
- `-Keys` 发的是**真实按键**，要把 MiniBIM 置到前台、抢走用户的焦点：用户正在用这台机器时不要用，或先问用户。脚本只在确认 MiniBIM 在前台时才按键
  （不在前台就不按，退出码 2），按过键后也只在它仍在前台时才从屏幕上取它的窗口矩形——`PrintWindow` 拿到的是 D3D 交换链上一次呈现之前的画面，看不到刚展开的菜单；
  这时它已不在前台就退回 `PrintWindow` 并提示。绝不截整个屏幕
- 用户设置 `%LOCALAPPDATA%\MiniBIM\settings.json` 在运行前备份、结束后还原：按键切换语言会写这个文件，不能把用户选好的语言改掉
- `--lang` 只对这一次启动有效，不写设置；没有 `-Lang` 时用用户的设置或系统语言
- 开始页的卡片来自用户的 `%LOCALAPPDATA%\MiniBIM\recent.json`（脚本不备份它）：要看卡片时先确认它在不在，临时写一份、截完删掉或还原；`-Run file.sample,file.start` 截叫到项目上的开始页
- 每种语言都看：中文的标点（全角逗号、括号）、长译文是否被截断、有没有漏翻的英文（漏翻多半是忘了 `Tr`，或没运行 `tools/extract_messages.py`，见 skill `i18n`）
- 窗口找不到时脚本会说进程是否退出；退出码非 0 多半是初始化失败（图形设备、界面描述文件），直接运行 `out/debug/MiniBIMWin/MiniBIMWin.exe` 看控制台日志
