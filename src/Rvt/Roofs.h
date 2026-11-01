// 移植自 BIMCraft crates/rvt/src/roofs.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
//
// 屋顶：迹线屋顶是草图画的边界，平的，在一个标高上，属于一个类型；拉伸屋顶是扫过去的一个轮廓。
//
// 迹线屋顶（类 ProfileRoof）同楼板、天花板一样读：草图线围成的环、引用列表里唯一的屋顶类型、它立在的标高与它的底面
// （包围盒的底部）。升起的屋顶（边设了坡度、类型沿坡面量厚，或别的方式带坡度）在包围盒底部放平进来，并说明坡度没读：
// 边的坡度块（RE-56）不读，这个读取器核对过的样例里没有屋顶设它。
//
// 拉伸屋顶（类 Roof）是它草图的线，即它顶面的轮廓：每条线的数据存着它在模型里的两端（RE-49 的有界直线），所以垂直于一根轴的
// 竖直平面里的轮廓整个读得出，包围盒说扫过多远。类型垂直于每个坡面量多厚，两端竖直切开，由包围盒核对：只有轮廓与类型
// 构成的包围盒就是记录存的那个（差一毫米以内）时屋顶才进来。这部分是 BIMCraft 从 Autodesk 样例的字节读出的，核对靠记录的
// 包围盒。别的类的屋顶（按面、在体量上）以它自己包围盒的轮廓进来，并说明
#pragma once

#include "Rvt/Importing.h"

#include <set>

namespace MiniBIM::Rvt::Roofs
{
    // OST_Roofs
    inline constexpr int64_t OstRoofs = -2'000'035;

    // 屋顶要读数据的 id：屋顶的、它们类型的与草图线的
    void Wanted(const Facts& f, std::set<uint32_t>& out);
    // 屋顶放进模型
    void Add(Importing& cx, const Facts& f, const Gathered& g);
}
