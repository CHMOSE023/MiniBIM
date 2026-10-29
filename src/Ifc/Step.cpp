// 移植自 BIMCraft crates/core/src/step.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Ifc/Step.h"
#include "Model/Text.h"
#include <algorithm>
#include <charconv>
#include <optional>
#include <system_error>

namespace MiniBIM::Bim::Ifc::Step
{
    V V::Int(int64_t v)
    {
        V out;
        out.kind = Kind::Int;
        out.i    = v;
        return out;
    }

    V V::Real(double v)
    {
        V out;
        out.kind = Kind::Real;
        out.x    = v;
        return out;
    }

    V V::Str(std::string v)
    {
        V out;
        out.kind = Kind::Str;
        out.s    = std::move(v);
        return out;
    }

    V V::Enum(std::string v)
    {
        V out;
        out.kind = Kind::Enum;
        out.s    = std::move(v);
        return out;
    }

    V V::Ref(uint64_t v)
    {
        V out;
        out.kind = Kind::Ref;
        out.ref  = v;
        return out;
    }

    V V::List(std::vector<V> v)
    {
        V out;
        out.kind = Kind::List;
        out.list = std::move(v);
        return out;
    }

    V V::Typed(std::string name, V v)
    {
        V out;
        out.kind = Kind::Typed;
        out.s    = std::move(name);
        out.list.push_back(std::move(v));
        return out;
    }

    bool V::operator==(const V& o) const
    {
        if (kind != o.kind)
            return false;
        switch (kind)
        {
        case Kind::Null:
            return true;
        case Kind::Int:
            return i == o.i;
        case Kind::Real:
            return x == o.x;
        case Kind::Str:
        case Kind::Enum:
            return s == o.s;
        case Kind::Ref:
            return ref == o.ref;
        case Kind::List:
            return list == o.list;
        case Kind::Typed:
            return s == o.s && list == o.list;
        }
        return false;
    }

    namespace
    {
        // 列表最多套这么多层
        constexpr size_t kDepth = 32;

        bool IsDigit(char c) { return c >= '0' && c <= '9'; }
        bool IsAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
        bool IsAlnum(char c) { return IsDigit(c) || IsAlpha(c); }

        std::string AsciiUpper(std::string_view s)
        {
            std::string out(s);
            for (char& c : out)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            return out;
        }

        // 照 Rust 的 u32::from_str_radix(h, 16)：可带一个 "+"，然后至少一位十六进制数字
        std::optional<uint32_t> Hex(std::string_view h)
        {
            if (!h.empty() && h.front() == '+')
                h.remove_prefix(1);
            if (h.empty())
                return std::nullopt;
            uint32_t v = 0;
            const auto [end, ec] = std::from_chars(h.data(), h.data() + h.size(), v, 16);
            if (ec != std::errc() || end != h.data() + h.size())
                return std::nullopt;
            return v;
        }

        // 参数里第一个字符串
        std::optional<std::string> FirstString(const std::vector<V>& args)
        {
            for (const V& v : args)
            {
                if (v.kind == V::Kind::Str)
                    return v.s;
                if (v.kind == V::Kind::List)
                    if (auto s = FirstString(v.list))
                        return s;
            }
            return std::nullopt;
        }

        class Parser
        {
        public:
            Parser(std::string_view s, size_t i) : m_s(s), i(i) {}

            int Peek() const { return i < m_s.size() ? static_cast<unsigned char>(m_s[i]) : -1; }
            std::string_view Rest() const { return i < m_s.size() ? m_s.substr(i) : std::string_view(); }

            // 跳过空白与 `/* 注释 */`
            void Skip()
            {
                for (;;)
                {
                    const int c = Peek();
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                        ++i;
                    else if (c == '/' && i + 1 < m_s.size() && m_s[i + 1] == '*')
                    {
                        const size_t k = Rest().find("*/");
                        i = k == std::string_view::npos ? m_s.size() : i + k + 2;
                    }
                    else
                        return;
                }
            }

