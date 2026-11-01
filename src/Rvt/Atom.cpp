// 移植自 BIMCraft crates/rvt/src/atom.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/part_atom.rs and src/project_information.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Atom.h).
#include "Rvt/Atom.h"
#include "Rvt/Bytes.h"
#include "Rvt/Container.h"
#include "Rvt/Inflate.h"

#include <algorithm>
#include <cstring>

namespace MiniBIM::Rvt
{
    namespace
    {
        // 最多套几层、看多少个标签
        constexpr size_t MaxDepth = 64;
        constexpr size_t MaxTags  = 100'000;

        bool Parse32(std::string_view s, int base, uint32_t& out)
        {
            if (s.starts_with('+'))
                s.remove_prefix(1);
            if (s.empty())
                return false;
            uint64_t v = 0;
            for (const char c : s)
            {
                int d = -1;
                if (c >= '0' && c <= '9')
                    d = c - '0';
                else if (base == 16 && c >= 'a' && c <= 'f')
                    d = c - 'a' + 10;
                else if (base == 16 && c >= 'A' && c <= 'F')
                    d = c - 'A' + 10;
                if (d < 0)
                    return false;
                v = v * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
                if (v > 0xffffffffu)
                    return false;
            }
            out = static_cast<uint32_t>(v);
            return true;
        }

        // 五个实体与数字的字符引用
        std::string Decode(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            std::string_view rest = text;
            for (size_t amp; (amp = rest.find('&')) != std::string_view::npos;)
            {
                out += rest.substr(0, amp);
                rest.remove_prefix(amp);
                bool done = false;
                const size_t semi = rest.find(';');
                if (semi != std::string_view::npos && semi <= 10)
                {
                    const std::string_view name = rest.substr(1, semi - 1);
                    uint32_t cp = 0;
                    bool have = true;
                    if (name == "amp")
                        cp = '&';
                    else if (name == "lt")
                        cp = '<';
                    else if (name == "gt")
                        cp = '>';
                    else if (name == "quot")
                        cp = '"';
                    else if (name == "apos")
                        cp = '\'';
                    else if (name.starts_with("#x"))
                        have = Parse32(name.substr(2), 16, cp);
                    else if (name.starts_with('#'))
                        have = Parse32(name.substr(1), 10, cp);
                    else
                        have = false;
                    if (have && Bytes::PutUtf8(out, cp))
                    {
                        rest.remove_prefix(semi + 1);
                        done = true;
                    }
                }
                if (!done)
                {
                    out += '&';
                    rest.remove_prefix(1);
                }
            }
            out += rest;
            return out;
        }

        // 标签里的 name="value"
        std::optional<std::string> Attribute(std::string_view tag, std::string_view name)
        {
            const size_t at = tag.find(std::string(name) + "=");
            if (at == std::string_view::npos)
                return std::nullopt;
            const std::string_view rest = tag.substr(at + name.size() + 1);
            if (rest.empty() || (rest.front() != '"' && rest.front() != '\''))
                return std::nullopt;
            const size_t end = rest.find(rest.front(), 1);
            if (end == std::string_view::npos)
                return std::nullopt;
            return Decode(rest.substr(1, end - 1));
        }

        void Category(Atom& a, const std::string& term)
        {
            if (term.find('.') != std::string::npos && std::all_of(term.begin(), term.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; }))
                if (!a.omniclass)
                    a.omniclass = term;
            a.categories.push_back(term);
        }

