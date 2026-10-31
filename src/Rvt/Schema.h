// 移植自 BIMCraft crates/rvt/src/schema.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/formats.rs (`schema_classes` and
// its `GrammarReader`: the record grammar of `Formats/Latest`, which rvt-rs credits to STE1200, Discussion #112).
// Changed: rewritten for BIMCraft; only the class list is kept (name, tag, base, version, field count), field names and
// types are checked for the grammar and dropped, every read is bounds-checked, and nesting is capped; then rewritten in C++
// for MiniBIM.
//
// Formats/Latest：Revit 嵌在每个文件里的类表。每个类有一个标签（tag）：它在表里的位置（第一个类是 12，之后每个定义加一，
// 内嵌的也算），分区里的记录用标签指明它的类。标签随版本变，所以按名字在文件自己的类表里找，不假定数值。
//
// 语法（rvt-rs，来自 STE1200 在 2014 到 2026 的类表上的测量，BIMCraft 在四个样例上核对过）：记录之间是一个为 0 的 u16；
// 一个类是带计数（u16）的名字、基类引用（u16，第 15 位置位表示基类紧接着内嵌定义，前面有一个为 0 的 u16）、u32 版本、
// u32 字段数、各字段（u32 计数的名字与类型）、u32 计数的 16 字节 GUID 列表。类型是一个 u32（种类字节与修饰字节）：
// 修饰的高半字节为 0x10 时多一个 u32 的数组长度，种类 0x0d 多 `01 00 00 00 20` 与内层类型（内层有类引用时再跟一个），
// 种类 0x0e 按值时多一个类引用。类引用是 u16，第 15 位置位表示内嵌定义。标签本身不存；内嵌引用重复它，读的时候核对，
// 读错时停下而不是把后面的标签都错开
#pragma once

#include "Rvt/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MiniBIM::Rvt
{
    class Revit;

    struct Schema
    {
        struct Class
        {
            uint16_t                tag = 0;
            std::string             name;
            std::optional<uint16_t> base;   // 基类的标签
            uint32_t                version    = 0;
            uint32_t                fieldCount = 0;
            bool operator==(const Class&) const = default;
        };

        std::vector<Class>         classes;   // 按标签排
        size_t                     parsedBytes = 0;   // 流结束或某条记录不合语法之前读了多少字节
        std::optional<std::string> stopped;           // 为什么提前停下；读到末尾时为空

        const Class*            ByTag(uint16_t tag) const;
        std::optional<uint16_t> Tag(std::string_view name) const;

        // 读解开并去掉页的 Formats/Latest。不合语法的记录之前的类留下，stopped 说为什么停在那里
        static Schema Parse(std::span<const uint8_t> data);
    };

    // 读 Formats/Latest；类表可以提前停下（stopped），一个类也没有时是错
    bool ReadSchema(Revit& r, Schema& out, Error& error);
}
