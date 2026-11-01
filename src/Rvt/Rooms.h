// 移植自 BIMCraft crates/rvt/src/rooms.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
//
// 房间：一个标高上的名称与编号，以及围住它的墙里的一点。
//
// 房间的**名称**与**编号**是它对象里的参数条目（见 Rvt/Data.h）；**标高**同墙一样从它引用列表说的标高读。MiniBIM 由房间一点
// 四周的墙找出边界，所以房间的**点**是它记录包围盒里导入的墙围住的一点：围住包围盒中心时取中心，否则取包围盒上网格里
// 离中心最近、被围住的那点。墙围成的区域的包围盒要与房间的包围盒差在墙厚（600 毫米）以内，否则房间略去并列出：开敞的、
// 或者墙没导入的房间在 MiniBIM 里没有区域，这里不替它画轮廓。建一个标高的各个面费时间，房间又要试一格点，所以一个标高的面
// 为它所有的房间只建一次
#pragma once

#include "Rvt/Importing.h"

namespace MiniBIM::Rvt::Rooms
{
    // OST_Rooms
    inline constexpr int64_t OstRooms = -2'000'160;

    void Wanted(const Facts& f, Wants& out);
    // 房间放进模型
    void Add(Importing& cx, const Facts& f, const Gathered& g);
}
