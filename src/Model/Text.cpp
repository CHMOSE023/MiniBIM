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
