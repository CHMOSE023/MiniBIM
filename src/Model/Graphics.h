// 移植自 BIMCraft crates/core/src/graphics.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Ids.h"
#include "Model/Styles.h"
#include "Model/Units.h"
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// 可见性 / 图形，同 Revit：一个视图（标高的平面、平面视图、三维视图）按类别、按项目的视图过滤器、按元素隐藏或改画法
//（颜色、线宽、虚线、填充、半色调），另有视图比例与详细程度，以及它跟随的视图样板。阶段、设计选项、CAD 图层与 Linework 随后面的步骤移植（读文件时略去）
namespace MiniBIM::Bim
{
    struct Model;

    // 视图画剖到的东西画多细：粗略（一块填充，门窗只有洞口）、中等（复合层与层间的线，门扇、开启线、玻璃）、精细（再加层的图案）
    enum class Detail
    {
        Coarse,
        Medium,
        Fine,
    };

    // 三维视图怎么画面：着色、隐藏线、线框（MiniBIM 的三维视图现在都按着色画，记着写回文件）
    enum class VisualStyle
    {
        Shaded,
        HiddenLine,
        Wireframe,
    };

    // 线的虚实：实线、虚线、点线
    enum class Dash
    {
        Solid,
        Dashed,
        Dotted,
    };

    namespace DetailOf
    {
        // 英文名（界面翻译）：Coarse、Medium、Fine；文件与命令里是小写
        const char* Name(Detail d);
        std::optional<Detail> Parse(std::string_view s);
    }

    namespace VisualStyleOf
    {
        const char* Name(VisualStyle v);
        // 文件里的写法：shaded、hidden_line、wireframe
        const char* Key(VisualStyle v);
        // 大小写、空格、下划线都行：Hidden Line、hidden_line
        std::optional<VisualStyle> Parse(std::string_view s);
    }

    namespace DashOf
    {
        const char* Name(Dash d);
        std::optional<Dash> Parse(std::string_view s);
    }

    // 改过的一层图案：pattern 为空时这一层不画
    struct PatternFill
    {
        std::optional<std::string> pattern;
        Rgb3                       color{};
        bool operator==(const PatternFill&) const = default;
    };

    // 线与填充怎么画：线的颜色、线宽（笔）、虚实，实心填充的颜色，前景与背景图案
    struct Style
    {
        std::optional<Rgb3>        color;
        std::optional<uint8_t>     weight;
        std::optional<Dash>        dash;
        std::optional<Rgb3>        fill;
        std::optional<PatternFill> foreground;
        std::optional<PatternFill> background;

        bool Empty() const { return *this == Style{}; }
        // 自己的设置，没有的取 base 的
        Style Over(const Style& base) const;
        bool operator==(const Style&) const = default;
    };

    // 视图怎么画一样东西：隐藏，或看到的（projection）与剖到的（cut）线与填充另画，变淡（halftone，向纸色走一半），三维里透明（百分比）
    struct Override
    {
        bool    hidden = false;
        Style   projection;
        Style   cut;
        bool    halftone = false;
        uint8_t transparency = 0;

        bool Empty() const { return *this == Override{}; }
        Override Over(const Override& base) const;
        bool Check(const std::string& who, std::string& error) const;
        bool operator==(const Override&) const = default;
    };

    // 视图套用的一个过滤器：过滤器的名称与它挑出的元素的覆盖
    struct Applied
    {
        std::string filter;
        Override    over;
        bool operator==(const Applied&) const = default;
    };

    // 视图样板能控制的一组设置
    enum class Group
    {
        Categories,     // 类别的覆盖与隐藏
        Filters,        // 套用的过滤器
        Range,          // 平面的视图范围
        Phase,          // 显示的阶段与阶段过滤器（MiniBIM 还没有阶段，记着写回文件）
        Options,        // 显示的设计选项（同上）
        Scale,          // 平面、剖面、立面的比例
        Detail,         // 平面、剖面、立面的详细程度
        Style,          // 三维视图的视觉样式
    };

    namespace GroupOf
    {
        inline constexpr std::array<Group, 8> All = { Group::Categories, Group::Filters, Group::Range, Group::Phase,
                                                      Group::Options,    Group::Scale,   Group::Detail, Group::Style };
        // 文件与命令里的写法：categories、filters、range……
        const char* Key(Group g);
        std::optional<Group> Parse(std::string_view s);
    }

