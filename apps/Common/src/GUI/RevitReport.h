// 移植自 BIMCraft crates/ui-egui/src/revit.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Ifc/Ifc.h"
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Revit 文件在主窗口里的那部分里不碰界面控件的：认出 .rvt、读进来的项目叫什么，以及导入报告（AppTests 测这里）。
// 读取器本身（src/Rvt，Apache-2.0）主窗口不依赖：宿主链接了它时经 MainFrame::SetRevitReader 接上
namespace MiniBIM::RevitReport
{
    // 读取器：读 path，读进来的项目叫 name，读不了时原因放在 error（Rvt::ImportPath 就是这样）
    using Reader = std::function<bool(const std::string& path, std::string_view name, Bim::Ifc::Imported& out, std::string& error)>;

    // 路径是不是 Revit 项目（扩展名 .rvt，不分大小写）。族（.rfa）与样板不在这里读
    bool IsRevit(const std::string& path);
    // 读进来的项目的名称：文件名去掉扩展名，没有时为 Project
    std::string ProjectName(const std::string& path);

    // 导入的一条说明说略去了几个元素：`N walls left out, why (ids …)` 为 N。说别的（进来了、收窄了、挪了）的为空
    std::optional<size_t> CountLeftOut(std::string_view note);
    // 说明与末尾的元素 id：`2 walls left out, why (ids 7, 9)` 为 `2 walls left out, why` 与 `ids 7, 9`；没有 id 的整条是正文
    std::pair<std::string_view, std::optional<std::string_view>> SplitIds(std::string_view note);

    // 一次 Revit 导入说了什么：按类别进来了多少，导入的说明（略去了什么与原因、带着说明进来的）
    struct Report
    {
        std::string file;                                   // 文件名
        std::string schema;                                 // Revit 2024
        std::vector<std::pair<std::string, size_t>> counts; // 进来的：模型的类别与个数，多的在前
        std::vector<std::string> notes;

        size_t CameIn() const;
        size_t LeftOutCount() const;
        // 说略去了的说明、别的说明
        std::vector<std::string_view> LeftOut() const;
        std::vector<std::string_view> Noted() const;
    };

    // file 读进来的 imported 的报告
    Report Of(std::string file, const Bim::Ifc::Imported& imported);
    // 报告的纯文本（复制报告）：窗口里显示的全部，说明连同 id 一条一行
    std::string Text(const Report& r);
    // 状态栏的一句：Opened from Revit 2024: 12 elements, 3 left out.
    std::string StatusLine(const Report& r);
    // 类别的名称（随界面语言）：12 Walls、1 Level
    std::string CountLine(const std::string& category, size_t n);
}
