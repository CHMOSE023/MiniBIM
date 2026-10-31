// 移植自 BIMCraft crates/core/src/model.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Family.h"
#include "Model/Geom.h"
#include "Model/Ids.h"
#include "Model/Json.h"
#include "Model/Language.h"
#include "Model/Library.h"
#include "Model/Runs.h"
#include "Model/Curve.h"
#include "Model/Styles.h"
#include "Model/Units.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// 建筑模型。长度单位毫米，x 向东、y 向北、z 向上，每个标高有高程。
// 第 2a 步移植了标高、轴网、墙、楼板、门、窗与它们的类型、材质，第 2b 步加了屋顶、天花板、房间、房间分隔线，第 2c 步加了族与构件；
// BIMCraft 的其余元素（视图、图纸、结构与设备……）按 docs/PLAN.md 第 2 阶段的顺序陆续加进 Element。新增一种元素时 std::visit 会列出所有要补的分支
namespace MiniBIM::Bim
{
    // 平面视图剖切面在标高以上的高度（视图范围没另设时）；房间在这个高度找围住它的墙
    inline constexpr double CutHeight     = 1200.0;
    // 墙端距离小于它就连接，毫米
    inline constexpr double Tol           = 1.0;
    // 超过 1000 km 的坐标和长度拒绝
    inline constexpr double MaxCoord      = 1e9;
    // 上面没有标高的墙的高度
    inline constexpr double DefaultHeight = 3000.0;
    // 名称、标记、编号超过这么多字符拒绝
    inline constexpr size_t MaxText       = 256;
    // 一条命令或一个楼板轮廓最多的点数
    inline constexpr size_t MaxPoints     = 10000;
    // 楼板、屋顶最大的坡度，度
    inline constexpr double MaxSlope      = 75.0;
    // 一个类型最多的层数
    inline constexpr size_t MaxLayers     = 32;
    // 标高没有自己的范围时伸出模型多远，毫米
    inline constexpr double LevelPast     = 1000.0;
    // 本程序写出、能读的项目文件版本，与 BIMCraft 0733fd4 相同（见 BIMCraft model.rs 的 FORMAT）。
    // 10 加了尺寸标注的端点、方向与类型（MiniBIM 还没有尺寸，读入时略去），11 加了项目语言 sv、cs、fr（还没移植，读入时按英语并略去）。
    // 更新的版本拒绝打开；0 是还没有 GUID 的旧文件
    inline constexpr uint32_t FileFormat  = 11;

    // 平面视图的视图范围，同 Revit：标高以上的毫米。被 cut 平面剖切，往下看到 bottom，再往下到视图深度 depth；
    // 剖切面以上看到 top（没有时为上一个标高）为止的悬挑部分
    struct ViewRange
    {
        std::optional<double> top;
        double cut    = CutHeight;
        double bottom = 0.0;
        double depth  = 0.0;
        bool operator==(const ViewRange&) const = default;
    };

    // 标高及其平面视图：视图范围与裁剪范围（西南角、东北角；没有时为全部）。作为基准它是 extent 范围上的水平面
    //（没有时为模型的范围，再伸出 LevelPast）
    struct Level
    {
        std::string name;
        double      elevation = 0.0;
        ViewRange   viewRange;
        std::optional<std::array<P2, 2>> crop;
        std::optional<std::array<P2, 2>> extent;
        // 平面视图是否显示默认三维视图剖面框保留的部分
        bool        sectionBox = false;
        bool operator==(const Level&) const = default;
    };

    struct Grid
    {
        std::string name;
        P2          start;
        P2          end;
        bool operator==(const Grid&) const = default;
    };

    // 墙的定位线，同 Revit：墙中心线、核心层中心线，或面层面、核心层面的外侧或内侧
    enum class Location
    {
        Centre,
        CoreCentre,
        ExteriorFinish,
        InteriorFinish,
        ExteriorCore,
        InteriorCore,
    };

    namespace LocationOf
    {
        inline constexpr std::array<Location, 6> All = { Location::Centre, Location::CoreCentre, Location::ExteriorFinish,
                                                         Location::InteriorFinish, Location::ExteriorCore, Location::InteriorCore };
        // 命令与文件里的名称：centre、core-centre……
        const char* Name(Location l);
        // 菜单上的名称（英文原文，显示前 Tr）
        const char* Label(Location l);
        // 也认美式拼写 center
        std::optional<Location> FromName(std::string_view name);
    }

    // 墙的一层做什么：承重（结构，即核心层）、衬底、保温、隔汽（膜，可以薄到 0）、面层
    enum class Function
    {
        Structure,
        Substrate,
        Thermal,
        Membrane,
        Finish,
    };

    namespace FunctionOf
    {
        const char* Name(Function f);
        std::optional<Function> FromName(std::string_view name);
    }

    // 复合类型的一层：墙从外侧往里，楼板、屋顶、天花板从上往下。材质是项目库里的一个，或者没有
    struct Layer
    {
        // 与元素一样的 ID 和 GUID：建时给出，在列表里的位置不变就一直保留
        Id          id = 0;
        Guid        guid;
        Function    function = Function::Structure;
        std::string material;
        double      thickness = 0.0;
        bool operator==(const Layer&) const = default;
    };

    // 制图用的填充图案，由代码画：不论比例，线与线相隔几个像素
    enum class Pattern
    {
        None,
        Solid,
        Diagonal,
        Crosshatch,
        Horizontal,
        Vertical,
        Grid,
        Brick,
        Concrete,
        Insulation,
    };

    namespace PatternOf
    {
        inline constexpr std::array<Pattern, 10> All = { Pattern::None, Pattern::Solid, Pattern::Diagonal, Pattern::Crosshatch,
                                                         Pattern::Horizontal, Pattern::Vertical, Pattern::Grid, Pattern::Brick,
                                                         Pattern::Concrete, Pattern::Insulation };
        const char* Name(Pattern p);
        std::optional<Pattern> FromName(std::string_view name);
    }

    // 新项目的玻璃材质有多透明，百分数
    inline constexpr uint8_t GlassTransparency = 70;

