"""收集界面文字的英文原文，生成 assets/i18n/minibim.pot，并把新原文合并进各语言的 .po（说明见 docs/i18n.md）。

从哪里收集：
  - src/、apps/ 下的 C++ 源码：Tr("…")、TrC("上下文", "…")、TrN("单数", "复数", n)、N_("…")，参数必须是字符串字面量
    （注释里的不算）。MiniGUI 自带的文字（src/UI）同样用 Tr("…")
  - assets/ui/*.json 界面描述文件：键为 title / label / tooltip / text / search 的字符串值

用法（在仓库根目录运行）：
  python tools/extract_messages.py           生成 .pot，更新各 .po（保留已有译文；不再使用的条目改成 #~ 注释），打印各语言的完成度
  python tools/extract_messages.py --check   只检查：.pot 或某个 .po 需要更新时返回 1（CI 用）
  python tools/extract_messages.py --untranslated   同第一种，另外列出每种语言还没翻译（或标了 fuzzy）的条目
新增一种语言：复制 minibim.pot 为 <语言代码>.po（如 ja.po），填好文件头的 Language、X-Language-Name、Plural-Forms，再翻译。
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
I18N = ROOT / "assets" / "i18n"
POT = I18N / "minibim.pot"
SOURCE_DIRS = [ROOT / "src", ROOT / "apps"]
UI_DIR = ROOT / "assets" / "ui"
JSON_KEYS = {"title", "label", "tooltip", "text", "search"}

STR = r'"((?:[^"\\\n]|\\.)*)"'
CALLS = [
    ("Tr", re.compile(r"\bTr\(\s*" + STR + r"\s*\)")),
    ("N_", re.compile(r"\bN_\(\s*" + STR + r"\s*\)")),
    ("TrC", re.compile(r"\bTrC\(\s*" + STR + r"\s*,\s*" + STR + r"\s*\)")),
    ("TrN", re.compile(r"\bTrN\(\s*" + STR + r"\s*,\s*" + STR + r"\s*,")),
]


def strip_cpp_comments(text):
    """去掉 // 与 /* */ 注释（换成空格，保留换行），字符串、字符、原始字符串里的不动。"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == "R" and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            k = text.find("(", i + 2)
            delim = text[i + 2:k]
            j = text.find(")" + delim + '"', k)
            j = n if j < 0 else j + len(delim) + 2
            out.append(text[i:j])
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def unescape(s):
    return re.sub(r'\\(.)', lambda m: {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'"}.get(m.group(1), m.group(1)), s)


def escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t").replace("\r", "\\r")


class Entry:
    def __init__(self, ctx, msgid, plural=None):
        self.ctx, self.msgid, self.plural = ctx, msgid, plural
        self.refs = []          # 出现在哪些文件（相对路径，不带行号：免得每次改代码都改动词典）
        self.strs = {}          # 序号 → 译文
        self.flags = []
        self.comments = []      # 译者注释（# 开头）

    @property
    def key(self):
        return (self.ctx or "", self.msgid)


def collect():
    entries = {}

    def add(ctx, msgid, plural, ref):
        if not msgid:
            return
        e = entries.get((ctx or "", msgid))
        if e is None:
            e = entries[(ctx or "", msgid)] = Entry(ctx, msgid, plural)
        if plural and not e.plural:
            e.plural = plural
        if ref not in e.refs:
            e.refs.append(ref)

    for base in SOURCE_DIRS:
        for p in sorted(base.rglob("*")):
            if p.suffix not in (".cpp", ".h", ".hpp"):
                continue
            ref = p.relative_to(ROOT).as_posix()
            code = strip_cpp_comments(p.read_text(encoding="utf-8"))
            found = []
            for kind, pat in CALLS:
                for m in pat.finditer(code):
                    if kind in ("Tr", "N_"):
                        found.append((m.start(), None, unescape(m.group(1)), None))
                    elif kind == "TrC":
                        found.append((m.start(), unescape(m.group(1)), unescape(m.group(2)), None))
                    else:
                        found.append((m.start(), None, unescape(m.group(1)), unescape(m.group(2))))
            for _, ctx, msgid, plural in sorted(found, key=lambda f: f[0]):
                add(ctx, msgid, plural, ref)

    for p in sorted(UI_DIR.glob("*.json")):
        ref = p.relative_to(ROOT).as_posix()
        text = strip_cpp_comments(p.read_text(encoding="utf-8-sig"))
        text = re.sub(r",(\s*[}\]])", r"\1", text)      # MiniGUI 的 JSON 允许末尾多一个逗号

        def walk(v):
            if isinstance(v, dict):
                for k, x in v.items():
                    if k in JSON_KEYS and isinstance(x, str):
                        add(None, x, None, ref)
                    else:
                        walk(x)
            elif isinstance(v, list):
                for x in v:
                    walk(x)
        walk(json.loads(text))
    return list(entries.values())


def parse_po(path):
    """读 .po：返回 (文件头文本, 条目列表)。只认本工具写出的、以及常见编辑器保存的格式。"""
    header, entries = "", []
    cur, last, obsolete_lines = None, None, []
    pending_flags, pending_comments = [], []

    def flush():
        nonlocal cur, last, header
        if cur is not None:
            if cur.msgid == "" and not cur.ctx:
                header = cur.strs.get(0, "")
            else:
                entries.append(cur)
        cur, last = None, None

    for raw in path.read_text(encoding="utf-8-sig").splitlines():
        line = raw.strip()
        if not line:
            flush()
            continue
        if line.startswith("#~"):
            continue
        if line.startswith("#,"):
            pending_flags += [f.strip() for f in line[2:].split(",") if f.strip()]
            continue
        if line.startswith("#:") or line.startswith("#|") or line.startswith("#."):
            continue
        if line.startswith("#"):
            pending_comments.append(line)
            continue
        m = re.match(r'(msgctxt|msgid_plural|msgid|msgstr(?:\[(\d+)\])?)\s+"(.*)"$', line)
        if m:
            kw = m.group(1)
            val = unescape(m.group(3))
            # 条目之间没有空行时：已经读到 msgid 的条目遇到下一个 msgctxt / msgid 就结束
            if kw in ("msgctxt", "msgid") and cur is not None and cur.msgid is not None:
                flush()
            if cur is None:
                cur = Entry(None, None)
                cur.flags, cur.comments = pending_flags, pending_comments
                pending_flags, pending_comments = [], []
            if kw == "msgctxt":
                cur.ctx = val
                last = ("ctx", None)
            elif kw == "msgid":
                cur.msgid = val
                last = ("id", None)
            elif kw == "msgid_plural":
                cur.plural = val
                last = ("plural", None)
            else:
                idx = int(m.group(2)) if m.group(2) else 0
                cur.strs[idx] = val
                last = ("str", idx)
            continue
        if line.startswith('"') and line.endswith('"') and cur is not None and last:
            val = unescape(line[1:-1])
            if last[0] == "ctx":
                cur.ctx += val
            elif last[0] == "id":
                cur.msgid += val
            elif last[0] == "plural":
                cur.plural += val
            else:
                cur.strs[last[1]] += val
    flush()
    return header, entries


def nplurals(header):
    m = re.search(r"nplurals\s*=\s*(\d+)", header)
    return int(m.group(1)) if m else 2


def write_entry(out, e, forms, obsolete=False):
    p = "#~ " if obsolete else ""
    for c in e.comments:
        out.append(c)
    if e.refs and not obsolete:
        out.append("#: " + " ".join(e.refs))
    if e.flags:
        out.append("#, " + ", ".join(e.flags))
    if e.ctx:
        out.append(f'{p}msgctxt "{escape(e.ctx)}"')
    out.append(f'{p}msgid "{escape(e.msgid)}"')
    if e.plural is not None:
        out.append(f'{p}msgid_plural "{escape(e.plural)}"')
        for i in range(forms):
            out.append(f'{p}msgstr[{i}] "{escape(e.strs.get(i, ""))}"')
    else:
        out.append(f'{p}msgstr "{escape(e.strs.get(0, ""))}"')
    out.append("")


def render_header(header):
    lines = ['msgid ""', 'msgstr ""']
    for h in header.split("\n"):
        if h:
            lines.append(f'"{escape(h)}\\n"')
    return lines + [""]


POT_HEADER = ("Project-Id-Version: MiniBIM\n"
              "Language: \n"
              "X-Language-Name: \n"
              "MIME-Version: 1.0\n"
              "Content-Type: text/plain; charset=UTF-8\n"
              "Content-Transfer-Encoding: 8bit\n"
              "Plural-Forms: nplurals=2; plural=(n != 1);\n")


def build_pot(entries):
    out = ["# MiniBIM 界面文字的原文模板，由 tools/extract_messages.py 生成，不要手改。",
           "# 新增语言：复制本文件为 <语言代码>.po，填好文件头再翻译。"]
    out += render_header(POT_HEADER)
    for e in entries:
        write_entry(out, e, 2)
    return "\n".join(out)


def build_po(path, template):
    header, old = parse_po(path)
    forms = nplurals(header)
    old_by_key = {e.key: e for e in old}
    head_comments = []
    for raw in path.read_text(encoding="utf-8-sig").splitlines():
        if raw.startswith("#") and not raw.startswith("#,") and not raw.startswith("#:"):
            head_comments.append(raw)
        else:
            break
    out = head_comments + render_header(header)
    total = 0
    missing = []            # 还没有（完整）译文或标了 fuzzy 的条目
    for t in template:
        e = Entry(t.ctx, t.msgid, t.plural)
        e.refs = t.refs
        if t.key in old_by_key:
            o = old_by_key[t.key]
            e.strs, e.flags, e.comments = o.strs, o.flags, o.comments
        total += 1
        if not (e.strs and all(e.strs.get(i) for i in range(forms if e.plural is not None else 1)) and "fuzzy" not in e.flags):
            missing.append(e)
        write_entry(out, e, forms)
    used = {t.key for t in template}
    for o in old:
        if o.key not in used and any(o.strs.values()):
            write_entry(out, o, forms, obsolete=True)
    return "\n".join(out), total - len(missing), total, missing


def main():
    # 输出重定向时 Python 按系统代码页编码（CI 的 Windows 机器是 cp1252，写不出中文会抛异常），统一用 UTF-8
    if not sys.stdout.isatty():
        sys.stdout.reconfigure(encoding="utf-8")
    check = "--check" in sys.argv[1:]
    untranslated = "--untranslated" in sys.argv[1:]
    template = collect()
    stale = []

    pot = build_pot(template)
    if not POT.exists() or POT.read_text(encoding="utf-8") != pot:
        stale.append(POT)
        if not check:
            POT.write_text(pot, encoding="utf-8", newline="\n")

    for po in sorted(I18N.glob("*.po")):
        text, done, total, missing = build_po(po, template)
        if po.read_text(encoding="utf-8-sig") != text:
            stale.append(po)
            if not check:
                po.write_text(text, encoding="utf-8", newline="\n")
        print(f"{po.name}: {done}/{total} 条已翻译")
        if untranslated:
            for e in missing:
                ctx = f"[{e.ctx}] " if e.ctx else ""
                plural = f"  |  复数：{escape(e.plural)}" if e.plural is not None else ""
                fuzzy = "（fuzzy）" if "fuzzy" in e.flags else ""
                print(f"  {ctx}{escape(e.msgid)}{plural}{fuzzy}    ← {', '.join(e.refs)}")

    print(f"原文 {len(template)} 条")
    if stale:
        names = ", ".join(p.name for p in stale)
        if check:
            print(f"需要更新：{names}（运行 python tools/extract_messages.py）")
            return 1
        print(f"已更新：{names}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
