// 移植自 BIMCraft crates/rvt/src/preview.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/reader.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Preview.h).
#include "Rvt/Preview.h"
#include "Rvt/Bytes.h"
#include "Rvt/Container.h"
#include "Rvt/Inflate.h"

#include <cstring>
#include <string>

namespace MiniBIM::Rvt
{
    namespace
    {
        constexpr uint8_t  Signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
        constexpr size_t   MaxChunk  = MaxPng;
        constexpr uint32_t MaxSide   = 8192;
        constexpr size_t   MaxChunks = 10'000;

        uint32_t Be32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3]; }
    }

    bool Preview::Find(std::span<const uint8_t> stream, Preview& out, Error& error)
    {
        const auto bad = [&](const std::string& why)
        {
            error = Error{ ErrorKind::Format, "the preview image " + why };
            return false;
        };
        const size_t start = Bytes::Find(stream, Bytes::Span(Signature, 8));
        if (start == Bytes::npos)
            return bad("has no PNG in it");
        const std::span<const uint8_t> png = stream.subspan(start);
        size_t at = sizeof Signature;
        uint32_t width = 0, height = 0;
        for (size_t index = 0; index < MaxChunks; ++index)
        {
            if (png.size() < at + 8)
                return bad(index == 0 ? "is cut short" : "has no IEND chunk");
            const uint8_t* head = png.data() + at;
            const size_t len = Be32(head);
            const char kind[5] = { static_cast<char>(head[4]), static_cast<char>(head[5]), static_cast<char>(head[6]), static_cast<char>(head[7]), 0 };
            if (len > MaxChunk)
                return bad("has a chunk of " + std::to_string(len) + " bytes, over the limit of " + std::to_string(MaxChunk));
            if (index == 0 && (std::strcmp(kind, "IHDR") != 0 || len != 13))
                return bad("does not open with an IHDR chunk of 13 bytes");
            const size_t end = at + 8 + len + 4;
            if (png.size() < end)
                return bad("is cut short in a chunk");
            const uint8_t* data = head + 8;
            const uint32_t crc = Deflate::Crc32(data, len, Deflate::Crc32(head + 4, 4));
            if (crc != Be32(data + len))
                return bad("has a chunk " + std::string(kind) + " whose checksum is wrong");
            if (index == 0)
            {
                width  = Be32(data);
                height = Be32(data + 4);
                if (width < 1 || width > MaxSide || height < 1 || height > MaxSide)
                    return bad("has dimensions " + std::to_string(width) + " by " + std::to_string(height) + ", outside 1 to " + std::to_string(MaxSide));
            }
            at = end;
            if (at > MaxPng)
                return bad("is over " + std::to_string(MaxPng) + " bytes");
            if (std::strcmp(kind, "IEND") == 0)
            {
                out = Preview{ width, height, std::vector<uint8_t>(png.begin(), png.begin() + static_cast<std::ptrdiff_t>(at)) };
                return true;
            }
        }
        return bad("has more than " + std::to_string(MaxChunks) + " chunks");
    }

    bool ReadPreview(Revit& r, std::optional<Preview>& out, Error& error)
    {
        out.reset();
        if (!r.StreamLen("RevitPreview4.0"))
            return true;
        std::vector<uint8_t> stream;
        Preview p;
        if (!r.ReadStream("RevitPreview4.0", stream, error) || !Preview::Find(stream, p, error))
            return false;
        out = std::move(p);
        return true;
    }
}
