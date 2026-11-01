// 移植自 BIMCraft crates/core/src/dxf.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Geom.h"
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// DXF，照公开的 DXF 参考读：图层与模型空间的图元，各在图纸自己的单位里（文件知道时由 $INSUNITS 说是什么单位）。
// 平面画不了原样的（多段线的凸度、椭圆、样条）展平成曲线上的点；块放在每个 INSERT 放的地方，阵列与嵌套也算，有上限；
// 图纸空间、二进制文件与不画的图元略去并列出。CadImport 元素放读到的东西；这里不认识元素
namespace MiniBIM::Bim::Dxf
{
    // 文件多大之前读，字节
    inline constexpr size_t MaxBytes = size_t(128) << 20;
    // 放好块之后图纸最多几个形状
    inline constexpr size_t MaxShapes = 1000000;
    // 块里的块最深几层
    inline constexpr size_t MaxDepth = 16;
    // 文件的坐标离原点最远多少，它自己的单位
    inline constexpr double MaxFileCoord = 1e12;

    struct Rgb
    {
        uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb&) const = default;
    };

    // 图元或图层说的颜色：标准的索引色、真彩色，或随层、随块
    struct Color
    {
        enum class Kind
        {
            ByLayer,
            ByBlock,
            Index,
            True,
        };
        Kind    kind  = Kind::ByLayer;
        uint8_t index = 0;
        Rgb     rgb;

        static Color Indexed(uint8_t i) { return Color{ Kind::Index, i, {} }; }
        static Color TrueColor(uint8_t r, uint8_t g, uint8_t b) { return Color{ Kind::True, 0, { r, g, b } }; }
        // 颜色本身：图纸的墨色（7 号，白纸上的黑）与随层、随块的没有
        std::optional<Rgb> ToRgb() const;
        bool operator==(const Color&) const = default;
    };

    // 标准索引色：九个有名称的颜色，再是 24 个色相各五档深浅（各一深一浅），再是六个灰。7 号是图纸的墨色，这里没有；0 随块、256 随层，都不是索引
    std::optional<Rgb> Aci(uint8_t i);

    // 图纸的图层，照它的 LAYER 表
    struct Layer
    {
        std::string name;
        // 它的颜色：随层的图元取它
        Color       color = Color::Indexed(7);
        // 线型的名称（文件没说时为 Continuous）
        std::string linetype = "Continuous";
        // 线宽，毫米；没有时为默认
        std::optional<double> weight;
        // 关闭：它的图元不显示
        bool off    = false;
        // 冻结：不显示也不重生成
        bool frozen = false;
        bool operator==(const Layer&) const = default;
    };

    // 线型：图纸单位的划与空（空为负，点为 0），再乘图纸的 ltscale
    struct Linetype
    {
        std::string         name;
        std::vector<double> dashes;
        bool operator==(const Linetype&) const = default;
    };

    enum class Horizontal
    {
        Left,
        Center,
        Right,
    };

    enum class Vertical
    {
        Baseline,
        Bottom,
        Middle,
        Top,
    };

    // 文字的点在它的哪里
    struct Align
    {
        Horizontal h = Horizontal::Left;
        Vertical   v = Vertical::Baseline;
        bool operator==(const Align&) const = default;
    };

    // 图元画的东西，图纸单位
    struct Line
    {
        P2 a, b;
        bool operator==(const Line&) const = default;
    };
    struct Polyline
    {
        std::vector<P2> pts;
        bool            closed = false;
        bool operator==(const Polyline&) const = default;
    };
    struct Circle
    {
        P2     c;
        double r = 0.0;
        bool operator==(const Circle&) const = default;
    };
    // 从 start 逆时针到 end，度
    struct Arc
    {
        P2     c;
        double r = 0.0, start = 0.0, end = 0.0;
        bool operator==(const Arc&) const = default;
    };
    struct Point
    {
        P2 p;
        bool operator==(const Point&) const = default;
    };
    // 一行字：height 是大写字母的高，angle 是基线从正东逆时针的度数
    struct Text
    {
        P2          at;
        double      height = 0.0, angle = 0.0;
        std::string text;
        Align       align;
        bool operator==(const Text&) const = default;
    };
    // 填充的边界，或二维实体：solid 时填满，否则是图案（视图尽量画）
    struct Fill
    {
        std::vector<std::vector<P2>> rings;
        bool                         solid = false;
        bool operator==(const Fill&) const = default;
    };
    using Shape = std::variant<Line, Polyline, Circle, Arc, Point, Text, Fill>;

    // 形状外面的框，图纸单位：圆是整个圆，圆弧是扫过的，文字是它的点
    std::array<P2, 2> Bounds(const Shape& s);
    // 这几个框外面的框
    std::optional<std::array<P2, 2>> Union(const std::vector<std::array<P2, 2>>& boxes);
    // 从 start 起扫 sweep 度（正为逆时针）的圆弧上的点，两端都在
    std::vector<P2> ArcPoints(P2 c, double r, double start, double sweep);

    // 模型空间的图元，放好了：图层、怎样画、形状
    struct Entity
    {
        std::string                layer;
        Color                      color;
        // 它自己的线型名称；没有时取图层的
        std::optional<std::string> linetype;
        // 它自己的线宽，毫米
        std::optional<double>      weight;
        Shape                      shape;
        bool operator==(const Entity&) const = default;
    };

    // 读到的 DXF 文件：它画的东西，在它自己的单位里
    struct Drawing
    {
        // 毫米到图纸单位，来自 $INSUNITS；文件没有单位、或单位不是盖房子用的时为空，由用户说
        std::optional<double> scale;
        // 单位的名称（mm、m、in、ft……）或 unitless
        std::string unit;
        // 文件说的 $ACADVER，或空
        std::string version;
        // LAYER 表，以及图元提到而表里没有的图层（默认的样子）
        std::vector<Layer>    layers;
        std::vector<Linetype> linetypes;
        // $LTSCALE：每个线型的划都乘它
        double ltscale = 1.0;
        std::vector<Entity> entities;
        // 文件说的 $EXTMIN 与 $EXTMAX
        std::optional<std::array<P2, 2>> extents;
        // 略去了什么，各几个：不画的图元种类、图纸空间、嵌套太深或没定义的块、没有大小的图元
        std::map<std::string, size_t> leftOut;

        const Layer* LayerOf(std::string_view name) const;
        // 画的一切外面的框，图纸单位
        std::optional<std::array<P2, 2>> Bounds() const;
        bool operator==(const Drawing&) const = default;
    };

    // DXF 文件里的图纸；读不了时返回 false，原因放在 error
    bool Read(std::string_view bytes, Drawing& out, std::string& error);
    // Read 读文字（已是 UTF-8）
    bool ReadText(std::string_view text, Drawing& out, std::string& error);
}
