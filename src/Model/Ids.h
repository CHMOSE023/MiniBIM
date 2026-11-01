// 移植自 BIMCraft crates/core/src/ids.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// 每个元素有两个名字，同 Revit：ID 是从 FirstId 起的数，人、AI 代理输入它，明细表显示它，从别的文件进来的元素
// 原 ID 被占用时会给新的；GUID 生成一次、永不改变，凡是指向元素的地方（墙指标高、门指墙、尺寸指所量的东西）都存 GUID，
// 所以换了 ID 也不会断。
//
// 需要 GUID 的地方也可以先写一个数：指那个 ID 的元素，直到模型把它换成元素的 GUID（Guid::OfId、Model::Resolve）。
// 命令里写 "level": 100000 就是这样，旧文件（还没有 GUID 时）也这样打开
namespace MiniBIM::Bim
{
    struct Element;

    using Id = uint64_t;

    // 项目给出的第一个 ID：ID 有六位数，建过 90 万个元素后更多
    inline constexpr Id FirstId = 100000;
    // 链接模型的元素在视图所画的模型里的 ID 从这里起：超过项目自己的
    inline constexpr Id Linked = 100000000;

    // SplitMix64：把种子打散到全部 64 位
    uint64_t Mix(uint64_t z);

    // 元素的 GUID：128 位随机数（第 4 版 UUID），写作 xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx。高 64 位为 0 的
    // （随机生成的不会这样）代表低 64 位那个 ID 的元素，直到解析。默认值（全 0）不指任何元素，Model::Check 会报出来
    struct Guid
    {
        uint64_t hi = 0;
        uint64_t lo = 0;

        // 新的 GUID：系统随机数（取不到时用时钟、进程号和计数混合）
        static Guid New();
        // 由 seed 与 n 确定的 GUID，每次相同：文件里没有给出时用
        static Guid Seeded(uint64_t seed, uint64_t n);
        // ID 为 id 的元素，直到模型换成它的 GUID
        static constexpr Guid OfId(Id id) { return Guid{ 0, id }; }

        // 还不是 GUID 时它代表的 ID
        std::optional<Id> Pending() const { return hi == 0 ? std::optional<Id>(lo) : std::nullopt; }
        bool IsNil() const { return hi == 0 && lo == 0; }

        // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx，大小写均可，可带花括号；或者一串数字（ID）
        static std::optional<Guid> Parse(std::string_view s);
        // 还是 ID 时写成那个数
        std::string ToString() const;

        auto operator<=>(const Guid&) const = default;
    };

    struct GuidHash
    {
        size_t operator()(const Guid& g) const { return std::hash<uint64_t>()(g.hi ^ Mix(g.lo)); }
    };

    // 项目的元素，按 ID，各带 GUID。文件里是一个以 ID 为键的对象，每个元素带它的 guid。
    // 元素以 shared_ptr<const Element> 保存：复制模型（撤销快照）只复制指针，改动时才复制那一个元素（GetMut）
    class Elements
    {
    public:
        using Ptr = std::shared_ptr<const Element>;
        using Map = std::map<Id, Ptr>;

        const Element* Get(Id id) const;
        const Element* Get(Guid g) const;
        // 要改的元素：与撤销快照共用时先复制一份
        Element* GetMut(Id id);
        Element* GetMut(Guid g);
        bool Contains(Id id) const { return m_items.count(id) != 0; }
        bool Contains(Guid g) const;
        // k 找到的元素的 ID / GUID
        std::optional<Id>   IdOf(Guid g) const;
        std::optional<Guid> GuidOf(Id id) const;
        std::optional<Guid> GuidOf(Guid g) const;

        // 把 e 放在 id：那里已有元素时保留它的 GUID（元素就是这样修改的）；新元素得到自己的 GUID
        void Insert(Id id, Element e);
        // 以 GUID guid 放在 id；guid 是 ID 或别的元素已有时返回 false，原因放在 error
        bool InsertAs(Id id, Guid guid, Element e, std::string& error);
        bool InsertAs(Id id, Guid guid, Ptr e, std::string& error);
        // 一个没有元素用的 GUID
        Guid Fresh() const;
        bool Remove(Id id);
        bool Remove(Guid g);
        void Clear();

        const Map& Items() const { return m_items; }
        size_t     Size() const { return m_items.size(); }
        bool       Empty() const { return m_items.empty(); }
        // 最大的 ID；没有元素时为空
        std::optional<Id> LastId() const;

        // 同样的元素换成 to(id) 的 ID，GUID 不变；有两个撞到同一个 ID 时为空
        std::optional<Elements> Renumbered(const std::function<Id(Id)>& to) const;
        // 同样的元素：wanted 列出的 ID 换成它给的 GUID（是 GUID、又没被别的先占时），其余保留自己的
        Elements Regiven(const std::map<Id, Guid>& wanted) const;

        bool operator==(const Elements& other) const;

    private:
        std::optional<Id> Find(Guid g) const;

        Map                                    m_items;
        std::map<Id, Guid>                     m_guids;
        std::unordered_map<Guid, Id, GuidHash> m_ids;
    };
}
