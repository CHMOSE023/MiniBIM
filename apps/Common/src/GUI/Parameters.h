// 移植自 BIMCraft crates/ui-egui/src/properties.rs、browser.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Json.h"
#include "Model/Units.h"
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 特性面板的参数表与取值的写法，以及项目浏览器的搜索：只依赖核心库，不碰界面控件（AppTests 测这里）
namespace MiniBIM::Props
{
    // 参数的编辑方式
    enum class Kind
    {
        Text,
        Number,         // 没有单位的数：角度、坡度
        Length,         // 长度或高度，按项目单位
        Point,          // [x, y]，按项目单位，写作 x, y
        Flag,
        Level,          // 标高，下拉列表
        LevelName,      // 标高，只显示名称
        TopLevel,       // 标高，或"未连接"（墙的顶部约束）
        Height,         // 墙高：顶部未连接时可写，否则是两个标高定出来的，只显示
        Limit,          // 长度或空（空时提示 Row::hint 说的意思，如"上一层"）
        Shown,          // 由别的推出，只显示
        LengthShown,    // 长度（或几个），只显示
        LengthIf,       // 有的元素才有的长度（拉伸屋顶的起止）：可写，没有时不列这一行
        AreaShown,      // 面积，m²（英制显示 ft²），只显示
        MetresShown,    // 长度，m，只显示
        LocationLine,   // 墙的定位线，下拉列表
        Attached,       // 墙顶或墙底是否贴着屋顶、楼板，只显示
        Justification,  // run 的高度在截面哪条线上（顶、中、底），下拉列表
    };

    // 一行参数：组、`elements` 里的字段（点号进入下一层，如 view_range.cut）、名称、编辑方式。组与名称是英文原文，显示前 Tr
    struct Row
    {
        const char* group;
        const char* key;
        const char* label;
        Kind        kind;
        const char* hint = nullptr;     // Limit 为空时的意思
    };

    // 类别的参数（没有的类别为空）
    std::span<const Row> RowsOf(std::string_view category);
    // 楼层平面的参数：读写它的标高
    std::span<const Row> PlanRows();

    // 元素 JSON（Bim::ElementJson）里 key 的值，穿过点号。JSON 里 0 的数可能略去（楼板的偏移），所以长度、数没有时是 0；
    // 写着 null 的就是没有
    Bim::Json FieldOf(const Bim::Json& e, std::string_view key, Kind kind);
    // `modify` 的参数 {key: val}，按点号嵌套：modify 把对象合并进去
    Bim::Json Nested(std::string_view key, Bim::Json val);

    // 值的通常写法：数保留两位小数，列表用逗号隔开，是否写 Yes / No（英文原文）
    std::string Text(const Bim::Json& v);
    // kind 的值在面板上怎么写，长度与面积按 units
    std::string Shown(Kind kind, const Bim::Json& v, Bim::Units units);
    // 输进去的文字 → `modify` 要的值（长度按 units 读）；读不出时返回 false，error 是给人看的原因（已翻译）
    bool Parse(Kind kind, std::string_view text, Bim::Units units, Bim::Json& out, std::string& error);

    // 类别的名称（英文原文，显示前 Tr）：several 为复数
    const char* CategoryName(std::string_view category, bool several);

    // ── 项目浏览器的搜索 ─────────────────────────────────────
    // 搜索框里的词（空白隔开，最多 64 个字符）
    std::vector<std::string> Words(std::string_view find);
    // word 是否出现在 text 里：不分大小写，* 是任意一串字符，? 是任意一个字符
    bool Matches(std::string_view text, std::string_view word);
}
