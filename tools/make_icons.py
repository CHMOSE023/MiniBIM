"""生成 MiniBIM 用到的全部图标（说明见 .agents/features/icons.md）。

两类：
  - 命令图标 assets/icons/<名称>.png，64×64 RGBA：风格与绘图工具类 Icon 沿用 MiniCAD 的 tools/make_icons.py
    （灰色线条 (179,179,179) + 青色强调 (0,192,232)，不透明度 224；4 倍尺寸绘制后缩小抗锯齿）。
    命令与图标的对应写在 assets/ui/minibim_ui.json 的 "commands" 里（"icon": "<名称>.png"）
  - 程序图标：透明背景上一座绿色的等轴测楼（与 MiniCAD 的程序图标同一种绿，平涂、不用渐变；命令图标仍是灰 + 青蓝）。
    assets/icons/app.ico（16～256 多种尺寸，编进 exe，见 apps/Win32/src/MiniBIM.rc）与 app.png（64×64，标题栏）
项目里的图标一律由本脚本生成，不手画、不从别处拷图片：要新图标时在这里加一个 @icon 函数，运行本脚本，再在界面描述文件里引用。

用法（在仓库根目录；生成需要 Pillow：pip install pillow）：
  python tools/make_icons.py                     生成全部图标（覆盖本脚本生成的文件）
  python tools/make_icons.py --preview <文件.png> 生成之外，再把全部图标拼成一张预览图（深、浅两种底色，程序图标的各个尺寸）
  python tools/make_icons.py --check             只检查，不需要 Pillow（CI 用）：界面描述文件引用的图标都由本脚本定义，
                                                 定义的图标都已生成，assets/icons 里没有本脚本以外来的图片；有问题时返回 1
"""
import json
import math
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "icons"
UI_DIR = ROOT / "assets" / "ui"
APP_FILES = ["app.ico", "app.png"]

S = 4                          # 绘制倍数（64 网格 × 4，再缩小抗锯齿）
G = (179, 179, 179, 224)       # 线条
A = (0, 192, 232, 224)         # 强调
AF = (0, 192, 232, 80)         # 强调色填充
GF = (179, 179, 179, 60)       # 灰色填充
W = 4                          # 线宽（64 网格）


class Icon:
    """64×64 网格上的画笔（照 MiniCAD 的同名类）。坐标都是 64 网格里的，内部按 S 倍绘制。"""

    def __init__(self):
        from PIL import Image, ImageDraw
        self.im = Image.new("RGBA", (64 * S, 64 * S), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)

    @staticmethod
    def P(pts):
        return [(x * S, y * S) for x, y in pts]

    def line(self, pts, c=G, w=W):
        self.d.line(self.P(pts), fill=c, width=int(w * S), joint="curve")
        for x, y in (pts[0], pts[-1]):     # 圆头
            r = w * S / 2
            self.d.ellipse([x * S - r, y * S - r, x * S + r, y * S + r], fill=c)

    def poly(self, pts, c=G, fill=None, w=W):
        if fill:
            self.d.polygon(self.P(pts), fill=fill)
        self.line(list(pts) + [pts[0]], c, w)

    def rect(self, x0, y0, x1, y1, c=G, fill=None, w=W, r=0):
        if r:
            self.d.rounded_rectangle([x0 * S, y0 * S, x1 * S, y1 * S], radius=r * S, outline=c, fill=fill, width=int(w * S))
        else:
            self.poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], c, fill, w)

    def circle(self, cx, cy, r, c=G, fill=None, w=W):
        self.d.ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], outline=c, fill=fill, width=int(w * S))

    def ellipse(self, cx, cy, rx, ry, c=G, fill=None, w=W):
        self.d.ellipse([(cx - rx) * S, (cy - ry) * S, (cx + rx) * S, (cy + ry) * S], outline=c, fill=fill, width=int(w * S))

    def arc(self, cx, cy, r, a0, a1, c=G, w=W, ry=None):
        ry = ry or r
        self.d.arc([(cx - r) * S, (cy - ry) * S, (cx + r) * S, (cy + ry) * S], a0, a1, fill=c, width=int(w * S))

    def dot(self, x, y, r=3.5, c=A):
        self.circle(x, y, r, c, fill=c, w=0.5)

    def arrow(self, x0, y0, x1, y1, c=A, w=W, head=8, both=False):
        self.line([(x0, y0), (x1, y1)], c, w)
        self.head(x0, y0, x1, y1, c, head)
        if both:
            self.head(x1, y1, x0, y0, c, head)

    def head(self, x0, y0, x1, y1, c=A, head=8):
        a = math.atan2(y1 - y0, x1 - x0)
        p1 = (x1 - head * math.cos(a - 0.45), y1 - head * math.sin(a - 0.45))
        p2 = (x1 - head * math.cos(a + 0.45), y1 - head * math.sin(a + 0.45))
        self.d.polygon(self.P([(x1, y1), p1, p2]), fill=c)

    def dashed(self, pts, c=G, w=3, dash=5, gap=4):
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            L = math.hypot(x1 - x0, y1 - y0)
            t = 0.0
            while t < L:
                t1 = min(L, t + dash)
                self.line([(x0 + (x1 - x0) * t / L, y0 + (y1 - y0) * t / L), (x0 + (x1 - x0) * t1 / L, y0 + (y1 - y0) * t1 / L)], c, w)
                t += dash + gap

    def image(self):
        from PIL import Image
        return self.im.resize((64, 64), Image.LANCZOS)


