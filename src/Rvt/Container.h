// 移植自 BIMCraft crates/rvt/src/container.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/reader.rs (opening a file as a
// Compound File, its size and stream limits, listing and reading streams) and src/streams.rs (the stream names).
// Changed: rewritten for BIMCraft (the file read through the container on demand, every limit one field of Limits, the
// signature and Revit-ness checked up front, a failure inside the container an error and never a crash); then rewritten in
// C++ for MiniBIM on its own Compound File reader (Rvt/Cfb.h).
//
// 打开 Revit 文件：复合文档（MS-CFB）容器、它的流，以及让恶意文件无害的上限。流按需读出，存的原样（ReadStream）或解开的（Inflate、InflateAll）
#pragma once

#include "Rvt/Error.h"
#include "Rvt/Gzip.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MiniBIM::Rvt
{
    namespace Cfb
    {
        class File;
        class Source;
    }

    // 每个 Revit 文件都有、别的复合文档不大会有的流
    inline constexpr const char* BasicFileInfoStream = "BasicFileInfo";

    // 文件能让读取器做多少事；超过任何一项都拒绝，报错说出是哪一项
    struct Limits
    {
        uint64_t maxFile     = 2ull << 30;     // 文件的字节数
        size_t   maxStreams  = 1024;           // 容器里流的个数
        uint64_t maxStream   = 512ull << 20;   // 一个流存着的字节（按目录项说的；流整个读进来）
        size_t   maxInflated = 512u << 20;     // 一个流所有 gzip 段解开后的字节
    };

    class Revit
    {
    public:
        Revit();
        ~Revit();
        Revit(Revit&&) noexcept;
        Revit& operator=(Revit&&) noexcept;

        // 打开 path（UTF-8）处的文件：现在只读容器，流到要时才读
        static bool Open(const std::string& path, const Limits& limits, Revit& out, Error& error);
        // 打开已在内存里的文件
        static bool FromBytes(std::vector<uint8_t> bytes, const Limits& limits, Revit& out, Error& error);

        // 每个流的路径（Global/Latest），排好序
        std::vector<std::string> StreamNames() const;
        // 流的目录项说的存储大小
        std::optional<uint64_t> StreamLen(std::string_view name) const;
        // 流存着的字节（带着页）
        bool ReadStream(std::string_view name, std::vector<uint8_t>& out, Error& error);
        // 流的第一段 gzip 解开（Global/* 流、类表）；容器分页存它时先去掉页
        bool Inflate(std::string_view name, Gzip::Inflated& out, Error& error);
        // 流的所有 gzip 段解开接起来（分区）
        bool InflateAll(std::string_view name, Gzip::Inflated& out, Error& error);

        const Limits& GetLimits() const { return m_limits; }

    private:
        static bool New(std::unique_ptr<Cfb::Source> source, uint64_t size, const Limits& limits, Revit& out, Error& error);
        bool Unpaged(std::string_view name, std::vector<uint8_t>& out, Error& error);
        Error Failure(std::string_view name, const Gzip::Fail& fail) const;

        std::unique_ptr<Cfb::File> m_file;
        // 每个流：路径与它在 m_file->Streams() 里的下标，按路径排好序
        std::vector<std::pair<std::string, size_t>> m_streams;
        Limits m_limits;
    };
}
