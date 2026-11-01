// 移植自 BIMCraft crates/rvt/src/schema.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/formats.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Schema.h).
#include "Rvt/Schema.h"
#include "Rvt/Bytes.h"
#include "Rvt/Container.h"

#include <algorithm>
#include <cstdio>

namespace MiniBIM::Rvt
{
    namespace
    {
        constexpr uint16_t FirstTag   = 12;
        constexpr size_t   MaxNesting = 64;
        constexpr uint32_t MaxFields  = 2'000;
        constexpr uint32_t MaxArray   = 65'536;
        constexpr size_t   MaxGuids   = 60'000;

        // Rust 的 {:#x}
        std::string Hex(size_t v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "0x%zx", v);
            return buf;
        }

        class Reader
        {
        public:
            explicit Reader(std::span<const uint8_t> data) : m_data(data) {}

            std::vector<Schema::Class> classes;
            uint16_t                   nextTag = FirstTag;
            std::string                why;

            std::optional<uint16_t> U16(size_t at) const { return Bytes::U16At(m_data, at); }
            std::optional<uint32_t> U32(size_t at) const { return Bytes::U32At(m_data, at); }

            // 读一个类：成功时给出它的标签与结束的位置
            bool Class(size_t at, size_t depth, uint16_t& tag, size_t& next)
            {
                if (depth > MaxNesting)
                    return Fail("classes nest too deep at " + Hex(at));
                std::string name;
                size_t p = 0;
                if (!Name(at, 2, name, p))
                    return false;
                tag = nextTag;
                if (tag + 1 >= 0x8000)
                    return Fail("too many classes at " + Hex(at));
                nextTag = static_cast<uint16_t>(tag + 1);
                const auto reference = U16(p);
                if (!reference)
                    return Past("base reference", p);
                p += 2;
                std::optional<uint16_t> base;
                if (*reference & 0x8000)
                {
                    uint16_t inlineTag = 0;
                    if (!Inline(p, *reference, depth, inlineTag, p))
                        return false;
                    base = inlineTag;
                }
                else if (*reference != 0)
                    base = *reference;
                const auto version = U32(p), fieldCount = U32(p + 4);
                if (!version)
                    return Past("version", p);
                if (!fieldCount)
                    return Past("field count", p + 4);
                p += 8;
                if (*fieldCount > MaxFields)
                    return Fail("field count " + std::to_string(*fieldCount) + " at " + Hex(p - 4));
                for (uint32_t i = 0; i < *fieldCount; ++i)
                {
                    std::string field;
                    bool ref = false;
                    if (!Name(p, 4, field, p) || !Type(p, depth, ref, p))
                        return false;
                }
                const auto guids = U32(p);
                if (!guids)
                    return Past("GUID count", p);
                p += 4;
                if (*guids > MaxGuids || static_cast<uint64_t>(*guids) * 16 > m_data.size() - std::min(p, m_data.size()))
                    return Fail("GUID count " + std::to_string(*guids) + " at " + Hex(p - 4));
                p += static_cast<size_t>(*guids) * 16;
                classes.push_back({ tag, std::move(name), base, *version, *fieldCount });
                next = p;
                return true;
            }

        private:
            std::span<const uint8_t> m_data;

            bool Fail(std::string reason)
            {
                why = std::move(reason);
                return false;
            }
            bool Past(const char* what, size_t at) { return Fail(std::string(what) + " past the end at " + Hex(at)); }

            // 计数宽 width（2 或 4）字节的 ASCII 名字
            bool Name(size_t at, size_t width, std::string& out, size_t& end)
            {
                size_t len = 0;
                if (width == 2)
                {
                    const auto n = U16(at);
                    if (!n)
                        return Past("name length", at);
                    len = *n;
                }
                else
                {
                    const auto n = U32(at);
                    if (!n)
                        return Past("name length", at);
                    len = *n;
                }
                const size_t start = at + width;
                if (len < 1 || len > 250 || start > m_data.size() || m_data.size() - start < len)
                    return Fail("bad name length " + std::to_string(len) + " at " + Hex(at));
                const auto raw = m_data.subspan(start, len);
                if (!std::all_of(raw.begin(), raw.end(), [](uint8_t b) { return b >= 0x20 && b < 0x7f; }))
                    return Fail("name is not ASCII at " + Hex(at));
                out.assign(raw.begin(), raw.end());
                end = start + len;
                return true;
            }

            bool Inline(size_t at, uint16_t reference, size_t depth, uint16_t& tag, size_t& next)
            {
                if (U16(at) != uint16_t(0))
                    return Fail("inline pad is not 0 at " + Hex(at));
                if (!Class(at + 2, depth + 1, tag, next))
                    return false;
                if (tag != (reference & 0x7fff))
                    return Fail("inline tag " + std::to_string(tag) + " does not match reference " + std::to_string(reference & 0x7fff) + " at " + Hex(at));
                return true;
            }

