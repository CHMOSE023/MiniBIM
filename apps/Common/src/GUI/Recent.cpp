// 移植自 BIMCraft crates/ui-egui/src/recent.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/Recent.h"
#include "I18n/Translator.h"
#include "Model/Json.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace MiniBIM
{
    namespace
    {
        std::filesystem::path ToPath(const std::string& utf8)
        {
            return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }

        std::string ToUtf8(const std::filesystem::path& p)
        {
            const std::u8string u8 = p.u8string();
            return std::string(u8.begin(), u8.end());
        }

        // 同一个文件经别的写法（相对路径、..）到达时是同一项
        std::string Absolute(const std::string& path)
        {
            std::error_code ec;
            const std::filesystem::path p = std::filesystem::absolute(ToPath(path), ec);
            return ec ? path : ToUtf8(p.lexically_normal());
        }

        std::string FileName(const std::string& path)
        {
            const std::string name = ToUtf8(ToPath(path).filename());
            return name.empty() ? path : name;
        }
    }

    RecentFiles RecentFiles::Load(const std::string& path)
    {
        RecentFiles recent;
        std::error_code ec;
        const std::filesystem::path p = ToPath(path);
        if (!std::filesystem::is_regular_file(p, ec) || std::filesystem::file_size(p, ec) > MaxFileBytes || ec)
            return recent;
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        const Bim::Json j = Bim::Json::parse(ss.str(), nullptr, false);
        if (!j.is_object() || !j.contains("files") || !j["files"].is_array())
            return recent;
        // 从最旧的推起，最后最新的在最上面
        const Bim::Json& files = j["files"];
        for (auto it = files.rbegin(); it != files.rend(); ++it)
            if (it->is_string() && !it->get<std::string>().empty())
                recent.Push(it->get<std::string>());
        return recent;
    }

    bool RecentFiles::Save(const std::string& path, std::string& error) const
    {
        const std::filesystem::path target = ToPath(path);
        std::error_code ec;
        if (target.has_parent_path())
            std::filesystem::create_directories(target.parent_path(), ec);
        std::filesystem::path part = target;
        part += ".part";
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            out << Bim::Json{ { "files", m_paths } }.dump(2) << '\n';
            if (!out)
            {
                error = ToUtf8(part) + ": can't be written";
                return false;
            }
        }
        std::filesystem::rename(part, target, ec);
        if (ec)
        {
            error = path + ": " + ec.message();
            std::filesystem::remove(part, ec);
            return false;
        }
        return true;
    }

    void RecentFiles::Push(const std::string& path)
    {
        const std::string p = Absolute(path);
        m_paths.erase(std::remove(m_paths.begin(), m_paths.end(), p), m_paths.end());
        m_paths.insert(m_paths.begin(), p);
        if (m_paths.size() > MaxFiles)
            m_paths.resize(MaxFiles);
    }

    void RecentFiles::Remove(const std::string& path)
    {
        const std::string p = Absolute(path);
        m_paths.erase(std::remove_if(m_paths.begin(), m_paths.end(), [&](const std::string& q) { return q == path || q == p; }), m_paths.end());
    }

    std::vector<std::string> RecentFiles::Labels() const
    {
        std::vector<std::string> out;
        for (const std::string& p : m_paths)
        {
            const std::string n = FileName(p);
            const auto same = std::count_if(m_paths.begin(), m_paths.end(), [&](const std::string& q) { return FileName(q) == n; });
            const std::string folder = ToUtf8(ToPath(p).parent_path().filename());
            out.push_back(same < 2 || folder.empty() ? n : n + " (" + folder + ")");
        }
        return out;
    }

    std::string RecentFiles::Age(long long seconds)
    {
        const long long minutes = seconds / 60, hours = seconds / 3600, days = seconds / 86400;
        if (minutes < 1)
            return Tr("Just now");
        if (hours < 1)
            return Format(TrN("{0} min ago", "{0} min ago", minutes), { std::to_string(minutes) });
        if (days < 1)
            return Format(TrN("{0} h ago", "{0} h ago", hours), { std::to_string(hours) });
        if (days < 2)
            return Tr("Yesterday");
        if (days < 31)
            return Format(TrN("{0} day ago", "{0} days ago", days), { std::to_string(days) });
        if (days < 365)
            return Format(TrN("{0} month ago", "{0} months ago", days / 30), { std::to_string(days / 30) });
        return Format(TrN("{0} year ago", "{0} years ago", days / 365), { std::to_string(days / 365) });
    }
}
