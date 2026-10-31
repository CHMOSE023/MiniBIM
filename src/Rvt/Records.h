// 移植自 BIMCraft crates/rvt/src/records.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_element_records.rs (the
// 64-bit element record, its fixed offsets, the counted reference lists after the box, the record chain and the
// second-prologue rule), src/partition_element_records_2023.rs (the 32-bit form, 2015 to 2023) and
// src/partition_type_records.rs (the bounding-box-less record of a type).
// Changed: rewritten for BIMCraft; the record marker always comes from the file's own schema, every offset is
// bounds-checked, records are handed to a callback instead of collected whole, and one function reads either form; then
// rewritten in C++ for MiniBIM.
//
// 分区（Partitions/NN）里每个元素一条记录，靠标记找到。记录开头是头（ElementId、记录大小、一个常数、一个计数），
// 结尾是重复大小的 16 字节尾巴。2024 起记录的框架从开头算有固定偏移：BuiltInCategory（负的 i64，Autodesk 公开的 API 常数）
// 在 +0x12，容器引用在 +0x32，放置种类在 +0x42，类标签在 +0x4a，标记在 +0x50（类表里 Outline 与 ElementParents 的标签，
// 中间四个 ff），+0x58 是六个 f64 的包围盒（英尺），之后两个带计数的 ElementId 列表：第一个里有元素的类型、约束到的标高、
// 连接的墙，门窗的宿主是它自己的 id 前面那一格；第二个的最后一格是草图线的所有者。有的框架没有自己的 ElementId
// （第二序言）：它在另一条记录的身体里，用那条记录的 id。到 2023 版 ElementId 是标记前 52 字节的 u32，类别在标记前 38 字节，
// 列表里是 u32。
//
// 放置的实例：没有容器、种类是放置的记录。类型也有记录，没有包围盒：它的标记只有后六个字节，放置种类是 0xffff8080，
// 格子列表里有它自己的 id
#pragma once

