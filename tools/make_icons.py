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


@icon("ExportIfc")         # file.exportIfc：导出 IFC（等轴测的方块 + 向外的箭头）
def _(i):
    i.poly([(6, 22), (20, 15), (34, 22), (20, 29)], G, w=3.5)
    i.line([(6, 22), (6, 44), (20, 51), (34, 44), (34, 22)], G, 3.5)
    i.line([(20, 29), (20, 51)], G, 3.5)
    i.arrow(30, 38, 58, 38, A, 4.5, 10)


@icon("StartPage")         # file.start：开始页（一个窗口，里面排着几张卡片，一张强调）
def _(i):
    i.rect(6, 10, 58, 54, G, w=3.5, r=4)
    i.line([(6, 20), (58, 20)], G, 3)
    i.rect(13, 27, 29, 47, A, AF, w=3)
    i.rect(35, 27, 51, 35, G, w=3); i.rect(35, 39, 51, 47, G, w=3)


@icon("Back")              # 开始页的"回到项目"：向左的箭头，回到一页图纸上
def _(i):
    i.poly([(26, 8), (48, 8), (56, 16), (56, 56), (26, 56)], G, w=3.5)
    i.arrow(44, 32, 8, 32, A, 4.5, 10)


@icon("Settings")          # file.settings：设置（一个八齿的齿轮，中间的轴孔强调）
def _(i):
    import math
    pts = []
    for k in range(8):
        for da, r in ((-0.22, 18), (-0.14, 26), (0.14, 26), (0.22, 18)):
            a = k * math.pi / 4 + da
            pts.append((32 + r * math.cos(a), 32 + r * math.sin(a)))
    i.poly(pts, G, GF, w=3.5)
    i.circle(32, 32, 8, A, AF, w=3.5)


@icon("Shortcuts")         # file.shortcuts：键盘快捷键（一块键盘，打的两个键强调）
def _(i):
    i.rect(4, 14, 60, 50, G, w=3.5, r=4)
    for k, x in enumerate((10, 22, 34, 46)):
        i.rect(x, 21, x + 8, 29, A if k in (1, 2) else G, AF if k in (1, 2) else None, w=3)
    i.line([(18, 41), (46, 41)], G, 4)


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


@icon("Undo")              # edit.undo：撤销（向左掉头的箭头：下面一横绕过右边回到上面，往左指）
def _(i):
    i.arc(36, 34, 16, 270, 450, G, w=4.5)
    i.line([(36, 50), (22, 50)], G, 4.5)
    i.line([(36, 18), (16, 18)], A, 4.5)
    i.head(20, 18, 8, 18, A, head=12)


@icon("Redo")              # edit.redo：重做（撤销的镜像，往右指）
def _(i):
    i.arc(28, 34, 16, 90, 270, G, w=4.5)
    i.line([(28, 50), (42, 50)], G, 4.5)
    i.line([(28, 18), (48, 18)], A, 4.5)
    i.head(44, 18, 56, 18, A, head=12)


@icon("Delete")            # edit.delete：删除（照 MiniCAD：垃圾桶 + 叉）
def _(i):
    i.rect(14, 18, 50, 56, G, w=3, r=3); i.line([(8, 14), (56, 14)], G, 3); i.line([(26, 14), (26, 8), (38, 8), (38, 14)], G, 3)
    i.line([(24, 28), (40, 46)], A); i.line([(40, 28), (24, 46)], A)


@icon("FlipWall")          # modify.flip：翻转墙（俯视的一段墙，外侧那面强调，两头的弯箭头把它翻到另一边）
def _(i):
    i.rect(8, 26, 56, 38, G, GF, w=3)
    i.line([(8, 26), (56, 26)], A, 4)
    i.arrow(32, 20, 32, 6, A, 3.5, 8)
    i.arrow(32, 44, 32, 58, G, 3.5, 8)


@icon("Select")            # modify.select：修改（选择）：鼠标箭头
def _(i):
    i.poly([(18, 8), (18, 50), (28, 40), (36, 56), (43, 53), (35, 37), (48, 37)], G, AF, w=3.5)


@icon("Wall")              # arch.wall：墙（平面上的一段墙：两条线夹着填充，起点终点）
def _(i):
    i.rect(6, 24, 58, 40, G, GF, w=3.5)
    i.dot(6, 32, 4); i.dot(58, 32, 4)


@icon("Door")              # arch.door：门（墙上的缺口，门扇与开启弧）
def _(i):
    i.rect(4, 44, 16, 54, G, GF, w=3); i.rect(48, 44, 60, 54, G, GF, w=3)
    i.line([(16, 44), (16, 12)], A, 4)
    i.arc(16, 44, 32, 270, 360, A, w=3)


@icon("Window")            # arch.window：窗（墙上的一段，中间三条玻璃线）
def _(i):
    i.rect(4, 24, 18, 40, G, GF, w=3); i.rect(46, 24, 60, 40, G, GF, w=3)
    for y in (24, 32, 40):
        i.line([(18, y), (46, y)], A, 3)


