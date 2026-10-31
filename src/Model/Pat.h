// 移植自 BIMCraft crates/core/src/pat.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Styles.h"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// PAT 文件里的填充图案（AutoCAD 公开的文本格式，Revit 的填充图案对话框也导入导出它）。文件里一个个图案：
// `*名称, 说明` 一行，接着每族线一行 `角度, x, y, 错开, 偏移[, 实, -虚, …]`（偏移是横过去，错开是沿着线，虚为负，0 为点）。
// `;` 起是注释，其中两种是设置：`;%UNITS=MM` 或 `INCH`（文件没说时为毫米），以及名称行之后的 `;%TYPE=MODEL` 或 `DRAFTING`
//（没说时为制图）。只照公开的格式说明读写，不带 Autodesk 的文字或文件。
//
// 读得宽容：不成图案的（没有线、一族线叠在一起或什么都不画、族或虚实段太多）留在外面并说明原因，其余照读。
// 恶意的文件有上限：只读前 MaxPatterns 个图案，其余只计数；留在外面的最多列出 MaxLeftOut 个；坏的行只引用开头
namespace MiniBIM::Bim::Pat
{
    inline constexpr size_t MaxPatterns = 2000;
    inline constexpr size_t MaxLeftOut  = 100;
    inline constexpr size_t MaxText     = 16u << 20;

    // 文件里的图案，按出现的顺序（名称后的说明不保留）；留在外面的各带原因：leftOut 只列前 MaxLeftOut 个，leftOutCount 是全部
    struct Read
    {
        std::vector<std::pair<std::string, FillPattern>> patterns;
        std::vector<std::pair<std::string, std::string>> leftOut;
        size_t leftOutCount = 0;
        bool operator==(const Read&) const = default;
    };

    // PAT 文件的内容 text 里的图案；太长时返回 false
    bool ReadText(std::string_view text, Read& out, std::string& error);
    // 文件的字节：不是 UTF-8 时按 Latin-1 读（老 PAT 文件常是 Windows 代码页：重音字母读对，0x80 到 0x9F 的标点成了控制字符，名称里去掉）
    std::string Decode(std::string_view bytes);
    // 图案写成毫米的 PAT 文件，实心的不写（PAT 文件说不出实心）
    std::string Write(const std::vector<std::pair<std::string, const FillPattern*>>& patterns);
}