#include "Rvt/Error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace MiniBIM::Rvt
{
    struct Schema;

    namespace Records
    {
        // 2024 起框架里的偏移
        inline constexpr size_t CategoryAt  = 0x12;
        inline constexpr size_t ContainerAt = 0x32;
        inline constexpr size_t KindAt      = 0x42;
        inline constexpr size_t MarkerAt    = 0x50;
        // 放置的实例的种类，以及"没有容器"
        inline constexpr uint32_t KindPlaced  = 0xffff'ef7f;
        // 类型的记录的种类
        inline constexpr uint32_t KindType    = 0xffff'8080;
        inline constexpr uint64_t NoContainer = ~0ull;
        // 读取器当作长度的值离零最远多少英尺。Revit 的工作范围是离原点 20 英里（105,600 英尺），更远的有限值是损坏；
        // 换成毫米后也远在模型自己的上限之内，恶意的数进不了导入器向模型要的几何
        inline constexpr double MaxFeet = 3.0e5;
        // 2023 及以前：从标记往回的偏移
        inline constexpr size_t CategoryBeforeMarker = 38;
        inline constexpr size_t HeaderTagAfterId     = 8;

        bool InBand(int64_t category);
        bool Declared(const std::vector<uint32_t>& ids, uint32_t id);
        // 没有自己的 ElementId 的框架：0，或不是 u32 ElementId 的值
        inline bool NoId(uint64_t raw) { return raw == 0 || raw >= 0xffff'ffffull; }
    }

    // 扫描时从中读文件布局的类的标签。类的标签是它在类表里的位置，随版本变，所以都按名字找
    struct Tags
    {
        uint16_t                outline = 0;
        uint16_t                parents = 0;
        uint16_t                header  = 0;
        uint16_t                plane   = 0;
        uint16_t                cellList = 0;                // CellList：元素序列化数据的开头
        std::optional<uint16_t> verticalRegions;             // VerticalRegionsStructure：框住类型的层数
        std::optional<uint16_t> material;                    // Material：材质对象的类
        std::optional<uint16_t> physicalParamSet;            // PhysicalParamSet：材质自己的名字在它前面结束
        std::optional<uint16_t> patternHelper;               // PatternHelper：框住给材质命名的字符串
        std::optional<uint16_t> profileRoof;                 // ProfileRoof：迹线屋顶
        std::optional<uint16_t> roof;                        // Roof：拉伸屋顶

        static bool Of(const Schema& schema, Tags& out, Error& error);
        // `[Outline] ff ff ff ff [ElementParents]`：元素记录的标记
        std::array<uint8_t, 8> Marker() const;
        // `05 00 00 00` 与 Plane 的标签：标高的高程前面的
        std::array<uint8_t, 6> ElevationMarker() const;
        // 元素序列化数据开头的头：`ff ff ff ff`、CellList 的标签、`01 00 00 00`。后面是元素的 id，2024 起 u64，之前 u32
        std::array<uint8_t, 10> DataHeader() const;
    };

    // 元素的一条记录：在哪、是什么、包围盒与引用列表
    struct Record
    {
        uint32_t              partition = 0;   // 所在分区的编号（Partitions/21 是 21）：后面的分区存着编辑过的元素的新副本
        size_t                offset    = 0;   // 框架在分区里开始的位置
        uint32_t              id        = 0;
        int64_t               category  = 0;
        uint16_t              classTag  = 0;
        bool                  placed    = false;   // 没有容器、种类是放置的：模型里单独的实例
        std::array<double, 6> bbox{};              // 模型的包围盒（英尺）：最小 x、y、z，最大 x、y、z
        std::vector<uint64_t> refs;                // 第一个引用列表，照原样
        std::vector<uint64_t> refs2;               // 第二个引用列表，照原样
        bool                  inside = false;      // 框架没有自己的 id：它在另一条记录的身体里（第二序言），用那条的 id

        // 第一个列表里它自己的 id 前面那一格：门窗的宿主墙
        std::optional<uint32_t> Preceding() const;
        // 包围盒在各轴上的长度
        std::array<double, 3> Extent() const;
        bool operator==(const Record&) const = default;
    };

    // 没有包围盒的记录：类型（墙、楼板、屋顶、天花板等的，种类 0xffff8080）、材质（OST_Materials，0xffff8020）之类
    struct TypeRecord
    {
        uint32_t partition = 0;
        uint32_t id        = 0;
        int64_t  category  = 0;
        uint32_t kind      = 0;

        bool IsType() const { return kind == Records::KindType; }
        bool operator==(const TypeRecord&) const = default;
    };

    namespace Records
    {
        // 分区的记录链里的一条：开始、结束与 ElementId
        struct Link
        {
            size_t   start = 0;
            size_t   end   = 0;
            uint64_t id    = 0;
        };

        // 2024 起分区开头的记录链：头（u64 id、u32 大小、u16 常数、u16 计数）、身体、重复大小的 16 字节尾巴。常数是标记最后的标签减 12
        std::vector<Link> Chain(std::span<const uint8_t> buf, const std::array<uint8_t, 8>& marker);
        // 包着 at 的那条记录
        const Link* Enclosing(const std::vector<Link>& chain, size_t at);

        using KeepRecord = std::function<void(Record)>;
        using KeepType   = std::function<void(const TypeRecord&)>;
        // 2024 起分区里的记录，逐条交给 keep
        void Records64(std::span<const uint8_t> buf, const Tags& tags, const std::vector<uint32_t>& ids, uint32_t partition, const KeepRecord& keep);
        // 2023 及以前分区里的记录
        void Records32(std::span<const uint8_t> buf, const Tags& tags, const std::vector<uint32_t>& ids, uint32_t partition, const KeepRecord& keep);
        // 2024 起没有包围盒的记录
        void Types64(std::span<const uint8_t> buf, const Tags& tags, const std::vector<uint32_t>& ids, uint32_t partition, const KeepType& keep);
        // 2023 及以前没有包围盒的记录：像标高的那样从标记往回排
        void Types32(std::span<const uint8_t> buf, const Tags& tags, const std::vector<uint32_t>& ids, uint32_t partition, const KeepType& keep);
    }
}