@icon("Column")            # arch.column：柱（俯视的一根方柱：方框里一个叉，立在两条轴线的交点上）
def _(i):
    for a, b in ((4, 17), (47, 60)):
        i.dashed([(a, 32), (b, 32)], G, 3); i.dashed([(32, a), (32, b)], G, 3)
    i.rect(20, 20, 44, 44, A, AF, w=4)
    i.line([(20, 20), (44, 44)], A, 3); i.line([(44, 20), (20, 44)], A, 3)


@icon("Component")         # arch.component：构件（俯视的一张扶手椅）
def _(i):
    i.rect(14, 14, 50, 50, G, w=3.5, r=4)
    i.rect(14, 14, 50, 24, G, GF, w=3)
    i.rect(14, 24, 22, 50, A, AF, w=3); i.rect(42, 24, 50, 50, A, AF, w=3)


@icon("Roof")              # arch.roof：迹线屋顶（双坡屋顶的侧面，屋檐伸出墙外）
def _(i):
    i.poly([(4, 36), (32, 12), (60, 36)], A, w=4.5)
    i.line([(12, 32), (12, 56), (52, 56), (52, 32)], G, 4)


@icon("Floor")             # arch.floor：楼板（等轴测的一块板）
def _(i):
    i.poly([(4, 34), (32, 20), (60, 34), (32, 48)], A, AF, w=3.5)
    i.line([(4, 34), (4, 40), (32, 54), (60, 40), (60, 34)], G, 3.5)
    i.line([(32, 48), (32, 54)], G, 3.5)


@icon("Ceiling")           # arch.ceiling：天花板（上面一块吊顶，下面的标高线）
def _(i):
    i.rect(6, 12, 58, 22, A, AF, w=3.5)
    for x in (19, 32, 45):
        i.line([(x, 12), (x, 22)], A, 2.5)
    i.line([(14, 22), (14, 30)], G, 3); i.line([(50, 22), (50, 30)], G, 3)
    i.line([(6, 52), (58, 52)], G, 4)


@icon("Room")              # arch.room：房间（一圈墙，中间房间的叉与点）
def _(i):
    i.rect(8, 10, 56, 54, G, w=5)
    i.line([(18, 20), (46, 44)], A, 2.5); i.line([(46, 20), (18, 44)], A, 2.5)
    i.dot(32, 32, 5)


@icon("Level")             # arch.level：标高（一条水平线，左端标高三角，上面一条虚线）
def _(i):
    i.line([(18, 40), (60, 40)], A, 4)
    i.poly([(6, 30), (18, 30), (12, 40)], A, AF, w=3)
    i.dashed([(18, 16), (60, 16)], G, 3)


@icon("Grid")              # arch.grid：轴网（竖着的点划线，上端的轴号圆圈）
def _(i):
    i.circle(32, 14, 10, A, w=3.5)
    i.dashed([(32, 24), (32, 60)], G, 3.5)


@icon("Move")              # modify.move：移动（十字四向箭头）
def _(i):
    i.arrow(32, 32, 32, 6, A, 3.5, 9); i.arrow(32, 32, 32, 58, A, 3.5, 9)
    i.arrow(32, 32, 6, 32, A, 3.5, 9); i.arrow(32, 32, 58, 32, A, 3.5, 9)
    i.rect(24, 24, 40, 40, G, GF, w=3)


@icon("Copy")              # modify.copy：复制（两个错开的方块，加号）
def _(i):
    i.rect(6, 6, 36, 36, G, w=3.5)
    i.rect(22, 22, 52, 52, A, AF, w=3.5)
    i.line([(37, 30), (37, 44)], A, 3.5); i.line([(30, 37), (44, 37)], A, 3.5)


@icon("Rotate")            # modify.rotate：旋转（转过一个角的方块，弧形箭头）
def _(i):
    i.poly([(14, 40), (34, 50), (24, 58), (4, 48)], G, GF, w=3)
    i.arc(32, 36, 22, 200, 330, A, w=4)
    i.head(46, 22, 52, 25, A, head=11)


@icon("MirrorPick")        # modify.mirrorPick：镜像 - 拾取轴（点划线轴两边各一个三角）
def _(i):
    i.dashed([(32, 4), (32, 60)], A, 3.5)
    i.poly([(26, 14), (26, 50), (6, 50)], G, GF, w=3)
    i.poly([(38, 14), (38, 50), (58, 50)], A, AF, w=3)


@icon("MirrorDraw")        # modify.mirrorDraw：镜像 - 绘制轴（斜的轴、两端的点）
def _(i):
    i.line([(14, 58), (50, 6)], A, 3.5)
    i.dot(14, 58, 4.5); i.dot(50, 6, 4.5)
    i.poly([(8, 20), (26, 10), (20, 34)], G, GF, w=3)


@icon("Align")             # modify.align：对齐（一条参照线，一块方块靠上去）
def _(i):
    i.line([(10, 4), (10, 60)], A, 4.5)
    i.rect(26, 18, 52, 46, G, GF, w=3.5)
    i.arrow(24, 32, 14, 32, A, 3.5, 8)


@icon("Trim")              # modify.trim：修剪 / 延伸到角（两面墙伸到一个拐角）
def _(i):
    i.line([(8, 16), (40, 16)], G, 6)
    i.line([(48, 56), (48, 24)], G, 6)
    i.dashed([(40, 16), (48, 16), (48, 24)], A, 3.5)
    i.dot(48, 16, 4.5)


