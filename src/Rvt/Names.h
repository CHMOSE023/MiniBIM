// 移植自 BIMCraft crates/rvt/src/names.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/partition_names.rs (the name entries
// of families and family types, RE-38, RE-109, RE-145).
// Changed: rewritten for BIMCraft; both entry forms are read by one module, every offset is bounds-checked, and the newest
// partition's entries win; then rewritten in C++ for MiniBIM.
//
// 族与族类型的名字。2024 起分区里每个载入的族与族类型有一条名字条目：元素的 u64 id、UTF-16 码元数、名字、元素的
// BuiltInCategory，靠类别（最高五个字节是 ff）找到。到 2023 版条目是 `01 00 00 00`、u32 id、u32 长度与名字，没有类别。
// 前一次保存之后改过的元素在后面的分区里还有一条：最新的分区的那条才算
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace MiniBIM::Rvt::Names
{
    // 一条名字条目
    struct Entry
    {
        uint32_t               id = 0;
        std::string            name;       // UTF-8
        std::optional<int64_t> category;   // 元素的 BuiltInCategory：32 位形式里没有

        bool operator==(const Entry&) const = default;
    };

    // 2024 起分区里 ids 声明过的条目
    void Entries64(std::span<const uint8_t> buf, const std::vector<uint32_t>& ids, std::vector<Entry>& out);
    // 2023 及以前分区里的条目
    void Entries32(std::span<const uint8_t> buf, const std::vector<uint32_t>& ids, std::vector<Entry>& out);

    // 一个 id 在最新的分区里的值；同一分区的几份副本不一致时没有
    template <class V>
    class Latest
    {
    public:
        void Offer(uint32_t id, uint32_t partition, V value)
        {
            const auto it = m_found.find(id);
            if (it != m_found.end() && it->second.first == partition)
            {
                if (!it->second.second || !(*it->second.second == value))
                    it->second.second.reset();
            }
            else if (it == m_found.end() || it->second.first < partition)
                m_found.insert_or_assign(id, std::pair{ partition, std::optional<V>(std::move(value)) });
        }

        std::map<uint32_t, V> Values() const
        {
            std::map<uint32_t, V> out;
            for (const auto& [id, held] : m_found)
                if (held.second)
                    out.emplace(id, *held.second);
            return out;
        }

    private:
        std::map<uint32_t, std::pair<uint32_t, std::optional<V>>> m_found;
    };
}
