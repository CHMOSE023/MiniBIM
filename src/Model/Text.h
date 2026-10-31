#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 模型核心用到的小工具：去空白、ASCII 小写、数的读写。数的读写不随系统区域设置变（小数点总是 "."）
namespace MiniBIM::Bim::Text
{
    std::string_view Trim(std::string_view s);
    std::string      AsciiLower(std::string_view s);
    std::string      AsciiUpper(std::string_view s);
    bool             StartsWith(std::string_view s, std::string_view prefix);
    bool             EndsWith(std::string_view s, std::string_view suffix);
    std::string      ReplaceAll(std::string s, std::string_view from, std::string_view to);
    std::vector<std::string_view> SplitWhitespace(std::string_view s);
    // 字符数（UTF-8 码点数），名称长度的上限按它算
    size_t           CharCount(std::string_view utf8);
    // 前 n 个字符（UTF-8 码点）
    std::string      TakeChars(std::string_view utf8, size_t n);
    // 字节串按 UTF-8 解成码点：非法的序列（截断、过长编码、代理区、超出 U+10FFFF）各成一个 U+FFFD（照 Rust 的 from_utf8_lossy）
    std::u32string   Utf32(std::string_view bytes);
    // 码点 c 的 UTF-8 接在 out 后面；不是码点的（代理区、超出 U+10FFFF）写 U+FFFD
    void             AppendUtf8(std::string& out, char32_t c);
    // 字节串当作 UTF-8：非法的序列换成 U+FFFD
    std::string      Lossy(std::string_view bytes);

    // 读一个数：前后可有空白，可带正负号、小数点、指数，也认 inf、nan（照 Rust 的 f64::from_str）；整个字符串都要是这个数
    std::optional<double> ParseNumber(std::string_view s);
    // 能原样读回的最短写法，不用科学计数法：3000、300.5、0.1（照 Rust 的 {} 格式）
    std::string           Shortest(double x);
}
