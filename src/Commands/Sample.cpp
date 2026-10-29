// 移植自 BIMCraft crates/core/src/sample.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Commands/Sample.h"

namespace MiniBIM::Bim
{
    // 与 BIMCraft 的 SAMPLE 逐行对应；门窗的族（Single Door、Casement Window……）换成项目自己的同尺寸类型，
    // 还没移植的命令（roof、room、ceiling、component、dimension、tag、elevation、section、sheet、place、text、spot）先略去
    const char* const SampleScript = R"(# Sample house: 10 x 7 m, four rooms; its roof, rooms, ceilings, furniture, annotation and sheets come as their commands are ported.
new {"name": "Sample House"}
grid {"start": [0, -1500], "end": [0, 8500]}
grid {"start": [6000, -1500], "end": [6000, 8500]}
grid {"start": [10000, -1500], "end": [10000, 8500]}
grid {"start": [-1500, 0], "end": [11500, 0], "name": "A"}
grid {"start": [-1500, 7000], "end": [11500, 7000]}
# Outer walls drawn clockwise, their brick outside.
wall {"points": [[0, 0], [0, 7000], [10000, 7000], [10000, 0]], "closed": true, "type": "Exterior - 300mm"}
wall {"points": [[6000, 0], [6000, 7000]], "type": "Interior - 100mm"}
wall {"points": [[0, 4000], [6000, 4000]], "type": "Interior - 100mm"}
wall {"points": [[6000, 3500], [10000, 3500]], "type": "Interior - 100mm"}
# Doors and windows of the project's own types, as big as the sample's families.
door {"at": [2000, 0], "type": "Single 900 x 2100", "flip_facing": true}
door {"at": [4500, 4000], "type": "Single 800 x 2100"}
door {"at": [6000, 1750], "type": "Single 800 x 2100", "flip_hand": true}
door {"at": [6000, 5250], "type": "Single 800 x 2100", "flip_facing": true}
window {"at": [4300, 0], "type": "Fixed 1200 x 1500"}
window {"at": [3000, 7000], "type": "Fixed 1200 x 1500"}
window {"at": [8000, 7000], "type": "Fixed 1200 x 1500"}
window {"at": [10000, 5250], "type": "Fixed 600 x 900"}
window {"at": [8000, 0]}
floor {}
)";
}
