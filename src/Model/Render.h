// 移植自 BIMCraft crates/core/src/render.rs、pen.rs、styles.rs（f3bbb33）的平面图部分，MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Model.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

// 平面图：一个标高的平面画成显示列表（像素坐标里填色的环和折线），界面画它、拾取它。制图惯例：剖切面穿过的墙粗轮廓加灰色填充，
// 剖切面以下看到的墙细线（低于视图范围底部的变淡）；门有门扇与开启线、窗有玻璃线，族的门窗画剖到的形体与族的平面线；
// 屋顶画屋脊与斜脊（上一层的屋顶在视图范围顶部以下时画虚线）；房间画标签；轴网画虚线和两端的圆圈。
//
// 第 2d 步移植的是 BIMCraft 平面图的核心：视图的图形覆盖（Look）、填充与线型图案、裁剪范围、注释（尺寸、标记、文字……）、
// 楼梯与设备随那些模块移植时再加
namespace MiniBIM::Bim::Render
{
    struct Rgb
    {
        uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb&) const = default;
    };

    inline constexpr Rgb Paper      = { 255, 255, 255 };
    // 剖到的东西的填充
    inline constexpr Rgb Poche      = { 168, 168, 168 };
    inline constexpr Rgb LightPoche = { 212, 212, 212 };
    inline constexpr Rgb Ink        = { 25, 25, 25 };
    inline constexpr Rgb Faint      = { 120, 120, 120 };
    inline constexpr Rgb GridColor  = { 60, 100, 150 };
    // 体量的轮廓，虚线
    inline constexpr Rgb MassColor  = { 88, 112, 168 };

    // 线是元素的哪部分：看到的（剖切面以下、注释）、剖到的、字、图案
    enum class Part
    {
        Projection,
        Cut,
        Text,
        Pattern,
    };

    using Px = std::pair<float, float>;

    // 显示列表的一项：填色的环（各环同向，重叠处合成一块），或折线（宽 width 像素，dash 是实虚相间的像素长度）
    struct Prim
    {
        enum class Kind
        {
            Fill,
            Line,
        };
        Kind                          kind = Kind::Line;
        std::vector<std::vector<Px>>  rings;
        std::vector<Px>               pts;
        float                         width = 1.0f;
        Rgb                           color;
        std::optional<std::vector<float>> dash;
        bool                          closed = false;
    };

    // 模型毫米（y 向上）到图像像素（y 向下）
    struct View
    {
        // 每毫米多少像素
        double   scale  = 1.0;
        double   cx     = 0.0;
        double   cy     = 0.0;
        uint32_t width  = 0;
        uint32_t height = 0;

        Px ToPx(P2 p) const;
        // 像素处的模型点：ToPx 的反向
        P2 ToMm(Px p) const;
    };

    // 线的粗细：0 是发丝线，1（最细）到 16，各是纸上的毫米数，同 Revit
    struct Pen
    {
        uint8_t n = 0;

        static constexpr uint8_t Hairline   = 0;
        static constexpr uint8_t Cut        = 10;
        static constexpr uint8_t Heavy      = 7;
        static constexpr uint8_t Medium     = 6;
        static constexpr uint8_t Bubble     = 5;
        static constexpr uint8_t Projection = 4;
        static constexpr uint8_t Fine       = 1;
        // 只是记号：画完后换成元素类别的笔（对象样式），剖到的用 cut，看到的用 projection
        static constexpr uint8_t ByCategory = 255;
        // 纸上多宽，毫米
        double Mm() const;
    };

    // 一个视图里各支笔画多宽：模型的（按视图比例取最近的一列）和注释的，纸上毫米
    struct Weights
    {
        std::array<double, 17> model{};
        std::array<double, 17> annotation{};

        // 1 : n 时的线宽：项目单位起步的线宽表（BIMCraft 的 LineWeights::default_for）在最近的比例上
        static Weights At(Units units, double n);
        // 项目的线宽表 lw（Model::LineWeightsOf）在 1 : n 时
        static Weights Of(const LineWeights& lw, double n);
        double Mm(uint8_t pen, bool isAnnotation) const;
    };

    class Drawing
    {
    public:
        explicit Drawing(View v) : view(v) {}

        View view;
        std::vector<Prim>              prims;
        // 每项画的元素：拾取与高亮用
        std::vector<std::optional<Id>> owners;
        std::vector<Part>              parts;
        // 每项用的笔：最后按它定宽；字、普通填充没有
        std::vector<std::optional<uint8_t>> pens;
        Weights weights;
        // 注释放大的倍数（随视图比例），以及纸张像素的倍数（定线宽）
        float k     = 1.0f;
        float paper = 1.0f;

        void Own(std::optional<Id> owner) { m_owner = owner; }
        void SetPart(Part p) { m_part = p; }
        void Push(Prim p, std::optional<uint8_t> pen = std::nullopt);
        void Stroke(std::vector<Px> pts, uint8_t pen, Rgb color, std::optional<std::vector<float>> dash, bool closed);
        void Fill(const std::vector<Ring>& rings, Rgb color);
        void Line(const std::vector<P2>& pts, uint8_t pen, Rgb color, bool closed);
        void Dashed(const std::vector<P2>& pts, uint8_t pen, Rgb color, bool closed);
        // text 以 at 为中心，大写字母 height 像素高（单线字）
        void Text(std::string_view text, Px at, float height, Rgb color);
        // from 起推进来的各项以 about 为中心放大 k 倍：注释按一像素一像素排好，再按视图比例放大
        void Grow(size_t from, Px about);
        float Width(uint8_t pen) const;
        float PxOf(float px) const { return px * k; }

        // 离像素 at 最近、在 tol 像素以内的元素：在填色里面胜过在线上；一样近的取后画的（显示在上面）。
        // 链接造的东西画出来、能捕捉，从不拾取
        std::optional<Id> Pick(Px at, float tol) const;
        // tol 以内的每个元素，按 Pick 的先后
        std::vector<Id> PickAll(Px at, float tol) const;

        // 链接造的东西放到最前面，项目画的盖在它上面：各项保持画的先后与笔
        void LinkedUnder();
        // 裁到像素 lo 到 hi 的框里：keep 认的元素的各项整个留着
        void Crop(Px lo, Px hi, const std::function<bool(std::optional<Id>)>& keep);
        // 照视图的图形改各元素的线与填充：颜色、线宽、虚实、填充色，半色调向纸色走一半
        void Dress(const Look& look);
        // 画完后：类别的笔换成对象样式的，再按笔定线宽（注释的元素用注释的线宽）
        void ByCategory(const Model& m);
        void Weigh(const Model& m);

    private:
        std::optional<Id> m_owner;
        Part              m_part = Part::Projection;
    };

    // 把 pts 放进 width × height 像素的视图（四周留边，注释随比例放大）
    View Fit(const std::vector<P2>& pts, uint32_t width, uint32_t height, double n);
    // c 向纸色走一半（半色调）
    Rgb Halftone(Rgb c);
    // 平面 plan（标高的平面或平面视图）显示的一切：墙、楼板、房间、构件、轴网、屋顶……的点；裁剪了的为裁剪框的两角
    std::vector<P2> PlanShows(const Model& m, Guid plan);
    // 平面 plan，放进 width × height 像素
    Drawing Plan(const Model& m, Guid plan, uint32_t width, uint32_t height);
    // 平面 plan，按给定的视图画：视图的比例、图形（隐藏、覆盖、详细程度）与裁剪
    Drawing Draw(const Model& m, Guid plan, View view);
    // 还不在模型里的元素 e，照平面 plan 会画它的样子（各项归它，ID 是放进模型后会有的那个）：工具预览用（门窗、构件、屋顶……）。
    // 移植自 BIMCraft 的 render::sketch，做法不同：放进模型的一份副本里画整个平面，只留它的几项
    Drawing Sketch(const Model& m, Guid plan, View view, const Element& e);

    // ── 剖面与立面（第 5e3 步，移植自 render.rs 的 section）────────
    // 剖面或立面 view 的线剖到的，各元素的环（沿线离起点、向上的毫米）：界面由三维场景的三角形算出（ModelScene::Cut）
    using SectionCuts = std::vector<std::pair<Id, std::vector<Ring>>>;
    // 剖面看到的模型（剖到的与线后、深度以内的）沿线与高度的范围；什么也没看到时 any 为假
    struct Seen
    {
        double first = 0.0, last = 0.0, lo = 0.0, hi = 0.0;
        bool   any   = false;
    };
    // 标高与墙、柱、屋顶、体量、run 的高度，从最低到最高（look 隐藏的不算；什么也没有时为 0 到 3000）
    std::pair<double, double> ModelHeights(const Model& m, const Look& look);
    // crop 命令不给区域时剖面或立面裁到的底与顶：ModelHeights 再上下各多出轴网伸出的 600 毫米
    std::array<double, 2> SectionHeights(const Model& m, Guid view);
    // 剖面或立面 view 显示的范围（沿线、向上的毫米）的两角，右边留出标高名称的地方：适配视口用
    std::vector<P2> SectionShows(const Model& m, Guid view, const Seen& seen);
    // 剖面或立面 view 按 v 画的：剖到的灰色填充与轮廓（复合墙的层线、看到的形体由界面画）、标高（名称与高度写在线的末端外）、
    // 线穿过的轴网、视图名称；裁剪了的裁到框里
    Drawing DrawSection(const Model& m, Guid view, View v, const SectionCuts& cuts, const Seen& seen);

    // ── 图纸（第 5e4 步，移植自 sheet.rs，Sheets.cpp）────────────
    // 图纸都按界面的取景 page 画：纸上的毫米（从左下角起，y 向上）到屏幕像素
    // 图纸上一个视口的框：视图按 inner 画（取景同 page 一样大，视图的中心落在框的中心）；三维视图的 inner 是相机平面上离 middle 的毫米
    struct Frame
    {
        Id                viewport = 0;
        Guid              view;
        View              inner;
        Px                lo, hi;
        std::optional<P3> middle;
        bool              table = false;
    };
    // 剖面、立面剖到的与看到的范围（界面由三维场景算出）：图纸上的剖面按它画
    using SheetCuts = std::map<Guid, std::pair<SectionCuts, Seen>>;
    // 视图 view 按 1 : scale 放到纸上要多大，毫米：它显示的四周再留 12 毫米；明细表为表格的大小；不是能放的视图时为空
    std::optional<P2> ViewportSize(const Model& m, Guid view, double scale);
    // 视图 view 按 1 : scale 默认放在图纸 sheet 的哪里（纸上毫米）：已放的视图的右边，没有时在标题栏以上的中间；右边放不下时放右上角
    P2 SpotOn(const Model& m, Guid sheet, Guid view, double scale);
    // 纸（宽与高，毫米）放进 width × height 像素，四周留 20 像素
    View FitPaper(P2 paper, uint32_t width, uint32_t height);
    // 图纸 sheet 上各视口的框
    std::vector<Frame> Frames(const Model& m, Guid sheet, const View& page);
    // 图纸 sheet：边框与标题栏（归图纸），各视口里平面、剖面（剖到的、标高、轴网）与明细表的表格，框下的视图标题（都归视口）。
    // 纸的白底、三维视图的样子与剖面看到的形体不在里面，由界面画在下面
    Drawing DrawSheet(const Model& m, Guid sheet, const View& page, const SheetCuts& cuts);
}
