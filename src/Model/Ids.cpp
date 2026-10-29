// 移植自 BIMCraft crates/core/src/ids.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Ids.h"
#include "Model/Model.h"
#include "Model/Text.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>

#ifdef _WIN32
#include <process.h>
#define MINIBIM_GETPID _getpid
#else
#include <unistd.h>
#define MINIBIM_GETPID getpid
#endif

namespace MiniBIM::Bim
{
    namespace
    {
        uint64_t Nanos()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        uint64_t RotateLeft(uint64_t x, int n)
        {
            return (x << n) | (x >> (64 - n));
        }

        // 第 4 版、变体 10
        Guid Versioned(uint64_t hi, uint64_t lo)
        {
            hi = (hi & ~uint64_t(0xF000)) | uint64_t(0x4000);
            lo = (lo & ~(uint64_t(0xC0) << 56)) | (uint64_t(0x80) << 56);
            return Guid{ hi, lo };
        }

        int HexDigit(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }
    }

    uint64_t Mix(uint64_t z)
    {
        z += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    Guid Guid::New()
    {
        uint64_t hi = 0, lo = 0;
        bool ok = false;
        try
        {
            std::random_device rd;
            hi = (uint64_t(rd()) << 32) | rd();
            lo = (uint64_t(rd()) << 32) | rd();
            ok = hi != 0 || lo != 0;
        }
        catch (...)
        {
        }
        if (!ok)
        {
            static std::atomic<uint64_t> count{ 0 };
            const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
            hi = Mix(Nanos() ^ RotateLeft(uint64_t(MINIBIM_GETPID()), 32));
            lo = Mix(hi ^ (n * 0x9E3779B97F4A7C15ull));
        }
        return Versioned(hi, lo);
    }

    Guid Guid::Seeded(uint64_t seed, uint64_t n)
    {
        const uint64_t hi = Mix(seed ^ Mix(n));
        const uint64_t lo = Mix(hi ^ RotateLeft(n, 29));
        return Versioned(hi, lo);
    }

    std::optional<Guid> Guid::Parse(std::string_view s)
    {
        s = Text::Trim(s);
        if (!s.empty() && s.size() <= 20 && s.find_first_not_of("0123456789") == std::string_view::npos)
        {
            // 一串数字是 ID（超出 64 位的不是）
            uint64_t v = 0;
            for (const char c : s)
            {
                const uint64_t d = static_cast<uint64_t>(c - '0');
                if (v > (UINT64_MAX - d) / 10)
                    return std::nullopt;
                v = v * 10 + d;
            }
            return OfId(v);
        }
        if (s.size() >= 2 && s.front() == '{' && s.back() == '}')
            s = s.substr(1, s.size() - 2);
        // 8-4-4-4-12
        if (s.size() != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
            return std::nullopt;
        uint64_t parts[2] = { 0, 0 };
        int digits = 0;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (i == 8 || i == 13 || i == 18 || i == 23)
                continue;
            const int d = HexDigit(s[i]);
            if (d < 0)
                return std::nullopt;
            uint64_t& part = parts[digits < 16 ? 0 : 1];
            part = (part << 4) | static_cast<uint64_t>(d);
            ++digits;
        }
        const Guid g{ parts[0], parts[1] };
        if (g.Pending())
            return std::nullopt;
        return g;
    }

    std::string Guid::ToString() const
    {
        if (const auto id = Pending())
            return std::to_string(*id);
        char h[33];
        std::snprintf(h, sizeof(h), "%016llx%016llx", static_cast<unsigned long long>(hi), static_cast<unsigned long long>(lo));
        const std::string x(h, 32);
        return x.substr(0, 8) + "-" + x.substr(8, 4) + "-" + x.substr(12, 4) + "-" + x.substr(16, 4) + "-" + x.substr(20, 12);
    }

    // ── Elements ─────────────────────────────────────────────────
    std::optional<Id> Elements::Find(Guid g) const
    {
        const auto it = m_ids.find(g);
        if (it != m_ids.end())
            return it->second;
        return g.Pending();
    }

    const Element* Elements::Get(Id id) const
    {
        const auto it = m_items.find(id);
        return it == m_items.end() ? nullptr : it->second.get();
    }

    const Element* Elements::Get(Guid g) const
    {
        const auto id = Find(g);
        return id ? Get(*id) : nullptr;
    }

    Element* Elements::GetMut(Id id)
    {
        const auto it = m_items.find(id);
        if (it == m_items.end())
            return nullptr;
        // 与撤销快照（或别的模型副本）共用时先复制
        if (it->second.use_count() > 1)
            it->second = std::make_shared<Element>(*it->second);
        return const_cast<Element*>(it->second.get());
    }

    Element* Elements::GetMut(Guid g)
    {
        const auto id = Find(g);
        return id ? GetMut(*id) : nullptr;
    }

    bool Elements::Contains(Guid g) const
    {
        const auto id = Find(g);
        return id && Contains(*id);
    }

    std::optional<Id> Elements::IdOf(Guid g) const
    {
        const auto id = Find(g);
        if (id && Contains(*id))
            return id;
        return std::nullopt;
    }

    std::optional<Guid> Elements::GuidOf(Id id) const
    {
        const auto it = m_guids.find(id);
        if (it == m_guids.end())
            return std::nullopt;
        return it->second;
    }

    std::optional<Guid> Elements::GuidOf(Guid g) const
    {
        const auto id = Find(g);
        return id ? GuidOf(*id) : std::nullopt;
    }

    void Elements::Insert(Id id, Element e)
    {
        if (!m_guids.count(id))
        {
            const Guid g = Fresh();
            m_guids[id] = g;
            m_ids[g] = id;
        }
        m_items[id] = std::make_shared<Element>(std::move(e));
    }

    bool Elements::InsertAs(Id id, Guid guid, Element e, std::string& error)
    {
        return InsertAs(id, guid, std::make_shared<Element>(std::move(e)), error);
    }

    bool Elements::InsertAs(Id id, Guid guid, Ptr e, std::string& error)
    {
        if (guid.Pending())
        {
            error = "element " + std::to_string(id) + ": " + guid.ToString() + " is an id, not a GUID";
            return false;
        }
        const auto other = m_ids.find(guid);
        if (other != m_ids.end() && other->second != id)
        {
            error = "elements " + std::to_string(other->second) + " and " + std::to_string(id) + " have the same GUID " + guid.ToString();
            return false;
        }
        const auto old = m_guids.find(id);
        if (old != m_guids.end())
            m_ids.erase(old->second);
        m_guids[id] = guid;
        m_ids[guid] = id;
        m_items[id] = std::move(e);
        return true;
    }

    Guid Elements::Fresh() const
    {
        for (;;)
        {
            const Guid g = Guid::New();
            if (!m_ids.count(g))
                return g;
        }
    }

    bool Elements::Remove(Id id)
    {
        const auto g = m_guids.find(id);
        if (g != m_guids.end())
        {
            m_ids.erase(g->second);
            m_guids.erase(g);
        }
        return m_items.erase(id) != 0;
    }

    bool Elements::Remove(Guid g)
    {
        const auto id = Find(g);
        return id && Remove(*id);
    }

    void Elements::Clear()
    {
        m_items.clear();
        m_guids.clear();
        m_ids.clear();
    }

    std::optional<Id> Elements::LastId() const
    {
        if (m_items.empty())
            return std::nullopt;
        return m_items.rbegin()->first;
    }

    std::optional<Elements> Elements::Renumbered(const std::function<Id(Id)>& to) const
    {
        Elements out;
        std::string error;
        for (const auto& [id, e] : m_items)
        {
            const auto g = GuidOf(id);
            if (!g)
                continue;
            if (out.Contains(to(id)))
                return std::nullopt;
            if (!out.InsertAs(to(id), *g, e, error))
                return std::nullopt;
        }
        return out;
    }

    bool Elements::operator==(const Elements& other) const
    {
        if (m_items.size() != other.m_items.size() || m_guids != other.m_guids)
            return false;
        for (auto a = m_items.begin(), b = other.m_items.begin(); a != m_items.end(); ++a, ++b)
            if (a->first != b->first || !(a->second == b->second || *a->second == *b->second))
                return false;
        return true;
    }
}
