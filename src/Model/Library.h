// 移植自 BIMCraft crates/core/src/library.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Ids.h"
#include <algorithm>
#include <compare>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

// 项目的库与元素一样有 ID 和 GUID：每个类型、材质、族、族类型、族参数（BIMCraft 还有图片、参数、视图过滤器……，移植到时再加）。
// ID 与元素的出自同一个计数（一个 ID 在整个项目里只指一样东西），从 LastId 往下数；GUID 生成一次、永不改变，改名也不变。
//
// 用到库条目的地方在内存里写它的名称（type_name、层的 material……），Library 知道哪个名称是哪个 GUID。
// 文件里这些名称一律换成 GUID（Model::ToFile），所以改名只改文件里的名称本身。名称不能是 GUID，两者不会混淆
namespace MiniBIM::Bim
{
    // 库的 ID 从这里往下数
    inline constexpr Id LastId = 999999;

    // 库条目是什么。文件里写作 "type:wall"、"material" 等；BIMCraft 有、这里还没移植的种类照样读得出来，
    // 打开文件后没有对应的东西，由 Model::SyncLibrary 去掉
    struct Kind
    {
        enum class Tag
        {
            Type,               // 某个类别的类型：wall、door……（category）
            Material,
            Picture,
            Cloud,
            Cad,
            Family,
            Parameter,
            ViewFilter,
            ViewTemplate,
            Phase,
            PhaseFilter,
            OptionSet,
            DesignOption,
            FamilyType,
            FamilyParameter,
            LinePattern,
            LineStyle,
            FillPattern,
            FilledRegionType,
            DetailGroup,
            TextType,
        };

        Tag         tag = Tag::Material;
        std::string category;

        static Kind Type(std::string category) { return Kind{ Tag::Type, std::move(category) }; }
        static Kind Of(Tag t) { return Kind{ t, {} }; }

        // 报错、列表里的叫法：wall type、material
        std::string Label() const;
        // 文件里的写法：type:wall、material
        std::string ToString() const;
        static std::optional<Kind> Parse(std::string_view s);

        auto operator<=>(const Kind&) const = default;
    };

    // 库条目：是什么、名称、GUID
    struct Entry
    {
        Kind        kind;
        std::string name;
        Guid        guid;

        bool operator==(const Entry&) const = default;
    };

    // 库条目的一部分，自己带 ID 和 GUID，在列表里的位置不变就一直保留：类型的层、族的形体与平面符号。
    // what 是什么，of 属于谁，在其中的位置
    struct Part
    {
        std::string what;
        std::string of;
        std::string owner;      // 所属类型或族的名称
        size_t      index = 0;
        Guid        guid;

        // 报错、列表里的叫法：layer 2 of wall type Exterior - 300mm
        std::string Name() const;
        bool operator==(const Part&) const = default;
    };

    // 库的 ID 和 GUID，按 ID；文件里是一个以 ID 为键的对象。部件的也在这里，部件自己也存着
    class Library
    {
    public:
        const std::map<Id, Entry>& Entries() const { return m_byId; }
        const Entry* Get(Id id) const;
        // 有条目或部件用着 id
        bool Taken(Id id) const { return m_byId.count(id) != 0 || m_parts.count(id) != 0; }
        const Part* GetPart(Id id) const;
        const std::map<Id, Part>& Parts() const { return m_parts; }
        std::optional<std::pair<Id, const Part*>> PartByGuid(Guid g) const;
        std::optional<Id>   IdOf(const Kind& kind, std::string_view name) const;
        std::optional<Guid> GuidOf(const Kind& kind, std::string_view name) const;
        std::optional<std::pair<Id, const Entry*>> ByGuid(Guid g) const;
        size_t Size() const { return m_byId.size(); }
        bool   Empty() const { return m_byId.empty(); }

        // 在 id 放一个条目；ID、名称或 GUID 与别的重复时返回 false，原因放在 error
        bool Insert(Id id, Entry e, std::string& error);
        // kind 的 old 改名为 new：同一个条目，ID 和 GUID 不变
        void Renamed(const Kind& kind, const std::string& oldName, const std::string& newName);
        // 族 old 改名为 new：连同它的类型与参数（"族: 类型"）
        void FamilyRenamed(const std::string& oldName, const std::string& newName);

        // 条目或部件用过的最小 ID（包括已经去掉的）
        std::optional<Id> LowestId() const;
        // 往下到 id 的 ID 都有东西占着，新的从它下面数
        void Hold(Id id) { m_lowest = m_lowest == 0 ? id : std::min(m_lowest, id); }
        void Remove(Id id);

        bool operator==(const Library& o) const { return m_byId == o.m_byId; }

    private:
        friend struct Model;

        std::map<Id, Entry>                          m_byId;
        std::map<std::pair<Kind, std::string>, Id>   m_byName;
        std::unordered_map<Guid, Id, GuidHash>       m_byGuid;
        std::map<Id, Part>                           m_parts;
        // 这个库占用过的最小 ID（条目或部件），即使现在已经没有了（还没有时为 0）：新的 ID 从它下面数，
        // 项目开着时删掉的东西的 ID 不会再给出去。文件里存的是在用的 ID，不存它
        Id                                           m_lowest = 0;
    };
}