    // 项目库的材质：着色视图里的颜色、平面和剖面里剖切面的图案、俯视楼板（仰视天花板）时表面的图案，
    // 以及三维视图里透过它的表面能看到身后多少（transparency，百分数：0 不透明到 100）。
    // BIMCraft 的材质还有填充样式名称与贴图，移植图纸、贴图时再加
    struct Material
    {
        std::array<uint8_t, 3> color{ 200, 200, 200 };
        Pattern cut     = Pattern::None;
        Pattern surface = Pattern::None;
        uint8_t transparency = 0;
        bool operator==(const Material&) const = default;
    };

    // 墙：定位线从 start 到 end，直线或转过 arc 度的圆弧；location 是定位线是墙的哪条线；
    // 外侧在 start → end 的左边，同 Revit，所以顺时针画的墙外侧朝外
    struct Wall
    {
        std::string typeName;
        Guid        level;                  // 底部标高
        P2          start;
        P2          end;
        std::optional<Guid> topLevel;       // 顶部约束：一个标高；没有时为高 height 的非连接墙
        double      height     = 0.0;
        double      baseOffset = 0.0;
        // 从起点到终点转过的角度，逆时针为正：0 为直墙，180 为向右凸出的半圆
        double      arc        = 0.0;
        Location    location   = Location::Centre;
        // 顶部从下面贴着的屋顶或楼板（代替顶部约束）；底部立在其上的那个
        std::optional<Guid> attachTop;
        std::optional<Guid> attachBase;
        // 存的参数值：现在只有体量的记号（Model/Massing.h 的 Massing::Face），项目参数移植后也在这里
        std::map<std::string, Json> params;
        bool operator==(const Wall&) const = default;
    };

    // 草图线（移植自 BIMCraft sketch.rs 的 SketchLine）：直线，或从 start 到 end 转过 arc 度的圆弧（同墙）。楼板、天花板、屋顶记着画它们
    // 或最后编辑它们的草图，"编辑边界"从它开始；每条线是一个部件，有自己的 ID 与 GUID。BIMCraft 的其他曲线还没移植（Model/Sketch.h）
    struct SketchLine
    {
        Id     id = 0;
        Guid   guid;
        P2     start;
        P2     end;
        double arc = 0.0;
        // 填充区域的边界线：自己的线样式（没有时用区域的），或不画（invisible）
        std::optional<std::string> style;
        bool   invisible = false;
        bool operator==(const SketchLine&) const = default;
    };

    // 楼板：顶面以下的一块板，沿 boundary（逆时针），openings 处开洞（在里面、互不相交）。顶面在标高上方 offset；
    // 有坡度箭头时（平面上从尾到头）尾部在 offset，向头部以 slope 度升高（负值下降）。厚度为类型的厚度，垂直于坡面量
    struct Floor
    {
        std::string       typeName;
        Guid              level;
        Ring              boundary;
        std::vector<Ring> openings;
        double            offset = 0.0;
        double            slope  = 0.0;
        std::optional<std::array<P2, 2>> arrow;
        // 承重：结构楼板，同 Revit 的"楼板：结构"
        bool              structural = false;
        // 画它或最后编辑它的草图；别的方式建的（沿墙、文件里没有）为空
        std::vector<SketchLine> sketch;
        // 顶面模型图案的起点与转角（度）：Revit 对齐、旋转图案
        P2                patternOrigin;
        double            patternAngle = 0.0;
        // 存的参数值：体量的记号（Massing::FloorMark、Massing::Face）
        std::map<std::string, Json> params;
        bool operator==(const Floor&) const = default;
    };

    // 门或窗：在宿主墙里，中心离墙定位线起点 offset 毫米；sill 是底边在墙底以上的高度（门为 0）
    struct Opening
    {
        // 类型：项目的门窗类型，或带族时族的类型
        std::string typeName;
        // 族的门窗：族的名称
        std::optional<std::string> family;
        Guid        host;
        double      offset = 0.0;
        double      sill   = 0.0;
        std::string mark;
        // 开向（门）或朝向（窗）翻到墙的另一侧
        bool        flipFacing = false;
        // 铰链在另一侧门框
        bool        flipHand   = false;
        // 它的族的实例参数值，按名称（BIMCraft 还在这里存项目参数的值，项目参数移植后再加）
        std::map<std::string, Json> params;
        bool operator==(const Opening&) const = default;
    };

    struct Door : Opening
    {
        bool operator==(const Door&) const = default;
    };

    struct Window : Opening
    {
        bool operator==(const Window&) const = default;
    };

    // 房间分隔线，同 Revit：在 level 上从 start 到 end。它像墙一样围合房间（开敞空间里的两个房间：起居室与走廊），
    // 此外什么都不是：没有实体、不剖切、没有自己的面积
    struct RoomSeparator
    {
        Guid level;
        P2   start;
        P2   end;
        bool operator==(const RoomSeparator&) const = default;
    };

    // 天花板：沿 boundary（逆时针），openings 处开洞（在里面、互不相交）；底面在标高上方 offset，上面是类型的厚度
    struct Ceiling
    {
        std::string       typeName;
        Guid              level;
        Ring              boundary;
        std::vector<Ring> openings;
        double            offset = 0.0;
        std::vector<SketchLine> sketch;
        bool operator==(const Ceiling&) const = default;
    };

    // 房间：point 是房间里的一点，边界由它四周的墙找出，所以墙动时跟着变
    struct Room
    {
        Guid        level;
        P2          point;
        std::string name;
        std::string number;
        bool operator==(const Room&) const = default;
    };

    // 构件：族 family 的类型 typeName，立在 level 上 at 处、高出 offset，转 angle 度（从东逆时针）；mirrored 时是镜像。
    // params 是它的族的实例参数值
    struct Component
    {
        std::string family;
        std::string typeName;
        Guid        level;
        P2          at;
        double      angle    = 0.0;
        double      offset   = 0.0;
        bool        mirrored = false;
        std::map<std::string, Json> params;
        bool operator==(const Component&) const = default;
    };

    // 柱：立在 level 上 at 处，宽度沿 angle 度（从东逆时针）：到 topLevel，或 height 毫米高；底部高出标高 baseOffset。
    // 建筑柱（Column）与结构柱（StructuralColumn）同样的数据，类别与类型各自分开
    struct Column
    {
        std::string         typeName;
        Guid                level;
        P2                  at;
        double              angle  = 0.0;
        std::optional<Guid> topLevel;
        double              height = 0.0;
        double              baseOffset = 0.0;
        bool operator==(const Column&) const = default;
    };

