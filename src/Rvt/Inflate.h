// MiniBIM 按 RFC 1951（deflate）自己写的解压，代替 BIMCraft 用的 flate2；随 src/Rvt 以 Apache-2.0 发布，见 src/Rvt/NOTICE
// 不信任输入：输出有上限，解不开时说原因，不抛异常；stb 的 zlib 不说读了多少输入，读 Revit 的流要知道一段 gzip 在哪结束，所以自己写
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MiniBIM::Rvt::Deflate
{
    enum class Status
    {
        Ok,
        Bad,       // 不是合法的 deflate 流（reason 说哪里）
        TooMuch,   // 解出的超过 room
    };

    // data[0, size) 起的 raw deflate 流解出来接在 out 后面，这一段最多 room 字节。成功时 consumed 是流占的字节数
    // （到最后一块结束，补齐到整字节）；失败时 out 恢复原样
    Status Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t room, size_t& consumed, std::string& reason);

    // CRC-32（gzip、ZIP、PNG 用的那个，多项式 0xEDB88320）；crc 为接着算的前一段的值
    uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0);
}
