---
name: i18n
description: 改了界面文字（Tr / TrN / TrC / N_、界面描述文件的标题）、补译文或新增一种语言时用：收集原文、更新 assets/i18n 下的词典、列出没翻译的条目、核对显示。也用于 skill test 报"词典需要更新"之后。不用于改多语言模块本身的代码（那是普通的代码改动，读 docs/i18n.md）。
covers: tools/extract_messages.py, assets/i18n/*, .agents/skills/i18n/*
---

# 界面文字与词典

做法的完整说明在 `docs/i18n.md`，这里是操作步骤。

## 改了界面文字之后

1. 确认代码里的写法收集得到：参数是**一个字符串字面量**（`Tr("Zoom to Fit")`，不能拼接、不能传变量）；之后才翻译的用 `N_("…")` 标记；
   带数值的用 `Format(Tr("{0} walls"), …)` / `TrN`，不要把一句话拆成几段 `Tr`
2. 收集原文、更新词典（在仓库根目录）：

   ```powershell
   python tools/extract_messages.py --untranslated
   ```

   它重写 `assets/i18n/minibim.pot`，把新原文合并进每个 `.po`（保留已有译文；不再使用的条目改成 `#~` 注释），并列出每种语言没翻译的条目及其出处
3. 在 `assets/i18n/zh_CN.po` 里补上译文（用 Edit 工具，不要经 Bash 的 heredoc 写：`\n` 会变成真换行，见 `.agents/docs/troubleshooting.md`）：
   - 菜单文字没有助记符：原文不写 `&`，译文也不加 `(F)`（`File` → `文件`）；从 MiniCAD 或 BIMCraft 搬来的文字带 `&` 的要去掉
   - 占位 `{0}`、`{1}` 原样保留，可以调整顺序
   - 中文标点用全角（，：（）），英文原文里的分隔符（如 `{0}, {1}, {2}`）也照中文习惯翻译
   - 复数条目：中文只有 `msgstr[0]`（`nplurals=1`）
   - 不确定的译文可以标 `#, fuzzy`：程序不用它（显示英文），`--untranslated` 会列出来
4. 再运行一次 `python tools/extract_messages.py --check`（退出码 0），然后用 skill `test`（`I18nTests` 检查每个词典都能无警告读出）
5. 用 skill `ui-check` 分别以 `-Lang zh_CN`、`-Lang en` 截图看：有没有漏翻、截断

## 新增一种语言

1. 复制 `assets/i18n/minibim.pot` 为 `assets/i18n/<语言代码>.po`（`ja.po`、`pt_BR.po`；文件名就是语言代码）
2. 填文件头：`Language`（与文件名一致）、`X-Language-Name`（用这种语言自己写的名称，菜单里显示）、`Plural-Forms`（gettext 的写法，如日语 `nplurals=1; plural=0;`、法语 `nplurals=2; plural=(n > 1);`）
3. 翻译，然后同上第 4、5 步。不用改代码：程序启动时扫描 `i18n/` 目录，"文件 > 语言"里自动出现
4. 界面字体要覆盖这种文字：Windows 版是微软雅黑 UI + Segoe UI Symbol + Malgun Gothic（`src/UI/Platform/Win32/Win32Fonts.cpp`）；从右到左的文字和需要字形组合的文字暂不支持

## 合并 BIMCraft 的译文（界面移植之后）

BIMCraft 的 `translations/ui/zh.po`、`es.po`、`pt.po` 也以英文原文为键。原文完全相同的条目可以直接沿用译文：
先运行收集工具让 `.po` 里有这些原文，再按 msgid（和 msgctxt）对照着填。BIMCraft 的译文按 MIT 使用（见 `THIRD_PARTY_NOTICES.md`）。
第 3f 步合并过一次：`es.po`、`pt.po` 由此而来（BIMCraft 的 `{}` 按出现顺序改成 `{0}`、`{1}`；原文相同而意思不同的不沿用，如剪贴板的 `Cut`），
其余条目新译；西班牙语用 tú（同 BIMCraft），葡萄牙语用巴西的写法。`zh_CN.po` 保留 MiniBIM 自己的用词，不照搬 BIMCraft 的中文