@icon("Split")             # modify.split：拆分（一面墙，中间一道切口）
def _(i):
    i.rect(4, 26, 28, 38, G, GF, w=3); i.rect(36, 26, 60, 38, G, GF, w=3)
    i.line([(32, 12), (32, 52)], A, 4)


@icon("Offset")            # modify.offset：偏移（一面墙，旁边平行的一面，带距离箭头）
def _(i):
    i.rect(6, 12, 58, 22, G, GF, w=3)
    i.rect(6, 42, 58, 52, A, AF, w=3)
    i.arrow(32, 24, 32, 40, A, 3, 7, both=True)


@icon("EditBoundary")      # modify.editBoundary：编辑边界（俯视的一块楼板轮廓，一角有缺口，一支笔在描边）
def _(i):
    i.poly([(6, 10), (44, 10), (44, 30), (58, 30), (58, 56), (6, 56)], A, AF, w=3.5)
    i.line([(30, 44), (56, 18)], G, 6)
    i.d.polygon(i.P([(25, 49), (30, 44), (28, 46)]), fill=G)


@icon("FinishSketch")      # sketch.finish：完成编辑模式（对勾）
def _(i):
    i.line([(10, 34), (26, 50), (54, 14)], A, 7)


@icon("CancelSketch")      # sketch.cancel：取消编辑模式（叉）
def _(i):
    i.line([(14, 14), (50, 50)], G, 7); i.line([(50, 14), (14, 50)], G, 7)


@icon("SketchLine")        # sketch.line：草图直线（一条斜线，两端的点）
def _(i):
    i.line([(10, 52), (54, 12)], A, 4)
    i.dot(10, 52, 5); i.dot(54, 12, 5)


@icon("SketchRectangle")   # sketch.rectangle：草图矩形（矩形，两个对角的点）
def _(i):
    i.rect(10, 16, 54, 48, A, w=4)
    i.dot(10, 16, 5); i.dot(54, 48, 5)


@icon("PickWalls")         # sketch.pickWalls：拾取墙（一段墙，沿它的一条线，鼠标箭头点在墙上）
def _(i):
    i.rect(4, 10, 56, 24, G, GF, w=3)
    i.line([(4, 30), (56, 30)], A, 4)
    i.poly([(30, 20), (30, 54), (38, 46), (44, 58), (50, 55), (44, 43), (54, 43)], G, AF, w=3)


@icon("TypeProperties")    # modify.typeProperties：类型属性（特性表，右下一把扳手似的编辑笔）
def _(i):
    i.rect(6, 6, 46, 50, G, w=3, r=3); i.d.rectangle(i.P([(6, 6), (46, 15)]), fill=GF)
    for y in (24, 33, 42):
        i.line([(12, y), (20, y)], G, 3); i.line([(24, y), (40, y)], G, 3)
    i.line([(34, 56), (56, 34)], A, 6)
    i.d.polygon(i.P([(30, 60), (34, 56), (32, 58)]), fill=A)


# ── 结构选项卡 ─────────────────────────────────────────────
@icon("StructuralColumn")  # struct.column：结构柱（立面上的一根柱，柱身强调，上下两条标高线）
def _(i):
    i.line([(6, 10), (58, 10)], G, 3); i.line([(6, 54), (58, 54)], G, 3)
    i.rect(24, 10, 40, 54, A, AF, w=3.5)
    i.line([(24, 20), (40, 34)], A, 2.5); i.line([(24, 34), (40, 48)], A, 2.5)


@icon("Beam")              # struct.beam：梁（立面上搁在两根柱上的一根梁，梁强调）
def _(i):
    i.rect(8, 30, 18, 58, G, GF, w=3); i.rect(46, 30, 56, 58, G, GF, w=3)
    i.rect(4, 18, 60, 30, A, AF, w=3.5)


@icon("StructuralFloor")   # struct.floor：结构楼板（等轴测的一块板，下面一根承托它的梁）
def _(i):
    i.poly([(4, 28), (32, 14), (60, 28), (32, 42)], A, AF, w=3.5)
    i.line([(4, 28), (4, 34), (32, 48), (60, 34), (60, 28)], G, 3.5)
    i.line([(32, 42), (32, 48)], G, 3.5)
    i.line([(18, 41), (18, 50), (46, 50), (46, 41)], G, 3)


@icon("Footing")           # struct.footing：独立基础（立面：柱脚立在一块宽的基础上，基础强调，地坪线）
def _(i):
    i.line([(4, 22), (60, 22)], G, 3)
    i.rect(26, 6, 38, 36, G, GF, w=3)
    i.rect(10, 36, 54, 52, A, AF, w=3.5)


@icon("WallFooting")       # struct.wallFooting：条形基础（剖面：一面墙立在比它宽的条形基础上，基础强调）
def _(i):
    i.line([(4, 22), (60, 22)], G, 3)
    i.rect(24, 6, 40, 38, G, GF, w=3)
    i.line([(24, 14), (40, 30)], G, 2)
    i.rect(8, 38, 56, 54, A, AF, w=3.5)
    i.dashed([(4, 58), (60, 58)], A, 2.5)


