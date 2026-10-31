// 移植自 BIMCraft crates/rvt/src/info.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/basic_file_info.rs (the `Key: value`
// text block, its list of known keys, and the narrow `True`/`False` quirk).
// Changed: rewritten for BIMCraft (the block found by its first line, each line split on its CR LF bytes and read from its
// own start, the release from the `Format` or `Revit Build` line only, nothing guessed from the binary head); then rewritten
// in C++ for MiniBIM.
//
// BasicFileInfo：版本、构建号、文档 GUID 与最后一次保存的情况。流是二进制的头（版本与构建号是带长度的 UTF-16 字符串，还有别的字段），
// 后面是以 CR LF 分开的 UTF-16 的 `键: 值` 行。头里有一个字节的字段，所以文字可能从奇数偏移开始；有一个值（IsSingleUserCloudModel）
// 在宽字符的文字里写成窄的 C 字符串，所以每行从它自己的第一个字节读，窄的值另行解开。最后保存者的名字（Username，只在开了工作共享时有）
// 与保存的路径也在这里：只显示，不写进项目或 IFC 文件
#pragma once

#include "Rvt/Error.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace MiniBIM::Rvt
{
    class Revit;

    struct BasicFileInfo
    {
        uint32_t                   release = 0;          // 版本年份，2024
        std::optional<std::string> build;                // 20230308_1635(x64)
        std::optional<std::string> guid;                 // Unique Document GUID，文档跨保存的身份
        std::optional<std::string> locale;               // Locale when saved，ENU
        std::optional<std::string> worksharing;          // Not enabled，或这一份在工作共享项目里的角色
        std::optional<std::string> username;             // 最后保存者的名字；不开工作共享时为空
        std::optional<std::string> lastSavePath;         // 最后保存到的完整路径（保存的那台机器上）
        std::optional<std::string> centralModelPath;
        std::vector<std::pair<std::string, std::string>> properties;   // 块里的每一行，按文件里的顺序，值照写的（空的也在）

        // 读流的字节
        static bool Parse(std::span<const uint8_t> bytes, BasicFileInfo& out, Error& error);
        // 块里一行的值
        std::optional<std::string> Property(std::string_view key) const;
    };

    // 读出并解析 BasicFileInfo 流
    bool ReadBasicFileInfo(Revit& r, BasicFileInfo& out, Error& error);
}