ICONS = {}


def icon(name):
    def deco(f):
        ICONS[name] = f
        return f
    return deco


# ── 共用的图形 ─────────────────────────────────────────────────
def cube(i, face=None, fill_all=False, edges=G):
    """等轴测立方体（三维视图、构件）：face 为 top / left / right 时那一面用强调色"""
    top = [(32, 8), (54, 19), (32, 30), (10, 19)]
    left = [(10, 19), (32, 30), (32, 56), (10, 45)]
    right = [(32, 30), (54, 19), (54, 45), (32, 56)]
    faces = {"top": top, "left": left, "right": right}
    if fill_all:
        i.d.polygon(i.P(top), fill=(179, 179, 179, 110))
        i.d.polygon(i.P(left), fill=(179, 179, 179, 70))
        i.d.polygon(i.P(right), fill=(179, 179, 179, 40))
    if face:
        i.d.polygon(i.P(faces[face]), fill=AF)
    i.line([(10, 19), (32, 8), (54, 19), (54, 45), (32, 56), (10, 45), (10, 19)], edges)
    i.line([(10, 19), (32, 30), (54, 19)], edges)
    i.line([(32, 30), (32, 56)], edges)
    if face:
        i.line(list(faces[face]) + [faces[face][0]], A)


# ── 命令图标（名称 = 文件名，界面描述文件里写 "icon": "<名称>.png"）──────
@icon("HomeView")          # view.home：主视图（西南等轴测）
def _(i):
    i.poly([(10, 30), (32, 10), (54, 30)], G, w=4)
    i.line([(16, 26), (16, 54), (48, 54), (48, 26)], G, 4)
    i.rect(27, 38, 37, 54, A, AF, w=3.5)


@icon("ZoomFit")           # view.zoomFit：缩放匹配（照 MiniCAD 的 ZoomExtents）
def _(i):
    i.circle(28, 28, 16); i.line([(40, 40), (56, 56)], G, 6)
    for (x, y, dx, dy) in ((6, 6, 1, 1), (58, 6, -1, 1), (6, 58, 1, -1)):
        i.line([(x + dx * 10, y), (x, y), (x, y + dy * 10)], A, 3)


@icon("About")             # help.about：关于
def _(i):
    i.circle(32, 32, 24, G, w=3.5)
    i.dot(32, 20, 3.5); i.line([(32, 29), (32, 45)], A, 5)


@icon("New")               # file.new：新建项目（一页纸 + 加号）
def _(i):
    i.poly([(14, 6), (36, 6), (46, 16), (46, 58), (14, 58)], G, w=3.5)
    i.line([(36, 6), (36, 16), (46, 16)], G, 3)
    i.circle(46, 46, 12, A, fill=(0, 0, 0, 0), w=0.1)
    i.d.ellipse(i.P([(34, 34), (58, 58)]), fill=(0, 192, 232, 224))
    i.line([(46, 39), (46, 53)], (255, 255, 255, 240), 3.5)
    i.line([(39, 46), (53, 46)], (255, 255, 255, 240), 3.5)


@icon("Open")              # file.open：打开项目（文件夹，前面那片翻开）
def _(i):
    i.line([(8, 50), (8, 14), (24, 14), (29, 20), (50, 20), (50, 28)], G, 4)
    i.poly([(8, 50), (17, 30), (58, 30), (50, 50)], A, AF, w=4)


