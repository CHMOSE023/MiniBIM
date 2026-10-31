// 移植自 BIMCraft crates/rvt/src/inventory.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_element_records.rs (the
// 64-bit element record, its fixed offsets, the record chain and the second-prologue rule, the BuiltInCategory names),
// src/partition_element_records_2023.rs (the 32-bit form, 2015 to 2023) and src/partition_level_records.rs (Level records and
// their name and elevation blocks).
// Changed: rewritten for BIMCraft; the record marker always comes from the file's own schema, every offset is bounds-checked,
// only the id, category, class and whether the record is a placed instance are kept (no bounding boxes, no reference
// lists), the walk over a partition is one function per record form, and a Level's Building Story setting and the Levels that
// sit inside other elements (RE-51, RE-118) are read from the same blocks; then rewritten in C++ for MiniBIM.
//
// 元素清单：项目里有哪些元素（按 BuiltInCategory），以及它的标高。
//
// 记录的找法见 Rvt/Records.h。放置的实例是没有容器、种类是放置的记录：族的类型与组的成员也有记录，只数放置的实例
// 才与 Revit 自己导出 IFC 时写出的元素对得上。
//
// 标高没有包围盒，所以没有 Outline 标记：靠它的类别找到，名字与高程在另一个以它的 ElementId 为键的块里（一串 ff、
// 带计数的 UTF-16 名字，然后是出现两次的高程，英尺）。名字之后块里有 Revit 的"建筑楼层"设置，说这个标高是不是楼层。
// 在别的元素里的标高的块，那串 ff 被那个元素的 id 打断；它不是项目的标高
#pragma once

#include "Rvt/Data.h"
#include "Rvt/Error.h"
#include "Rvt/Records.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace MiniBIM::Rvt
{
    class Revit;

    // OST_Levels
    inline constexpr int64_t OstLevels = -2'000'240;
    // 知道记录形式的版本（rvt-rs 的），其中 2023 到 2026 在这里用样例文件核对过
    inline constexpr uint32_t FirstRelease = 2015;
    inline constexpr uint32_t LastRelease  = 2027;

    // 一个元素：ElementId、BuiltInCategory、类（文件类表里的标签）与是不是放置的实例。在几条记录里见到的元素是一个元素，
    // 其中任一条是放置的就算放置的
    struct Element
    {
        int64_t  category = 0;
        uint32_t id       = 0;
        uint16_t classTag = 0;
        bool     placed   = false;

        auto operator<=>(const Element&) const = default;
    };

    // 标高，高程照存的（英尺）
    struct Level
    {
        uint32_t            id = 0;
        std::string         name;
        double              elevationFeet = 0;
        std::optional<bool> buildingStory;   // Revit 的"建筑楼层"设置（块里有时）：Revit 自己导出 IFC 时只有建筑楼层才是楼层

        // 高程（毫米，1 英尺正好 304.8 毫米）
        double ElevationMm() const { return elevationFeet * 304.8; }
        bool operator==(const Level&) const = default;
    };

    // 一个类别有几个元素，其中几个是放置的实例
    struct Count
    {
        int64_t category = 0;
        size_t  elements = 0;
        size_t  placed   = 0;
        bool operator==(const Count&) const = default;
    };

    struct Inventory
    {
        size_t                declared = 0;       // 元素表声明的 ElementId 个数
        size_t                records  = 0;       // 读到的元素记录（见到两次的元素合并之前）
        std::vector<Element>  elements;           // 每个元素与类别一个，按类别再按 id 排。标高也在，类别是 OST_Levels
        std::vector<Level>    levels;             // 读得出名字与高程的标高，按高程排
        size_t                unreadLevels = 0;   // 读不出名字块的标高元素（放置的实例），不算在别的元素里的
        std::vector<uint32_t> ownedLevels;        // 块里写着包着它的另一个元素的标高元素：不是项目的标高，也不在 levels 里

        // 每个类别的元素数，按类别 id 排
        std::vector<Count> Counts() const;
        bool operator==(const Inventory&) const = default;
    };

    // BuiltInCategory 的名字（知道时）：id 是 Autodesk 公开的 API 常数，名字是 Revit 给类别起的（rvt-rs 在 Revit 自己导出的
    // 样例 IFC 里量的）
    const char* CategoryName(int64_t id);

    // 读项目的每个元素与它的标高。文件的版本必须是知道记录形式的（FirstRelease 到 LastRelease）；之外的是错，不猜
    bool ReadInventory(Revit& r, Inventory& out, Error& error);

    // 一遍读完项目的分区得到的：清单，以及要的那些类别的记录
    struct Surveyed
    {
        uint32_t                release = 0;
        Tags                    tags;
        std::vector<uint32_t>   ids;       // 元素表声明的 ElementId，排好序
        Inventory               inventory;
        std::vector<Record>     records;   // 要的类别的每条记录的每一份（改过的元素在几个分区里）
        std::vector<TypeRecord> types;     // 每个类别的每个类型的记录
    };

    // ReadInventory，以及 keep 说要的那些类别的记录
    bool Survey(Revit& r, const std::function<bool(int64_t)>& keep, Surveyed& out, Error& error);

    // 第二遍要读的
    struct Wants
    {
        std::set<uint32_t>                         data;        // 留下数据的元素
        std::set<uint32_t>                         materials;   // 材质的 id
        std::map<uint32_t, std::array<double, 6>>  columns;     // 柱与它们记录的包围盒
        std::set<uint32_t>                         rooms;       // 房间
    };

    // 第二遍找到的
    struct Gathered
    {
        Data::Elements                  data;
        std::map<uint32_t, std::string> materials;   // 名字读到一次且只有它用的每个材质的名字
        std::map<int64_t, uint32_t>     category;    // 对象样式给类别的材质（给了时）：没有自己材质的层用它
        // 每个族与族类型的名字与类别（2024 起），照最新的分区
        std::map<uint32_t, std::pair<std::string, std::optional<int64_t>>> names;
        std::map<uint32_t, double>                                         columnBottoms;   // 几何节点修正了记录包围盒的柱的底（英尺）
        std::map<uint32_t, std::pair<std::string, std::string>>            rooms;           // 每个房间的编号与名称
    };

    // wants 里那些元素的数据（每一份）与项目材质的名字：为记录提到的东西再读一遍分区
    bool Gather(Revit& r, uint32_t release, const Tags& tags, const std::vector<uint32_t>& ids, const Wants& wants, Gathered& out, Error& error);
}
