// 移植自 BIMCraft crates/core/src/align.rs 与 model.rs 的 reference_line（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Model.h"
#include <optional>
#include <utility>
#include <vector>

// 平面上的对齐：能被拾取的直边与轴线，以及把一条线放到另一条线上的刚体运动。与尺寸标注的参照无关（尺寸还没移植）。
// 墙：中心线、两个面、两端；门窗：中线与两侧门框；轴网：它的线；楼板、天花板、屋顶的边；构件的形体轮廓与两条轴线
namespace MiniBIM::Bim::Align
{
    using Line = std::pair<P2, P2>;

    // 元素 id 在平面上的直边与参照轴线；没有直线（弧墙的面、房间、标高）的为空
    std::vector<Line> Lines(const Model& m, Id id);
    // 离 at 最近的一条，和到它的距离（毫米）
    std::optional<std::pair<Line, double>> Nearest(const Model& m, Id id, P2 at);

    // 先绕 line 的中点转（较小的那个角，平行反向的不转半圈），再垂直平移到 reference 所在的直线上
    struct Placement
    {
        P2     about;
        double angle = 0.0;     // 度，逆时针，(-90, 90]
        P2     by;

        // 任一条线没有长度时返回 false，原因放在 error
        static bool Make(Line reference, Line line, Placement& out, std::string& error);
        P2 Map(P2 q) const;
    };
}
