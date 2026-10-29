// 移植自 BIMCraft crates/core/src/skeleton.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Geom.h"
#include <string>
#include <utility>
#include <vector>

// 多边形（含洞）的直骨架，每条边以自己的速度向内移动：角点在边移动、相遇时留下的线（屋顶的斜脊、天沟、屋脊），
// 以及每条边扫过的面（它的坡面）。不移动的边保持竖直，即山墙。
//
// 波前随时间移动：角点移动时保持在它两侧的边上。边事件是一条边在两个角点之间缩没了；分裂事件是一个凹角点撞上对面的边，
// 把波前切成两段，或者那条边是洞的边时，把两段连成一段
namespace MiniBIM::Bim::Skeleton
{
    // 一条边扫过的面的一圈：各角点，以及波前到达它的时间
    using Face = std::vector<std::pair<P2, double>>;

    // rings 的各条边以 speeds 向内移动扫过的面：rings 是外轮廓（逆时针、简单）与其中的洞（各自顺时针、在里面、互不相交），
    // 边按环依次编号，第 i 条边从第 i 点到下一点。对每条移动的边给出它的面的各圈：外圈（逆时针，从边的起点沿边走，
    // 再经骨架走回），然后是面里的洞（顺时针）；不移动的边没有面。失败时返回 false，原因放在 error
    bool Faces(const std::vector<Ring>& rings, const std::vector<double>& speeds, std::vector<std::vector<Face>>& out,
               std::string& error);
}