            // 出了什么错、在第几行
            std::string Error(std::string_view what) const
            {
                size_t line = 1;
                for (size_t k = 0; k < i && k < m_s.size(); ++k)
                    line += m_s[k] == '\n';
                return "line " + std::to_string(line) + ": " + std::string(what);
            }

            bool Expect(char b, std::string& error)
            {
                if (Peek() == static_cast<unsigned char>(b))
                {
                    ++i;
                    return true;
                }
                error = Error(std::string("expected `") + b + "`");
                return false;
            }

            // 接下来是关键字 w 时跳过它
            bool Word(std::string_view w)
            {
                const bool after = i + w.size() < m_s.size() && (IsAlnum(m_s[i + w.size()]) || m_s[i + w.size()] == '_');
                if (Text::StartsWith(Rest(), w) && !after)
                {
                    i += w.size();
                    return true;
                }
                return false;
            }

            bool Keyword(std::string& out, std::string& error)
            {
                const size_t start = i;
                while (i < m_s.size() && (IsAlnum(m_s[i]) || m_s[i] == '_' || m_s[i] == '!'))
                    ++i;
                if (i == start)
                {
                    error = Error("expected a name");
                    return false;
                }
                out = AsciiUpper(m_s.substr(start, i - start));
                return true;
            }

            bool Uint(uint64_t& out, std::string& error)
            {
                const size_t start = i;
                while (i < m_s.size() && IsDigit(m_s[i]))
                    ++i;
                const std::string_view digits = m_s.substr(start, i - start);
                const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), out);
                if (digits.empty() || ec != std::errc() || end != digits.data() + digits.size())
                {
                    error = Error("`" + std::string(digits) + "` is not an entity number");
                    return false;
                }
                return true;
            }

            bool Number(V& out, std::string& error)
            {
                const size_t start = i;
                while (i < m_s.size() && (IsDigit(m_s[i]) || m_s[i] == '.' || m_s[i] == 'E' || m_s[i] == 'e' || m_s[i] == '+' || m_s[i] == '-'))
                    ++i;
                const std::string_view t = m_s.substr(start, i - start);
                if (t.find_first_of(".Ee") == std::string_view::npos)
                {
                    // 照 Rust 的 i64::from_str：可带一个 "+" 或 "-"；放不下 i64 的当作实数
                    std::string_view digits = t;
                    const bool plus = !digits.empty() && digits.front() == '+';
                    if (plus)
                        digits.remove_prefix(1);
                    int64_t n = 0;
                    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
                    if (!digits.empty() && !(plus && digits.front() == '-') && ec == std::errc() && end == digits.data() + digits.size())
                    {
                        out = V::Int(n);
                        return true;
                    }
                }
                if (const auto x = Text::ParseNumber(t))
                {
                    out = V::Real(*x);
                    return true;
                }
                error = Error("`" + std::string(t) + "` is not a number");
                return false;
            }

            // `'…'`，里面的引号写两次
            bool String(V& out, std::string& error)
            {
                ++i;
                std::string raw;
                for (;;)
                {
                    const int c = Peek();
                    if (c < 0)
                    {
                        error = Error("a string doesn't end");
                        return false;
                    }
                    if (c == '\'' && i + 1 < m_s.size() && m_s[i + 1] == '\'')
                    {
                        raw.push_back('\'');
                        i += 2;
                    }
                    else if (c == '\'')
                    {
                        ++i;
                        out = V::Str(Decode(Text::Lossy(raw)));
                        return true;
                    }
                    else
                    {
                        raw.push_back(static_cast<char>(c));
                        ++i;
                    }
                }
            }

            // 到下一个 end 为止（跳过它）的文字
            bool Until(char end, std::string& out, std::string& error)
            {
                const size_t k = Rest().find(end);
                if (k == std::string_view::npos)
                {
                    error = Error(std::string("no closing `") + end + "`");
                    return false;
                }
                out = Text::Lossy(Rest().substr(0, k));
                i += k + 1;
                return true;
            }