# ── 卫浴、电气、防雷、数据选项卡 ─────────────────────────────
@icon("Pipe")              # mep.pipe：管道（俯视的一段管，拐一个弯，两端的点）
def _(i):
    i.line([(6, 22), (42, 22), (42, 58)], A, 10)
    i.dot(6, 22, 4.5, G); i.dot(42, 58, 4.5, G)


@icon("PlumbingFixture")   # mep.plumbing：卫生器具（俯视的一个洗手盆：台面里一个椭圆的盆，后面的龙头）
def _(i):
    i.rect(6, 12, 58, 54, G, GF, w=3.5, r=4)
    i.ellipse(32, 36, 18, 12, A, AF, w=3.5)
    i.line([(32, 12), (32, 22)], G, 4); i.dot(32, 36, 3, G)


@icon("PipeFitting")       # mep.fitting：管件（三通：一根直管，中间接出一根，接头强调）
def _(i):
    i.line([(4, 24), (60, 24)], G, 7)
    i.line([(32, 24), (32, 60)], G, 7)
    i.rect(20, 14, 44, 36, A, AF, w=3.5, r=3)


@icon("PipeAccessory")     # mep.accessory：管道附件（管上的一个阀门：两个对顶的三角，上面的手轮）
def _(i):
    i.line([(4, 40), (16, 40)], G, 7); i.line([(48, 40), (60, 40)], G, 7)
    i.poly([(16, 28), (32, 40), (16, 52)], A, AF, w=3)
    i.poly([(48, 28), (32, 40), (48, 52)], A, AF, w=3)
    i.line([(32, 40), (32, 16)], G, 3); i.line([(22, 14), (42, 14)], G, 4)


@icon("ElectricalEquipment")  # mep.equipment：电气设备（墙上的一个配电箱，里面一排断路器，一个闪电记号）
def _(i):
    i.rect(12, 6, 52, 58, G, GF, w=3.5, r=3)
    for x in (20, 28, 36, 44):
        i.line([(x, 14), (x, 24)], G, 3.5)
    i.poly([(34, 30), (24, 44), (32, 44), (28, 54), (40, 38), (32, 38)], A, AF, w=2.5)


@icon("Device")            # mep.device：设备（墙上的一个插座：面板，两个插孔与接地孔）
def _(i):
    i.rect(10, 10, 54, 54, G, GF, w=3.5, r=6)
    i.circle(32, 32, 14, A, AF, w=3)
    i.dot(26, 30, 3, A); i.dot(38, 30, 3, A); i.dot(32, 39, 2.5, A)


@icon("LightingFixture")   # mep.lighting：照明设备（吊在板下的一盏灯，下面的光线）
def _(i):
    i.line([(6, 8), (58, 8)], G, 3.5)
    i.line([(32, 8), (32, 20)], G, 3)
    i.poly([(18, 34), (24, 20), (40, 20), (46, 34)], A, AF, w=3)
    for x0, x1 in ((22, 14), (32, 32), (42, 50)):
        i.line([(x0, 42), (x1, 56)], A, 2.5)


@icon("Conduit")           # mep.conduit：线管（一根细管，两头的接线盒，里面一根线）
def _(i):
    i.rect(4, 24, 16, 40, G, GF, w=3); i.rect(48, 24, 60, 40, G, GF, w=3)
    i.line([(16, 28), (48, 28)], A, 3); i.line([(16, 36), (48, 36)], A, 3)
    i.dashed([(18, 32), (46, 32)], G, 2, 4, 3)


@icon("CableTray")         # mep.cableTray：电缆桥架（等轴测的一段槽形桥架，底上的孔，里面几根电缆）
def _(i):
    i.poly([(4, 34), (36, 18), (60, 30), (28, 46)], A, AF, w=3)
    i.line([(4, 34), (4, 26)], A, 3); i.line([(28, 46), (28, 38)], A, 3)
    i.line([(60, 30), (60, 22)], A, 3); i.line([(36, 18), (36, 10)], A, 3)
    i.line([(4, 26), (36, 10)], A, 3); i.line([(28, 38), (60, 22)], A, 3)
    for k in (0, 1, 2):
        i.line([(12 + 6 * k, 33 + 3 * k), (42 + 6 * k, 18 + 3 * k)], G, 2.5)


@icon("Wire")              # mep.wire：导线（两个设备之间的一根弧形导线，两端的接点）
def _(i):
    i.rect(4, 40, 16, 56, G, GF, w=3); i.rect(48, 40, 60, 56, G, GF, w=3)
    i.arc(32, 44, 22, 180, 360, A, 3.5, ry=28)
    i.line([(23, 18), (27, 12)], A, 3); i.line([(29, 18), (33, 12)], A, 3); i.line([(35, 18), (39, 12)], A, 3)


@icon("AirTerminal")       # mep.airTerminal：接闪器（屋脊上立的一根避雷针，上面的闪电）
def _(i):
    i.poly([(4, 58), (32, 42), (60, 58)], G, w=3.5)
    i.line([(32, 42), (32, 20)], A, 4)
    i.poly([(30, 4), (20, 18), (28, 18), (24, 28), (38, 12), (30, 12)], A, AF, w=2.5)


@icon("DownConductor")     # mep.downConductor：引下线（立面上的一面墙，沿它竖着引下的导线，向下的箭头）
def _(i):
    i.rect(8, 6, 30, 58, G, GF, w=3)
    i.line([(38, 6), (38, 50)], A, 4)
    i.head(38, 30, 38, 58, A, 10)
    i.line([(4, 58), (60, 58)], G, 3)