    struct StructuralColumn : Column
    {
        bool operator==(const StructuralColumn&) const = default;
    };

    // 独立基础：平放的柱，height 是它的厚度，类型是基础的
    struct Footing : Column
    {
        bool operator==(const Footing&) const = default;
    };

    // run 的高度在截面的哪条线上：顶面、中线或底面
    enum class Justify
    {
        Top,
        Centre,
        Bottom,
    };

    namespace JustifyOf
    {
        // 项目文件与 IFC 里的写法：top、centre、bottom
        const char* Name(Justify j);
        // 也认美式拼写 center；不分大小写
        std::optional<Justify> FromName(std::string_view name);
    }

    // 两点之间的直线元素（Model/Runs.h）：level 上从 start 到 end，justify 那条线在起点高出标高 offset、在终点 endOffset（没有时同 offset），
    // 截面是类型的；system 是它所属的系统（"Água fria"、回路）。梁、条形基础、管道、线管、桥架、导线、引下线、接地线、缆线
    // 都是同样的数据，类别与类型各自分开
    struct Run
    {
        std::string           typeName;
        Guid                  level;
        P2                    start;
        P2                    end;
        double                offset = 0.0;
        std::optional<double> endOffset;
        Justify               justify = Justify::Centre;
        std::string           system;
        bool operator==(const Run&) const = default;
    };

#define MINIBIM_RUN(Name)                                       struct Name : Run                                           {                                                               bool operator==(const Name&) const = default;           };
    MINIBIM_RUN(Beam)
    MINIBIM_RUN(StripFooting)
    MINIBIM_RUN(Pipe)
    MINIBIM_RUN(Conduit)
    MINIBIM_RUN(CableTray)
    MINIBIM_RUN(Wire)
    MINIBIM_RUN(DownConductor)
    MINIBIM_RUN(GroundConductor)
    MINIBIM_RUN(Cable)
#undef MINIBIM_RUN

    // 元素是 run 的类别，同 Element::AsRun
    inline constexpr std::array<const char*, 9> RunCategories = { "beam",     "stripfooting",  "pipe",            "conduit", "cabletray",
                                                                  "wire",     "downconductor", "groundconductor", "cable" };
    bool IsRunCategory(std::string_view category);

    // 拉伸屋顶的形状：底面在过 line 的竖直平面里的轮廓，各点为 [沿线距起点, 高于屋顶底部]，从线左边 from 到 to 毫米处垂直于线扫过
    struct Extrusion
    {
        std::array<P2, 2> line;
        std::vector<P2>   profile;
        double            from = 0.0;
        double            to   = 0.0;
        bool operator==(const Extrusion&) const = default;
    };

    // 屋顶：在 level 上方 offset。迹线屋顶沿 boundary（伸出 overhang），绕开 courtyards（其中的洞，有自己的檐口）：
    // slope 为 0 时平的，否则除 gables 外的每条边以 slope 度向上倾斜，坡面相交处为斜脊、天沟、屋脊（直骨架）；
    // gables 里的边（第 i 条从第 i 点到下一点，天井的边接着外轮廓编号）保持竖直即山墙，gable 让矩形的两条短边竖直。
    // 或者是拉伸屋顶 extrusion。openings 在平面上开洞；join 是它插进的屋顶，它在那个屋顶的顶面处收住
    struct Roof
    {
        std::string         typeName;
        Guid                level;
        Ring                boundary;
        std::vector<Ring>   courtyards;
        double              slope    = 0.0;
        double              overhang = 0.0;
        bool                gable    = false;
        std::vector<size_t> gables;
        double              offset   = 0.0;
        std::vector<Ring>   openings;
        std::optional<Guid> join;
        std::optional<Extrusion> extrusion;
        std::vector<SketchLine>  sketch;
        // 存的参数值：体量的记号（Massing::Face）
        std::map<std::string, Json> params;

        // 迹线的各条边，编号同 gables：外轮廓的，然后是每个天井的
        std::vector<Segment> Edges() const;
        bool operator==(const Roof&) const = default;
    };

    // 体量（Revit 的概念体量，移植自 BIMCraft model.rs 的 Mass）：footprint 从 level 上方 baseOffset 拉伸到上一层，或高 height，或到 topLevel；
    // 有 top（与 boundary 点点对应、同向的顶部轮廓）时在两者之间直线放样，斜的体量是顶部轮廓横移了的。平面、三维里虚的
    struct Mass
    {
        std::string                name;
        Guid                       level;
        Ring                       boundary;
        std::optional<Guid>        topLevel;
        double                     height     = 0.0;
        double                     baseOffset = 0.0;
        std::optional<std::vector<P2>> top;
        // 存的参数值（项目参数移植后用）
        std::map<std::string, Json> params;

        // 顶部轮廓不是底面轮廓：在两者之间放样
        bool Lofted() const;
        // 底面轮廓，不论 boundary 怎样转都是逆时针
        Ring Footprint() const;
        // 顶部轮廓，同 Footprint 逆时针、点点对应；直上直下时就是底面轮廓
        Ring TopRing() const;
        bool operator==(const Mass&) const = default;
    };

    // ── 注释（第 5d 步，移植自 BIMCraft model.rs、lines.rs、fills.rs）──────────────
    // MiniBIM 还没有平面视图元素（随第 5e 步），注释所在的视图是一个标高（它自己的平面）；BIMCraft 文件里在别的视图（平面视图、
    // 剖面、立面、图纸）里的注释读入时略去

    // 尺寸量到的线：墙的中心线（默认）、左面或右面（从起点往终点看）、起点或终点；门窗的中心或朝墙起点、终点的洞口边；轴网的线；
    // 直的详图线或它的起点、终点
    enum class Face
    {
        Centre,
        Left,
        Right,
        Start,
        End,
    };

    namespace FaceOf
    {
        // 文件与命令里的名称：centre、left、right、start、end
        const char* Name(Face f);
        // 报错里的说法：centre line、left face……
        const char* Said(Face f);
        std::optional<Face> FromName(std::string_view s);
        // 镜像过去的墙（反着走）上同一条线：起点与终点对调
        Face Mirrored(Face f);
        // 翻转的墙（沿同一条线反着走）上同一条线：左右对调，起点与终点对调
        Face Flipped(Face f);
    }

