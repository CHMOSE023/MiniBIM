#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// 项目文件里放大块数据（CAD 图纸的 DXF、点云）的写法：zlib（RFC 1950 / 1951）压过，再写成 base64，与 BIMCraft 的文件相同。
// MiniBIM 自己写的（核心库不依赖 zlib）：压缩用 LZ77 与固定的哈夫曼码，解压认全部三种块；不信任输入，解出的有上限，解不开时说原因
namespace MiniBIM::Bim::Zlib
{
    // bytes 压成 zlib 流
    std::string Deflate(std::string_view bytes);
    // zlib 流解出来，最多 most 字节；解不开或超过 most 时返回 false，原因放在 error
    bool Inflate(std::string_view zlib, size_t most, std::string& out, std::string& error);

    std::string Base64(std::string_view bytes);
    // base64 的字节；不是 base64 时为空（空白略过，末尾的 = 可有可无）
    std::optional<std::string> Unbase64(std::string_view text);

    // 项目文件里存的样子：Deflate 再 Base64
    std::string Pack(std::string_view bytes);
    // Pack 的反向，最多 most 字节；what 是报错里说的谁的数据（"a CAD drawing"）
    bool Unpack(std::string_view what, std::string_view text, size_t most, std::string& out, std::string& error);
}
