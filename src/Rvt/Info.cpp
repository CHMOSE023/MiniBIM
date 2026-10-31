// 移植自 BIMCraft crates/rvt/src/info.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/basic_file_info.rs.
// Changed: rewritten for BIMCraft, then in C++ for MiniBIM (see Rvt/Info.h).
#include "Rvt/Info.h"
#include "Rvt/Bytes.h"
#include "Rvt/Container.h"

#include <algorithm>

namespace MiniBIM::Rvt
{
    namespace
    {
        // 留下的行数与值的最大长度，免得恶意的流让块变大
        constexpr size_t MaxLines      = 256;
        constexpr size_t MaxValueBytes = 4096;

        // 块的第一行（Revit 2016 起），以及 2016 到 2018 版那种写法的 `Revit Build`
        constexpr const char* BlockStarts[2] = { "Worksharing:", "Revit Build:" };

        bool IsDigit(char c) { return c >= '0' && c <= '9'; }

        // text 开头四位里的年份，Revit 从 2008 起都这样写
        std::optional<uint32_t> Year(std::string_view text)
        {
            const std::string_view t = Bytes::Trim(text);
            if (t.size() < 4)
                return std::nullopt;
            std::string_view digits = t.substr(0, 4);
            if (digits.front() == '+')
                digits.remove_prefix(1);
            if (digits.empty() || !std::all_of(digits.begin(), digits.end(), IsDigit))
                return std::nullopt;
            uint32_t year = 0;
            for (const char c : digits)
                year = year * 10 + static_cast<uint32_t>(c - '0');
            if (year < 2000 || year > 2100)
                return std::nullopt;
            return year;
        }

        // `Autodesk Revit 2016 (Build: 20150220_1515(x64))` 得出 `20150220_1515(x64)`
        std::optional<std::string> LegacyBuild(std::string_view line)
        {
            const size_t at = line.find("(Build: ");
            if (at == std::string_view::npos)
                return std::nullopt;
            std::string_view build = line.substr(at + 8);
            while (!build.empty() && (build.back() == ' ' || build.back() == '\t' || build.back() == '\r' || build.back() == '\n'))
                build.remove_suffix(1);
            while (!build.empty() && build.back() == ')')
                build.remove_suffix(1);
            if (build.empty())
                return std::nullopt;
            return std::string(build) + (build.back() == ')' ? "" : "(x64)");
        }

        bool IsLabel(std::string_view key)
        {
            const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
            return !key.empty() && key.size() <= 96 && alpha(key.front()) &&
                   std::all_of(key.begin(), key.end(), [&](char c) { return alpha(c) || IsDigit(c) || c == ' ' || c == '\'' || c == '-' || c == '_'; });
        }

        bool IsSpace(uint32_t c) { return c == ' ' || (c >= 0x09 && c <= 0x0d) || c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
                                          c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000; }

        // 值的文字：窄的字符串里没有 NUL，宽的 ASCII 每个字符后面一个 NUL
        std::string Text(Bytes::Span value)
        {
            Bytes::Span trimmed = value;
            if (!trimmed.empty() && trimmed.front() == 0)
                trimmed = trimmed.subspan(1);
            if (!trimmed.empty() && trimmed.back() == 0)
                trimmed = trimmed.first(trimmed.size() - 1);
            if (trimmed.size() >= 2 && std::all_of(trimmed.begin(), trimmed.end(), [](uint8_t b) { return b >= 0x20 && b < 0x7f; }))
                return std::string(trimmed.begin(), trimmed.end());
            std::vector<uint16_t> units = Bytes::Units(value);
            // 两端去掉 NUL 与空白（码元层面即可：这些都在基本平面里）
            while (!units.empty() && (units.front() == 0 || IsSpace(units.front())))
                units.erase(units.begin());
            while (!units.empty() && (units.back() == 0 || IsSpace(units.back())))
                units.pop_back();
            return Bytes::FromUtf16(units, false).value_or("");
        }

