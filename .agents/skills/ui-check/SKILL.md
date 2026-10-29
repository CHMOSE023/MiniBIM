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
```

截图在 `out/tmp/shots/<Name>.png`，用 Read 工具看。选项：`-Keys`（SendKeys 语法：`{F10}` 激活菜单栏并停在第一个菜单，`{RIGHT}` / `{LEFT}` 换菜单，
`{DOWN}` 展开、在菜单里往下，`{RIGHT}` 打开子菜单，`{ENTER}`、`{ESC}`；菜单没有助记符，Alt+字母打不开）、
`-Wait`（窗口出现后等多久再截，默认 3000 毫秒）、`-Name`、`-Release`。

## 要知道的

- 窗口会真的出现在用户桌面上，截完立即关闭（`WM_CLOSE`）。不带 `-Keys` 时用 `PrintWindow` 只截这个窗口，不需要前台、不受遮挡，随时可以用
- `-Keys` 发的是**真实按键**，要把 MiniBIM 置到前台、抢走用户的焦点：用户正在用这台机器时不要用，或先问用户。脚本只在确认 MiniBIM 在前台时才按键
  （不在前台就不按，退出码 2），按过键后也只在它仍在前台时才从屏幕上取它的窗口矩形——`PrintWindow` 拿到的是 D3D 交换链上一次呈现之前的画面，看不到刚展开的菜单；
  这时它已不在前台就退回 `PrintWindow` 并提示。绝不截整个屏幕
- 用户设置 `%LOCALAPPDATA%\MiniBIM\settings.json` 在运行前备份、结束后还原：按键切换语言会写这个文件，不能把用户选好的语言改掉
- `--lang` 只对这一次启动有效，不写设置；没有 `-Lang` 时用用户的设置或系统语言
- 每种语言都看：中文的标点（全角逗号、括号）、长译文是否被截断、有没有漏翻的英文（漏翻多半是忘了 `Tr`，或没运行 `tools/extract_messages.py`，见 skill `i18n`）
- 窗口找不到时脚本会说进程是否退出；退出码非 0 多半是初始化失败（图形设备、界面描述文件），直接运行 `out/debug/MiniBIMWin/MiniBIMWin.exe` 看控制台日志
