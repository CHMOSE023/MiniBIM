// 移植自 BIMCraft crates/rvt/src/types.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_compound_structure.rs (a
// type's layers and a wall's orientation, RE-53, RE-112), src/partition_names.rs (the name a type gives itself in its data,
// RE-111) and src/partition_type_records.rs (the one type a record's reference list names, RE-28).
// Changed: rewritten for BIMCraft; the readers take the bytes of one element's data, every offset is bounds-checked, the frame
// tags come from the file's own schema instead of a table per release, and a name is looked for only in the type's own data;
// then rewritten in C++ for MiniBIM.
//
// 元素的数据关于它的类型说了什么：类型的名字、层，以及墙的哪一面是外侧。
//
// 类型有自己的记录，没有包围盒（见 Rvt/Records.h 的 TypeRecord），它的序列化数据以元素数据的头与 id 开头。在这些数据里：
// - **名字**是第一个框住的字符串：`ff ff ff ff`、u16 的字段标签（不会是 0xffff）、u32 的 UTF-16 码元数与名字；
// - **层**（墙、楼板、屋顶或天花板的类型）是 u32 的个数与每层一条记录，外侧（或上面）在前：宽度（英尺，只有膜为 0）、
//   材质的 ElementId（全 1：按类别）、压型板轮廓的 id，以及功能（1 结构、2 衬底、3 保温或空气层、4 与 5 面层、100 膜、
//   200 结构压型板）。个数前面是 `ff ff ff ff` 与 VerticalRegionsStructure 的标签，或者类型的名字与一个为 0 的 u32；
// - **墙**的数据在 `ff ff ff ff 01 00 00 00` 之后有三个 u32：定位线（0 到 5）、一个 0 到 2 的字与翻转标志。标志设了时
//   墙的外侧（第一层）在定位线方向的右边
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace MiniBIM::Rvt::Types
{
    // 在类型的数据里找层或墙的朝向时看多远
    inline constexpr size_t LayerWindow = 0x2000;

    // 某个版本里层记录各字段的位置
    struct LayerLayout
    {
        size_t recordLen  = 0;       // 每条记录的字节数
        size_t functionAt = 0;       // u32 的功能
        size_t materialAt = 0;       // 材质的 ElementId
        size_t deckAt     = 0;       // 压型板轮廓的 ElementId
        bool   wideIds    = false;   // id 是 u64（Revit 2024 起）而不是 u32

        bool operator==(const LayerLayout&) const = default;
    };

    // 某个版本的层记录布局
    std::optional<LayerLayout> LayoutOf(uint32_t release);

    // 类型的复合结构里的一层
    struct CompoundLayer
    {
        double                  widthFeet = 0;
        std::optional<uint32_t> material;       // 层的材质；取类别的材质时为空
        uint32_t                function = 0;   // 1 结构、2 衬底、3 保温或空气层、4 与 5 面层、100 膜、200 结构压型板；0 未指定

        bool operator==(const CompoundLayer&) const = default;
    };

    // 类型数据里第一个框住的层列表。tag 是类表里 VerticalRegionsStructure 的标签（小端两个字节）
    std::optional<std::vector<CompoundLayer>> FindLayers(std::span<const uint8_t> data, std::array<uint8_t, 2> tag, const LayerLayout& layout,
                                                         const std::set<uint32_t>& materials, const std::vector<uint32_t>& ids);

    // 墙的数据关于定位线与两面说了什么
    struct WallOrientation
    {
        uint32_t locationLine = 0;       // Revit 的定位线设置，0 到 5。存的线不论它是什么都是墙的中心线
        uint32_t word         = 0;       // 0 到 2 的一个字。Revit 2015 到 2020 不写它，读作 0
        bool     flip         = false;   // 设了时墙的外侧在定位线方向的右边；没设时在左边

        bool operator==(const WallOrientation&) const = default;
    };

    // 墙的朝向：没有锚点或有墙不会取的值时为空。翻转是一个字节；之后的字节是第二个标志（0 后跟 `00 00` 或 1 后跟 `ff ff`），不读
    std::optional<WallOrientation> Orientation(std::span<const uint8_t> data, uint32_t release);

    // 元素数据里第一个框住的名字：`ff ff ff ff`、u16 标签（不会是 0xffff）、u32 长度与名字。另一个元素的元素数据头 header
    // 不算框。2023 以前一个字符的名字不要：那是只有一个 id 的列表的 id 读成了名字
    std::optional<std::string> FirstName(std::span<const uint8_t> data, const std::array<uint8_t, 10>& header, uint32_t release);

    // refs 里唯一一个属于该类别类型（types）的 id：记录的引用列表恰好提到一次它放置自的类型。一个也没有或不止一个时为空
    std::optional<uint32_t> UniqueType(const std::vector<uint64_t>& refs, const std::set<uint32_t>& types);
}
