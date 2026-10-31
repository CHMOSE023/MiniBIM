// 移植自 BIMCraft crates/rvt/src/fixture.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
//
// 测试用的合成 Revit 文件。Autodesk 的文件从不放进仓库：测试在这里现做它要的容器，流由测试命名，字节是测试说的样子。
// BIMCraft 用 cfb 与 flate2 写；这里自己写复合文档（MS-CFB 第 3、4 版）与 deflate（固定 Huffman 加 LZ77，或存储块）
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Fixture
{
    using Bytes   = std::vector<uint8_t>;
    using Streams = std::vector<std::pair<std::string, Bytes>>;

    Bytes Of(std::string_view s);
    Bytes Repeat(std::string_view s, size_t times);
    Bytes Concat(std::initializer_list<Bytes> parts);
    // deflate 压不小的数据（xorshift），同样的长度每次一样
    Bytes Noise(size_t len);

    // raw deflate 流：store 时全用存储块，否则一个固定 Huffman 块（LZ77，窗口 32 KiB）
    Bytes Deflate(std::span<const uint8_t> data, bool store = false);

    // 标准写法的 gzip 段：10 字节的头、deflate 流、CRC-32 与长度
    Bytes Gzip(std::span<const uint8_t> data);
    // deflate 流照原样存数据的 gzip 段（没有尾巴），data 的每个字节都能在段里找到
    Bytes StoredGzip(std::span<const uint8_t> data);
    // 没有尾巴的 gzip 段，据说 Revit 自己的读取也接受
    Bytes TruncatedGzip(std::span<const uint8_t> data);
    // Global/* 流的样子：八个字节，然后一段 gzip
    Bytes Global(std::span<const uint8_t> data);
    // 分区流：44 字节的头，然后每块一段 gzip（带尾巴），块之间 32 字节的头
    Bytes Partition(const std::vector<Bytes>& chunks);
    // 容器给 Revit 分页存的流：每 PagePayload 字节的数据后面一段不是数据的尾巴（填 0xa5）
    Bytes Paged(std::span<const uint8_t> payload);

    // 装着这些流的复合文档（"Global/Latest" 同时建出存储 Global）；version 为 3（512 字节的扇区）或 4（4096）
    Bytes Container(const Streams& streams, int version = 3);

    // 合成的 BasicFileInfo 里放什么
    struct Basic
    {
        uint32_t    release  = 2024;
        std::string build    = "20230308_1635(x64)";
        std::string guid     = "badabcab-cff7-4b43-8935-d6520041e7ca";
        std::string username;
        std::string path     = "C:\\Users\\someone\\Documents\\house.rvt";
        bool        odd      = false;   // 二进制的头里多一个字节，文字块落在奇数位置（有的版本就是这样）
        bool        legacy   = false;   // 2016 到 2018 版写 `Revit Build: Autodesk Revit 2016 (Build: ...)`，之后的写 `Format:` 与 `Build:`
    };
    // Revit 写的 BasicFileInfo：二进制的头（版本与构建号是带计数的 UTF-16），然后 UTF-16 的 `Key: value` 行，CR LF 分开，
    // 其中一行（IsSingleUserCloudModel）的值是夹在宽文字里的窄字符串
    Bytes BasicFileInfo(const Basic& b);

    // 类表里的一个类
    struct Class
    {
        std::string              name;
        std::vector<std::string> fields;
        const Class*             base = nullptr;   // 内联写在它里面的基类
    };
    // 读取器认得的语法写的、去掉页的 Formats/Latest：一个为 0 的 u16，然后每个类（带计数的名字、基类引用、版本、字段数、
    // 字段，没有 GUID）。标签从 12 起按定义的顺序排，内联的基类也算
    Bytes Schema(const std::vector<Class>& classes);

    // 只有一个条目的 ZIP，像 Revit 的 ProjectInformation（条目名是带用户文件夹的路径）
    Bytes ZipEntry(std::span<const uint8_t> body, bool deflate);

    // width × height 个黑点的 PNG，块与 CRC 都对
    Bytes Png(uint32_t width, uint32_t height);
}