@icon("EarthingElectrode")  # mep.earthing：接地极（地坪线下打进去的一根棒，接地记号）
def _(i):
    i.line([(4, 18), (60, 18)], G, 3)
    i.line([(32, 6), (32, 52)], A, 4)
    i.poly([(28, 52), (36, 52), (32, 60)], A, A, w=2)
    for k, half in enumerate((14, 9, 4)):
        i.line([(46 - half / 2 + 2, 28 + 7 * k), (46 + half / 2 + 2, 28 + 7 * k)], G, 3)


@icon("InspectionBox")     # mep.inspection：接地检查井（俯视的方形井盖，中间一根接地极的圆点）
def _(i):
    i.rect(10, 10, 54, 54, G, GF, w=3.5, r=3)
    i.rect(18, 18, 46, 46, A, w=3)
    i.circle(32, 32, 6, A, AF, w=3)
    i.line([(14, 14), (18, 18)], G, 2.5); i.line([(50, 50), (46, 46)], G, 2.5)


@icon("GroundConductor")   # mep.groundConductor：接地导体（俯视的一圈接地环，四角的接地极）
def _(i):
    i.rect(10, 10, 54, 54, A, w=4)
    i.line([(10, 32), (54, 32)], A, 2.5); i.line([(32, 10), (32, 54)], A, 2.5)
    for x, y in ((10, 10), (54, 10), (54, 54), (10, 54)):
        i.dot(x, y, 5, G)


@icon("DataOutlet")        # mep.dataOutlet：数据插座（面板上一个网口的轮廓）
def _(i):
    i.rect(10, 10, 54, 54, G, GF, w=3.5, r=6)
    i.poly([(20, 24), (44, 24), (44, 40), (38, 40), (38, 46), (26, 46), (26, 40), (20, 40)], A, AF, w=3)
    for x in (25, 30, 34, 39):
        i.line([(x, 26), (x, 31)], A, 1.8)


@icon("VoiceOutlet")       # mep.voiceOutlet：语音插座（面板，上面一个电话听筒）
def _(i):
    i.rect(10, 10, 54, 54, G, GF, w=3.5, r=6)
    i.arc(32, 40, 16, 200, 340, A, 5)
    i.rect(14, 34, 22, 42, A, AF, w=2.5, r=2); i.rect(42, 34, 50, 42, A, AF, w=2.5, r=2)


@icon("Rack")              # mep.rack：机柜（立面的一台落地机柜，几层设备，一层强调）
def _(i):
    i.rect(14, 4, 50, 58, G, GF, w=3.5, r=2)
    for y in (12, 22, 32, 42):
        i.rect(19, y, 45, y + 6, G, w=2)
    i.rect(19, 22, 45, 28, A, AF, w=2.5)
    i.dot(40, 15, 1.5, A)
    i.line([(18, 58), (18, 62)], G, 3); i.line([(46, 58), (46, 62)], G, 3)


@icon("PatchPanel")        # mep.patchPanel：配线架（一条横的面板，一排网口，挂耳）
def _(i):
    i.rect(4, 20, 60, 44, G, GF, w=3.5, r=2)
    for x in (12, 22, 32, 42):
        i.rect(x, 26, x + 7, 34, A, AF, w=2)
    i.line([(12, 39), (49, 39)], G, 2)
    i.dot(8, 32, 1.8, G); i.dot(56, 32, 1.8, G)


@icon("Cabling")           # mep.cabling：综合布线（从配线架拉到插座的一束线缆）
def _(i):
    i.rect(4, 8, 22, 22, G, GF, w=3); i.rect(42, 42, 60, 56, G, GF, w=3)
    for k in (-3, 0, 3):
        i.line([(22, 15 + k), (32 + k, 15 + k), (32 + k, 49 - k), (42, 49 - k)], A, 2.2)


# ── 体量选项卡 ─────────────────────────────────────────────
# 等轴测的体量盒子：顶面四点，竖边向下 26
MASS_TOP = [(32, 8), (56, 20), (32, 32), (8, 20)]


def mass_box(i, c=G, fill=None, w=3):
    i.poly([(8, 20), (32, 32), (32, 58), (8, 46)], c, fill, w=w)
    i.poly([(32, 32), (56, 20), (56, 46), (32, 58)], c, fill, w=w)
    i.poly(MASS_TOP, c, fill, w=w)


@icon("Mass")              # massing.mass：体量（等轴测的一个盒子，强调）
def _(i):
    mass_box(i, A, AF, 3.5)


@icon("MassFloors")        # massing.floors：体量楼层（灰色的体量，里面几层强调的楼板）
def _(i):
    mass_box(i, G, None, 2.5)
    i.poly(MASS_TOP, A, AF, w=3)
    for dz in (12, 24):
        i.line([(8, 20 + dz), (32, 32 + dz), (56, 20 + dz)], A, 3.5)


@icon("WallByFace")        # massing.wallByFace：面墙（灰色的体量，左前的面是一面强调的墙）
def _(i):
    mass_box(i, G, None, 2.5)
    i.poly([(8, 20), (32, 32), (32, 58), (8, 46)], A, AF, w=3.5)


