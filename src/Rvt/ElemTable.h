// 移植自 BIMCraft crates/rvt/src/elem_table.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/elem_table.rs (the record layouts of
// `Global/ElemTable` and the rule that picks one, which rvt-rs credits to STE1200's frame measured by puzzbobb).
// Changed: rewritten for BIMCraft; only the ElementIds are read (not the owners), the 12-byte fallback layout is not
// supported (a table neither size fits is an error), and every count and offset is checked before it is used; then
// rewritten in C++ for MiniBIM.
//
// Global/ElemTable：文件里的每个 ElementId，升序。一个 u16（类表里 ElemTable 的标签）与第 2 字节起的 u32 记录数之后，
// 记录从第 6 字节开始：到 2023 版每条 28 字节（[u32 id][u32 id][16 字节][u32 owner]），2024 起 40 字节
// （[u64 id][12 字节][u64 id][u64 owner][u32]）。第二个 id 是元素的 ElementId。大小靠找不靠猜：说的条数放得下、
// ElementId 一路严格上升的那种；两种都行时取剩下的尾巴短的
#pragma once

#include "Rvt/Error.h"

#include <cstdint>
#include <span>
#include <vector>

namespace MiniBIM::Rvt
{
    class Revit;

    // 解开的 Global/ElemTable 声明的 ElementId，升序
    bool ParseElemTable(std::span<const uint8_t> data, std::vector<uint32_t>& out, Error& error);
    bool ReadElementIds(Revit& r, std::vector<uint32_t>& out, Error& error);
}
