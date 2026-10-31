// 移植自 BIMCraft crates/ui-egui/src/shortcuts.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/Shortcuts.h"
#include <algorithm>
#include <cctype>

namespace MiniBIM
{
    namespace
    {
        bool AlNum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
        char Upper(char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }

        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.remove_suffix(1);
            return s;
        }
    }

    bool Shortcuts::IsKey(std::string_view keys)
    {
        return keys.size() == 2 && AlNum(keys[0]) && AlNum(keys[1]);
    }

    std::string Shortcuts::Normalize(std::string_view keys)
    {
        std::string k(Trim(keys));
        std::transform(k.begin(), k.end(), k.begin(), Upper);
        return k;
    }

    void Shortcuts::SetDefaults(std::vector<std::pair<std::string, std::string>> defaults)
    {
        m_defaults = std::move(defaults);
    }

    std::string Shortcuts::Default(std::string_view keys) const
    {
        for (const auto& [k, command] : m_defaults)
            if (k == keys)
                return command;
        return {};
    }

    void Shortcuts::Set(std::string_view keys, const std::string& command)
    {
        const std::string k = Normalize(keys);
        if (!IsKey(k))
            return;
        if (const std::string d = Default(k); !d.empty() && d == command)
            m_custom.erase(k);
        else
            m_custom[k] = command;
    }

    void Shortcuts::Remove(std::string_view keys)
    {
        const std::string k = Normalize(keys);
        if (Default(k).empty())
            m_custom.erase(k);
        else
            m_custom[k] = std::string();
    }

    std::string Shortcuts::Find(std::string_view keys) const
    {
        const std::string k = Normalize(keys);
        if (const auto it = m_custom.find(k); it != m_custom.end())
            return it->second;
        return Default(k);
    }

    std::string Shortcuts::Conflict(std::string_view keys, std::string_view command) const
    {
        const std::string now = Find(keys);
        return now != command ? now : std::string();
    }

    std::vector<Shortcuts::Entry> Shortcuts::All() const
    {
        std::vector<Entry> all;
        for (const auto& [k, command] : m_defaults)
            if (!m_custom.contains(k) && std::none_of(all.begin(), all.end(), [&](const Entry& e) { return e.keys == k; }))
                all.push_back({ k, command, false });
        for (const auto& [k, command] : m_custom)
            if (!command.empty())
                all.push_back({ k, command, true });
        return all;
    }

    std::string Shortcuts::Type(char c)
    {
        if (!AlNum(c))
            return {};
        m_typed.push_back(Upper(c));
        if (m_typed.size() > 2)
            m_typed.erase(0, 1);
        if (m_typed.size() < 2)
            return {};
        std::string command = Find(m_typed);
        if (!command.empty())
            m_typed.clear();
        return command;
    }

    Bim::Json Shortcuts::ToJson() const
    {
        Bim::Json j = Bim::Json::object();
        for (const auto& [k, command] : m_custom)
            j[k] = command;
        return j;
    }

    void Shortcuts::FromJson(const Bim::Json& j)
    {
        m_custom.clear();
        if (!j.is_object())
            return;
        for (const auto& [k, command] : j.items())
            if (IsKey(k) && k == Normalize(k) && command.is_string())
                m_custom[k] = command.get<std::string>();
    }

    std::vector<std::pair<std::string, std::string>> Shortcuts::ParseFile(std::string_view content)
    {
        std::vector<std::pair<std::string, std::string>> entries;
        const Bim::Json j = Bim::Json::parse(content, nullptr, false);
        if (j.is_object())
        {
            for (const auto& [k, desc] : j.items())
                if (desc.is_string())
                    entries.emplace_back(Normalize(k), desc.get<std::string>());
            return entries;
        }
        size_t at = 0;
        while (at <= content.size())
        {
            size_t end = content.find('\n', at);
            if (end == std::string_view::npos)
                end = content.size();
            const std::string_view line = Trim(content.substr(at, end - at));
            at = end + 1;
            if (line.empty() || line.front() == '#')
                continue;
            if (line.find("<Shortcut") != std::string_view::npos)
            {
                auto attr = [&](std::string_view name) -> std::string_view
                {
                    const std::string pattern = std::string(name) + "=\"";
                    const size_t p = line.find(pattern);
                    if (p == std::string_view::npos)
                        return {};
                    const size_t from = p + pattern.size(), to = line.find('"', from);
                    return to == std::string_view::npos ? std::string_view() : line.substr(from, to - from);
                };
                std::string_view command = attr("CommandName");
                if (command.empty())
                    command = attr("CommandId");
                std::string_view keys = attr("Shortcuts");
                if (command.empty() || keys.empty())
                    continue;
                // 一条命令可以有几个键，用 # 隔开
                while (true)
                {
                    const size_t hash = keys.find('#');
                    entries.emplace_back(Normalize(keys.substr(0, hash)), std::string(command));
                    if (hash == std::string_view::npos)
                        break;
                    keys.remove_prefix(hash + 1);
                }
                continue;
            }
            // 第一个分隔符（: = - 或 en dash）前是键，后是说明；键两边的 * 是 Markdown 的粗体
            size_t sep = std::string_view::npos, len = 1;
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == ':' || line[i] == '=' || line[i] == '-')
                {
                    sep = i;
                    break;
                }
                if (line.substr(i, 3) == "\xE2\x80\x93")
                {
                    sep = i;
                    len = 3;
                    break;
                }
            }
            if (sep == std::string_view::npos)
                continue;
            std::string_view key = Trim(line.substr(0, sep));
            while (!key.empty() && key.front() == '*')
                key.remove_prefix(1);
            while (!key.empty() && key.back() == '*')
                key.remove_suffix(1);
            entries.emplace_back(Normalize(key), std::string(Trim(line.substr(sep + len))));
        }
        return entries;
    }

    std::pair<size_t, size_t> Shortcuts::Add(const std::vector<std::pair<std::string, std::string>>& parsed,
                                             const std::function<std::string(const std::string&)>& resolve)
    {
        size_t added = 0, skipped = 0;
        for (const auto& [keys, desc] : parsed)
        {
            const std::string command = resolve(desc);
            if (!command.empty() && IsKey(keys) && Conflict(keys, command).empty())
            {
                Set(keys, command);
                ++added;
            }
            else
                ++skipped;
        }
        return { added, skipped };
    }
}
