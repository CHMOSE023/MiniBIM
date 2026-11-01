// 移植自 BIMCraft crates/core/src/csg.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Geom.h"
#include <cstdint>
#include <string>
#include <vector>

// 实体减去实体，供族的空心形体用：二叉空间分割里的凸多边形，做法同 csg.js。树用工作表建、走，不递归
//（很多面的光滑曲面会让树和面一样深），做的每件事都记在 Meter 上，所以恶意的形状以报错结束，不会卡住
namespace MiniBIM::Bim::Csg
{
    // 每个实体最多的三角形数
    inline constexpr size_t MaxTriangles = 20000;

    // 切割还能花多少：拿角点比平面的次数、把多边形切成两半的次数
    struct Meter
    {
        uint64_t work   = 40000000;
        uint64_t splits = 400000;
    };

    // solid 减去 voidShape 占去的部分：两者都是朝外的三角形围成的封闭曲面；剩下的三角形放进 out，被整个吞掉时为空。
    // 离得远的空心形体原样留下 solid。失败（三角形太多、太费工夫）时返回 false，原因放在 error
    bool Subtract(const std::vector<Tri>& solid, const std::vector<Tri>& voidShape, Meter& meter, std::vector<Tri>& out, std::string& error);
}
