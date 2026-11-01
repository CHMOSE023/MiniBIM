// 移植自 BIMCraft crates/rvt/src/floors.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
//
// 楼板与天花板：草图画的边界带洞口，在一个标高上，属于一个类型。
//
// 楼板的**边界**是它的草图（见 Rvt/Sketch.h）：所有者是它的草图线，闭合成一个外环与其中的洞。**类型**是引用列表里唯一的
// 楼板类型，层从上往下，厚度是层的和。**标高**是列表说的那一个，否则为包围盒顶部或以下最高的标高，**偏移**是包围盒顶部
// 高出那个标高多少。只有包围盒与类型一样厚时楼板才是平的：更高的包围盒是带坡度或有高差的楼板，坡度不读，所以它在包围盒
// 底部放平进来，并说明。几个分开的环的草图建几块楼板。端点不相接或环相交的草图仍建楼板（端点用直线连上，否则是它包围盒的
// 轮廓），并说明是哪种。没有层的类型没有自己的厚度：取包围盒的高度。天花板同样读，它的标高与偏移是包围盒底部（底面）的
#pragma once

#include "Rvt/Importing.h"
#include "Rvt/Roofs.h"
#include "Rvt/Sketch.h"
#include "Rvt/Walls.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace MiniBIM::Rvt::Floors
{
    // OST_Floors
    inline constexpr int64_t OstFloors = -2'000'032;
    // OST_Ceilings
    inline constexpr int64_t OstCeilings = -2'000'038;
    // OST_SketchLines
    inline constexpr int64_t OstSketchLines = -2'000'045;

    // 每个草图元素的草图线：第二个引用列表里说到它的记录。文件把那个 id 放在开头、中间或最后，后面还可能说这条线碰到别的什么，
    // 所以所有者是那里唯一是楼板、天花板或屋顶的 id
    std::map<uint32_t, std::vector<const Record*>> SketchLines(const Facts& f);
    // 楼板要读数据的 id：楼板的、它们类型的与草图线的
    void Wanted(const Facts& f, std::set<uint32_t>& out);

    // 草图元素的线围成的
    struct Regions
    {
        std::vector<Sketch::Piece> pieces;
        // 什么也不围、略去的草图线：浮在楼板轮廓里的一条线，样例里有
        std::vector<uint32_t> dangling;
        // 草图没闭合，端点用直线连上了
        bool closed = false;
        // 线根本围不成边界，所以以它自己包围盒的轮廓进来
        bool boxed = false;
    };

    // 草图元素的线从它们的数据读出后围成的区域；线的数据里没有曲线时由读得出的那些围：读不出的线什么也不围，旁边的线围成的环仍闭合
    std::optional<Regions> RegionsOf(const std::vector<const Record*>& lines, const Gathered& g, const std::array<double, 6>& bbox);
    // 英尺的环换成毫米，到微米，去掉重合的点
    Bim::Ring RingOf(const std::vector<Sketch::Pt>& points);
    // 元素立在哪个标高，由它的引用列表与量包围盒的高度 z（英尺）：说的唯一一个，说的里面在 z 或以下最高的，或 z 或以下最高的标高
    std::optional<uint32_t> LevelOf(const Record& r, const std::map<uint32_t, double>& elevations, double z);

    // 楼板与天花板共有的：草图画的边界、带层的类型与标高
    struct Sketched
    {
        std::vector<Sketch::Piece> pieces;
        std::string                typeName;
        Bim::Guid                  level;
        // 包围盒的顶部（天花板、屋顶为底部）高出标高多少
        double offset = 0.0;
        // 包围盒与类型一样厚。带坡度或有高差的不是：它放平进来，是它的替身
        bool flat = false;
    };

    // 读哪种草图元素：类别，以及标高与偏移取包围盒的顶部（楼板）还是底部（天花板的底面）
    struct Which
    {
        int64_t category = 0;
        bool    atTop    = true;
    };

    inline constexpr Which Floor{ OstFloors, true };
    inline constexpr Which Ceiling{ OstCeilings, false };
    // 屋顶：底面是包围盒的底部，同天花板
    inline constexpr Which Roof{ Roofs::OstRoofs, false };

    // 起类型名字：每个类型一次
    using TypeNamed = std::function<std::string(Importing&, uint32_t, const Walls::LayeredType&)>;

    // 读楼板或天花板 r：它的草图、类型（由 typeNamed 建，每个一次）与标高；读不了时 why 说为什么
    bool SketchedOf(Importing& cx, const Facts& f, const Gathered& g, const Record& r, Which which, const std::map<uint32_t, std::vector<const Record*>>& sketches,
                    const TypeNamed& typeNamed, Sketched& out, std::string& why);
    // 楼板与天花板放进模型
    void Add(Importing& cx, const Facts& f, const Gathered& g);
}
