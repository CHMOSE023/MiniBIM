// 移植自 BIMCraft crates/core/src/pen.rs、styles.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Units.h"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 管理选项卡的线宽与对象样式（第 5f 步）：每支笔在纸上画多宽，以及每个模型类别看到的、剖到的线用哪支笔。
// 两者都是项目的设置：没设过时是项目起步的，设过之后存在项目文件里
namespace MiniBIM::Bim
{
    namespace Pens
    {
        // 笔 0（发丝线）到 16
        inline constexpr uint8_t Max = 16;
        // 每支笔在 1 : 100 时纸上的宽度，毫米，笔 0 起
        inline constexpr std::array<double, 17> Widths = { 0.0, 0.10, 0.13, 0.15, 0.18, 0.20, 0.22, 0.25, 0.28, 0.30, 0.35, 0.40, 0.50, 0.70, 1.00, 1.40, 2.00 };
        // 模型线宽最多几个比例，笔的宽度范围（纸上毫米）
        inline constexpr size_t MaxScales = 32;
        inline constexpr double MinWidth  = 0.01;
        inline constexpr double MaxWidth  = 20.0;
    }

    // 笔 1 到 16 的宽度
    using PenRow = std::array<double, 16>;

    // 模型线宽的一列：1 : scale 时笔 1 到 16 的宽度
    struct ScalePens
    {
        double scale = 100.0;
        PenRow widths{};
        bool operator==(const ScalePens&) const = default;
    };

    // 项目的线宽（管理 > 线宽）：模型的笔按比例，最细的比例在前；注释的笔（尺寸、标记、文字、轴网、标高与记号）不随比例
    struct LineWeights
    {
        std::vector<ScalePens> model;
        PenRow                 annotation{};

        // 项目单位 units 起步的：注释的笔是 Pens::Widths；模型的在 1 : 100 时同样，在这种单位常用的比例上，
        // 越细越宽、越粗越窄（按细了几倍的四次方根），到百分之一毫米
        static LineWeights Default(Units units);
        // 1 : n 时笔 0 到 16 的宽度：模型的取最近的比例那一列（按粗细几倍算，一样近取细的），注释的照原样
        std::array<double, 17> ModelAt(double n) const;
        std::array<double, 17> Annotation() const;
        // 比例 n 的那一列
        const ScalePens* Column(double n) const;
        // 一到 MaxScales 个比例，各不相同、都是视图能有的比例，每个宽度在范围内；不然 error 说要改什么
        bool Check(std::string& error) const;
        // 模型的比例按从细到粗排
        void Sort();
        bool operator==(const LineWeights&) const = default;
    };

    // 一个类别的笔：看到的（projection）与剖到的（cut）
    struct ObjectStyle
    {
        uint8_t projection = 4;
        uint8_t cut        = 10;
        bool operator==(const ObjectStyle&) const = default;
    };

    namespace ObjectStyles
    {
        // 项目起步的（BIMCraft 的）：剖到的笔 10、看到的笔 4；门窗剖到的（门扇与框）笔 6，栏杆看到的笔 6；体量剖到的与看到的都是笔 4
        ObjectStyle Default(std::string_view category);
        // 对象样式有这一类：视图画的模型类别（Graphics::Model）
        bool Known(std::string_view category);
        bool Check(const ObjectStyle& s, const std::string& category, std::string& error);
    }
}
