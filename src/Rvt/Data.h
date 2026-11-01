// 移植自 BIMCraft crates/rvt/src/data.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_names.rs (the element-data
// header and its id), src/partition_beam_axes.rs (`scan_first_records`: an element's data runs to the next header) and
// src/partition_materials.rs (a material object and its name, RE-58, RE-116).
// Changed: rewritten for BIMCraft; the header tag and the tags that frame a material's name come from the file's own schema,
// every copy of an element's data is kept with its partition (the caller picks), and a material's name is read by the rules
// the samples confirm (its own name field, the name that ends at `PhysicalParamSet`, the first string framed by
// `PatternHelper`, the first `-1001203` entry); then rewritten in C++ for MiniBIM.
//
// 元素的序列化数据与材质的名字。
//
// 元素除了记录（见 Rvt/Records.h）还有数据：参数、定位线、类型的层。它以 `ff ff ff ff`、CellList 的标签、`01 00 00 00`
// 与元素的 id（2024 起 u64，之前 u32 再加四个字节）开头，到下一个这样的头为止。前一次保存后改过的元素在后面的分区里再写一份，
// 前面那份还在，所以读出每一份并带上分区的编号。
//
// 材质是以 id 为键的对象（`01 00 00 00` 与 id），类表里 Material 的标签在固定的距离之后（u64 id 后 0x47 字节，u32 id 后 0x27）。
// 名字依次取：类标签后它自己的名字字段（两到四个为 0 的 u32 再一个带计数的字符串，后面跟的是内置参数 id 时不算）；
// 恰好在 `ff ff ff ff` 与 PhysicalParamSet 的标签之前结束的那一个带计数的字符串；PatternHelper 的标签框住的第一个字符串；
// 第一条内置参数 -1001203；2024 起还有第一个以八个 0 字节、一个 id 与八个 ff 收尾的字符串。各份副本不一致的 id 没有名字，
// 两个材质会同名的名字也不要：Revit 的材质名不重复，其中一个读错了。
//
// 房间的编号与名称是房间对象（`01 00 00 00` 与它的 id，2023 及以前 u32，之后 u64）里的参数条目：第一个 ROOM_NUMBER（-1006901）、
// 一个带计数的 UTF-16 字符串，紧接着 ROOM_NAME（-1006900）与另一个（参数 id 与该版本的 ElementId 一样宽），都在 id 之后 0x400 字节内。
// 空的文字计数为 0。以最新的分区的副本为准，空的也算。
//
// 柱的记录的包围盒可能从 0.0 开始而柱在更高处开始：柱的几何节点里的包围盒更准。节点是柱的 u64 id、u64 3 与 u64 -1，id 前 96 与
// 48 字节各有一个六个 f64 的包围盒；第二个是 Revit 的 z 范围，两个的平面范围都等于记录的。
//
// 没有自己材质的层取它类别的材质，在文档的对象样式里：到 2024 是类别的 id、一个未设的 id、`01 00 00 00`、另一个未设的 id 与材质
// （都是 u64）；之前从 id 的 i32 起是一个未设的字、1 或 2、`01 00 00 00`、`ff ff ff ff` 与 PatternHelper 的标签、八个字节、
// `i32 -3000010` 与材质（u32）。只在写到的每一处都一样时才读
#pragma once

#include "Rvt/Names.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace MiniBIM::Rvt
{
    struct Tags;
}

namespace MiniBIM::Rvt::Data
{
    // 元素数据的一份副本：所在分区的编号，以及从它的 id 之后到下一个元素的头（或窗口）为止的字节
    struct Copy
    {
        uint32_t             partition = 0;
        std::vector<uint8_t> bytes;

        bool operator==(const Copy&) const = default;
    };

    // 每个要的元素的各份数据
    using Elements = std::map<uint32_t, std::vector<Copy>>;

    // 元素的数据最多读多远
    inline constexpr size_t Window = 0x4'0000;

    // 分区里 wanted 那些元素的数据
    void ElementData(std::span<const uint8_t> buf, const Tags& tags, bool wide, const std::set<uint32_t>& wanted, uint32_t partition, Elements& out);

    // read 读得出东西的最新一份副本，以及更早的副本是否读得不一样
    template <class T>
    std::optional<std::pair<T, bool>> Newest(const std::vector<Copy>& copies, const std::function<std::optional<T>(std::span<const uint8_t>)>& read)
    {
        std::vector<std::pair<uint32_t, T>> all;
        for (const Copy& c : copies)
            if (auto v = read(c.bytes))
                all.emplace_back(c.partition, std::move(*v));
        if (all.empty())
            return std::nullopt;
        std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        T last = std::move(all.back().second);
        all.pop_back();
        bool differs = false;
        for (const auto& [partition, v] : all)
            differs = differs || !(v == last);
        return std::pair<T, bool>{ std::move(last), differs };
    }

    // 对象样式给 categories 的材质，照分区里写的：每个类别找到的所有值
    void CategoryMaterials(std::span<const uint8_t> buf, const Tags& tags, uint32_t release, const std::vector<int64_t>& categories,
                           std::map<int64_t, std::set<uint64_t>>& out);

    // 柱的几何节点的两个包围盒
    using Box  = std::array<double, 6>;
    using Node = std::pair<Box, Box>;

    // 分区里 targets 中的柱的每个几何节点
    void ColumnNodes(std::span<const uint8_t> buf, const std::map<uint32_t, Box>& targets, std::map<uint32_t, std::vector<Node>>& out);
    // 几何节点给出且一致的每根柱的底（英尺）：只给记录的包围盒的起点与节点的不同、平面与顶相同的柱
    std::map<uint32_t, double> ColumnBottoms(const std::map<uint32_t, std::vector<Node>>& nodes, const std::map<uint32_t, Box>& targets);

    // 分区里 rooms 中每个房间的编号与名称
    void RoomEntries(std::span<const uint8_t> buf, bool wide, const std::set<uint32_t>& rooms, uint32_t partition, Names::Latest<std::pair<std::string, std::string>>& out);

    // 分区里材质的名字：每个 id 取它的对象给的名字，这里的几份副本不一致时为空
    void MaterialNames(std::span<const uint8_t> buf, const Tags& tags, bool wide, const std::vector<uint32_t>& ids, std::map<uint32_t, std::optional<std::string>>& out);
}
