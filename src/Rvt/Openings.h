// 移植自 BIMCraft crates/rvt/src/openings.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
//
// 门窗：立在墙里，中心在沿墙某处，从窗台往上。
//
// 门窗的**宿主**是它引用列表说的一面墙。列表按 ElementId 升序，所以宿主是记录自己 id 以下最近的墙，否则是以上最近的
// （RE-23、RE-85）；幕墙不是宿主，只有幕墙可选的门窗没有宿主。它的**洞**是实例沿墙的包围盒：包围盒沿墙的长度是宽度，
// 高度是包围盒的，窗台是包围盒底部高出墙底多少；中心在包围盒的中心。沿轴的墙里包围盒沿轴的长度就是宽度；斜墙里宽度从
// 包围盒解出（纯几何），45° 附近与包围盒不是转过的矩形时除外。弧墙里的门窗，或那些情况，略去并列出。**类型**是列表说的
// 它类别的唯一一个有该类别名字条目的族符号，名字就是那条。门的开向与左右由族画，不读：每个门窗用默认的翻转
#pragma once

#include "Rvt/Importing.h"

#include <optional>
#include <set>
#include <string>
#include <utility>

namespace MiniBIM::Rvt::Openings
{
    // OST_Doors
    inline constexpr int64_t OstDoors = -2'000'023;
    // OST_Windows
    inline constexpr int64_t OstWindows = -2'000'014;

    // 门窗不从数据读什么：记录与类型的名字就是全部
    void Wanted(const Facts& f, std::set<uint32_t>& out);
    // 门窗放进模型
    void Add(Importing& cx, const Facts& f, const Gathered& g);

    // 族实例的类型：引用列表说的它类别的唯一一个有该类别名字条目的族符号，以及名字。一个也没有、或两个名字不同时为空
    std::optional<std::pair<uint32_t, std::string>> SymbolOf(const Record& r, const std::set<uint32_t>& symbols, const Gathered& g, int64_t category);
    // 族实例的类型的名字（SymbolOf）
    std::optional<std::string> SymbolName(const Record& r, const std::set<uint32_t>& symbols, const Gathered& g, int64_t category);
}
