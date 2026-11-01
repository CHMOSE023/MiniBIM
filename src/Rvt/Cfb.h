// MiniBIM 按 MS-CFB（复合文档文件格式，微软公开的规范）自己写的读取，代替 BIMCraft 用的 cfb；随 src/Rvt 以 Apache-2.0 发布，见 src/Rvt/NOTICE
// 只读。文件不可信：链有环、扇区号越界、目录树有环都报错或停下，不崩、不无限循环；流的内容按需读
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace MiniBIM::Rvt::Cfb
{
    // 文件的字节：磁盘上的或内存里的
    class Source
    {
    public:
        virtual ~Source() = default;
        virtual uint64_t Size() const = 0;
        // 读 off 起的 n 个字节；读不全时返回 false
        virtual bool ReadAt(uint64_t off, void* dst, size_t n) = 0;
    };

    std::unique_ptr<Source> FromMemory(std::vector<uint8_t> bytes);
    // path 为 UTF-8；打不开时返回空
    std::unique_ptr<Source> FromFile(const std::string& path);

    struct Stream
    {
        std::string path;    // 不带开头的斜杠，各级以 / 分开（Global/Latest）
        uint64_t    size  = 0;
        uint32_t    start = 0;
        bool        mini  = false;   // 存在迷你流里（小于 4096 字节的）
    };

    class File
    {
    public:
        // 读头、分配表、目录；出错时 error 说哪里坏了（英文，接在 "the Compound File container is damaged: " 后面）
        bool Open(std::unique_ptr<Source> source, std::string& error);
        // 所有的流（不含存储），按目录树的顺序
        const std::vector<Stream>& Streams() const { return m_streams; }
        // 流的内容，最多 size 字节；链提前断了时只给读到的（由调用方比较长度）
        bool Read(const Stream& s, std::vector<uint8_t>& out, std::string& error);

    private:
        bool Sector(uint32_t n, uint8_t* dst, size_t len, size_t at = 0);
        bool Chain(uint32_t start, const std::vector<uint32_t>& table, size_t limit, std::vector<uint32_t>& out, std::string& error);

        std::unique_ptr<Source> m_source;
        uint32_t                m_shift = 9;
        uint32_t                m_sectors = 0;   // 文件里有几个完整或部分的扇区（头以后）
        uint32_t                m_cutoff = 4096;
        bool                    m_v3 = true;
        std::vector<uint32_t>   m_fat, m_miniFat, m_miniStream;
        uint64_t                m_miniSize = 0;
        std::vector<Stream>     m_streams;
    };
}
