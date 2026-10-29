// 移植自 BIMCraft crates/core/src/sample.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Commands/Sample.h"

namespace MiniBIM::Bim
{
    // 与 BIMCraft 的 SAMPLE 逐行对应；还没移植的命令（dimension、tag、elevation、section、sheet、place、text、spot）先略去
    const char* const SampleScript = R"(# Sample house: 10 x 7 m, four rooms under a hipped roof and their ceilings, furnished; its annotation and sheets come as their commands are ported.
new {"name": "Sample House"}
grid {"start": [0, -1500], "end": [0, 8500]}
grid {"start": [6000, -1500], "end": [6000, 8500]}
grid {"start": [10000, -1500], "end": [10000, 8500]}
grid {"start": [-1500, 0], "end": [11500, 0], "name": "A"}
grid {"start": [-1500, 7000], "end": [11500, 7000]}
# Outer walls drawn clockwise, their brick outside; their tops follow the roof once it's on.
wall {"points": [[0, 0], [0, 7000], [10000, 7000], [10000, 0]], "closed": true, "type": "Exterior - 300mm"}
wall {"points": [[6000, 0], [6000, 7000]], "type": "Interior - 100mm"}
wall {"points": [[0, 4000], [6000, 4000]], "type": "Interior - 100mm"}
wall {"points": [[6000, 3500], [10000, 3500]], "type": "Interior - 100mm"}
# Doors and windows of the project's families (forms, plan swing, glass), and three of its own plain types.
door {"at": [2000, 0], "family": "Single Door", "type": "900 x 2100", "flip_facing": true}
door {"at": [4500, 4000], "type": "Single 800 x 2100"}
door {"at": [6000, 1750], "family": "Single Door", "type": "800 x 2100", "flip_hand": true}
door {"at": [6000, 5250], "type": "Single 800 x 2100", "flip_facing": true}
window {"at": [4300, 0], "family": "Casement Window", "type": "1200 x 1500"}
window {"at": [3000, 7000], "family": "Casement Window", "type": "1200 x 1500"}
window {"at": [8000, 7000], "family": "Fixed Window", "type": "1200 x 1500"}
window {"at": [10000, 5250], "type": "Fixed 600 x 900"}
window {"at": [8000, 0]}
floor {}
roof {}
modify {"ids": [100007, 100008, 100009, 100010], "attach_top": 100024}
room {"point": [3000, 2000], "name": "Living"}
room {"point": [1500, 5000], "name": "Kitchen"}
room {"point": [8400, 4200], "name": "Bedroom"}
room {"point": [7600, 2600], "name": "Study"}
# A ceiling in every room, 2600 mm up, tiled in the kitchen.
ceiling {"point": [3000, 2000]}
ceiling {"point": [3000, 5500], "type": "Tile 600 x 600"}
ceiling {"point": [8000, 5250]}
ceiling {"point": [8000, 1750]}
# Furniture from the project's families: a sofa; a dining table, its chairs and a base cabinet; a bed and a wardrobe; a desk and its chair.
component {"family": "Sofa", "type": "2000 x 900", "at": [3000, 3950]}
component {"family": "Table", "type": "1200 x 800", "at": [4300, 5900]}
component {"family": "Chair", "at": [4000, 5225], "angle": 180}
component {"family": "Chair", "at": [4600, 5225], "angle": 180}
component {"family": "Chair", "at": [4000, 6575]}
component {"family": "Chair", "at": [4600, 6575]}
component {"family": "Base Cabinet", "type": "1800 x 600", "at": [1200, 6850]}
component {"family": "Bed", "type": "1600 x 2000", "at": [8000, 6850]}
component {"family": "Wardrobe", "type": "1200 x 600", "at": [6050, 4200], "angle": 90}
component {"family": "Desk", "type": "1400 x 700", "at": [9850, 1500], "angle": -90}
component {"family": "Chair", "at": [8875, 1500], "angle": 90}
)";
}