@icon("Save")              # file.save：保存（盒子 + 向下存进去的箭头）
def _(i):
    i.line([(18, 30), (10, 30), (10, 56), (54, 56), (54, 30), (46, 30)], G, 4)
    i.arrow(32, 6, 32, 42, A, 4.5, 10)


@icon("SaveAs")            # file.saveAs：另存为（盒子 + 箭头 + 另一个名字的笔）
def _(i):
    i.line([(14, 30), (8, 30), (8, 56), (44, 56), (44, 42)], G, 4)
    i.arrow(26, 8, 26, 42, A, 4.5, 10)
    i.line([(42, 30), (56, 16)], A, 6)
    i.d.polygon(i.P([(38, 34), (42, 30), (40, 36)]), fill=A)


@icon("SampleHouse")       # file.sample：示例房屋（等轴测的房子，墙上一扇门）
def _(i):
    cube(i, fill_all=True)
    i.poly([(17, 34), (25, 38), (25, 53), (17, 49)], A, AF, w=3)


@icon("Exit")              # file.exit：退出（门 + 向外的箭头）
def _(i):
    i.line([(30, 14), (12, 14), (12, 50), (30, 50)], G, 4)
    i.arrow(24, 32, 56, 32, A, 4, 9)


@icon("Language")          # 语言（地球）：语言菜单是动态子菜单，现在还没有地方显示，留给以后的设置面板 / 工具栏
def _(i):
    i.circle(32, 32, 22, G, w=3.5)
    i.ellipse(32, 32, 9, 22, A, w=3)
    i.line([(11, 26), (53, 26)], A, 3); i.line([(11, 38), (53, 38)], A, 3)


@icon("View3D")            # view.3d：三维视图（等轴测立方体，顶面强调）
def _(i):
    cube(i, face="top")


@icon("FloorPlan")         # view.plan：楼层平面（俯视的一圈墙，下边开着门，门扇与开启线）
def _(i):
    i.line([(26, 54), (8, 54), (8, 10), (56, 10), (56, 54), (44, 54)], G, 6)
    i.line([(26, 54), (26, 36)], A, 3.5)
    i.arc(26, 54, 18, 270, 360, A, w=3)


@icon("LevelUp")           # view.levelUp：上一层的平面（两条标高线，向上的箭头）
def _(i):
    i.line([(6, 50), (40, 50)], G, 4); i.line([(6, 22), (40, 22)], G, 4)
    i.arrow(50, 54, 50, 14, A, w=4.5, head=10)


@icon("LevelDown")         # view.levelDown：下一层的平面（两条标高线，向下的箭头）
def _(i):
    i.line([(6, 50), (40, 50)], G, 4); i.line([(6, 22), (40, 22)], G, 4)
    i.arrow(50, 10, 50, 50, A, w=4.5, head=10)


# ── 程序图标 ───────────────────────────────────────────────────
APP_GREEN = (88, 226, 19)      # 程序图标的绿色：与 MiniCAD 的程序图标同色（纯色，不用渐变）


def app_icon(size):
    """MiniBIM 的程序图标：透明背景上一座绿色的等轴测楼（平涂、不用渐变）：左侧面 APP_GREEN，顶面浅一些、右侧面深一些，
    侧面的白色横线是三层楼板。不画成平面的小房子：那样太像"首页"按钮。按 1024 绘制，再缩小到 size"""
    from PIL import Image, ImageDraw
    N = 1024
    im = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    cx, w, t0, h = 512, 380, 40, 560      # 中心、半宽、顶点 y、楼高：楼填满画布（宽 760，高 w + h = 940）
    top_face = [(cx, t0), (cx + w, t0 + w / 2), (cx, t0 + w), (cx - w, t0 + w / 2)]
    left = [(cx - w, t0 + w / 2), (cx, t0 + w), (cx, t0 + w + h), (cx - w, t0 + w / 2 + h)]
    right = [(cx, t0 + w), (cx + w, t0 + w / 2), (cx + w, t0 + w / 2 + h), (cx, t0 + w + h)]
    d.polygon(left, fill=APP_GREEN + (255,))
    d.polygon(right, fill=(58, 176, 12, 255))
    d.polygon(top_face, fill=(168, 244, 120, 255))
    # 楼层线：三层
    for f in (1, 2):
        dy = h * f / 3
        d.line([(cx - w, t0 + w / 2 + dy), (cx, t0 + w + dy), (cx + w, t0 + w / 2 + dy)], fill=(255, 255, 255, 255), width=28, joint="curve")
    # 转角线：让小尺寸下也分得清两个侧面
    d.line([(cx, t0 + w), (cx, t0 + w + h)], fill=(40, 140, 8, 255), width=14)
    return im.resize((size, size), Image.LANCZOS)


ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def save_app_icons():
    images = [app_icon(s) for s in ICO_SIZES]
    images[-1].save(OUT / "app.ico", format="ICO", sizes=[(s, s) for s in ICO_SIZES], append_images=images[:-1])
    app_icon(64).save(OUT / "app.png")


