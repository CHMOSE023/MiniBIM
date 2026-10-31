// MiniBIM 为 Revit 读取器写的字节工具，随 src/Rvt 以 Apache-2.0 发布，见 src/Rvt/NOTICE
#include "Rvt/Bytes.h"

#include <algorithm>
#include <cstring>

namespace MiniBIM::Rvt::Bytes
{
    std::optional<uint16_t> U16At(Span buf, size_t at)
    {
        if (at > buf.size() || buf.size() - at < 2)
            return std::nullopt;
        return U16(buf.data() + at);
    }

    std::optional<uint32_t> U32At(Span buf, size_t at)
    {
        if (at > buf.size() || buf.size() - at < 4)
            return std::nullopt;
        return U32(buf.data() + at);
    }

    std::optional<uint64_t> U64At(Span buf, size_t at)
    {
        if (at > buf.size() || buf.size() - at < 8)
            return std::nullopt;
        return U64(buf.data() + at);
    }

    std::optional<double> F64At(Span buf, size_t at)
    {
        const auto bits = U64At(buf, at);
        if (!bits)
            return std::nullopt;
        double v;
        std::memcpy(&v, &*bits, 8);
        return v;
    }

    size_t Find(Span hay, Span needle, size_t from)
    {
        if (needle.empty() || from > hay.size() || hay.size() - from < needle.size())
            return npos;
        // 分区里要找几百万次短的模式：先 memchr 第一个字节再比较，比每次建 Boyer-Moore 表快
        const uint8_t* p   = hay.data() + from;
        const uint8_t* end = hay.data() + (hay.size() - needle.size()) + 1;
        while (p < end)
        {
            p = static_cast<const uint8_t*>(std::memchr(p, needle[0], static_cast<size_t>(end - p)));
            if (!p)
                return npos;
            if (std::memcmp(p, needle.data(), needle.size()) == 0)
                return static_cast<size_t>(p - hay.data());
            ++p;
        }
        return npos;
    }

    size_t FindLast(Span hay, Span needle)
    {
        if (needle.empty() || hay.size() < needle.size())
            return npos;
        for (size_t at = hay.size() - needle.size() + 1; at-- > 0;)
            if (std::memcmp(hay.data() + at, needle.data(), needle.size()) == 0)
                return at;
        return npos;
    }

    bool Matches(Span buf, size_t at, Span want)
    {
        return at <= buf.size() && buf.size() - at >= want.size() && std::equal(want.begin(), want.end(), buf.begin() + static_cast<std::ptrdiff_t>(at));
    }

    std::vector<uint8_t> Wide(std::string_view utf8)
    {
        std::vector<uint8_t> out;
        const auto put = [&](uint32_t u)
        {
            out.push_back(static_cast<uint8_t>(u & 0xff));
            out.push_back(static_cast<uint8_t>(u >> 8));
        };
        for (size_t i = 0; i < utf8.size();)
        {
            const uint8_t c = static_cast<uint8_t>(utf8[i]);
            uint32_t cp = c;
            size_t n = 1;
            if (c >= 0xf0 && i + 3 < utf8.size())
                cp = (c & 0x07u) << 18 | (utf8[i + 1] & 0x3fu) << 12 | (utf8[i + 2] & 0x3fu) << 6 | (utf8[i + 3] & 0x3fu), n = 4;
            else if (c >= 0xe0 && i + 2 < utf8.size())
                cp = (c & 0x0fu) << 12 | (utf8[i + 1] & 0x3fu) << 6 | (utf8[i + 2] & 0x3fu), n = 3;
            else if (c >= 0xc0 && i + 1 < utf8.size())
                cp = (c & 0x1fu) << 6 | (utf8[i + 1] & 0x3fu), n = 2;
            i += n;
            if (cp >= 0x10000)
            {
                cp -= 0x10000;
                put(0xd800 + (cp >> 10));
                put(0xdc00 + (cp & 0x3ff));
            }
            else
                put(cp);
        }
        return out;
    }

