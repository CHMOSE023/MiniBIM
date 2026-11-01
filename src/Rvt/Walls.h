// 移植自 BIMCraft crates/rvt/src/walls.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/element_record_level_refs.rs (the
// levels a record's reference list names, RE-27, RE-59), src/element_record_wall_joins.rs (what the record box of a wall
// is, RE-26, RE-29) and src/partition_compound_structure.rs (a wall's location line and which side is its exterior, RE-54).
// Changed: rewritten for BIMCraft, which builds a parametric wall from them instead of a solid; then rewritten in C++ for
// MiniBIM.
//
// 墙：直的与弯的，带它类型的层。
//
// 墙的**定位线**是它数据里的第一条曲线记录（见 Rvt/Curves.h）：不论定位线设置是什么都是它的中心线，所以 MiniBIM 的墙
// location 是 Centre，两端是线的两端。它的**记录包围盒**是没裁剪的墙（定位线到原始端点，各向外胀半个厚度）：Revit 自己导出时
// 相接处把它裁回去，MiniBIM 自己在墙相接处裁，所以两端取线的。**高度**是包围盒的，**标高**是引用列表说的两个
// （底部是在包围盒底部或以下的较高的那个）；只有包围盒的顶部就在另一个标高上时，墙才*到*那个标高，否则与包围盒一样高。
// **类型**是引用列表里唯一的墙类别的类型；类型的层是它的复合结构，**外侧在前**，正是外侧在左边画的墙在 MiniBIM 里的顺序。
// 外侧在定位线右边的墙掉个头
#pragma once

#include "Rvt/Importing.h"
#include "Rvt/Types.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace MiniBIM::Rvt::Walls
{
    // OST_Walls
    inline constexpr int64_t OstWalls = -2'000'011;

    // 墙要读数据的 id：墙的与它们类型的
    void Wanted(const Facts& f, std::set<uint32_t>& out);
    // 墙放进模型
    void Add(Importing& cx, const Facts& f, const Gathered& g);

    // 记录的引用列表说的各个项目标高，不重复，按列表的顺序
    std::vector<uint32_t> NamedLevels(const std::vector<uint64_t>& refs, const std::map<uint32_t, double>& elevations);
    // 高程在 z 英尺或以下（差 0.001 英尺以内）最高的标高；没有时为最低的；有两个一样高时为空
    std::optional<uint32_t> LevelAtOrBelow(const std::map<uint32_t, double>& elevations, double z);
    // 包围盒从底到顶的元素的底部与顶部约束：说了两个标高时，底部是在底部或以下的较高的那个（否则是较低的），顶部是另一个；
    // 一个时它是底部；没有时为底部或以下的标高
    std::pair<std::optional<uint32_t>, std::optional<uint32_t>> Constraints(const Record& r, const std::map<uint32_t, double>& elevations);
    // Revit 的层功能换成 MiniBIM 的
    Bim::Function FunctionOf(uint32_t revit);

    // 从文件读出的类型：名字、层（宽度毫米、功能、材质名）与它们加起来的厚度
    struct LayeredType
    {
        std::optional<std::string> name;
        std::vector<Bim::Layer>    layers;
        double                     thickness = 0.0;
    };

    // 有层的类别的类型 id：名字、层与厚度，取它数据的最新一份。数据里没有层时为空
    std::optional<LayeredType> LayeredTypeOf(const Facts& f, const Gathered& g, uint32_t id, int64_t category);
    // 类型的层换成 MiniBIM 的，以及它们加起来的厚度
    LayeredType Layered(std::optional<std::string> name, const std::vector<Types::CompoundLayer>& layers, const Gathered& g, int64_t category);
}
