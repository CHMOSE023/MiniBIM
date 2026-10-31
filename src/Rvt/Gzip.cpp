// 移植自 BIMCraft crates/rvt/src/gzip.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/compression.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Gzip.h).
#include "Rvt/Gzip.h"
#include "Rvt/Bytes.h"
#include "Rvt/Inflate.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace MiniBIM::Rvt::Gzip
{
    namespace
    {
        // 第一段在流里多远以内找
        constexpr size_t FirstMemberWithin = 256;

        const Bytes::Span MagicSpan(Magic, 3);

        // at 处 gzip 头连同可选字段的长度；截断了或用了保留的标志时为空
        std::optional<size_t> HeaderLen(std::span<const uint8_t> data, size_t at)
        {
            if (at > data.size() || data.size() - at < 10 || std::memcmp(data.data() + at, Magic, 3) != 0)
                return std::nullopt;
            const uint8_t flags = data[at + 3];
            if (flags & 0xe0)
                return std::nullopt;
            size_t pos = at + 10;
            if (flags & 0x04)
            {
                if (data.size() - pos < 2)
                    return std::nullopt;
                pos += 2 + Bytes::U16(data.data() + pos);
            }
            for (const uint8_t flag : { uint8_t(0x08), uint8_t(0x10) })
                if (flags & flag)
                {
                    if (pos >= data.size())
                        return std::nullopt;
                    const auto nul = std::find(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end(), 0);
                    if (nul == data.end())
                        return std::nullopt;
                    pos = static_cast<size_t>(nul - data.begin()) + 1;
                }
            if (flags & 0x02)
                pos += 2;
            if (pos > data.size())
                return std::nullopt;
            return pos - at;
        }

        // 把 at 处的一段解到 out 后面，out 最多再长 room 字节（失败时 out 不变）。end 为这段在 data 里结束的位置，
        // trailer 为它后面 8 个字节是否是它的 CRC-32 与长度
        bool MemberInto(std::span<const uint8_t> data, size_t at, std::vector<uint8_t>& out, size_t room, size_t& end, bool& trailer, Fail& fail)
        {
            const auto head = HeaderLen(data, at);
            if (!head)
            {
                fail = Fail{ Fail::Kind::Bad, at, "the gzip header is cut short" };
                return false;
            }
            const size_t body = at + *head, start = out.size();
            size_t used = 0;
            std::string reason;
            switch (Deflate::Inflate(data.data() + body, data.size() - body, out, room, used, reason))
            {
            case Deflate::Status::TooMuch:
                fail = Fail{ Fail::Kind::TooMuch, at, {} };
                return false;
            case Deflate::Status::Bad:
                fail = Fail{ Fail::Kind::Bad, at, reason };
                return false;
            case Deflate::Status::Ok:
                break;
            }
            end = body + used;
            const uint32_t crc = Deflate::Crc32(out.data() + start, out.size() - start);
            trailer = data.size() - end >= 8 && Bytes::U32(data.data() + end) == crc &&
                      Bytes::U32(data.data() + end + 4) == static_cast<uint32_t>(out.size() - start);
            return true;
        }
    }

    bool IsPaged(std::string_view stream)
    {
        if (stream.starts_with("Partitions/"))
        {
            const std::string_view rest = stream.substr(11);
            return !rest.empty() && rest.find('/') == std::string_view::npos;
        }
        for (const std::string_view s : { "Formats/Latest", "Global/ContentDocuments", "Global/DocumentIncrementTable", "Global/ElemTable",
                                          "Global/History", "Global/Latest", "Global/PartitionTable" })
            if (stream == s)
                return true;
        return false;
    }

    void StripPages(std::vector<uint8_t>& data)
    {
        const size_t full = data.size() / PageStored;
        for (size_t page = 1; page < full; ++page)
            std::memmove(data.data() + page * PagePayload, data.data() + page * PageStored, PagePayload);
        if (full > 0)
        {
            const size_t tail = full * PageStored, kept = data.size() - tail;
            std::memmove(data.data() + full * PagePayload, data.data() + tail, kept);
            data.resize(full * PagePayload + kept);
        }
    }

    bool InflateFirst(std::span<const uint8_t> data, size_t room, Inflated& out, Fail& fail)
    {
        fail = Fail{};
        const Bytes::Span head = data.first(std::min(data.size(), FirstMemberWithin));
        for (size_t at = Bytes::Find(head, MagicSpan); at != Bytes::npos; at = Bytes::Find(head, MagicSpan, at + 3))
        {
            Inflated got;
            size_t end = 0;
            bool trailer = false;
            Fail f;
            if (MemberInto(data, at, got.bytes, room, end, trailer, f))
            {
                got.ends.push_back(got.bytes.size());
                got.members    = 1;
                got.trailersOk = trailer ? 1 : 0;
                out = std::move(got);
                return true;
            }
            fail = f;
            if (f.kind == Fail::Kind::TooMuch)
                return false;
        }
        return false;
    }

    bool InflateMembers(std::span<const uint8_t> data, size_t room, Inflated& out, Fail& fail)
    {
        out = Inflated{};
        std::optional<Fail> last;
        size_t floor = 0;
        // 刚解开的一段里的魔数是数据，不是一段：从那段的结尾（有 CRC 与长度时再过 8 个字节）接着找
        for (size_t at = Bytes::Find(data, MagicSpan); at != Bytes::npos; at = Bytes::Find(data, MagicSpan, std::max(at + 3, floor)))
        {
            if (at < floor)
                continue;
            size_t end = 0;
            bool trailer = false;
            Fail f;
            if (MemberInto(data, at, out.bytes, room - std::min(room, out.bytes.size()), end, trailer, f))
            {
                out.ends.push_back(out.bytes.size());
                ++out.members;
                out.trailersOk += trailer ? 1 : 0;
                floor = end + (trailer ? 8 : 0);
            }
            else if (f.kind == Fail::Kind::TooMuch)
            {
                fail = f;
                return false;
            }
            else
            {
                ++out.skipped;
                last = f;
            }
        }
        if (out.members == 0)
        {
            fail = last.value_or(Fail{});
            return false;
        }
        return true;
    }
}
