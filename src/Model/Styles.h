// 移植自 BIMCraft crates/core/src/lines.rs、fills.rs、text.rs（f3bbb33）的数据部分，MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Geom.h"
#include "Model/Language.h"
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// 注释用的库条目（第 5d 步）：线型图案与线样式（详图线画的样子）、填充图案与填充区域类型、文字类型。
// 它们与类型、材质一样是项目的库条目，元素按名称引用，文件里写 GUID。管理它们的命令（line_style、fill_pattern……）随第 5f 步移植；
// 这里有数据、检查、起步的那几个，以及画法要的东西
namespace MiniBIM::Bim
{
    using Rgb3 = std::array<uint8_t, 3>;

    // ── 线型图案与线样式（lines.rs）────────────────────────────
    // 线型图案的一段，长度是纸上的毫米
    struct LineSegment
    {
        enum class Kind { Dash, Space, Dot, DotLength, Offset, Wave, Zigzag, Text };
        Kind        kind      = Kind::Dash;
        double      length    = 0.0;     // 点（Dot）没有长度
        uint8_t     count     = 0;       // Offset：几条线
        double      distance  = 0.0;     // Offset：线距
        double      frequency = 0.0;     // Wave、Zigzag：这一段里几个周期
        double      amplitude = 0.0;
        double      size      = 0.0;     // Text：字高
        std::string text;

        // 这一段沿中心线占多长，纸上毫米
        double Length() const;
        bool operator==(const LineSegment&) const = default;
    };

    // 一段段重复的线型图案。不是"实、虚、实、虚……"的（偏移线、波浪、字）这里先画成实线
    struct LinePattern
    {
        std::vector<LineSegment> segments;

        bool Check(const std::string& name, std::string& error) const;
        // 实、虚相间（点算实）：画线时能直接当虚线画
        bool IsDash() const;
        // 纸上每毫米 pxPerMm 像素时实、虚的像素长度；重复得太密看不清时为空（画实线）
        std::optional<std::vector<float>> Dash(float pxPerMm) const;
        bool operator==(const LinePattern&) const = default;
    };

    // 线样式：笔、颜色、线型图案（没有时为实线）
    struct LineStyle
    {
        uint8_t                    pen = 1;
        Rgb3                       color{};
        std::optional<std::string> pattern;
        bool operator==(const LineStyle&) const = default;
    };

    namespace Lines
    {
        // 没有图案的线：不是库条目，图案不能叫这个名字
        inline constexpr const char* Solid = "Solid";
        // 详图线没说样式时用的（英文名）
        inline constexpr const char* Thin  = "Thin Lines";
        inline constexpr size_t MaxSegments = 32;
        inline constexpr double MinSegment  = 0.05;
        inline constexpr double MaxSegment  = 100.0;

        // 项目起步的线型图案与线样式，按项目语言起名
        std::map<std::string, LinePattern> Patterns(Language language);
        std::map<std::string, LineStyle>   Styles(Language language);
        // 详图线没说样式时用的：项目语言里的"细线"，否则英文名的，否则黑色细实线的那个，否则第一个
        const std::string* DefaultStyle(const std::map<std::string, LineStyle>& styles, Language language);
        // 笔在范围内、图案在 patterns 里
        bool CheckStyle(const LineStyle& s, const std::string& name, const std::map<std::string, LinePattern>& patterns, std::string& error);
    }

    // ── 填充图案与填充区域类型（fills.rs）──────────────────────
    // 填充图案的一族平行线：angle 度、过 origin，相邻两条隔 spacing、沿线错开 shift；dashes 是实（正）虚（负）长度，0 为点，空为整条
    struct HatchLine
    {
        double              angle   = 0.0;
        P2                  origin;
        double              shift   = 0.0;
        double              spacing = 0.0;
        std::vector<double> dashes;
        bool operator==(const HatchLine&) const = default;
    };

    // 填充图案：一族族线，长度是纸上毫米（制图）或建筑里的毫米（模型）；没有线时是实心填充
    struct FillPattern
    {
        bool                   model = false;
        std::vector<HatchLine> lines;