            bool Value(size_t depth, V& out, std::string& error)
            {
                Skip();
                const int c = Peek();
                if (c == '$' || c == '*')
                {
                    ++i;
                    out = V();
                    return true;
                }
                if (c == '\'')
                    return String(out, error);
                // 二进制，写成十六进制数字
                if (c == '"')
                {
                    ++i;
                    std::string t;
                    if (!Until('"', t, error))
                        return false;
                    out = V::Str(std::move(t));
                    return true;
                }
                if (c == '#')
                {
                    ++i;
                    uint64_t r = 0;
                    if (!Uint(r, error))
                        return false;
                    out = V::Ref(r);
                    return true;
                }
                if (c == '.')
                {
                    ++i;
                    std::string e;
                    if (!Until('.', e, error))
                        return false;
                    out = V::Enum(AsciiUpper(Text::Trim(e)));
                    return true;
                }
                if (c == '(')
                {
                    std::vector<V> l;
                    if (!List(depth + 1, l, error))
                        return false;
                    out = V::List(std::move(l));
                    return true;
                }
                if (c >= 0 && (IsDigit(static_cast<char>(c)) || c == '-' || c == '+'))
                    return Number(out, error);
                if (c >= 0 && (IsAlpha(static_cast<char>(c)) || c == '_' || c == '!'))
                {
                    std::string name;
                    std::vector<V> args;
                    if (!Keyword(name, error) || !List(depth + 1, args, error))
                        return false;
                    V v = args.size() == 1 ? std::move(args.front()) : V::List(std::move(args));
                    out = V::Typed(std::move(name), std::move(v));
                    return true;
                }
                error = Error("unexpected character");
                return false;
            }

            // `(A(…)B(…))`，读过去
            bool Complex(std::string& error)
            {
                if (!Expect('(', error))
                    return false;
                for (;;)
                {
                    Skip();
                    if (Peek() == ')')
                    {
                        ++i;
                        return true;
                    }
                    std::string name;
                    std::vector<V> args;
                    if (!Keyword(name, error) || !List(1, args, error))
                        return false;
                }
            }

            // `(a, b, …)`
            bool List(size_t depth, std::vector<V>& out, std::string& error)
            {
                if (depth > kDepth)
                {
                    error = Error("lists nested too deep");
                    return false;
                }
                Skip();
                if (!Expect('(', error))
                    return false;
                Skip();
                if (Peek() == ')')
                {
                    ++i;
                    return true;
                }
                for (;;)
                {
                    V v;
                    if (!Value(depth, v, error))
                        return false;
                    out.push_back(std::move(v));
                    Skip();
                    if (Peek() == ',')
                        ++i;
                    else if (Peek() == ')')
                    {
                        ++i;
                        return true;
                    }
                    else
                    {
                        error = Error("expected `,` or `)`");
                        return false;
                    }
                }
            }

        private:
            std::string_view m_s;

