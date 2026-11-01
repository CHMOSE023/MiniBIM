// 移植自 BIMCraft crates/core/src/sketch.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Geom.h"
#include "Model/Model.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

// 草图：Revit 的草图模式，楼板、天花板、屋顶迹线的轮廓唯一的画法。草图是一些直线和圆弧（Bim::SketchLine），先后、方向随意；
// Solve 把它们在 Tol 以内首尾相接成闭合的环，再嵌套：不在任何环里的是外轮廓，外轮廓里的是洞，洞里的又是外轮廓。
// 解不出的，一次说出所有问题和涉及的线（编号从 0），好标出给画的人看：线太短或超出范围、两条线相交或一条端在另一条中间、
// 两条线叠在一起、端点没有接上别的线、三条以上的线交于一点。
//
// BIMCraft 的草图线可以是任何曲线（多段线、圆、椭圆、样条，curves.rs）；这里只移植了直线与圆弧，别的随 curves.rs 移植
namespace MiniBIM::Bim::Sketch
{
    // 一个草图最多多少条线；最多切成多少段直线（圆弧切成弦）
    inline constexpr size_t MaxLines    = MaxPoints;
    inline constexpr size_t MaxPieces   = 200000;
    // 最多说出几个问题
    inline constexpr size_t MaxProblems = 100;

    // 线为什么连不成环，与涉及的线（编号从 0，按给出的顺序）
    struct Problem
    {
        enum class Kind
        {
            Empty,          // 一条线都没有
            TooMany,        // 太多：count 条 what（"lines"、"pieces"、"steps"、"loops"）
            OutOfRange,     // 点不是数，或超出 MaxCoord
            TooShort,       // 线长不超过 Tol
            Cross,          // 两条线相交，或一条的端点在另一条中间，在 at；一条线两次时是它自己相交
            Overlap,        // 两条线叠在一起超过 Tol
            Open,           // 端点（at）没有接上别的线：环在那里开着
            Branch,         // 三条以上的线交于 at：一个环每个点只经过一次
            NoArea,         // 一个环围不出面积
            Separate,       // 只要一个外轮廓时有 count 个
        };
        Kind                kind = Kind::Empty;
        std::vector<size_t> lines;
        std::optional<P2>   at;
        size_t              count = 0;
        std::string         what;

        // 先改哪个：线本身的问题，再是相交、重叠，再是分叉、开口
        int Rank() const;
        // 一句话说出来，线按 name 叫（line 999812、line 2）
        std::string Said(const std::function<std::string(size_t)>& name) const;
        bool operator==(const Problem&) const = default;
    };

    // 几个问题说成一句，前几个说出来
    std::string Say(const std::vector<Problem>& problems, const std::function<std::string(size_t)>& name);
    // 线怎么叫：有 ID 的叫 ID，还没有的按在列表里的位置
    std::function<std::string(size_t)> Namer(const std::vector<SketchLine>& lines);

    // 线画出来经过的点：直线两端，圆弧切成弦（弦离弧不超过 1 毫米）
    std::vector<P2> Points(const SketchLine& l);
    double Length(const SketchLine& l);
    // 从 a 到 b 经过 through 的圆弧（through 离直线不超过 Tol 时是直线）
    SketchLine ArcThrough(P2 a, P2 b, P2 through);
    // 离 p 多远
    double Distance(const SketchLine& l, P2 p);
    // 中点（沿它一半处）
    P2 Middle(const SketchLine& l);

    // 线围出的：每个外轮廓（逆时针）与它的洞（顺时针），按各自第一条线画出的先后
    struct Profile
    {
        std::vector<Geom::Region> regions;
        double Area() const;
    };

    // 解成环、嵌套成区域；或说出找到的每个问题（最多 MaxProblems 个），先改的在前（Problem::Rank）
    bool Solve(const std::vector<SketchLine>& lines, Profile& out, std::vector<Problem>& problems);
    // 解成楼板、天花板、屋顶要的一个外轮廓（带洞）
    bool SolveOne(const std::vector<SketchLine>& lines, Geom::Region& out, std::vector<Problem>& problems);
    // 外轮廓与洞的各条边，当作要编辑的直线（还没有 ID）
    std::vector<SketchLine> LinesOf(const Ring& outline, const std::vector<Ring>& holes);
    // lines 解出来是否正是 outline 与 holes（每个环同样的点、同样的顺序，起点与方向随意）：元素记着的草图还是它画的样子
    bool Draws(const std::vector<SketchLine>& lines, const Ring& outline, const std::vector<Ring>& holes);
    // 记着的草图照 Model::Check 检查文件里的东西那样查：线数、点在范围内、切成的段数
    bool Check(const std::string& who, const std::vector<SketchLine>& lines, std::string& error);
}