    struct Reference
    {
        Guid id;
        Face face = Face::Centre;
        bool operator==(const Reference&) const = default;
    };

    enum class DimKind
    {
        Aligned,
        Angular,
        Radial,
    };

    // 标高平面里的尺寸。对齐：平行的参照之间的距离，沿过 at 横穿它们的线；角度：相交的两条参照之间的角，在 at 那一侧，弧过 at；
    // 径向：圆的东西的半径，线朝 at。它量的是模型现在的样子，所以跟着它量的东西走
    struct Dimension
    {
        Guid                   level;
        std::optional<Guid>    view;      // 不是标高自己的平面时的平面视图（MiniBIM 还没有，读入时略去）
        DimKind                kind = DimKind::Aligned;
        std::vector<Reference> refs;
        P2                     at;
        bool operator==(const Dimension&) const = default;
    };

    // 文字注释：at 是第一行的左上角，按文字类型写，逆时针转 angle 度，有 leader 时引一条线到那里。override_format 时各段 runs
    // 改成粗、斜、下划线或不是
    struct Note
    {
        Guid                 view;
        P2                   at;
        std::string          text;
        std::string          typeName;
        double               angle = 0.0;
        std::optional<P2>    leader;
        bool                 overrideFormat = false;
        std::vector<TextRun> runs;
        bool operator==(const Note&) const = default;
    };

    // 高程点：标高平面里读 host 在 at 处的顶面高度
    struct Spot
    {
        Guid                view;
        P2                  at;
        std::optional<Guid> host;
        bool operator==(const Spot&) const = default;
    };

    // 标记：在 host 所在标高的平面上写它的一个字段（label；没有时为类别平常的那个，Tag::Usual），离它的点 offset 毫米，跟着它走
    struct Tag
    {
        Guid                       host;
        std::optional<Guid>        view;
        P2                         offset;
        bool                       leader = false;
        std::optional<std::string> label;

        // 类别 category 的标记平常写的字段：门窗的标记、房间的名称、别的为类型
        static const char* Usual(std::string_view category);
        bool operator==(const Tag&) const = default;
    };

    // 详图线：只画在一个视图里的线，按线样式 style 画。insulation（保温层宽）、cloud（云线弧长）与 revision 记着、写回文件，
    // 画成普通的线（随后面的阶段移植）；centreMark 在圆弧、圆的圆心画小十字
    struct DetailLine
    {
        Guid                       view;
        Curve                      curve;
        std::string                style;
        std::optional<double>      insulation;
        bool                       centreMark = false;
        std::optional<double>      cloud;
        std::optional<std::string> revision;
        bool operator==(const DetailLine&) const = default;
    };

    // 填充区域：草图围出的环（洞与岛按草图的嵌套）填上类型的图案，边界按线样式 lineStyle 画（没有时不画）
    struct FilledRegion
    {
        Guid                       view;
        std::string                typeName;
        std::optional<std::string> lineStyle;
        std::vector<SketchLine>    sketch;
        // 模型图案从哪里起、转多少度
        P2                         patternOrigin;
        double                     patternAngle = 0.0;
        bool operator==(const FilledRegion&) const = default;
    };

    // 坡屋顶迹线最多的点数：找坡面相交处费时间
    inline constexpr size_t MaxRoofPoints = 256;
    // 天花板默认在标高以上的高度
    inline constexpr double CeilingHeight = 2600.0;

    // 屋顶的一个坡面：平面上围着它的各圈（外圈逆时针，然后是其中的洞，顺时针），以及它的底面所在的平面
    using Slope = std::pair<std::vector<Ring>, Plane>;

    // 一个元素。文件里写作带 "category" 的对象：{"category": "wall", …}
    struct Element
    {
        using Variant = std::variant<Level, Grid, RoomSeparator, Wall, Floor, Ceiling, Door, Window, Room, Roof, Component, Column, StructuralColumn,
                                     Footing, Beam, StripFooting, Pipe, Conduit, CableTray, Wire, DownConductor, GroundConductor, Cable, Mass,
                                     Dimension, Note, Spot, Tag, DetailLine, FilledRegion>;
        Variant v;

        Element() = default;
        template <class T, class = std::enable_if_t<!std::is_same_v<std::decay_t<T>, Element> && std::is_constructible_v<Variant, T&&>>>
        Element(T&& x) : v(std::forward<T>(x)) {}

        template <class T> const T* As() const { return std::get_if<T>(&v); }
        template <class T> T*       As()       { return std::get_if<T>(&v); }
        // 门或窗
        const Opening* AsOpening() const;
        Opening*       AsOpening();
        // 建筑柱、结构柱或独立基础
        const Column* AsColumn() const;
        Column*       AsColumn();
        // 梁、条形基础、管道……不论类别（RunCategories）
        const Run* AsRun() const;
        Run*       AsRun();
        // 类别 category 的 run 成为元素；不是 run 的类别时为空
        static std::optional<Element> OfRun(std::string_view category, Run r);
        // 尺寸、文字、高程点、标记、详图线、填充区域
        bool IsAnnotation() const;
        // 楼板、天花板、屋顶、填充区域记着的草图；别的元素没有
        const std::vector<SketchLine>* SketchOf() const;
        std::vector<SketchLine>*       SketchOf();
        bool IsDoor() const   { return std::holds_alternative<Door>(v); }
        bool IsWindow() const { return std::holds_alternative<Window>(v); }

        // 类别：level、grid、roomseparator、wall、floor、ceiling、door、window、room、roof、component、column、structuralcolumn、footing，
        // 以及 RunCategories
        const char* Category() const;
        // 它的族与类型：构件的，或带族的门窗的
        std::optional<std::pair<std::string, std::string>> FamilyOf() const;
        std::string* FamilyMut();
        // 门窗立在墙里时要的族类别
        std::optional<Fam::Category> HostedCategory() const;
        // 它存的参数值：构件与门窗的族参数，墙、楼板、屋顶、体量的体量记号
        const std::map<std::string, Json>* Params() const;
        std::map<std::string, Json>*       ParamsMut();

        // 它指向的每个元素（GUID）交给 f，f 可以换成别的：标高、宿主、贴着的屋顶或楼板、插进的屋顶
        void RefsMut(const std::function<void(Guid&)>& f);
        std::vector<Guid> Refs() const;
        // 它用到的每个库条目名称交给 f（是什么、名称），f 可以换成别的：类型
        void LibRefsMut(const std::function<void(const Kind&, std::string&)>& f);