    bool PutUtf8(std::string& out, uint32_t cp)
    {
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xc0 | cp >> 6);
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xe0 | cp >> 12);
            out += static_cast<char>(0x80 | (cp >> 6 & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else
        {
            out += static_cast<char>(0xf0 | cp >> 18);
            out += static_cast<char>(0x80 | (cp >> 12 & 0x3f));
            out += static_cast<char>(0x80 | (cp >> 6 & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        return true;
    }

    std::optional<std::string> FromUtf16(const std::vector<uint16_t>& units, bool strict)
    {
        std::string out;
        for (size_t i = 0; i < units.size(); ++i)
        {
            const uint32_t u = units[i];
            if (u >= 0xd800 && u <= 0xdbff && i + 1 < units.size() && units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff)
            {
                PutUtf8(out, 0x10000 + ((u - 0xd800) << 10) + (units[i + 1] - 0xdc00u));
                ++i;
            }
            else if (u >= 0xd800 && u <= 0xdfff)
            {
                if (strict)
                    return std::nullopt;
                PutUtf8(out, 0xfffd);
            }
            else
                PutUtf8(out, u);
        }
        return out;
    }

    std::vector<uint16_t> Units(Span bytes)
    {
        std::vector<uint16_t> out;
        out.reserve(bytes.size() / 2);
        for (size_t i = 0; i + 1 < bytes.size(); i += 2)
            out.push_back(U16(bytes.data() + i));
        return out;
    }

    std::optional<std::u32string> CodePoints(const std::vector<uint16_t>& units)
    {
        std::u32string out;
        out.reserve(units.size());
        for (size_t i = 0; i < units.size(); ++i)
        {
            const uint32_t u = units[i];
            if (u >= 0xd800 && u <= 0xdbff && i + 1 < units.size() && units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff)
            {
                out += static_cast<char32_t>(0x10000 + ((u - 0xd800) << 10) + (units[i + 1] - 0xdc00u));
                ++i;
            }
            else if (u >= 0xd800 && u <= 0xdfff)
                return std::nullopt;
            else
                out += static_cast<char32_t>(u);
        }
        return out;
    }

    std::optional<std::u32string> CodePointsAt(Span buf, size_t at, size_t n)
    {
        if (at > buf.size() || n > (buf.size() - at) / 2)
            return std::nullopt;
        return CodePoints(Units(buf.subspan(at, 2 * n)));
    }

    std::string Utf8(std::u32string_view cps)
    {
        std::string out;
        for (const char32_t c : cps)
            PutUtf8(out, c);
        return out;
    }

    bool IsWhiteSpace(char32_t c)
    {
        return c == ' ' || (c >= 0x09 && c <= 0x0d) || c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
               c == 0x202f || c == 0x205f || c == 0x3000;
    }

    bool IsBlank(std::u32string_view cps)
    {
        return std::all_of(cps.begin(), cps.end(), IsWhiteSpace);
    }

    bool IsUtf8(std::string_view s)
    {
        for (size_t i = 0; i < s.size();)
        {
            const uint8_t c = static_cast<uint8_t>(s[i]);
            size_t n = 0;
            uint32_t cp = 0, min = 0;
            if (c < 0x80)
            {
                ++i;
                continue;
            }
            if ((c & 0xe0) == 0xc0)
                n = 1, cp = c & 0x1fu, min = 0x80;
            else if ((c & 0xf0) == 0xe0)
                n = 2, cp = c & 0x0fu, min = 0x800;
            else if ((c & 0xf8) == 0xf0)
                n = 3, cp = c & 0x07u, min = 0x10000;
            else
                return false;
            if (i + n >= s.size())
                return false;
            for (size_t k = 1; k <= n; ++k)
            {
                const uint8_t d = static_cast<uint8_t>(s[i + k]);
                if ((d & 0xc0) != 0x80)
                    return false;
                cp = cp << 6 | (d & 0x3fu);
            }
            if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
                return false;
            i += n + 1;
        }
        return true;
    }

    std::string_view Trim(std::string_view s)
    {
        const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
        while (!s.empty() && space(s.front()))
            s.remove_prefix(1);
        while (!s.empty() && space(s.back()))
            s.remove_suffix(1);
        return s;
    }
}
