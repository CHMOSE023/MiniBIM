// MiniBIM 为 Revit 读取器写的字节工具（代替 BIMCraft 用的 memchr 与 Rust 标准库的 UTF-16 解码），随 src/Rvt 以 Apache-2.0 发布，见 src/Rvt/NOTICE
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MiniBIM::Rvt::Bytes
{
    using Span = std::span<const uint8_t>;
    inline constexpr size_t npos = static_cast<size_t>(-1);

    // 小端整数；调用方先保证 p 起有足够的字节
    inline uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
    inline uint32_t U32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24; }
    inline uint64_t U64(const uint8_t* p) { return static_cast<uint64_t>(U32(p)) | static_cast<uint64_t>(U32(p + 4)) << 32; }

    // 越界时为空的读法（小端）
    std::optional<uint16_t> U16At(Span buf, size_t at);
    std::optional<uint32_t> U32At(Span buf, size_t at);
    std::optional<uint64_t> U64At(Span buf, size_t at);
    std::optional<double>   F64At(Span buf, size_t at);

    // needle 在 hay 里从 from 起第一次出现的位置；没有时为 npos
    size_t Find(Span hay, Span needle, size_t from = 0);
    // needle 在 hay 里每一处不重叠的出现，按顺序交给 f（memchr 的 memmem::find_iter）
    template <class F>
    void ForEach(Span hay, Span needle, F&& f)
    {
        for (size_t at = Find(hay, needle); at != npos; at = Find(hay, needle, at + needle.size()))
            f(at);
    }
    // 最后一处出现；没有时为 npos
    size_t FindLast(Span hay, Span needle);
    // buf 从 at 起是否正是 want；越界时为 false（Rust 的 buf.get(at..at + n) == Some(want)）
    bool Matches(Span buf, size_t at, Span want);
    inline Span Of(std::string_view s) { return Span(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

    // UTF-8 文字的 UTF-16LE 字节
    std::vector<uint8_t> Wide(std::string_view utf8);
    // UTF-16 码元成 UTF-8：strict 时不成对的代理返回空，否则换成 U+FFFD
    std::optional<std::string> FromUtf16(const std::vector<uint16_t>& units, bool strict);
    // 字节两两一个小端码元，落单的最后一个字节不要
    std::vector<uint16_t> Units(Span bytes);
    // 从 at 起 n 个码元（2n 字节）的码位；越界或有不成对的代理时为空（Rust 的 String::from_utf16）
    std::optional<std::u32string> CodePointsAt(Span buf, size_t at, size_t n);
    std::optional<std::u32string> CodePoints(const std::vector<uint16_t>& units);
    std::string Utf8(std::u32string_view cps);
    // Rust 的 char::is_control（Cc 类）与 char::is_whitespace
    inline bool IsControl(char32_t c) { return c < 0x20 || (c >= 0x7f && c <= 0x9f); }
    bool IsWhiteSpace(char32_t c);
    // 两端去掉 Unicode 空白之后是否为空（Rust 的 str::trim().is_empty()）
    bool IsBlank(std::u32string_view cps);
    // 合法的 UTF-8（Rust 的 str::from_utf8）
    bool IsUtf8(std::string_view s);
    // 码位 cp 的 UTF-8 加到 out 后面；不是 Unicode 标量值时返回 false
    bool PutUtf8(std::string& out, uint32_t cp);
    // 两端去掉 ASCII 空白
    std::string_view Trim(std::string_view s);
}
