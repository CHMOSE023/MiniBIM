// 移植自 BIMCraft crates/ui-egui/src/revit.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/RevitReport.h"
#include "GUI/Parameters.h"
#include "I18n/Translator.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>

namespace MiniBIM::RevitReport
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

        // Rust 的 str::parse::<usize>：可以带一个 +，其余全是数字，溢出时读不出
        std::optional<size_t> ParseCount(std::string_view s)
        {
            if (!s.empty() && s.front() == '+')
                s.remove_prefix(1);
            if (s.empty())
                return std::nullopt;
            size_t n = 0;
            for (const char c : s)
            {
                if (c < '0' || c > '9')
                    return std::nullopt;
                const size_t d = static_cast<size_t>(c - '0');
                if (n > (SIZE_MAX - d) / 10)
                    return std::nullopt;
                n = n * 10 + d;
            }
            return n;
        }
    }

    bool IsRevit(const std::string& path)
    {
        std::string ext = ToUtf8(ToPath(path).extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        return ext == ".rvt";
    }

    std::string ProjectName(const std::string& path)
    {
        const std::string stem = ToUtf8(ToPath(path).stem());
        return stem.empty() ? std::string("Project") : stem;
    }

    std::optional<size_t> CountLeftOut(std::string_view note)
    {
        const size_t comma = note.find(", ");
        if (comma == std::string_view::npos)
            return std::nullopt;
        const std::string_view head = note.substr(0, comma);
        const size_t space = head.find(' ');
        if (space == std::string_view::npos)
            return std::nullopt;
        constexpr std::string_view suffix = " left out";
        const std::string_view what = head.substr(space + 1);
        if (what.size() < suffix.size() || what.substr(what.size() - suffix.size()) != suffix)
            return std::nullopt;
        return ParseCount(head.substr(0, space));
    }

    std::pair<std::string_view, std::optional<std::string_view>> SplitIds(std::string_view note)
    {
        if (!note.empty() && note.back() == ')')
        {
            const std::string_view body = note.substr(0, note.size() - 1);
            if (const size_t open = body.rfind(" ("); open != std::string_view::npos)
            {
                const std::string_view ids = body.substr(open + 2);
                if (ids.substr(0, 4) == "ids ")
                    return { body.substr(0, open), ids };
            }
        }
        return { note, std::nullopt };
    }

    size_t Report::CameIn() const
    {
        size_t n = 0;
        for (const auto& [_, k] : counts)
            n += k;
        return n;
    }

    size_t Report::LeftOutCount() const
    {
        size_t n = 0;
        for (const std::string& note : notes)
            n += CountLeftOut(note).value_or(0);
        return n;
    }

    std::vector<std::string_view> Report::LeftOut() const
    {
        std::vector<std::string_view> out;
        for (const std::string& note : notes)
            if (CountLeftOut(note))
                out.push_back(note);
        return out;
    }

    std::vector<std::string_view> Report::Noted() const
    {
        std::vector<std::string_view> out;
        for (const std::string& note : notes)
            if (!CountLeftOut(note))
                out.push_back(note);
        return out;
    }

    Report Of(std::string file, const Bim::Ifc::Imported& imported)
    {
        Report r;
        r.file   = std::move(file);
        r.schema = imported.schema;
        r.notes  = imported.skipped;
        // 按类别名排好，再按个数从多到少（稳定：一样多的照类别名）
        std::map<std::string, size_t> counts;
        for (const auto& [_, e] : imported.model.elements.Items())
            ++counts[e->Category()];
        r.counts.assign(counts.begin(), counts.end());
        std::stable_sort(r.counts.begin(), r.counts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        return r;
    }

    std::string CountLine(const std::string& category, size_t n)
    {
        const char* name = Props::CategoryName(category, n != 1);
        return std::to_string(n) + " " + (*name ? Tr(name) : category);
    }

    std::string Text(const Report& r)
    {
        std::string out = r.file + " · " + r.schema + "\n\n";
        out += Format(Tr("Came in: {0}"), { std::to_string(r.CameIn()) }) + "\n";
        for (const auto& [category, n] : r.counts)
            out += CountLine(category, n) + "\n";
        out += "\n" + Format(Tr("Left out: {0}"), { std::to_string(r.LeftOutCount()) }) + "\n";
        const std::vector<std::string_view> left = r.LeftOut(), noted = r.Noted();
        if (left.empty())
            out += Tr("Nothing was left out.") + "\n";
        for (const std::string_view note : left)
            out += "• " + std::string(note) + "\n";
        if (!noted.empty())
        {
            out += "\n" + Tr("Noted") + "\n";
            for (const std::string_view note : noted)
                out += "• " + std::string(note) + "\n";
        }
        return out;
    }

    std::string StatusLine(const Report& r)
    {
        const size_t n = r.CameIn();
        const std::string elements = Format(TrN("{0} element", "{0} elements", static_cast<long long>(n)), { std::to_string(n) });
        return Format(Tr("Opened from {0}: {1}, {2} left out."), { r.schema, elements, std::to_string(r.LeftOutCount()) });
    }
}