@icon("FloorByFace")       # massing.floorByFace：面楼板（灰色的体量，顶面一块强调的板）
def _(i):
    mass_box(i, G, None, 2.5)
    i.poly([(8, 20), (32, 32), (56, 20), (56, 25), (32, 37), (8, 25)], A, AF, w=3)
    i.poly(MASS_TOP, A, AF, w=3)


@icon("RoofByFace")        # massing.roofByFace：面屋顶（灰色的体量，顶上挑出的一块强调的屋顶）
def _(i):
    mass_box(i, G, None, 2.5)
    i.poly([(32, 3), (62, 18), (32, 33), (2, 18)], A, AF, w=3.5)


@icon("UpdateByFace")      # massing.updateByFace：更新到面（灰色的体量，一个绕回来的强调箭头）
def _(i):
    mass_box(i, G, GF, 2.5)
    i.arc(32, 30, 14, 200, 520, A, 3.5)
    i.poly([(40, 10), (50, 16), (40, 22)], A, A, w=2)


def dim_ticks(i, x0, y0, x1, y1, c=A, w=3.5):
    """尺寸线两端的斜短划（建筑尺寸的记号）"""
    for x, y in ((x0, y0), (x1, y1)):
        i.line([(x - 4, y + 4), (x + 4, y - 4)], c, w)


@icon("DimAligned")        # annotate.aligned：对齐尺寸（两面灰色的墙，之间一条带斜短划的强调尺寸线）
def _(i):
    i.rect(6, 30, 14, 58, G, GF, w=3)
    i.rect(50, 30, 58, 58, G, GF, w=3)
    i.line([(10, 26), (10, 12)], G, 2.5); i.line([(54, 26), (54, 12)], G, 2.5)
    i.line([(6, 18), (58, 18)], A, 3.5)
    dim_ticks(i, 10, 18, 54, 18)


@icon("DimAngular")        # annotate.angular：角度尺寸（两条相交的灰线，夹角里一段强调的弧）
def _(i):
    i.line([(8, 54), (58, 54)], G, 3.5)
    i.line([(8, 54), (44, 12)], G, 3.5)
    i.arc(8, 54, 34, -49, 0, A, 3.5)
    i.dot(42, 54, 3); i.dot(30, 28, 3)


@icon("DimRadial")         # annotate.radial：径向尺寸（灰色的圆柱，圆心到边一条带箭头的强调半径线）
def _(i):
    i.circle(32, 32, 22, G, GF, w=3.5)
    i.dot(32, 32, 3)
    i.arrow(32, 32, 47.5, 16.5, A, 3.5, 9)


@icon("SpotElevation")     # annotate.spot：高程点（一块灰色的板，上面强调的三角记号与引线）
def _(i):
    i.poly([(6, 48), (40, 48), (58, 40), (24, 40)], G, GF, w=3)
    i.poly([(26, 42), (20, 32), (32, 32)], A, AF, w=3)
    i.line([(20, 32), (56, 32)], A, 3)
    i.line([(32, 12), (56, 12)], G, 3); i.line([(32, 20), (50, 20)], G, 3)


@icon("DetailLine")        # annotate.detailLine：详图线（一根强调的折线，两端的点，旁边一支灰色的笔）
def _(i):
    i.line([(8, 52), (24, 30), (40, 40), (56, 14)], A, 3.5)
    for x, y in ((8, 52), (24, 30), (40, 40), (56, 14)):
        i.dot(x, y, 3.5)


@icon("FilledRegion")      # annotate.filledRegion：填充区域（灰框的一块，里面强调的斜线图案）
def _(i):
    i.rect(8, 8, 56, 56, G, AF, w=3.5)
    for c in range(24, 112, 12):            # 斜线 x + y = c，截在框里
        x0, x1 = max(10, c - 54), min(54, c - 10)
        if x1 > x0:
            i.line([(x0, c - x0), (x1, c - x1)], A, 2.5)


@icon("MaskingRegion")     # annotate.maskingRegion：遮罩区域（灰色的线到强调的框就断开：框里的被盖住）
def _(i):
    for y in (14, 26, 38, 50):
        if 20 < y < 44:
            i.line([(6, y), (14, y)], G, 3); i.line([(50, y), (58, y)], G, 3)
        else:
            i.line([(6, y), (58, y)], G, 3)
    i.rect(18, 20, 46, 44, A, w=3.5)


@icon("Text")              # annotate.text：文字（一个大写的 A，下面两行灰色的字）
def _(i):
    i.line([(14, 40), (26, 8), (38, 40)], A, 4)
    i.line([(19, 28), (33, 28)], A, 3.5)
    i.line([(8, 48), (56, 48)], G, 3); i.line([(8, 56), (42, 56)], G, 3)


@icon("Measure")           # annotate.measure：测量（一把斜放的尺子，刻度强调）
def _(i):
    i.poly([(4, 44), (44, 4), (60, 20), (20, 60)], G, GF, w=3)
    for k in range(1, 6):
        x, y = 4 + k * 6.7, 44 - k * 6.7
        L = 9 if k % 2 else 5
        i.line([(x, y), (x + L * 0.7, y + L * 0.7)], A, 3)


