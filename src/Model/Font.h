// 移植自 BIMCraft crates/core/src/font.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include <string_view>
#include <utility>
#include <vector>

// 平面图注释用的单线字：大写字母（拉丁与西里尔）、数字、几个符号与拉丁语言的附加符号，画在 8 × 12 的格子上（y 向上）。
// 小写按大写画，同建筑字体；没有的字画成问号
namespace MiniBIM::Bim::Font
{
    // c（不分大小写）有字：否则画问号
    bool Has(char32_t c);
    // text 在 height 像素高时多宽
    float Width(std::string_view text, float height);
    // 写 text 的折线：大写字母 height 像素高，从基线上的 (x, y) 起，y 向下
    std::vector<std::vector<std::pair<float, float>>> Strokes(std::string_view text, float x, float y, float height);
}
