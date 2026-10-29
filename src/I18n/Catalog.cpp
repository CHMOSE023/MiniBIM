#include "I18n/Catalog.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace MiniBIM::I18n
{
    namespace
    {
        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
                s.remove_suffix(1);
            return s;
        }

        bool StartsWith(std::string_view s, std::string_view prefix)
        {
            return s.substr(0, prefix.size()) == prefix;
        }

        // "…" 形式的字符串（PO 的转义同 C）。不是这种形式时返回 false
        bool ParseQuoted(std::string_view s, std::string& out, std::string& error)
        {
            s = Trim(s);
            if (s.size() < 2 || s.front() != '"' || s.back() != '"')
            {
                error = "应为带引号的字符串";
                return false;
            }
            s = s.substr(1, s.size() - 2);
            for (size_t i = 0; i < s.size(); ++i)
            {
                const char c = s[i];
                if (c == '"')
                {
                    error = "字符串中间的引号没有转义";
                    return false;
                }
                if (c != '\\')
                {
                    out += c;
                    continue;
                }
                if (++i >= s.size())
                {
                    error = "字符串以 \\ 结尾";
                    return false;
                }
                switch (s[i])
                {
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case 'a':  out += '\a'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'v':  out += '\v'; break;
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '\'': out += '\''; break;
                case '?':  out += '?';  break;
                default:
                    error = std::string("不认识的转义 \\") + s[i];
                    return false;
                }
            }
            return true;
        }

        // ── Plural-Forms 表达式：递归下降，优先级同 C ─────────────────
        class PluralParser
        {
        public:
            PluralParser(std::string_view text, long long n) : m_s(text), m_n(n) {}

            long long Run()
            {
                const long long v = Ternary();
                Skip();
                return (m_ok && m_pos == m_s.size()) ? v : -1;
            }

        private:
            void Skip()
            {
                while (m_pos < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_pos])))
                    ++m_pos;
            }
            bool Eat(std::string_view op)
            {
                Skip();
                if (m_s.substr(m_pos, op.size()) != op)
                    return false;
                // "<" 不能吃掉 "<=" 的前半，"!" 不能吃掉 "!="
                if (op.size() == 1 && m_pos + 1 < m_s.size() && m_s[m_pos + 1] == '=' && (op == "<" || op == ">" || op == "!"))
                    return false;
                m_pos += op.size();
                return true;
            }

            long long Ternary()
            {
                const long long cond = Or();
                if (!Eat("?"))
                    return cond;
                const long long a = Ternary();
                if (!Eat(":"))
                {
                    m_ok = false;
                    return 0;
                }
                const long long b = Ternary();
                return cond ? a : b;
            }
            long long Or()
            {
                long long v = And();
                while (Eat("||"))
                {
                    const long long r = And();
                    v = (v || r) ? 1 : 0;
                }
                return v;
            }
            long long And()
            {
                long long v = Equality();
                while (Eat("&&"))
                {
                    const long long r = Equality();
                    v = (v && r) ? 1 : 0;
                }
                return v;
            }
            long long Equality()
            {
                long long v = Relational();
                while (true)
                {
                    if (Eat("=="))      v = (v == Relational()) ? 1 : 0;
                    else if (Eat("!=")) v = (v != Relational()) ? 1 : 0;
                    else return v;
                }
            }
            long long Relational()
            {
                long long v = Additive();
                while (true)
                {
                    if (Eat("<="))      v = (v <= Additive()) ? 1 : 0;
                    else if (Eat(">=")) v = (v >= Additive()) ? 1 : 0;
                    else if (Eat("<"))  v = (v <  Additive()) ? 1 : 0;
                    else if (Eat(">"))  v = (v >  Additive()) ? 1 : 0;
                    else return v;
                }
            }
            long long Additive()
            {
                long long v = Multiplicative();
                while (true)
                {
                    if (Eat("+"))      v += Multiplicative();
                    else if (Eat("-")) v -= Multiplicative();
                    else return v;
                }
            }
            long long Multiplicative()
            {
                long long v = Unary();
                while (true)
                {
                    if (Eat("*"))
                        v *= Unary();
                    else if (Eat("/") || Eat("%"))
                    {
                        const bool mod = m_s[m_pos - 1] == '%';
                        const long long r = Unary();
                        if (r == 0)
                        {
                            m_ok = false;
                            return 0;
                        }
                        v = mod ? v % r : v / r;
                    }
                    else
                        return v;
                }
            }
            long long Unary()
            {
                if (Eat("!"))
                    return Unary() ? 0 : 1;
                return Primary();
            }
            long long Primary()
            {
                Skip();
                if (Eat("("))
                {
                    const long long v = Ternary();
                    if (!Eat(")"))
                        m_ok = false;
                    return v;
                }
                if (m_pos < m_s.size() && m_s[m_pos] == 'n')
                {
                    ++m_pos;
                    return m_n;
                }
                if (m_pos < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_pos])))
                {
                    long long v = 0;
                    while (m_pos < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_pos])))
                        v = v * 10 + (m_s[m_pos++] - '0');
                    return v;
                }
                m_ok = false;
                return 0;
            }

            std::string_view m_s;
            size_t           m_pos = 0;
            long long        m_n   = 0;
            bool             m_ok  = true;
        };
    }

    long long EvaluatePluralExpression(std::string_view expr, long long n)
    {
        return PluralParser(expr, n).Run();
    }

    std::string NormalizeLanguage(std::string_view code)
    {
        code = Trim(code);
        if (const size_t cut = code.find_first_of(".@"); cut != std::string_view::npos)
            code = code.substr(0, cut);        // zh_CN.UTF-8、sr_RS@latin
        std::vector<std::string> parts;
        std::string part;
        for (const char c : code)
        {
            if (c == '-' || c == '_')
            {
                parts.push_back(std::move(part));
                part.clear();
            }
            else
                part += c;
        }
        parts.push_back(std::move(part));
        if (parts.empty() || parts[0].empty())
            return {};

        std::string language = parts[0];
        std::transform(language.begin(), language.end(), language.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (language == "c" || language == "posix")
            return "en";

        std::string script, region;
        for (size_t i = 1; i < parts.size(); ++i)
        {
            std::string p = parts[i];
            if (p.size() == 4)
            {
                std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                p[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
                script = p;
            }
            else if (p.size() == 2 || p.size() == 3)
            {
                std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                region = p;
            }
        }
        // 中文只按简体 / 繁体分：没写地区时由文字推出
        if (language == "zh" && region.empty())
        {
            if (script == "Hans") region = "CN";
            else if (script == "Hant") region = "TW";
        }
        return region.empty() ? language : language + "_" + region;
    }

    std::string Catalog::Key(std::string_view context, std::string_view english)
    {
        std::string key;
        if (!context.empty())
        {
            key.reserve(context.size() + 1 + english.size());
            key.append(context);
            key += '\x04';
        }
        key.append(english);
        return key;
    }

    void Catalog::ParseHeader(const std::string& header)
    {
        std::istringstream lines(header);
        std::string line;
        while (std::getline(lines, line))
        {
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            const std::string_view key   = Trim(std::string_view(line).substr(0, colon));
            const std::string_view value = Trim(std::string_view(line).substr(colon + 1));
            if (key == "Language")
                m_language = NormalizeLanguage(value);
            else if (key == "X-Language-Name")
                m_languageName = std::string(value);
            else if (key == "Plural-Forms")
            {
                m_pluralForms = std::string(value);
                if (const size_t p = value.find("nplurals="); p != std::string_view::npos)
                    m_pluralCount = std::max(1, std::atoi(std::string(value.substr(p + 9)).c_str()));
                if (const size_t p = value.find("plural="); p != std::string_view::npos)
                {
                    std::string_view expr = value.substr(p + 7);
                    if (const size_t end = expr.find(';'); end != std::string_view::npos)
                        expr = expr.substr(0, end);
                    m_pluralExpr = std::string(Trim(expr));
                }
            }
        }
    }

    bool Catalog::Parse(std::string_view text, std::vector<std::string>* warnings)
    {
        if (StartsWith(text, "\xEF\xBB\xBF"))
            text.remove_prefix(3);

        struct Entry
        {
            bool        hasContext = false, hasId = false, hasPlural = false, fuzzy = false;
            std::string context, id, plural;
            std::map<int, std::string> strs;
            int         line = 0;
        };
        Entry        entry;
        std::string* last = nullptr;        // 续行（以引号开头的行）接到哪一个字段
        bool         gotHeader = false;
        auto warn = [&](int line, const std::string& what)
        {
            if (warnings)
                warnings->push_back("第 " + std::to_string(line) + " 行：" + what);
        };
        auto flush = [&]
        {
            if (entry.hasId)
            {
                if (entry.id.empty() && !entry.hasContext)
                {
                    // 文件头：msgid "" 的译文是 "键: 值\n" 的列表
                    if (auto it = entry.strs.find(0); it != entry.strs.end())
                        ParseHeader(it->second);
                    gotHeader = true;
                }
                else if (!entry.fuzzy && !entry.strs.empty())
                {
                    // 每种形式都要有译文，缺一个就整条不用（显示英文原文，免得复数形式一半中文一半英文）
                    const int forms = entry.hasPlural ? entry.strs.rbegin()->first + 1 : 1;
                    std::vector<std::string> values(static_cast<size_t>(forms));
                    bool complete = true;
                    for (int i = 0; i < forms; ++i)
                    {
                        const auto it = entry.strs.find(i);
                        if (it == entry.strs.end() || it->second.empty())
                            complete = false;
                        else
                            values[static_cast<size_t>(i)] = it->second;
                    }
                    if (complete)
                        m_messages[Key(entry.context, entry.id)] = std::move(values);
                }
            }
            else if (!entry.strs.empty() || entry.hasContext)
                warn(entry.line, "条目没有 msgid");
            entry = Entry{};
            last  = nullptr;
        };

        int lineNo = 0;
        size_t pos = 0;
        while (pos <= text.size())
        {
            const size_t end = std::min(text.find('\n', pos), text.size());
            const std::string_view line = Trim(text.substr(pos, end - pos));
            pos = end + 1;
            ++lineNo;

            if (line.empty())
            {
                flush();
                if (end == text.size())
                    break;
                continue;
            }
            if (StartsWith(line, "#~"))         // 已废弃的条目
                continue;
            if (StartsWith(line, "#,"))
            {
                if (line.find("fuzzy") != std::string_view::npos)
                {
                    if (entry.hasId)
                        flush();
                    entry.fuzzy = true;
                }
                continue;
            }
            if (line.front() == '#')
                continue;

            std::string error;
            auto field = [&](std::string_view keyword, std::string& target) -> bool
            {
                target.clear();
                if (!ParseQuoted(line.substr(keyword.size()), target, error))
                {
                    warn(lineNo, std::string(keyword) + "：" + error);
                    target.clear();         // 不留半截译文
                    last = nullptr;
                    return false;
                }
                last = &target;
                return true;
            };

            if (StartsWith(line, "msgctxt "))
            {
                if (entry.hasId)
                    flush();
                entry.line = lineNo;
                entry.hasContext = field("msgctxt ", entry.context);
            }
            else if (StartsWith(line, "msgid_plural "))
                entry.hasPlural = field("msgid_plural ", entry.plural);
            else if (StartsWith(line, "msgid "))
            {
                if (entry.hasId)
                    flush();
                if (!entry.line)
                    entry.line = lineNo;
                entry.hasId = field("msgid ", entry.id);
            }
            else if (StartsWith(line, "msgstr["))
            {
                const size_t close = line.find(']');
                const int index = close == std::string_view::npos ? -1 : std::atoi(std::string(line.substr(7, close - 7)).c_str());
                if (index < 0 || index > 15)
                {
                    warn(lineNo, "msgstr[n] 的序号不对");
                    last = nullptr;
                    continue;
                }
                field(line.substr(0, close + 2), entry.strs[index]);
            }
            else if (StartsWith(line, "msgstr "))
                field("msgstr ", entry.strs[0]);
            else if (line.front() == '"')
            {
                std::string more;
                if (!last)
                    warn(lineNo, "续行前面没有字段");
                else if (!ParseQuoted(line, more, error))
                    warn(lineNo, error);
                else
                    *last += more;
            }
            else
                warn(lineNo, "不认识的行");

            if (end == text.size())
                break;
        }
        flush();
        return gotHeader || !m_messages.empty();
    }

    bool Catalog::LoadFile(const std::string& utf8Path, std::vector<std::string>* warnings)
    {
        std::ifstream in(std::filesystem::path(std::u8string(utf8Path.begin(), utf8Path.end())), std::ios::binary);
        if (!in)
            return false;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return Parse(buffer.str(), warnings);
    }

    const std::string* Catalog::Find(std::string_view context, std::string_view english) const
    {
        const auto it = m_messages.find(Key(context, english));
        return it == m_messages.end() || it->second.empty() ? nullptr : &it->second[0];
    }

    const std::string* Catalog::FindPlural(std::string_view context, std::string_view english, long long n) const
    {
        const auto it = m_messages.find(Key(context, english));
        if (it == m_messages.end())
            return nullptr;
        // 没写 Plural-Forms 时按英语的规则（1 为单数）
        const long long index = m_pluralExpr.empty() ? (n == 1 ? 0 : 1) : EvaluatePluralExpression(m_pluralExpr, n);
        if (index < 0 || index >= static_cast<long long>(it->second.size()) || index >= m_pluralCount)
            return nullptr;
        return &it->second[static_cast<size_t>(index)];
    }
}
