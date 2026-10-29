#include "I18n/Translator.h"
#include "Core/Log.h"
#include <algorithm>
#include <filesystem>

namespace MiniBIM::I18n
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

        // 目录里的 .po：规范写法的语言代码 → 文件路径
        std::vector<std::pair<std::string, std::string>> ListCatalogs(const std::string& dir)
        {
            std::vector<std::pair<std::string, std::string>> files;
            std::error_code ec;
            if (dir.empty() || !std::filesystem::is_directory(ToPath(dir), ec))
                return files;
            for (const auto& entry : std::filesystem::directory_iterator(ToPath(dir), ec))
            {
                if (!entry.is_regular_file(ec) || entry.path().extension() != ".po")
                    continue;
                const std::string code = NormalizeLanguage(ToUtf8(entry.path().stem()));
                if (!code.empty() && code != "en")
                    files.emplace_back(code, ToUtf8(entry.path()));
            }
            std::sort(files.begin(), files.end());
            return files;
        }
    }

    Translator& Translator::Instance()
    {
        static Translator translator;
        return translator;
    }

    std::vector<LanguageInfo> Translator::GetLanguages() const
    {
        std::vector<LanguageInfo> languages{ { "en", "English" } };
        for (const auto& [code, path] : ListCatalogs(m_dir))
        {
            Catalog catalog;
            catalog.LoadFile(path);
            languages.push_back({ code, catalog.GetLanguageName().empty() ? code : catalog.GetLanguageName() });
        }
        std::sort(languages.begin() + 1, languages.end(), [](const LanguageInfo& a, const LanguageInfo& b) { return a.code < b.code; });
        return languages;
    }

    std::string Translator::FindCatalogFile(const std::string& code) const
    {
        const auto files = ListCatalogs(m_dir);
        const std::string language = code.substr(0, code.find('_'));
        for (const auto& [c, path] : files)        // 完全一致
            if (c == code)
                return path;
        for (const auto& [c, path] : files)        // 只写语种的
            if (c == language)
                return path;
        for (const auto& [c, path] : files)        // 同语种的别的地区
            if (c.substr(0, c.find('_')) == language)
                return path;
        return {};
    }

    std::string Translator::SetLanguage(std::string_view code)
    {
        const std::string normalized = NormalizeLanguage(code);
        Catalog     catalog;
        std::string used = "en";
        if (!normalized.empty() && normalized != "en")
        {
            if (const std::string path = FindCatalogFile(normalized); !path.empty())
            {
                std::vector<std::string> warnings;
                if (catalog.LoadFile(path, &warnings))
                    used = NormalizeLanguage(ToUtf8(ToPath(path).stem()));
                else
                    LOG_WARN("语言：读不出词典 %s", path.c_str());
                for (const std::string& w : warnings)
                    LOG_WARN("语言：%s %s", path.c_str(), w.c_str());
            }
            else
                LOG_INFO("语言：没有 %s 的词典，用英文", normalized.c_str());
        }
        if (used == "en")
            catalog = Catalog{};
        UseCatalog(std::move(catalog), used);
        return used;
    }

    void Translator::UseCatalog(Catalog catalog, std::string language)
    {
        m_language = !language.empty() ? std::move(language) : (catalog.GetLanguage().empty() ? "en" : catalog.GetLanguage());
        m_catalog  = std::move(catalog);
        ++m_version;
    }

    std::string Translator::Translate(std::string_view context, std::string_view english) const
    {
        if (const std::string* s = m_catalog.Find(context, english))
            return *s;
        return std::string(english);
    }

    std::string Translator::TranslatePlural(std::string_view context, std::string_view english, std::string_view englishPlural, long long n) const
    {
        if (const std::string* s = m_catalog.FindPlural(context, english, n))
            return *s;
        return std::string(n == 1 ? english : englishPlural);
    }

    std::string Format(std::string_view pattern, std::initializer_list<std::string_view> args)
    {
        std::string out;
        out.reserve(pattern.size() + 16);
        for (size_t i = 0; i < pattern.size(); ++i)
        {
            const char c = pattern[i];
            if (c == '{' && i + 1 < pattern.size() && pattern[i + 1] == '{')
            {
                out += '{';
                ++i;
                continue;
            }
            if (c == '}' && i + 1 < pattern.size() && pattern[i + 1] == '}')
            {
                out += '}';
                ++i;
                continue;
            }
            if (c == '{')
            {
                size_t j = i + 1;
                size_t index = 0;
                while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9')
                    index = index * 10 + static_cast<size_t>(pattern[j++] - '0');
                if (j > i + 1 && j < pattern.size() && pattern[j] == '}' && index < args.size())
                {
                    out += *(args.begin() + index);
                    i = j;
                    continue;
                }
            }
            out += c;
        }
        return out;
    }
}