        bool operator==(const Element&) const = default;
    };

    // 墙类型：墙多厚；复合类型还有从外侧往里的各层，加起来等于厚度。核心层从第一个结构层到最后一个（没有结构层时为全部）
    struct WallType
    {
        double             thickness = 0.0;
        std::vector<Layer> layers;
        bool operator==(const WallType&) const = default;
    };

    // 楼板类型：板多厚；复合类型还有从上往下的各层
    struct FloorType
    {
        double             thickness = 0.0;
        std::vector<Layer> layers;
        bool operator==(const FloorType&) const = default;
    };

    // 门窗扇怎么开，名称同 IFC 的门操作类型；铰链（或推拉、折叠的方向）在哪一侧门框由门窗的 flipHand 决定
    enum class Operation
    {
        Swing,          // 一扇，铰链在一侧门框
        DoubleSwing,    // 两扇，各自铰在两侧门框，在中间相遇
        Sliding,        // 一扇沿墙推拉
        DoubleSliding,  // 两扇从中间向两边推拉
        Folding,        // 向一侧门框折叠
        Fixed,          // 不开
    };

    namespace OperationOf
    {
        inline constexpr std::array<Operation, 6> All = { Operation::Swing, Operation::DoubleSwing, Operation::Sliding,
                                                          Operation::DoubleSliding, Operation::Folding, Operation::Fixed };
        // 文件与命令里的写法：swing、double-swing……
        const char* Name(Operation o);
        // 英文名称，明细表的表头用
        const char* Label(Operation o);
        std::optional<Operation> Parse(std::string_view name);
        // 门的 IfcDoorTypeOperationEnum，right 时门扇在右；没说怎么开的门是平开门，同 BIMCraft 的门
        const char* IfcDoor(std::optional<Operation> op, bool right);
        // 门的开启方式与门扇是否在右：从 IfcDoorTypeOperationEnum（IFC4）或 IfcDoorStyleOperationEnum（IFC2x3）读，带不带点都行
        std::optional<std::pair<Operation, bool>> FromIfcDoor(std::string_view e);
        // 窗的 IfcWindowTypePartitioningEnum：两扇相遇的是左右两块，否则一块
        const char* IfcWindowPartitioning(std::optional<Operation> op);
    }

    // 门窗类型。sill 是窗的默认窗台高度；operation 是开启方式（给出时）
    struct OpeningType
    {
        double width  = 0.0;
        double height = 0.0;
        double sill   = 0.0;
        std::optional<Operation> operation;
        bool operator==(const OpeningType&) const = default;
    };

    // 屋顶类型：屋顶垂直于坡面量多厚；复合类型还有从上往下的各层
    struct RoofType
    {
        double             thickness = 0.0;
        std::vector<Layer> layers;
        bool operator==(const RoofType&) const = default;
    };

    // 天花板类型：多厚；有 grid 时是这么宽的方形板块，反射天花板平面画出它们；复合类型还有从上往下的各层
    struct CeilingType
    {
        double             thickness = 0.0;
        double             grid      = 0.0;
        std::vector<Layer> layers;
        bool operator==(const CeilingType&) const = default;
    };

    // 柱类型：width 乘 depth 的矩形，或 round 时直径 width 的圆
    struct ColumnType
    {
        double width = 0.0;
        double depth = 0.0;
        bool   round = false;
        bool operator==(const ColumnType&) const = default;
    };

    // run 的类型：截面宽 width、高 depth，或 round 时直径 width 的圆；用 material 做（库里的一个，或者没有）
    struct RunType
    {
        double      width = 0.0;
        double      depth = 0.0;
        bool        round = false;
        std::string material;
        bool operator==(const RunType&) const = default;
    };

    // 项目的类型，按类别、名称。BIMCraft 还有栏杆的类型，移植到时再加
    struct Types
    {
        std::map<std::string, WallType>    wall;
        std::map<std::string, FloorType>   floor;
        std::map<std::string, OpeningType> door;
        std::map<std::string, OpeningType> window;
        std::map<std::string, ColumnType>  column;
        std::map<std::string, RoofType>    roof;
        std::map<std::string, CeilingType> ceiling;
        std::map<std::string, ColumnType>  structuralColumn;
        std::map<std::string, ColumnType>  footing;
        // run 的类型，按类别、名称：runs["beam"]["Beam 200 x 500"]
        std::map<std::string, std::map<std::string, RunType>> runs;
        bool operator==(const Types&) const = default;
    };

    // 项目在哪里量房间的面积与周长（Revit 的"面积和体积计算"）
    enum class RoomArea
    {
        Centre,     // 墙中心线
        Finish,     // 墙面层面：墙以内的面积
    };

    // 平面视图的视图范围换算成高度：顶、剖切面、底、视图深度
    struct PlanRange
    {
        double top    = 0.0;
        double cut    = 0.0;
        double bottom = 0.0;
        double depth  = 0.0;
    };

    // `walls` → `wall`：类别不分大小写、单复数
    std::string NormalizeCategory(std::string_view s);

    struct Model
    {
        // 文件版本：写出时为 FileFormat；0 为还没有 GUID 的旧文件
        uint32_t    format = FileFormat;
        std::string name;
        // IFC GlobalId 的种子：同一模型每次导出 IFC 都保持不变
        uint64_t    uid = 0;
        // 长度、面积怎么显示和输入；模型本身用毫米
        Units       units    = Units::Millimetres;
        // 图纸上的字与命令起的名称用的语言
        Language    language = Language::English;
        RoomArea    roomArea = RoomArea::Centre;
        Id          nextId   = FirstId;
        Types       types;
        // 各层用的材质，按名称
        std::map<std::string, Material> materials;
        // 族，按名称：构件放置的东西
        std::map<std::string, Family> families;
        // 注释的库条目（Model/Styles.h），按名称：线型图案、线样式、填充图案、填充区域类型、文字类型
        std::map<std::string, LinePattern> linePatterns = Lines::Patterns(Language::English);
        std::map<std::string, LineStyle>   lineStyles   = Lines::Styles(Language::English);
        std::map<std::string, FillPattern> fillPatterns = Fills::Patterns(Language::English);
        std::map<std::string, RegionType>  regionTypes  = Fills::RegionTypes(Language::English);
        std::map<std::string, TextType>    textTypes    = TextTypes::Defaults(Language::English);
        Elements    elements;
        // 库的 ID 和 GUID：类型、材质
        Library     library;

