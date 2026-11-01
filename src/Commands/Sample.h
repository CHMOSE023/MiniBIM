// 移植自 BIMCraft crates/core/src/sample.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once

namespace MiniBIM::Bim
{
    // 示例房屋：用命令搭起来的一层小房子（10 × 7 m，四个房间），演示用，也检验整套命令。
    // BIMCraft 的示例还有标注、立面、剖面和图纸，这些命令移植后（docs/PLAN.md 第 2 阶段）逐行补回；
    // 现在只有其中已移植命令的部分
    extern const char* const SampleScript;
}
