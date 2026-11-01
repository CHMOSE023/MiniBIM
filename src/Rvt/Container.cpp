// 移植自 BIMCraft crates/rvt/src/container.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/reader.rs and src/streams.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Container.h).
#include "Rvt/Container.h"
#include "Rvt/Cfb.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace MiniBIM::Rvt
{
    namespace
    {
        constexpr uint8_t Signature[8] = { 0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1 };

        Error Err(ErrorKind k, std::string m) { return Error{ k, std::move(m) }; }

        Error TooBig(const std::string& what, uint64_t limit)
        {
            return Err(ErrorKind::TooBig, what + ", over the limit of " + std::to_string(limit));
        }

        bool CheckSize(uint64_t size, const Limits& limits, Error& error)
        {
            if (size > limits.maxFile)
            {
                error = TooBig("the file is " + std::to_string(size) + " bytes", limits.maxFile);
                return false;
            }
            return true;
        }
    }

    Revit::Revit() = default;
    Revit::~Revit() = default;
    Revit::Revit(Revit&&) noexcept = default;
    Revit& Revit::operator=(Revit&&) noexcept = default;

    bool Revit::Open(const std::string& path, const Limits& limits, Revit& out, Error& error)
    {
        const std::filesystem::path p(std::u8string(path.begin(), path.end()));
        std::error_code ec;
        const auto named = [&](const std::string& why) { return Err(ErrorKind::Io, path + ": " + why); };
        if (std::filesystem::is_directory(p, ec))
        {
            error = named("is a folder, not a Revit file");
            return false;
        }
        const uintmax_t size = std::filesystem::file_size(p, ec);
        if (ec)
        {
            error = named(ec.message());
            return false;
        }
        if (!CheckSize(size, limits, error))
            return false;
        auto source = Cfb::FromFile(path);
        if (!source)
        {
            error = named("cannot be opened");
            return false;
        }
        return New(std::move(source), size, limits, out, error);
    }

    bool Revit::FromBytes(std::vector<uint8_t> bytes, const Limits& limits, Revit& out, Error& error)
    {
        if (!CheckSize(bytes.size(), limits, error))
            return false;
        const uint64_t size = bytes.size();
        return New(Cfb::FromMemory(std::move(bytes)), size, limits, out, error);
    }

    bool Revit::New(std::unique_ptr<Cfb::Source> source, uint64_t size, const Limits& limits, Revit& out, Error& error)
    {
        uint8_t head[8] = {};
        const size_t got = static_cast<size_t>(std::min<uint64_t>(size, 8));
        if (got && !source->ReadAt(0, head, got))
        {
            error = Err(ErrorKind::Io, "the file cannot be read");
            return false;
        }
        if (size == 0)
        {
            error = Err(ErrorKind::NotRevit, "the file is empty, so it is not a Revit file");
            return false;
        }
        if (got < 8 || !std::equal(head, head + 8, Signature))
        {
            std::string seen;
            for (size_t i = 0; i < got; ++i)
            {
                char hex[4];
                std::snprintf(hex, sizeof hex, "%02X", head[i]);
                seen += (i ? " " : "") + std::string(hex);
            }
            error = Err(ErrorKind::NotRevit, "not a Revit file: it starts with " + seen +
                                                 ", not the Compound File signature D0 CF 11 E0 A1 B1 1A E1 that Revit files start with");
            return false;
        }
        Revit r;
        r.m_limits = limits;
        r.m_file   = std::make_unique<Cfb::File>();
        std::string why;
        if (!r.m_file->Open(std::move(source), why))
        {
            error = Err(ErrorKind::Damaged, "the Compound File container is damaged: " + why);
            return false;
        }
        const auto& streams = r.m_file->Streams();
        if (streams.size() > limits.maxStreams)
        {
            error = TooBig("the file has " + std::to_string(streams.size()) + " streams", limits.maxStreams);
            return false;
        }
        for (size_t i = 0; i < streams.size(); ++i)
            r.m_streams.emplace_back(streams[i].path, i);
        std::sort(r.m_streams.begin(), r.m_streams.end());
        if (!r.StreamLen(BasicFileInfoStream))
        {
            std::string names;
            for (size_t i = 0; i < r.m_streams.size() && i < 8; ++i)
                names += (i ? ", " : "") + r.m_streams[i].first;
            error = Err(ErrorKind::NotRevit, std::string("this is a Compound File but not a Revit one: it has no ") + BasicFileInfoStream +
                                                 " stream (its streams: " + names + ")");
            return false;
        }
        out = std::move(r);
        return true;
    }

    std::vector<std::string> Revit::StreamNames() const
    {
        std::vector<std::string> out;
        for (const auto& [name, _] : m_streams)
            out.push_back(name);
        return out;
    }

    std::optional<uint64_t> Revit::StreamLen(std::string_view name) const
    {
        for (const auto& [n, i] : m_streams)
            if (n == name)
                return m_file->Streams()[i].size;
        return std::nullopt;
    }

    bool Revit::ReadStream(std::string_view name, std::vector<uint8_t>& out, Error& error)
    {
        const Cfb::Stream* s = nullptr;
        for (const auto& [n, i] : m_streams)
            if (n == name)
                s = &m_file->Streams()[i];
        if (!s)
        {
            error = Err(ErrorKind::NoStream, "the file has no stream named " + std::string(name));
            return false;
        }
        if (s->size > m_limits.maxStream)
        {
            error = TooBig("stream " + std::string(name) + " is " + std::to_string(s->size) + " bytes", m_limits.maxStream);
            return false;
        }
        std::string why;
        if (!m_file->Read(*s, out, why))
        {
            error = Err(ErrorKind::Damaged, "stream " + std::string(name) + " cannot be read: " + why);
            return false;
        }
        if (out.size() != s->size)
        {
            error = Err(ErrorKind::Damaged, "stream " + std::string(name) + " holds " + std::to_string(out.size()) + " bytes but its directory entry says " +
                                                std::to_string(s->size) + ": the file is cut short or damaged");
            return false;
        }
        return true;
    }

    bool Revit::Unpaged(std::string_view name, std::vector<uint8_t>& out, Error& error)
    {
        if (!ReadStream(name, out, error))
            return false;
        if (Gzip::IsPaged(name))
            Gzip::StripPages(out);
        return true;
    }

    Error Revit::Failure(std::string_view name, const Gzip::Fail& fail) const
    {
        const std::string stream(name);
        switch (fail.kind)
        {
        case Gzip::Fail::Kind::NoMember:
            return Err(ErrorKind::NoGzip, "stream " + stream + " has no gzip data in it");
        case Gzip::Fail::Kind::Bad:
            return Err(ErrorKind::BadDeflate, "stream " + stream + " does not inflate at byte " + std::to_string(fail.offset) + ": " + fail.reason);
        case Gzip::Fail::Kind::TooMuch:
            break;
        }
        return Err(ErrorKind::TooMuch, "stream " + stream + " inflates to more than " + std::to_string(m_limits.maxInflated) +
                                           " bytes, the limit; the file is damaged or hostile");
    }

    bool Revit::Inflate(std::string_view name, Gzip::Inflated& out, Error& error)
    {
        std::vector<uint8_t> data;
        if (!Unpaged(name, data, error))
            return false;
        Gzip::Fail fail;
        if (!Gzip::InflateFirst(data, m_limits.maxInflated, out, fail))
        {
            error = Failure(name, fail);
            return false;
        }
        return true;
    }

    bool Revit::InflateAll(std::string_view name, Gzip::Inflated& out, Error& error)
    {
        std::vector<uint8_t> data;
        if (!Unpaged(name, data, error))
            return false;
        Gzip::Fail fail;
        if (!Gzip::InflateMembers(data, m_limits.maxInflated, out, fail))
        {
            error = Failure(name, fail);
            return false;
        }
        return true;
    }
}