        // 空项目：两个标高，几种起步用的类型与材质
        static Model New(std::string_view name, Language language = Language::English);

        // ── 元素 ────────────────────────────────────────────────
        // 放进一个新元素，返回它的 ID
        Id   Insert(Element e);
        // 取下一个 ID：比每个元素的都大（手改过的文件里 next_id 可能过时）
        Id   FreshId();
        // 用 ID 指向元素的地方（命令给的引用，或旧文件里的）换成它的 GUID。没有元素的 ID 原样留着，由 Check 报出
        void Resolve();
        // 迁移到 FileFormat：旧文件（还没有 GUID）读入时每个元素已给了 GUID，指向元素的 ID 换成 GUID，ID 平移到六位数
        void Migrate();

        // 报错里怎么称呼元素：它的 ID；项目里没有时为给出的东西本身
        std::string Shown(Guid k) const;
        std::string Shown(Id k) const;
        // a、b 是项目里的同一个元素
        bool Same(Guid a, Guid b) const;
        // 元素的 GUID；没有时为 nil（不是任何元素的）
        Guid GuidOf(Id k) const;
        Guid GuidOf(Guid k) const;

        const Level* GetLevel(Guid k) const;
        const Level* GetLevel(Id k) const;
        const Wall*  GetWall(Guid k) const;
        const Wall*  GetWall(Id k) const;
        // 标高，从低到高
        std::vector<std::pair<Id, const Level*>> Levels() const;
        double Elevation(Guid level) const;
        // level 之上的第一个标高
        std::optional<Guid> LevelAbove(Guid level) const;

        // ── 墙 ──────────────────────────────────────────────────
        double Thickness(const Wall& w) const;
        // 按标高与偏移的底部（不管贴着什么）：门窗的窗台从这里量
        double OwnBase(const Wall& w) const;
        // 按顶部约束的顶部（不管贴着什么）
        double OwnTop(const Wall& w) const;
        // 最低的底部 / 最高的顶部：贴着的东西决定，或按标高与约束
        double BaseZ(const Wall& w) const;
        double TopZ(const Wall& w) const;
        // level 的平面视图剖切到这面墙：墙跨过剖切面
        bool   CutBy(const Wall& w, Guid level) const;
        // 墙围合 level 上的房间：跨过标高以上 CutHeight（房间在这个高度找墙）
        bool   Bounding(const Wall& w, Guid level) const;
        bool   Spans(const Wall& w, double z) const;
        // level 平面视图的视图范围（高度）
        PlanRange GetPlanRange(Guid level) const;

        // ── 类型 ────────────────────────────────────────────────
        std::vector<std::string> TypeNames(std::string_view category) const;
        // 没指定类型时新元素用的：新模型起步就有的那个，否则按名称第一个
        std::optional<std::string> DefaultType(std::string_view category) const;
        // 门窗的尺寸与默认窗台：它的类型的；没有时返回 false，原因放在 error
        bool OpeningDims(const Element& e, OpeningType& out, std::string& error) const;
        std::optional<OpeningType> OpeningTypeOf(const Element& e) const;
        // 门窗所在的那一段墙，沿墙从起点量：它中点两边最近的、与它的墙在墙所在标高的平面上相交（穿过或从任一边连上）的墙的面，
        // 没有时到墙的起点、终点。临时尺寸量到这里（同 Revit 量到墙面）。弧墙上没有
        std::optional<std::pair<double, double>> OpeningSpan(const Opening& o) const;
        // 门窗离那一段的中点不超过 near 毫米时，中点的位置（沿墙）：放门窗时吸到中间
        std::optional<double> Centred(const Opening& o, double near) const;
        // 带层的类型：类别、名称、厚度、各层
        struct Layered
        {
            const char*               category;
            const std::string*        name;
            double                    thickness;
            const std::vector<Layer>* layers;
        };
        std::vector<Layered> LayeredTypes() const;

        // 元素所在的标高
        std::optional<Guid> LevelOf(const Element& e) const;

        // ── 柱 ──────────────────────────────────────────────────
        // 从底到顶的高度：标高加 baseOffset，到 topLevel 或高出 height
        std::pair<double, double> Span(Guid level, double baseOffset, const std::optional<Guid>& topLevel, double height) const;
        // 体量从下到上（Span）；高 z 的水平面穿过它吗；z 处的轮廓（放样的在两个轮廓之间）
        std::pair<double, double> MassSpan(const Mass& ms) const;
        bool MassCutBy(const Mass& ms, double z) const;
        Ring MassOutline(const Mass& ms, double z) const;
        // from 到 to 之间的体积（立方毫米）：截面积是高度的二次式，辛普森公式是精确的
        double MassVolume(const Mass& ms, double from, double to) const;
        // 它穿过的标高，从低到高：从底到顶（不含顶）；各带高程与到下一个（或顶）的高度
        struct MassSlice
        {
            Id     level;
            double z;
            double height;
        };
        std::vector<MassSlice> MassSlices(const Mass& ms) const;
        std::pair<double, double> ColumnSpan(const Column& c) const;
        // 柱的类型（建筑柱、结构柱、独立基础各自的类型表里）；不是柱或没有这个类型时为空
        const ColumnType* ColumnTypeOf(const Element& e) const;
        // 柱的平面轮廓，逆时针：矩形的四角，或圆柱的 24 边形；不是柱或没有类型时为空
        Ring ColumnOutline(const Element& e) const;
        // ── 梁与其他 run ────────────────────────────────────────
        // run 的类型；不是 run 或没有这个类型时为空
        const RunType* RunTypeOf(const Element& e) const;
        // 算好的 run：两端与截面。别的元素或没有这个类型时为空
        std::optional<Runs::Geom> RunGeom(const Element& e) const;

        // 类型的层用到、库里还没有的材质放进库：旧项目或别处来的文件打开时
        void AdoptMaterials();

