// 移植自 BIMCraft crates/rvt/src/atom.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/part_atom.rs (which elements of the
// Atom entry hold the title, the OmniClass term and the categories) and src/project_information.rs (the one-entry ZIP a
// project keeps the same entry in, and that its entry name is never read out).
// Changed: rewritten for BIMCraft with a small scanner of its own instead of quick-xml, hard limits on size, nesting and
// tags, and no copy of the raw XML kept; then rewritten in C++ for MiniBIM.
//
// Atom 条目：族的 PartAtom 流，或项目的 ProjectInformation（装着同样条目的 ZIP）。里面有文档的标题、保存时间、产品版本，
// 族的还有类别与 OmniClass 编码（23.40.20.14.17 是家具）。ZIP 条目自己的名字是嵌着保存者 Windows 用户文件夹的临时路径，跳过、从不返回
#pragma once

#include "Rvt/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace MiniBIM::Rvt
{
    class Revit;

    // 最多读多少 XML
    inline constexpr size_t MaxAtom = 1 << 20;

    struct Atom
    {
        std::optional<std::string> title;            // 条目自己的 <title>：文档的名字
        std::optional<std::string> updated;
        std::optional<std::string> productVersion;   // <A:product-version>，写条目的版本
        std::optional<std::string> omniclass;        // 第一个是带点数字的类别
        std::vector<std::string>   categories;       // 每个类别的 term，按顺序

        static bool Parse(std::span<const uint8_t> xml, Atom& out, Error& error);
        bool operator==(const Atom&) const = default;
    };

    // 项目在 ProjectInformation 里存的 ZIP 的第一个条目，按 Atom 条目解析；maxInflated 限制条目解开后的大小
    bool ProjectInformation(std::span<const uint8_t> bytes, size_t maxInflated, Atom& out, Error& error);

    // 文档的 Atom 条目：PartAtom（族）或 ProjectInformation（项目）；两者都没有时 out 为空
    bool DocumentAtom(Revit& r, std::optional<Atom>& out, Error& error);
}
