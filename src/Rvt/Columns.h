// 移植自 BIMCraft crates/rvt/src/columns.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
//
// 柱，建筑柱与结构柱：从一个标高往上立着的截面。
//
// 柱的**位置**是它实例在平面上的包围盒：中心与宽、深都是包围盒的。转过的角度不是直角的柱，包围盒比截面大，所以截面取它
// 族符号的外包框，按它放置的角度（实例自己的数据）转过去。**标高**与**高度**同墙一样读：列表说的两个标高，底部是在包围盒
// 底部或以下较高的那个，只有包围盒止于另一个标高时才到那个标高。**类型**是列表说的唯一有名字条目的族符号。
//
// 柱从更高处起而包围盒从地面起时，由柱的几何节点修正。截面的*形状*不读：这里读的文件数据不说柱是矩形、圆形、工字形还是
// 角钢，所以每根柱以它包围盒的矩形进来，并说明。圆柱或型钢因此画成实心的方形
#pragma once

#include "Rvt/Importing.h"

namespace MiniBIM::Rvt::Columns
{
    // OST_Columns
    inline constexpr int64_t OstColumns = -2'000'100;
    // OST_StructuralColumns
    inline constexpr int64_t OstStructuralColumns = -2'001'330;

    void Wanted(const Facts& f, Wants& out);
    // 柱放进模型
    void Add(Importing& cx, const Facts& f, const Gathered& g);
}
