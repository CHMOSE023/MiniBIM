// 移植自 BIMCraft crates/core/src/runs.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Geom.h"
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 两点之间的直线元素（梁、条形基础、管道、线管、桥架、导线、引下线、接地线、缆线）的几何：中心线的两端、平面上的投影、
// 三维里的棱柱或圆管。它不管类别，只有一张表给出 IFC 的类与它的默认值（能不能竖立、高度在截面的哪条线上）；
// 平面图、三维、捕捉与 IFC 都问它
namespace MiniBIM::Bim
{
    struct Model;
    struct Run;
    struct RunType;
    enum class Justify;
}

namespace MiniBIM::Bim::Runs
{
    // 圆管画成多少边
    inline constexpr size_t Sides = 24;

    // category 的 run 能不能竖着（立管、引下线）而不只是沿着走：梁与条形基础不能，竖着的是柱
    bool MayStand(std::string_view category);

    // 各类别在 IFC 里怎么写：类与预定义类型。导入读同一张表，类与类型对上的第一个类别
    struct IfcRow
    {
        const char* category;
        const char* ifcClass;
        const char* kind;
    };
    const std::vector<IfcRow>& IfcTable();
    // category 的 run 写成的 IFC 类与预定义类型
    std::optional<std::pair<const char*, const char*>> Ifc(std::string_view category);
    // IFC 产品是哪个类别的 run：按类 ifcClass、预定义类型 kind（不带点也行）与本程序写的文件里声明的类别 declared：
    // 类有这个类别时用它，否则是类型对上的那个，否则是这个类的第一个
    std::optional<const char*> FromIfc(std::string_view ifcClass, std::optional<std::string_view> kind, std::optional<std::string_view> declared);

    // 没说时高度在截面的哪条线上：梁与基础在顶面（工程师这么想：梁顶在板顶），输送东西的（管、线）在中线
    Justify DefaultJustify(std::string_view category);

    // 算好的 run：中心线从 a 到 b（毫米：x 东、y 北、z 上），截面垂直于这条线，宽 width、高 depth；圆的 width 是直径，depth 同它
    struct Geom
    {
        P3     a{};
        P3     b{};
        double width = 0.0;
        double depth = 0.0;
        bool   round = false;

        // 类型 t 的 r：它的高度是 justify 那条线的，所以中心线在它下面（顶）或上面（底）截面一半深（垂直于线量）
        static Geom Of(const Model& m, const Run& r, const RunType& t);
        // 沿中心线的真实长度
        double Length() const;
        // 平面上的长度
        double PlanLength() const;
        // 竖着：两端在平面上重合
        bool   Vertical() const;
        // 截面一半深，竖直量：线倾斜时比垂直于线量的一半大
        double Half() const;
        // 平面上的投影，逆时针：从一端到另一端宽 width 的一条；竖着时为它的截面，width 沿东、depth 沿北，或圆
        Ring   Footprint() const;
        // 最低与最高到哪里
        std::pair<double, double> ZSpan() const;
        // 当成棱柱：投影夹在底面与顶面之间，截面是矩形（圆的也是外切的方形）
        struct PrismOf
        {
            Ring  ring;
            Plane bottom;
            Plane top;
        };
        PrismOf Prism() const;
        // 圆的 run 当成朝外的三角形围成的圆管：先是两端的盖子，以及盖子有几个三角形
        std::pair<std::vector<Tri>, size_t> Tube() const;
        bool operator==(const Geom&) const = default;
    };

    // g（类别 category 的 run）模型不接受时的原因；接受时返回 true
    bool Check(const std::string& who, std::string_view category, const Geom& g, std::string& error);
}