        // ── 楼板 ────────────────────────────────────────────────
        Plane  FloorTop(const Floor& f) const;
        // 楼板底面在顶面以下多深（铅垂）：类型的厚度，垂直于坡面量
        double FloorDepth(const Floor& f) const;
        // 楼板的板块：底面减去洞口（被洞切开时为凸块），各是空间里逆时针的平面多边形；以及顶面在它上面多高
        std::vector<std::pair<std::vector<P3>, double>> FloorSlabs(const Floor& f) const;
        // 天花板的板块：底面减去洞口（被洞切开时为凸块），各自逆时针；以及它的厚度
        std::vector<std::pair<std::vector<P3>, double>> CeilingSlabs(const Ceiling& c) const;

        // ── 族与构件 ────────────────────────────────────────────
        // 构件在它自己坐标里的实体（是镜像时已镜像）：各可见形体的三角形、从上看的轮廓、从底到顶、用什么做；建不出来时为空
        std::vector<Fam::Piece> ComponentLocalPieces(const Component& c) const;
        // 构件在模型里的实体：转到、移到它所在的地方
        std::vector<Fam::Piece> ComponentPieces(const Component& c) const;
        // 放置族的界面给新门窗的族与类型：项目的单扇门或固定窗（常用尺寸），否则该类别的第一个族与它的第一个类型
        std::optional<std::pair<std::string, std::string>> DefaultFamily(std::string_view category) const;
        // 门窗族的坐标在哪里：洞口中间、墙中间的平面点，墙在那里的走向，窗台的高度
        std::optional<std::tuple<P2, P2, double>> OpeningFrame(const Opening& o) const;
        // 门窗族的实体与平面线，在族自己的坐标里，按门窗的翻转镜像过；不是族的或建不出来时为空
        std::optional<std::pair<std::vector<Fam::Piece>, std::vector<Ring>>> OpeningLocal(const Element& e) const;
        // 门窗族在它所在处的实体，在模型坐标里
        std::vector<Fam::Piece> OpeningPieces(const Element& e) const;
        // 门窗族在平面上画的线，在它所在处
        std::vector<Ring> OpeningStrokes(const Element& e) const;
        // 用到材质 name 的（类型的层以外）：族，或族的构件；没有时为空
        std::optional<std::string> MaterialUser(const std::string& name) const;
        // 族与它们的构件里的材质 from 改名为 to
        void RenameMaterialInFamilies(const std::string& from, const std::string& to);

        // ── 屋顶 ────────────────────────────────────────────────
        // 屋顶的平面轮廓：迹线，或拉伸扫过的矩形，逆时针
        Ring   RoofOutline(const Roof& r) const;
        // 底面在 plane 上时顶面高出多少：类型的厚度，铅垂
        double RoofRise(const Roof& r, const Plane& plane) const;
        // 屋顶的坡面。迹线上：在底部平的，或者除山墙外每条边从檐口向上直到坡面相交（迹线的直骨架），天井四周也是；
        // 或者拉伸轮廓的每一段扫过去。插进别的屋顶时在那个屋顶的顶面处收住
        bool   RoofFaces(const Roof& r, std::vector<Slope>& out, std::string& error) const;
        // 迹线哪些边竖直（山墙），编号同 gables：gables 里的，以及 gable 时矩形的两条短边
        bool   RoofUprights(const Roof& r, std::vector<bool>& out, std::string& error) const;
        // 屋顶的板块：每块底面在空间里的多边形，以及顶面高出它多少
        std::vector<std::pair<std::vector<P3>, double>> RoofSlabs(const Roof& r) const;
        // 屋顶的坡面减去其中的洞与开洞，被洞切开处为凸块
        bool   RoofPieces(const Roof& r, std::vector<std::pair<Ring, Plane>>& out, std::string& error) const;

        // ── 房间找墙用的线 ──────────────────────────────────────
        // 围合 level 上房间的墙的中心线（圆弧为弦）
        std::vector<Segment> WallLines(Guid level) const;
        // 围合 level 上房间的：墙的中心线与房间分隔线
        std::vector<Segment> BoundingLines(Guid level) const;
        // 围合 level 上房间的墙面（墙轮廓的各条边）。不含房间分隔线：在墙内拾取的楼板、天花板越过它们
        std::vector<Segment> FaceLines(Guid level) const;
        // level 上的房间分隔线
        std::vector<Segment> Separators(Guid level) const;
        // 在 at 处量时，level 上的房间在其中找边界的线
        std::vector<Segment> RoomLines(Guid level, RoomArea at) const;
        // 房间的边界，由它的点四周的墙找出，在项目量房间的地方（roomArea）；没围住时为空
        std::optional<Geom::Region> RoomRegion(const Room& r) const;
        // 在 at 处量的房间边界：墙中心线（Revit 的"在墙中心"），或墙面（"在墙面层"：墙以内的面积）
        std::optional<Geom::Region> RoomRegionAt(const Room& r, RoomArea at) const;
        // 在墙面量（at 为 Finish）时房间的点落在其墙体里的围合墙：那时墙的各个面把墙体自己围成一圈，墙里的点会把墙当成房间。
        // 在中心线量时没有东西围住墙体，没什么要拒绝的
        std::optional<Id> WallHolding(const Room& r, RoomArea at) const;

