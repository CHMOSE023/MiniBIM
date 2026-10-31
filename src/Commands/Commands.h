// 移植自 BIMCraft crates/core/src/commands.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Commands/Session.h"
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 命令表（Commands() 见 Session.h）之外，命令里有用、别处也要用的几个函数
namespace MiniBIM::Bim
{
    // 元素给 AI 代理读的样子：它的字段（引用写 GUID），加上 id、guid 与推算出的值（长度、高度、面积……）
    Json ElementJson(const Model& m, Id id);
    // door、window 命令（is_door 时为门）按参数 p 做出的门窗，不放进模型：工具预览、放到中间用。做不出时返回 false，原因放在 error
    bool OpeningOf(const Model& m, const Json& p, bool isDoor, Opening& out, std::string& error);
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
    // 做类别 category 元素的命令：structuralcolumn → structural_column，cabletray → cable_tray……（柱以外不认识的为 column）
    const char* CommandOf(std::string_view category);
    // 命令 command 做的 run 类别（pipe、cable_tray → cabletray……）；不做 run 时为空。CommandOf 的反向
    std::optional<const char*> RunCategoryOf(std::string_view command);
    // 命令要的 run（梁、管道……），points 每一段一个，还没放进模型：工具预览一次单击会放下的
    bool RunsOf(const Model& m, const Json& p, std::string_view category, std::vector<Element>& out, std::string& error);
    // 命令要的柱、结构柱或独立基础（category 为 column、structuralcolumn、footing），还没放进模型：工具预览用
    bool StoodOf(const Model& m, const Json& p, std::string_view category, Element& out, std::string& error);
    // dimension 命令把点认成的参照：level 的平面剖切到的东西（墙的面、中心线与端头，门窗的中心与两边，轴网）里 keep 认的、
    // 离 p 最近的参照线，100 毫米以内；尺寸工具拾取参照也用它
    std::optional<Reference> ReferenceNear(const Model& m, Guid level, P2 p, const std::function<bool(const Segment&)>& keep);
    // 径向尺寸：level 上圆柱的边离 p 最近（100 毫米以内）的那根
    std::optional<Reference> ArcNear(const Model& m, Guid level, P2 p);
    // 元素 id 经 map 平移或旋转后的样子（move、rotate 命令那样）；with 是一起动的元素（门窗随它们的墙）。
    // 链接把它造的东西照它站的地方带进视图用
    bool Moved(const Model& m, Id id, const std::function<P2(P2)>& map, const std::set<Id>& with, Element& out, std::string& error);
}