    // 视图的可见性与图形：跟随的视图样板，按类别（类别名）、按套用的过滤器与按元素（GUID）的覆盖，
    // 比例（1 : scale，没有时为项目单位的默认），详细程度，三维的视觉样式
    struct Graphics
    {
        std::optional<std::string>      templateName;
        std::map<std::string, Override> categories;
        std::vector<Applied>            filters;
        std::map<Guid, Override>        elements;
        // 阶段与设计选项还没有移植：读进来原样写回
        std::optional<std::string>         phase;
        std::optional<std::string>         phaseFilter;
        std::map<std::string, std::string> options;
        std::optional<double>           scale;
        Detail                          detail = Detail::Fine;
        VisualStyle                     style  = VisualStyle::Shaded;
        // 每张 CAD 图纸隐藏的图层，按图纸的名称：Revit 的"导入的类别"，一层一层地
        std::map<std::string, std::set<std::string>> layers;

        bool IsDefault() const { return *this == Graphics{}; }
        // 视图的比例 n（1 : n）：自己的，否则项目单位的默认（1 : 100，或 1/8" = 1'-0"）
        double ScaleN(Units units) const;
        bool Hides(std::string_view category) const;
        // 起作用的设置：自己的，样板控制的那几组取样板的
        Graphics Resolved(const Model& m) const;
        // 它跟随的、控制 group 的视图样板的名称：这一组设置不能由视图自己改
        const std::string* ControlledBy(const Model& m, Group group) const;
        // 类别都是视图画的、覆盖在范围内、比例在范围内、图案、过滤器与样板都存在
        bool Check(const Model& m, std::string& error) const;
        bool operator==(const Graphics&) const = default;
    };

    // 一个视图的图形落到模型的每个元素上：不画的，以及改了画法的（类别的覆盖，再是元素自己的）
    struct Look
    {
        std::set<Id>            hidden;
        std::map<Id, Override>  paint;
        VisualStyle             style = VisualStyle::Shaded;
        // 每张 CAD 图纸视图隐藏的图层
        std::map<std::string, std::set<std::string>> layers;

        // 视图隐藏的 CAD 图纸 cad 的图层；没有时为空
        const std::set<std::string>* HiddenLayers(const std::string& cad) const
        {
            const auto it = layers.find(cad);
            return it == layers.end() ? nullptr : &it->second;
        }
        bool Shows(Id id) const { return !hidden.count(id); }
        const Override* Paint(Id id) const
        {
            const auto it = paint.find(id);
            return it == paint.end() ? nullptr : &it->second;
        }
        bool Plain() const { return hidden.empty() && paint.empty(); }
    };

    namespace GraphicsOf
    {
        inline constexpr size_t MaxEntries = 100000;
        // 一个视图最多套用几个过滤器；项目最多几个视图过滤器、几个视图样板
        inline constexpr size_t MaxApplied   = 32;
        inline constexpr size_t MaxFilters   = 64;
        inline constexpr size_t MaxTemplates = 64;
        inline constexpr double MinScale   = 1.0;
        inline constexpr double MaxScale   = 100000.0;
        // 平面、剖面、三维视图画的类别（能隐藏、覆盖的）：模型的，再是注释的
        inline constexpr std::array<const char*, 26> Model = {
            "wall", "floor", "ceiling", "shaft", "mass", "door", "window", "room", "roomseparator", "column", "structuralcolumn", "footing", "beam",
            "stripfooting", "pipe", "conduit", "cabletray", "wire", "downconductor", "groundconductor", "cable", "component", "stair", "ramp",
            "railing", "roof",
        };
        inline constexpr std::array<const char*, 10> Annotation = { "dimension", "tag", "text", "line", "region", "spot", "grid", "level", "section",
                                                                    "elevation" };
        // 导入的类别：CAD 导入（BIMCraft 另有点云，随第 5g3 步移植）
        inline constexpr std::array<const char*, 1> Imported = { "cadimport" };
        // 视图画这一类吗
        bool Drawn(std::string_view category);
        // 视图画的类别，逗号分开：报错里列出来
        std::string Names();
    }
}