# ── 生成、预览、检查 ───────────────────────────────────────────
def generate():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, f in ICONS.items():
        i = Icon()
        f(i)
        i.image().save(OUT / f"{name}.png")
    save_app_icons()
    print(f"{len(ICONS)} 个命令图标 + 程序图标（{', '.join(APP_FILES)}）→ {OUT}")


def preview(path):
    """全部命令图标排成两行（深、浅底色），下面是程序图标的各个尺寸"""
    from PIL import Image, ImageDraw
    names = list(ICONS)
    cell = 80
    width = max(len(names) * cell, sum(ICO_SIZES) + 12 * len(ICO_SIZES)) + 16
    sheet = Image.new("RGBA", (width, cell * 2 + 280), (43, 43, 48, 255))
    ImageDraw.Draw(sheet).rectangle([0, cell, width, cell * 2], fill=(238, 238, 240, 255))
    for row in range(2):
        for k, name in enumerate(names):
            im = Image.open(OUT / f"{name}.png").convert("RGBA")
            sheet.alpha_composite(im, (8 + k * cell + 8, row * cell + 8))
    x = 8
    for s in ICO_SIZES:
        sheet.alpha_composite(app_icon(s), (x, cell * 2 + 12))
        x += s + 12
    sheet.save(path)
    print(f"预览：{path}（第一行深色底、第二行浅色底，顺序：{', '.join(names)}）")


def strip_comments(text):
    """去掉界面描述文件里的 // 与 /* */ 注释（字符串里的不动），以及末尾多余的逗号"""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return re.sub(r",(\s*[}\]])", r"\1", "".join(out))


def referenced_icons():
    """界面描述文件里引用的图标文件名 → 出处"""
    refs = {}

    def walk(v, where):
        if isinstance(v, dict):
            for key, x in v.items():
                if key == "icon" and isinstance(x, str):
                    refs.setdefault(x, where)
                else:
                    walk(x, where)
        elif isinstance(v, list):
            for x in v:
                walk(x, where)

    for p in sorted(UI_DIR.glob("*.json")):
        walk(json.loads(strip_comments(p.read_text(encoding="utf-8-sig"))), p.relative_to(ROOT).as_posix())
    return refs


def check():
    problems = []
    generated = {f"{name}.png" for name in ICONS} | set(APP_FILES)
    for file, where in referenced_icons().items():
        if file not in generated:
            problems.append(f"{where} 引用的图标 {file} 不是本脚本定义的（在 tools/make_icons.py 里加一个 @icon）")
    for file in sorted(generated):
        if not (OUT / file).exists():
            problems.append(f"assets/icons/{file} 还没生成（运行 python tools/make_icons.py）")
    for p in sorted(OUT.glob("*")):
        if p.is_file() and p.name not in generated:
            problems.append(f"assets/icons/{p.name} 不是本脚本生成的（图标都由 tools/make_icons.py 生成；不用了就删掉）")
    for msg in problems:
        print(msg)
    print(f"图标检查：{len(ICONS)} 个命令图标 + 程序图标，{len(problems)} 处问题")
    return 1 if problems else 0


def main():
    if not sys.stdout.isatty():
        sys.stdout.reconfigure(encoding="utf-8")
    args = sys.argv[1:]
    if "--check" in args:
        return check()
    try:
        import PIL  # noqa: F401
    except ImportError:
        print("需要 Pillow：pip install pillow")
        return 1
    generate()
    if "--preview" in args:
        k = args.index("--preview")
        if k + 1 >= len(args):
            print("--preview 后面要写预览图的路径")
            return 1
        preview(pathlib.Path(args[k + 1]))
    return check()


if __name__ == "__main__":
    sys.exit(main())
