#include "Model/Text.h"
#include <charconv>
#include <cmath>
#include <system_error>

namespace MiniBIM::Bim::Text
{
    namespace
    {
        bool IsSpace(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }
    }

    std::string_view Trim(std::string_view s)
    {
        while (!s.empty() && IsSpace(s.front()))
            s.remove_prefix(1);
        while (!s.empty() && IsSpace(s.back()))
            s.remove_suffix(1);
        return s;
    }

    std::string AsciiLower(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return out;
    }

    bool StartsWith(std::string_view s, std::string_view prefix)
    {
        return s.substr(0, prefix.size()) == prefix;
    }

    bool EndsWith(std::string_view s, std::string_view suffix)
    {
        return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
    }

    std::string ReplaceAll(std::string s, std::string_view from, std::string_view to)
    {
        if (from.empty())
            return s;
        for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
            s.replace(at, from.size(), to);
        return s;
    }

    std::vector<std::string_view> SplitWhitespace(std::string_view s)
    {
        std::vector<std::string_view> out;
        size_t i = 0;
        while (i < s.size())
        {
            while (i < s.size() && IsSpace(s[i]))
                ++i;
            const size_t start = i;
            while (i < s.size() && !IsSpace(s[i]))
                ++i;
            if (i > start)
                out.push_back(s.substr(start, i - start));
        }
        return out;
    }

    size_t CharCount(std::string_view utf8)
    {
        size_t n = 0;
        for (const char c : utf8)
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
                ++n;
        return n;
    }

    std::u32string Utf32(std::string_view bytes)
    {
        std::u32string out;
        const auto at = [&](size_t i) { return i < bytes.size() ? static_cast<unsigned char>(bytes[i]) : 0u; };
        for (size_t i = 0; i < bytes.size();)
        {
            const unsigned b = at(i);
            if (b < 0x80)
            {
                out.push_back(b);
                ++i;
                continue;
            }
            // 序列的长度、第二个字节的范围（排除过长编码、代理区与超出 U+10FFFF 的）
            size_t n = 0;
            unsigned lo = 0x80, hi = 0xBF;
            if (b >= 0xC2 && b <= 0xDF)
                n = 2;
            else if (b >= 0xE0 && b <= 0xEF)
            {
                n = 3;
                if (b == 0xE0)
                    lo = 0xA0;
                if (b == 0xED)
                    hi = 0x9F;
            }
            else if (b >= 0xF0 && b <= 0xF4)
            {
                n = 4;
                if (b == 0xF0)
                    lo = 0x90;
                if (b == 0xF4)
                    hi = 0x8F;
            }
            // 合法的前缀有几个字节：不合法时整个前缀换成一个 U+FFFD
            size_t ok = 1;
            char32_t cp = n == 2 ? (b & 0x1Fu) : n == 3 ? (b & 0x0Fu) : (b & 0x07u);
            for (size_t k = 1; k < n && i + k < bytes.size(); ++k)
            {
                const unsigned c = at(i + k);
                if (k == 1 ? (c < lo || c > hi) : (c < 0x80 || c > 0xBF))
                    break;
                cp = (cp << 6) | (c & 0x3Fu);
                ++ok;
            }
            out.push_back(n != 0 && ok == n ? cp : char32_t(0xFFFD));
            i += ok;
        }
        return out;
    }

    void AppendUtf8(std::string& out, char32_t c)
    {
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
            c = 0xFFFD;
        if (c < 0x80)
            out.push_back(static_cast<char>(c));
        else if (c < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else if (c < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }

    std::string Lossy(std::string_view bytes)
    {
        // 多半本来就是合法的 UTF-8：先看一遍，合法就原样返回
        const std::u32string cps = Utf32(bytes);
        bool clean = true;
        for (const char32_t c : cps)
            clean &= c != 0xFFFD;
        if (clean)
            return std::string(bytes);
        std::string out;
        for (const char32_t c : cps)
            AppendUtf8(out, c);
        return out;
    }

    std::optional<double> ParseNumber(std::string_view s)
    {
        s = Trim(s);
        // from_chars 不认开头的 "+"
        if (!s.empty() && s.front() == '+')
        {
            s.remove_prefix(1);
            if (!s.empty() && s.front() == '-')
                return std::nullopt;
        }
        if (s.empty())
            return std::nullopt;
        double x = 0.0;
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), x);
        if (ec == std::errc::result_out_of_range)
        {
            // 太大的数照 Rust 读成无穷大（再由调用方按 is_finite 拒绝）；太小的读成 0
            const bool negative = s.front() == '-';
            const size_t e = s.find_first_of("eE");
            const bool tiny = e != std::string_view::npos && e + 1 < s.size() && s[e + 1] == '-';
            if (end != s.data() + s.size())
                return std::nullopt;
            return tiny ? (negative ? -0.0 : 0.0) : (negative ? -INFINITY : INFINITY);
        }
        if (ec != std::errc() || end != s.data() + s.size())
            return std::nullopt;
        return x;
    }

    std::string Shortest(double x)
    {
        if (std::isnan(x))
            return "NaN";
        if (std::isinf(x))
            return x < 0 ? "-inf" : "inf";
        char buf[400];
        const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), x, std::chars_format::fixed);
        if (ec != std::errc())
            return "0";
        return std::string(buf, end);
    }
}
