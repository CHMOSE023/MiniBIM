// 移植自 BIMCraft crates/rvt/src/gzip.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/compression.rs (the gzip header
// parse, the list of paged streams, the bounded inflate loop, finding members by their magic).
// Changed: rewritten for BIMCraft with no panics, page stripping in place, members found by walking from the end of the
// last one, the inflate limit applied to a whole stream, and the trailer checked against the data; then rewritten in C++
// for MiniBIM on its own inflate (Rvt/Inflate.h).
// Not ported from rvt-rs: the page size and the data in a page, which BIMCraft observed in the sample files (see PageStored).
//
// Revit 的 gzip 流：流里是前缀后面的若干段标准 gzip。Global/* 流是 8 个字节再一段，Formats/Latest 一开头就是一段，分区是一个头
// 再接每段约 128 KiB 的若干段，段与段之间 40 个字节（上一段的 CRC-32 与长度，再 32 字节的头）。据说 Revit 有的段不写 CRC 与长度，
// 所以结尾缺了或不对只记下来，不算错误：数据好不好由 deflate 流决定。
// 容器把大的流存成页：每 64,896 个字节的数据后面跟 353 个不是数据的字节，解压前要先去掉（StripPages）
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MiniBIM::Rvt::Gzip
{
    inline constexpr uint8_t Magic[3] = { 0x1f, 0x8b, 0x08 };

    // 观察所得，不是移植的（BIMCraft gzip.rs 的说明）：在四个样例项目（rstbasicsampleproject.rvt，2023 到 2026 版）里，
    // 长于 64,896 字节的每个流的 gzip 段都只有去掉每 65,249 字节一页的最后 353 个字节（页从流的第 0 个字节数起）后才校验得过
    // （CRC-32 与长度）。证据与它的局限见 BIMCraft 的 docs/rvt-provenance.md 的 "The stored-page constants"
    inline constexpr size_t PageStored  = 65'249;   // 一整页在容器里占的字节
    inline constexpr size_t PagePayload = 64'896;   // 其中的数据；另外 353 个不属于任何 gzip 段

    // 容器是否把这个流存成页：分区、类表，以及存 gzip 的那些 Global 流（列表来自 rvt-rs）
    bool IsPaged(std::string_view stream);
    // 就地去掉每一整页的尾巴；不足一整页的最后一页整个保留
    void StripPages(std::vector<uint8_t>& data);

    // 流里的各段 gzip，解开接在一起
    struct Inflated
    {
        std::vector<uint8_t> bytes;
        std::vector<size_t>  ends;          // 每段的输出在 bytes 里结束的位置
        size_t               members = 0;
        size_t               trailersOk = 0;   // 后面跟着与输出相符的 CRC-32 与长度的段数
        size_t               skipped = 0;      // 找到了 gzip 魔数却解不开的（填充，或其实是数据的头）
    };

    // 为什么解不开；容器把它换成指明流名的 Error
    struct Fail
    {
        enum class Kind
        {
            NoMember,
            Bad,
            TooMuch,
        } kind = Kind::NoMember;
        size_t      offset = 0;
        std::string reason;
    };

    // 已去掉页的流里的第一段，在流的前 256 个字节里找
    bool InflateFirst(std::span<const uint8_t> data, size_t room, Inflated& out, Fail& fail);
    // 已去掉页的流里的每一段，按顺序接起来；room 限制全部输出
    bool InflateMembers(std::span<const uint8_t> data, size_t room, Inflated& out, Fail& fail);
}