        bool IsSolid() const { return lines.empty(); }
        bool Check(const std::string& name, std::string& error) const;
        bool operator==(const FillPattern&) const = default;
    };

    // 填充区域类型：前景与背景图案（没有时不画）各带颜色；masking 时先用纸色盖住下面的东西
    struct RegionType
    {
        std::optional<std::string> foreground;
        Rgb3                       foregroundColor{};
        std::optional<std::string> background;
        Rgb3                       backgroundColor{};
        bool                       masking = false;
        bool operator==(const RegionType&) const = default;
    };

    namespace Fills
    {
        // 每个项目都有的实心填充图案，填充区域默认的类型，遮罩区域的类型（英文名）
        inline constexpr const char* SolidFill  = "Solid fill";
        inline constexpr const char* SolidBlack = "Solid Black";
        inline constexpr const char* Masking    = "Masking";
        inline constexpr size_t MaxFamilies = 32;
        inline constexpr size_t MaxDashes   = 16;
        inline constexpr double MinSpacing  = 0.05;
        inline constexpr double MaxSpacing  = 100000.0;

        std::map<std::string, FillPattern> Patterns(Language language);
        std::map<std::string, RegionType>  RegionTypes(Language language);
        // 填充区域没说类型时用的：项目语言里的"实心黑色"，否则英文名的，否则第一个
        const std::string* DefaultRegionType(const std::map<std::string, RegionType>& types, Language language);
        bool CheckRegionType(const RegionType& t, const std::string& name, const std::map<std::string, FillPattern>& patterns, std::string& error);

        // 图案在 rings（外轮廓与洞，方向随意：在奇数个环里的是里面）里的线段，模型毫米。k 是图案的一个长度单位合多少模型毫米
        //（制图图案为视图比例，模型图案为 1）；图案从 origin 起、转 angle 度。只要 window（最低角、最高角）以内的，
        // 最多 max 段；更多时（太密）为空
        std::optional<std::vector<std::array<P2, 2>>> Strokes(const FillPattern& p, const std::vector<Ring>& rings, double k,
                                                              std::array<P2, 2> window, size_t max, P2 origin = {}, double angle = 0.0);
    }

    // ── 文字类型（text.rs）──────────────────────────────────────
    // 文字类型：字体、大写字母在纸上的高度（毫米）、粗斜下划线、宽度系数、颜色，以及离字 borderOffset 毫米的边框（笔 borderPen）。
    // MiniBIM 只有一种单线字（Model/Font.h），字体名记着、写回文件，画的时候都用单线字
    struct TextType
    {
        std::string font        = "Arial";
        double      size        = 2.5;
        bool        bold        = false;
        bool        italic      = false;
        bool        underline   = false;
        double      widthFactor = 1.0;
        Rgb3        color{};
        bool        border      = false;
        double      borderOffset = 1.0;
        uint8_t     borderPen   = 1;

        bool Check(const std::string& name, std::string& error) const;
        bool operator==(const TextType&) const = default;
    };

    // 文字注释的一段（from 到 to 个字符，换行算一个）改成粗、斜、下划线或不是，不管类型怎么说
    struct TextRun
    {
        size_t              from = 0;
        size_t              to   = 0;
        std::optional<bool> bold;
        std::optional<bool> italic;
        std::optional<bool> underline;
        bool operator==(const TextRun&) const = default;
    };

    namespace TextTypes
    {
        // 一条文字注释最多的字符数与格式段数；行距是字高的几倍
        inline constexpr size_t MaxNote   = 4000;
        inline constexpr size_t MaxRuns   = 1000;
        inline constexpr double LinePitch = 1.6;

        // 项目起步的：2.5mm、3.5mm、5mm 的 Arial
        std::map<std::string, TextType> Defaults(Language language);
        // 新注释没说类型时用的：2.5mm，否则第一个
        const std::string* Default(const std::map<std::string, TextType>& types);
        // 字高 size 的平常 Arial 类型的名称：已有就用它，没有就加一个（<字高>mm，重名时加 2、3……）
        std::string ForSize(std::map<std::string, TextType>& types, double size);
        bool CheckRuns(const std::string& who, const std::vector<TextRun>& runs, size_t len, std::string& error);
    }
}