        public:
            size_t i;
        };
    }

    bool Parse(std::string_view text, File& out, std::string& error)
    {
        out = File();
        const size_t header = text.find("HEADER;");
        if (header == std::string_view::npos)
        {
            error = "not a STEP file: it has no HEADER section";
            return false;
        }
        Parser p(text, header + 7);
        for (;;)
        {
            p.Skip();
            if (p.Word("ENDSEC"))
            {
                p.Skip();
                if (!p.Expect(';', error))
                    return false;
                break;
            }
            std::string name;
            std::vector<V> args;
            if (!p.Keyword(name, error) || !p.List(0, args, error))
                return false;
            p.Skip();
            if (!p.Expect(';', error))
                return false;
            if (name == "FILE_SCHEMA")
                out.schema = AsciiUpper(Text::Trim(FirstString(args).value_or("")));
        }
        p.Skip();
        if (!p.Word("DATA"))
        {
            error = p.Error("expected the DATA section");
            return false;
        }
        p.Skip();
        // 第 3 版可以给数据段起名：`DATA('name', ('IFC4'));`
        if (p.Peek() == '(')
        {
            std::vector<V> ignored;
            if (!p.List(0, ignored, error))
                return false;
            p.Skip();
        }
        if (!p.Expect(';', error))
            return false;
        for (;;)
        {
            p.Skip();
            if (p.Word("ENDSEC"))
                break;
            uint64_t id = 0;
            if (!p.Expect('#', error) || !p.Uint(id, error))
                return false;
            p.Skip();
            if (!p.Expect('=', error))
                return false;
            p.Skip();
            // 复合实例 `(A(…)B(…))` 是几个实体合成的一个；IFC 里没有要读的
            std::optional<Entity> entity;
            if (p.Peek() == '(')
            {
                if (!p.Complex(error))
                    return false;
            }
            else
            {
                Entity e;
                if (!p.Keyword(e.name, error) || !p.List(0, e.args, error))
                    return false;
                entity = std::move(e);
            }
            p.Skip();
            if (!p.Expect(';', error))
                return false;
            if (entity)
                out.entities[id] = std::move(*entity);
        }
        return true;
    }

    std::string Decode(std::string_view s)
    {
        if (s.find('\\') == std::string_view::npos)
            return std::string(s);
        std::string out;
        std::string_view rest = s;
        for (size_t k = rest.find('\\'); k != std::string_view::npos; k = rest.find('\\'))
        {
            out.append(rest.substr(0, k));
            rest.remove_prefix(k);
            const bool wide = Text::StartsWith(rest, "\\X4\\");
            if (Text::StartsWith(rest, "\\\\"))
            {
                out.push_back('\\');
                rest.remove_prefix(2);
            }
            else if (Text::StartsWith(rest, "\\X2\\") || wide)
            {
                const std::string_view r = rest.substr(4);
                const size_t found = r.find("\\X0\\");
                const size_t end = found == std::string_view::npos ? r.size() : found;
                // 按字符切成每 4（UTF-16）或 8（UTF-32）位一组，不是十六进制数的组丢掉
                const std::u32string digits = Text::Utf32(r.substr(0, end));
                const size_t width = wide ? 8 : 4;
                std::vector<uint32_t> units;
                for (size_t at = 0; at + width <= digits.size(); at += width)
                {
                    std::string chunk;
                    for (size_t j = at; j < at + width; ++j)
                        Text::AppendUtf8(chunk, digits[j]);
                    if (const auto u = Hex(chunk))
                        units.push_back(*u);
                }
                if (wide)
                    for (const uint32_t u : units)
                        Text::AppendUtf8(out, u <= 0x10FFFF && !(u >= 0xD800 && u <= 0xDFFF) ? u : 0xFFFD);
                else
                    for (size_t j = 0; j < units.size(); ++j)
                    {
                        const uint32_t u = units[j];
                        if (u >= 0xD800 && u <= 0xDBFF && j + 1 < units.size() && units[j + 1] >= 0xDC00 && units[j + 1] <= 0xDFFF)
                        {
                            Text::AppendUtf8(out, 0x10000 + ((u - 0xD800) << 10) + (units[j + 1] - 0xDC00));
                            ++j;
                        }
                        else
                            Text::AppendUtf8(out, u >= 0xD800 && u <= 0xDFFF ? 0xFFFD : u);
                    }
                rest = end + 4 <= r.size() ? r.substr(end + 4) : std::string_view();
            }
            else if (Text::StartsWith(rest, "\\X\\") && rest.size() >= 5 && Hex(rest.substr(3, 2)))
            {
                // 两位十六进制数是 ISO 8859-1 的一个字节，也就是 U+0000–U+00FF
                Text::AppendUtf8(out, *Hex(rest.substr(3, 2)));
                rest.remove_prefix(5);
            }
            else if (Text::StartsWith(rest, "\\S\\") && rest.size() > 3)
            {
                // 下一个字符加 128
                const char32_t c = Text::Utf32(rest.substr(3, 4)).front();
                const size_t len = std::min<size_t>(c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4, rest.size() - 3);
                const char32_t up = c + 128;
                if (up <= 0x10FFFF && !(up >= 0xD800 && up <= 0xDFFF))
                    Text::AppendUtf8(out, up);
                rest.remove_prefix(3 + len);
            }
            else if (Text::StartsWith(rest, "\\P") && rest.size() > 3 && rest[3] == '\\')
                rest.remove_prefix(4);
            else
            {
                out.push_back('\\');
                rest.remove_prefix(1);
            }
        }
        out.append(rest);
        return out;
    }
}