@icon("Tag")               # annotate.tag：按类别标记（灰色的门洞，旁边一个强调的标记框）
def _(i):
    i.line([(6, 48), (20, 48)], G, 4); i.line([(44, 48), (58, 48)], G, 4)
    i.arc(20, 48, 22, 270, 360, G, 2.5); i.line([(20, 48), (20, 26)], G, 3)
    i.rect(34, 8, 58, 24, A, AF, w=3.5, r=4)
    i.line([(40, 16), (52, 16)], A, 3)


@icon("TagAll")            # annotate.tagAll：全部标记（斜排的三个强调标记框）
def _(i):
    for x, y in ((4, 42), (20, 24), (36, 6)):
        i.rect(x, y, x + 24, y + 16, A, AF, w=3, r=4)
        i.line([(x + 6, y + 8), (x + 18, y + 8)], A, 3)


@icon("Materials")         # manage.materials：材质（三个色块的球，一块强调）
def _(i):
    i.circle(32, 32, 24, G, w=3.5)
    i.circle(24, 26, 7, A, AF, w=3)
    i.circle(40, 26, 7, G, GF, w=3)
    i.circle(32, 42, 7, G, GF, w=3)


@icon("ProjectParameters") # manage.parameters：项目参数（一张参数表：几行名称 | 值，加的那一行强调，旁边一个加号）
def _(i):
    i.rect(6, 8, 46, 56, G, w=3, r=3)
    for y in (20, 32):
        i.line([(12, y), (22, y)], G, 3); i.line([(28, y), (40, y)], G, 3)
    i.line([(12, 44), (22, 44)], A, 3); i.line([(28, 44), (40, 44)], A, 3)
    i.line([(52, 30), (52, 50)], A, 3.5); i.line([(42, 40), (62, 40)], A, 3.5)


@icon("ObjectStyles")      # manage.objectStyles：对象样式（一段墙的剖面：粗的剖切线强调，细的看线灰色）
def _(i):
    i.rect(8, 14, 30, 50, A, AF, w=5)
    i.line([(38, 14), (58, 14)], G, 2); i.line([(38, 50), (58, 50)], G, 2)
    i.line([(38, 14), (38, 50)], G, 2); i.line([(58, 14), (58, 50)], G, 2)


@icon("LineWeights")       # manage.lineWeights：线宽（由细到粗的四条横线，最粗的强调）
def _(i):
    for y, w in ((12, 1.5), (24, 3), (37, 5)):
        i.line([(10, y), (54, y)], G, w)
    i.line([(10, 52), (54, 52)], A, 8)


@icon("LineStyles")        # manage.lineStyles：线样式（实线、虚线、点划线，点划线强调）
def _(i):
    i.line([(8, 14), (56, 14)], G, 3.5)
    i.dashed([(8, 32), (56, 32)], G, 3.5, dash=8, gap=5)
    for x0, x1 in ((8, 22), (30, 44), (52, 56)):
        i.line([(x0, 50), (x1, 50)], A, 3.5)
    for x in (26, 48):
        i.dot(x, 50, 2)


@icon("FillPatterns")      # manage.fillPatterns：填充图案（方框里的斜线填充，一条强调）
def _(i):
    i.rect(8, 8, 56, 56, G, w=3)
    for k in range(-2, 4):
        x0 = 8 + k * 14
        pts = [(max(8, x0), 56 - max(0, 8 - x0)), (min(56, x0 + 48), 8 + max(0, x0 + 48 - 56))]
        i.line(pts, A if k == 1 else G, 3 if k == 1 else 2)


@icon("Purge")             # manage.purge：清除未使用（一把扫帚，扫出去的几个强调色的小方块）
def _(i):
    i.line([(50, 6), (30, 34)], G, 4)
    i.poly([(22, 30), (38, 40), (30, 58), (8, 50)], G, GF, w=3)
    i.line([(17, 42), (12, 54)], G, 2); i.line([(25, 45), (21, 56)], G, 2)
    i.rect(44, 46, 50, 52, A, AF, w=2); i.rect(52, 36, 58, 42, A, AF, w=2)


@icon("Properties")        # view.properties：特性面板（照 MiniCAD 的 PropertiesPanel：名称 | 值的几行）
def _(i):
    i.rect(8, 8, 56, 56, G, w=3, r=4); i.d.rectangle(i.P([(8, 8), (56, 18)]), fill=AF)
    for y in (28, 38, 48):
        i.line([(14, y), (26, y)], G, 3); i.line([(32, y), (50, y)], A, 3)


@icon("ProjectBrowser")    # view.browser：项目浏览器（一棵树：根，下面缩进的几项）
def _(i):
    i.rect(8, 8, 20, 18, A, AF, w=3)
    i.line([(14, 18), (14, 50)], G, 3)
    for y in (32, 50):
        i.line([(14, y), (24, y)], G, 3); i.rect(26, y - 5, 56, y + 5, G, w=3)


@icon("VisibilityGraphics")  # view.visibility：可见性 / 图形（一只眼睛，下面勾选与去掉勾的两行）
def _(i):
    i.d.ellipse(i.P([(8, 10), (56, 34)]), outline=A, width=int(3.5 * S))
    i.circle(32, 22, 6, A, AF, w=3)
    i.rect(8, 40, 18, 50, G, w=3); i.line([(10, 45), (13, 48), (17, 41)], A, 3); i.line([(24, 45), (56, 45)], G, 3)
    i.rect(8, 54, 18, 63, G, w=3); i.line([(24, 58.5), (48, 58.5)], G, 3)