        // 一行：由普通字符组成的键、冒号、值。键按 UTF-16 读到冒号为止；冒号与空格后面是值，是宽的文字，除非是窄的 ASCII 字符串
        std::optional<std::pair<std::string, std::string>> Line(Bytes::Span raw)
        {
            const std::vector<uint16_t> units = Bytes::Units(raw);
            const auto colon = std::find(units.begin(), units.end(), static_cast<uint16_t>(':'));
            if (colon == units.end())
                return std::nullopt;
            const auto key = Bytes::FromUtf16(std::vector<uint16_t>(units.begin(), colon), true);
            if (!key || !IsLabel(*key))
                return std::nullopt;
            const size_t after = (static_cast<size_t>(colon - units.begin()) + 1) * 2;
            if (after > raw.size())
                return std::nullopt;
            Bytes::Span value = raw.subspan(after);
            if (!value.empty())
            {
                if (value.size() < 2 || value[0] != ' ' || value[1] != 0)
                    return std::nullopt;
                value = value.subspan(2);
            }
            // 流的最后一行以窄的 CR LF 结尾，别的行是宽的
            if (value.size() >= 2 && value[value.size() - 2] == 0x0d && value.back() == 0x0a)
                value = value.first(value.size() - 2);
            value = value.first(std::min(value.size(), MaxValueBytes));
            return std::pair{ *key, Text(value) };
        }

        // 从 block 第一个字节起的 `键: 值` 行
        std::vector<std::pair<std::string, std::string>> Lines(Bytes::Span block)
        {
            static constexpr uint8_t crlf[4] = { 0x0d, 0, 0x0a, 0 };
            std::vector<std::pair<std::string, std::string>> out;
            size_t at = 0;
            while (at < block.size() && out.size() < MaxLines)
            {
                size_t end = Bytes::Find(block, Bytes::Span(crlf, 4), at);
                if (end == Bytes::npos)
                    end = block.size();
                if (auto line = Line(block.subspan(at, end - at)))
                    out.push_back(std::move(*line));
                at = end + 4;
            }
            return out;
        }
    }

    bool BasicFileInfo::Parse(std::span<const uint8_t> bytes, BasicFileInfo& out, Error& error)
    {
        const auto bad = [&](const std::string& why)
        {
            error = Error{ ErrorKind::Format, "BasicFileInfo " + why };
            return false;
        };
        size_t start = Bytes::npos;
        for (const char* anchor : BlockStarts)
        {
            const std::vector<uint8_t> w = Bytes::Wide(anchor);
            start = std::min(start, Bytes::Find(bytes, w));
        }
        if (start == Bytes::npos)
            return bad("has no block of `Key: value` lines, so it is not what Revit writes");
        BasicFileInfo info;
        info.properties = Lines(bytes.subspan(start));
        const auto find = [&](std::string_view key) -> std::optional<std::string>
        {
            for (const auto& [k, v] : info.properties)
                if (k == key)
                    return v.empty() ? std::nullopt : std::optional<std::string>(v);
            return std::nullopt;
        };
        std::optional<uint32_t> release;
        if (const auto f = find("Format"))
            release = Year(*f);
        if (!release)
            if (const auto rb = find("Revit Build"))
                if (const size_t at = rb->find("Autodesk Revit "); at != std::string::npos)
                    release = Year(std::string_view(*rb).substr(at + 15));
        if (!release)
            return bad("names no release (no `Format` or `Revit Build` line with a year)");
        info.release = *release;
        info.build   = find("Build");
        if (!info.build)
            if (const auto rb = find("Revit Build"))
                info.build = LegacyBuild(*rb);
        info.guid             = find("Unique Document GUID");
        info.locale           = find("Locale when saved");
        info.worksharing      = find("Worksharing");
        info.username         = find("Username");
        info.lastSavePath     = find("Last Save Path");
        info.centralModelPath = find("Central Model Path");
        out = std::move(info);
        return true;
    }

    std::optional<std::string> BasicFileInfo::Property(std::string_view key) const
    {
        for (const auto& [k, v] : properties)
            if (k == key)
                return v;
        return std::nullopt;
    }

    bool ReadBasicFileInfo(Revit& r, BasicFileInfo& out, Error& error)
    {
        std::vector<uint8_t> bytes;
        return r.ReadStream(BasicFileInfoStream, bytes, error) && BasicFileInfo::Parse(bytes, out, error);
    }
}