        // parents 里的元素之下的一个元素结束了，里面的文字是 text
        void ElementEnded(Atom& a, const std::vector<std::string_view>& parents, std::string_view name, const std::string& text)
        {
            if (text.empty())
                return;
            const size_t colon = name.rfind(':');
            const std::string_view local = colon == std::string_view::npos ? name : name.substr(colon + 1);
            const bool plain = colon == std::string_view::npos;
            const bool inEntry = plain && parents.size() == 1 && parents[0] == "entry";
            if (local == "title" && inEntry)
            {
                if (!a.title)
                    a.title = text;
            }
            else if (local == "updated" && inEntry)
            {
                if (!a.updated)
                    a.updated = text;
            }
            else if (local == "product-version")
            {
                if (!a.productVersion)
                    a.productVersion = text;
            }
            else if (local == "term" && !parents.empty() && parents.back() == "category")
                Category(a, text);
        }
    }

    bool Atom::Parse(std::span<const uint8_t> bytes, Atom& out, Error& error)
    {
        const auto bad = [&](const std::string& why)
        {
            error = Error{ ErrorKind::Format, "the Atom entry " + why };
            return false;
        };
        if (bytes.size() > MaxAtom)
            return bad("is " + std::to_string(bytes.size()) + " bytes, over the limit of " + std::to_string(MaxAtom));
        const std::string_view xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!Bytes::IsUtf8(xml))
            return bad("is not UTF-8");
        Atom atom;
        std::vector<std::string_view> stack;
        std::string text;
        std::string_view rest = xml;
        size_t tags = 0;
        for (size_t open; (open = rest.find('<')) != std::string_view::npos;)
        {
            text += rest.substr(0, open);
            rest.remove_prefix(open);
            std::string_view skipTo = ">";
            bool element = false;
            if (rest.starts_with("<!--"))
                skipTo = "-->";
            else if (rest.starts_with("<![CDATA["))
                skipTo = "]]>";
            else if (rest.starts_with("<?"))
                skipTo = "?>";
            else if (!rest.starts_with("<!"))
                element = true;
            const size_t close = rest.find(skipTo);
            if (close == std::string_view::npos)
                return bad("is cut short in the middle of markup");
            std::string_view body = rest.substr(1, close - 1);
            rest.remove_prefix(close + skipTo.size());
            if (!element)
                continue;
            if (++tags > MaxTags)
                return bad("has too many tags");
            if (body.starts_with('/'))
            {
                const std::string_view name = Bytes::Trim(body.substr(1));
                if (stack.empty() || stack.back() != name)
                    return bad("closes `" + std::string(name) + "` where it is not open");
                stack.pop_back();
                ElementEnded(atom, stack, name, Decode(Bytes::Trim(text)));
                text.clear();
                continue;
            }
            const bool empty = body.ends_with('/');
            while (body.ends_with('/'))
                body.remove_suffix(1);
            const std::string_view trimmed = Bytes::Trim(body);
            const std::string_view name = trimmed.substr(0, std::min(trimmed.size(), trimmed.find_first_of(" \t\r\n\f\v")));
            if (name == "category")
                if (const auto term = Attribute(body, "term"))
                    Category(atom, *term);
            text.clear();
            if (!empty)
            {
                if (stack.size() >= MaxDepth)
                    return bad("nests deeper than " + std::to_string(MaxDepth) + " levels");
                stack.push_back(name);
            }
        }
        if (!stack.empty())
            return bad("is cut short: an element is never closed");
        out = std::move(atom);
        return true;
    }

    bool ProjectInformation(std::span<const uint8_t> bytes, size_t maxInflated, Atom& out, Error& error)
    {
        const auto bad = [&](const std::string& why)
        {
            error = Error{ ErrorKind::Format, "ProjectInformation " + why };
            return false;
        };
        const auto number = [&](size_t at, size_t len) -> std::optional<size_t>
        {
            if (at + len > bytes.size())
                return std::nullopt;
            size_t n = 0;
            for (size_t i = len; i-- > 0;)
                n = n << 8 | bytes[at + i];
            return n;
        };
        if (bytes.size() < 4 || std::memcmp(bytes.data(), "PK\x03\x04", 4) != 0)
            return bad("is not a ZIP archive");
        const auto method = number(8, 2), size = number(18, 4), nameLen = number(26, 2), extraLen = number(28, 2);
        if (!method || !size || !nameLen || !extraLen)
            return bad("is cut short in its first entry's header");
        // 条目的名字跳过，从不读出
        const size_t from = 30 + *nameLen + *extraLen;
        if (from > bytes.size())
            return bad("has no data after its first entry's header");
        const std::span<const uint8_t> data = bytes.subspan(from);
        const size_t limit = std::min(maxInflated, MaxAtom);
        std::vector<uint8_t> body;
        if (*method == 0)
        {
            if (*size > data.size())
                return bad("is cut short in its stored entry");
            if (*size > limit)
                return bad("holds an entry over the limit of " + std::to_string(limit) + " bytes");
            body.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(*size));
        }
        else if (*method == 8)
        {
            size_t used = 0;
            std::string reason;
            switch (Deflate::Inflate(data.data(), data.size(), body, limit, used, reason))
            {
            case Deflate::Status::TooMuch:
                return bad("holds an entry that inflates past the limit of " + std::to_string(limit) + " bytes");
            case Deflate::Status::Bad:
                return bad("does not inflate: " + reason);
            case Deflate::Status::Ok:
                break;
            }
        }
        else
            return bad("uses compression method " + std::to_string(*method) + ", which this reader does not know");
        return Atom::Parse(body, out, error);
    }

    bool DocumentAtom(Revit& r, std::optional<Atom>& out, Error& error)
    {
        out.reset();
        std::vector<uint8_t> stored;
        Atom atom;
        if (r.StreamLen("PartAtom"))
        {
            if (!r.ReadStream("PartAtom", stored, error) || !Atom::Parse(stored, atom, error))
                return false;
            out = std::move(atom);
            return true;
        }
        if (r.StreamLen("ProjectInformation"))
        {
            if (!r.ReadStream("ProjectInformation", stored, error) || !ProjectInformation(stored, MaxAtom, atom, error))
                return false;
            out = std::move(atom);
        }
        return true;
    }
}