@icon("CropView")          # view.crop：裁剪视图（两个直角的裁剪框角，框住一块灰色的平面）
def _(i):
    i.rect(18, 18, 46, 46, G, GF, w=3)
    i.line([(14, 4), (14, 50), (60, 50)], A, 4)
    i.line([(4, 14), (50, 14), (50, 60)], A, 4)


@icon("DuplicateView")     # view.duplicate：复制视图（两张错开的平面，前面一张强调）
def _(i):
    i.rect(6, 6, 42, 42, G, GF, w=3)
    i.rect(22, 22, 58, 58, A, AF, w=3.5)
    i.line([(30, 40), (50, 40)], A, 3); i.line([(40, 30), (40, 50)], A, 3)


@icon("DuplicateDetailing") # view.detailing：带细节复制（同上，前面那张里有一条尺寸）
def _(i):
    i.rect(6, 6, 42, 42, G, GF, w=3)
    i.rect(22, 22, 58, 58, A, AF, w=3.5)
    i.line([(28, 44), (52, 44)], A, 3)
    i.line([(28, 38), (28, 50)], A, 2.5); i.line([(52, 38), (52, 50)], A, 2.5)
    i.line([(32, 32), (48, 32)], G, 3)


@icon("DependentView")     # view.dependent：相关视图（一张灰色的平面，里面强调的虚框是相关视图的裁剪）
def _(i):
    i.rect(6, 6, 58, 58, G, GF, w=3)
    for x0, y0, x1, y1 in ((14, 14, 26, 14), (32, 14, 44, 14), (44, 14, 44, 26), (44, 32, 44, 44),
                           (44, 44, 32, 44), (26, 44, 14, 44), (14, 44, 14, 32), (14, 26, 14, 14)):
        i.line([(x0, y0), (x1, y1)], A, 3.5)
    i.line([(44, 44), (56, 56)], A, 3)


@icon("Section")           # view.section：剖面（一栋房子的剖面：灰色的墙与楼板，强调色的剖面线与看向的三角旗）
def _(i):
    i.rect(10, 20, 16, 54, G, GF, w=2.5)
    i.rect(48, 20, 54, 54, G, GF, w=2.5)
    i.rect(10, 34, 54, 38, G, GF, w=2.5)
    i.poly([(8, 20), (32, 6), (56, 20)], G, w=3)
    i.line([(4, 58), (60, 58)], A, 3.5)
    i.poly([(4, 58), (12, 58), (4, 50)], A, AF, w=2.5)


@icon("Elevation")         # view.elevation：立面（房子的正面，前面一个强调色的圆圈记号）
def _(i):
    i.rect(8, 22, 44, 52, G, GF, w=3)
    i.poly([(4, 24), (26, 8), (48, 24)], G, w=3)
    i.rect(20, 36, 30, 52, G, w=2.5)
    i.circle(50, 46, 8, A, AF, w=3)
    i.poly([(44, 46), (50, 36), (56, 46)], A, AF, w=2.5)


@icon("Sheet")             # view.sheet：图纸（横放的纸：灰色的边框，右下角强调色的标题栏）
def _(i):
    i.rect(4, 12, 60, 52, G, w=3)
    i.rect(10, 17, 55, 47, G, w=2)
    i.rect(34, 37, 55, 47, A, AF, w=2.5)


@icon("PlaceView")         # view.placeView：放视图（图纸上一个强调色的视图框，里面一个小平面）
def _(i):
    i.rect(4, 12, 60, 52, G, w=3)
    i.rect(14, 20, 42, 44, A, AF, w=2.5)
    i.rect(20, 26, 36, 38, A, w=2.5)
    i.line([(46, 20), (46, 44)], G, 2.5)


@icon("Schedule")          # view.schedule：明细表（一张表：强调的标题行，下面灰色的格子）
def _(i):
    i.rect(6, 8, 58, 56, G, w=3)
    i.rect(6, 8, 58, 20, A, AF, w=3)
    for y in (31, 42):
        i.line([(6, y), (58, y)], G, 2.5)
    for x in (24, 41):
        i.line([(x, 8), (x, 56)], G, 2.5)


@icon("ViewFilters")       # view.filters：视图过滤器（一个漏斗，漏下去的是强调色）
def _(i):
    i.poly([(6, 8), (58, 8), (38, 32), (38, 52), (26, 58), (26, 32)], G, GF, w=3)
    i.line([(18, 18), (46, 18)], A, 3.5)
    i.circle(32, 44, 4, A, AF, w=2.5)


@icon("ViewTemplate")      # view.template：由当前视图创建样板（一张平面，旁边强调的印章）
def _(i):
    i.rect(6, 6, 44, 44, G, GF, w=3)
    i.line([(12, 18), (38, 18)], G, 2.5); i.line([(12, 28), (30, 28)], G, 2.5)
    i.rect(30, 44, 58, 58, A, AF, w=3)
    i.line([(44, 30), (44, 44)], A, 3.5)
    i.circle(44, 28, 5, A, AF, w=3)


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