        // ── 注释（Annotate.cpp）────────────────────────────────
        // 注释在的平面（标高）；不是注释时为空
        std::optional<Guid> AnnotationPlan(const Element& e) const;
        // level 的平面画注释 e
        bool AnnotationIn(const Element& e, Guid level) const;
        // 元素在平面上的点（标记指着它）：门窗在墙中线上的中心、墙的中点、柱与构件的插入点、房间的点、run 的中点、楼板等轮廓的平均点
        std::optional<P2> Anchor(Guid id) const;
        // 门窗 host 的新标记离它中心多远：墙外 400 毫米，在门不开向的一侧（窗朝外的一侧）；别的东西右上 400、400
        P2 TagOffset(Guid host) const;
        // 标记写什么：它宿主的那个字段；还没有值时为 ?
        std::string TagText(const Tag& t) const;
        // 类别 category 的标记能写的字段（随明细表移植前：类型、族、标高、标记、名称、编号、面积……）
        std::vector<std::string> TagFields(std::string_view category) const;
        // 元素 host 的字段 field 的值（显示用，项目单位）；没有这个字段时为空
        std::optional<std::string> FieldText(Guid host, const std::string& field) const;
        // 尺寸量到的线，或为什么没有。直墙的线伸到它的轮廓那么远，端头、门窗的中心与洞口边从墙面横到墙面
        bool ReferenceLine(const Reference& r, Segment& out, std::string& error) const;
        // 元素 id 上尺寸能量到的线：墙的中心线、两面、两端，门窗的中心与两边，轴网的线，直的详图线与两端
        std::vector<std::pair<Reference, Segment>> ReferenceLines(Guid id) const;
        // id 的参照线里 keep 认的、离 p 最近的那条与距离
        std::optional<std::pair<Reference, double>> ReferenceAt(Guid id, P2 p, const std::function<bool(const Segment&)>& keep) const;
        // 径向尺寸量的圆：圆柱的圆心与半径，弧墙的（中心线或一面），圆或圆弧的详图线
        bool ReferenceArc(const Reference& r, P2& centre, double& radius, std::string& error) const;
        // 角度尺寸的弧：两条线的交点、起始射线的角（弧度，从正东逆时针）、逆时针扫到另一条的角：四个角里含 at 的那个
        std::optional<std::tuple<P2, double, double>> AngleSector(const Dimension& d) const;
        // 对齐尺寸线的方向（与第一条参照垂直：朝东，或南北走向时朝北），以及各参照与它的交点：离 at 多远、参照的线，按这个方向排好
        std::pair<P2, std::vector<std::pair<double, Segment>>> Crossings(const Dimension& d) const;
        // 尺寸显示的数：对齐的各段长度（从西到东或从南到北）、角度的度数、径向的半径
        std::vector<double> DimensionValues(const Dimension& d) const;
        // level 的平面里 p 处的高程点读谁：站在这个标高上的东西在那里最高的顶面（天花板除外）
        std::optional<Id> SpotHost(Guid level, P2 p) const;
        // 高程点读的高度：宿主在它的点处的顶面；宿主不在那里时为空
        std::optional<double> SpotValue(const Spot& sp) const;
        // 元素 id 在 p 处的顶面高度（p 不在它上面时为空）：楼板、屋顶、墙、柱、基础、run、构件
        std::optional<double> TopAt(Id id, P2 p) const;

        // ── 检查 ────────────────────────────────────────────────
        // 每条命令之后检查整个模型；不通过时命令回滚。error 说明要改什么，关于元素的以"类别 ID: "开头
        bool Check(std::string& error) const;

        // ── 库（Library.cpp）────────────────────────────────────
        // 库现在有的每个条目：是什么、名称
        std::vector<std::pair<Kind, std::string>> LibraryNames() const;
        // 库的 ID 和 GUID 与库里的东西对得上：不缺、不多；每个部件有自己的、别的东西没有的 ID 和 GUID
        bool LibraryInStep() const;
        // 库的每个部件，带它存着的 ID 和 GUID，按顺序
        std::vector<std::pair<Id, Part>> PartsNow() const;
        // 库里没有 ID 和 GUID 的东西给上，不再有的去掉；改名保留（Library::Renamed）；没有或与别人重复的部件给新的
        void SyncLibrary();
        // 新条目、新部件的 GUID：由模型、是什么（部件为 nullptr）与名称得出
        using GuidMaker = std::function<Guid(const Model&, const Kind*, const std::string&)>;
        // 同 SyncLibrary，新条目与新部件的 GUID 由 guid 给出（与已有的重复时另给一个）：IFC 导入每次读出一样的 GUID
        void SyncLibraryWith(const GuidMaker& guid);
        // 去掉每个部件的 ID 和 GUID，由 SyncLibraryWith 重新给
        void ForgetParts();
        // 库的下一个 ID：从 LastId 往下数，元素从 FirstId 往上数；两者相遇时库接着元素往上
        Id   FreshLibraryId();
        // 模型里每个库条目名称交给 f：元素的、类型的层的材质
        void LibRefsMut(const std::function<void(const Kind&, std::string&)>& f);
        // text 在 kind 里指的名称：它的名称或 GUID
        std::optional<std::string> LibraryName(const Kind& kind, std::string_view text) const;

        // ── 文件（ModelJson.cpp）────────────────────────────────
        // 项目文件的内容：库条目的名称一律写 GUID
        bool ToFile(std::string& out, std::string& error) const;
        Json ToJson() const;
        // 读项目文件：本版本不认识的内容（BIMCraft 新加的字段、还没移植的元素与设置、写错的键）略去，
        // 略去的位置以 JSON 指针列在 leftOut（/elements/100004/finish）。版本更新的文件整个拒绝
        static bool Read(std::string_view text, Model& out, std::vector<std::string>& leftOut, std::string& error);

    private:
        bool CheckIds(std::string& error) const;
        // 注释的库条目（Annotate.cpp）与一个注释元素
        bool CheckStyles(std::string& error) const;
        bool CheckAnnotation(const std::string& who, const Element& e, std::string& error) const;
        // 族 f 的元素给的值是它的实例参数的：是实例参数、不由公式算出、材质在库里
        bool FamilyValuesOk(const std::string& who, const std::string& name, const Family& f, const std::map<std::string, Json>& params,
                            std::string& error) const;
        // 构件 e 自己在材质参数里写的材质
        std::vector<std::string> ComponentMaterials(const Element& e) const;
        // 构件和带族门窗的族实例参数值按 key 对 "族: 参数" 换的键（文件里是 GUID）；材质参数的值按 material 换
        void RekeyFamilyParams(const std::function<std::optional<std::string>(const std::string&, const std::string&)>& key,
                               const std::function<std::optional<std::string>(const std::string&)>& material);
        bool PartsInStep() const;
        void SyncParts(const GuidMaker& make);
        // 文件里的 GUID 换回名称，没有 ID 和 GUID 的给上：ToFile 的反向
        void NamesFromFile();
        friend struct ModelJsonAccess;
    };

    // 新模型的 uid：时钟与进程号混合
    uint64_t FreshUid();
    // p 到微米：本应精确的点去掉浮点误差（不写出 -0）
    P2 Micron(P2 p);
    // Model::Check 的报错说的是哪个元素：关于元素的报错以它的类别与 ID 开头，`door 12: …`。导入用它略去违反规则的元素
    std::optional<Id> Culprit(std::string_view error);
}
