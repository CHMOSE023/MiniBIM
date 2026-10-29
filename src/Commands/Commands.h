// 移植自 BIMCraft crates/core/src/commands.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Commands/Session.h"
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 命令表（Commands() 见 Session.h）之外，命令里有用、别处也要用的几个函数
namespace MiniBIM::Bim
{
    // 元素给 AI 代理读的样子：它的字段（引用写 GUID），加上 id、guid 与推算出的值（长度、高度、面积……）
    Json ElementJson(const Model& m, Id id);
    // floor 命令默认的楼板轮廓：level 上围合房间的墙的中心线围出的最大一块的外轮廓（逆时针），以及其中独立的环
    //（不与外墙相连、里面没有房间：天井），各从西南角起顺时针
    std::optional<std::pair<Ring, std::vector<Ring>>> FloorOutline(const Model& m, Guid level);
    // roof 命令默认的屋顶迹线：level 下面一个标高上墙的外轮廓，伸出墙的外面 overhang 毫米（逆时针），以及伸出后仍敞开的天井。
    // 没有时为空，原因放在 error
    std::optional<std::pair<Ring, std::vector<Ring>>> RoofFootprint(const Model& m, Guid level, double overhang, std::string& error);
    // taken 所在序列的下一个：最大的数加一，没有时为 "1"。序列是纯数（"1"、"2"）或前缀加数（"P1"、"J-01"）
    std::string NextNumber(const std::vector<std::string>& taken);
    // 环按角点、到微米，从西南角起
    Ring Tidy(const Ring& ring);
}