            bool ClassRef(size_t at, size_t depth, size_t& next)
            {
                const auto reference = U16(at);
                if (!reference)
                    return Past("class reference", at);
                if (!(*reference & 0x8000))
                {
                    next = at + 2;
                    return true;
                }
                uint16_t tag = 0;
                return Inline(at + 2, *reference, depth, tag, next);
            }

            // 读一个类型：ref 为它是否带类引用
            bool Type(size_t at, size_t depth, bool& ref, size_t& next)
            {
                if (depth > MaxNesting)
                    return Fail("types nest too deep at " + Hex(at));
                const auto word = U32(at);
                if (!word)
                    return Past("type word", at);
                at += 4;
                const uint8_t kind = static_cast<uint8_t>(*word & 0xff), modifier = static_cast<uint8_t>(*word >> 8 & 0xff);
                const uint8_t high = modifier & 0xf0;
                if (high != 0x00 && high != 0x10 && high != 0x50 && high != 0x60)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "0x%02x", modifier);
                    return Fail(std::string("unknown modifier ") + buf + " at " + Hex(at - 4));
                }
                if (high == 0x10)
                {
                    const auto count = U32(at);
                    if (!count)
                        return Past("array count", at);
                    if (*count > MaxArray)
                        return Fail("array count " + std::to_string(*count) + " at " + Hex(at));
                    at += 4;
                }
                if (kind == 0x0d)
                {
                    static constexpr uint8_t filler[5] = { 1, 0, 0, 0, 0x20 };
                    if (at > m_data.size() || m_data.size() - at < 5 || !std::equal(filler, filler + 5, m_data.begin() + static_cast<std::ptrdiff_t>(at)))
                        return Fail("array filler at " + Hex(at));
                    bool inner = false;
                    size_t after = 0;
                    if (!Type(at + 5, depth + 1, inner, after))
                        return false;
                    ref = inner;
                    if (inner)
                        return ClassRef(after, depth, next);
                    next = after;
                    return true;
                }
                if (kind == 0x0e && (modifier & 0x0f) == 0)
                {
                    ref = true;
                    return ClassRef(at, depth, next);
                }
                ref  = false;
                next = at;
                return true;
            }
        };
    }

    const Schema::Class* Schema::ByTag(uint16_t tag) const
    {
        if (classes.empty() || tag < classes.front().tag)
            return nullptr;
        const size_t i = tag - classes.front().tag;
        return i < classes.size() && classes[i].tag == tag ? &classes[i] : nullptr;
    }

    std::optional<uint16_t> Schema::Tag(std::string_view name) const
    {
        for (const Class& c : classes)
            if (c.name == name)
                return c.tag;
        return std::nullopt;
    }

    Schema Schema::Parse(std::span<const uint8_t> data)
    {
        Reader reader(data);
        size_t at = 0;
        std::optional<std::string> stopped;
        for (;;)
        {
            if (at + 2 > data.size())
                break;
            if (reader.U16(at) != uint16_t(0))
            {
                stopped = "record separator is not 0 at " + Hex(at);
                break;
            }
            at += 2;
            if (at >= data.size() || std::all_of(data.begin() + static_cast<std::ptrdiff_t>(at), data.end(), [](uint8_t b) { return b == 0; }))
            {
                at = data.size();
                break;
            }
            const uint16_t tag = reader.nextTag;
            const size_t count = reader.classes.size();
            uint16_t got = 0;
            size_t next = 0;
            if (!reader.Class(at, 0, got, next))
            {
                reader.classes.resize(count);
                reader.nextTag = tag;
                stopped = reader.why;
                break;
            }
            at = next;
        }
        std::stable_sort(reader.classes.begin(), reader.classes.end(), [](const Class& a, const Class& b) { return a.tag < b.tag; });
        Schema s;
        s.classes     = std::move(reader.classes);
        s.parsedBytes = at;
        s.stopped     = std::move(stopped);
        return s;
    }

    bool ReadSchema(Revit& r, Schema& out, Error& error)
    {
        Gzip::Inflated got;
        if (!r.Inflate("Formats/Latest", got, error))
            return false;
        Schema schema = Schema::Parse(got.bytes);
        if (schema.classes.empty())
        {
            error = Error{ ErrorKind::Format, "Formats/Latest holds no class the reader can read (" + schema.stopped.value_or("it is empty") + ")" };
            return false;
        }
        out = std::move(schema);
        return true;
    }
}
