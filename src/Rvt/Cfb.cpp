// MiniBIM 按 MS-CFB（复合文档文件格式）自己写的读取，随 src/Rvt 以 Apache-2.0 发布，见 src/Rvt/NOTICE
#include "Rvt/Cfb.h"
#include "Rvt/Bytes.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace MiniBIM::Rvt::Cfb
{
    namespace
    {
        constexpr uint32_t EndOfChain = 0xfffffffe;
        constexpr uint32_t MaxRegSect = 0xfffffffa;
        constexpr uint32_t NoStream   = 0xffffffff;
        constexpr size_t   EntrySize  = 128;
        constexpr uint8_t  Signature[8] = { 0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1 };

        class Memory final : public Source
        {
        public:
            explicit Memory(std::vector<uint8_t> b) : m_bytes(std::move(b)) {}
            uint64_t Size() const override { return m_bytes.size(); }
            bool ReadAt(uint64_t off, void* dst, size_t n) override
            {
                if (off > m_bytes.size() || m_bytes.size() - off < n)
                    return false;
                if (n)
                    std::memcpy(dst, m_bytes.data() + off, n);
                return true;
            }

        private:
            std::vector<uint8_t> m_bytes;
        };

        class Disk final : public Source
        {
        public:
            Disk(std::ifstream f, uint64_t size) : m_file(std::move(f)), m_size(size) {}
            uint64_t Size() const override { return m_size; }
            bool ReadAt(uint64_t off, void* dst, size_t n) override
            {
                if (off > m_size || m_size - off < n)
                    return false;
                m_file.clear();
                m_file.seekg(static_cast<std::streamoff>(off));
                m_file.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
                return static_cast<size_t>(m_file.gcount()) == n;
            }

        private:
            std::ifstream m_file;
            uint64_t      m_size;
        };

        std::string Num(uint64_t v) { return std::to_string(v); }
    }

    std::unique_ptr<Source> FromMemory(std::vector<uint8_t> bytes) { return std::make_unique<Memory>(std::move(bytes)); }

    std::unique_ptr<Source> FromFile(const std::string& path)
    {
        const std::filesystem::path p(std::u8string(path.begin(), path.end()));
        std::ifstream f(p, std::ios::binary);
        if (!f)
            return nullptr;
        f.seekg(0, std::ios::end);
        const auto size = f.tellg();
        if (size < 0)
            return nullptr;
        f.seekg(0);
        return std::make_unique<Disk>(std::move(f), static_cast<uint64_t>(size));
    }

    bool File::Sector(uint32_t n, uint8_t* dst, size_t len, size_t at)
    {
        const uint64_t off = ((static_cast<uint64_t>(n) + 1) << m_shift) + at;
        return m_source->ReadAt(off, dst, len);
    }

    bool File::Chain(uint32_t start, const std::vector<uint32_t>& table, size_t limit, std::vector<uint32_t>& out, std::string& error)
    {
        out.clear();
        for (uint32_t cur = start; cur != EndOfChain;)
        {
            if (cur > MaxRegSect || cur >= table.size() || cur >= m_sectors)
            {
                error = "a sector chain points to sector " + Num(cur) + ", past the end of the file";
                return false;
            }
            if (out.size() >= limit)
            {
                error = "a sector chain runs in a loop";
                return false;
            }
            out.push_back(cur);
            cur = table[cur];
        }
        return true;
    }

    bool File::Open(std::unique_ptr<Source> source, std::string& error)
    {
        m_source = std::move(source);
        m_streams.clear();
        const uint64_t size = m_source->Size();
        uint8_t h[512];
        if (size < sizeof h || !m_source->ReadAt(0, h, sizeof h))
        {
            error = "the file is shorter than a Compound File header";
            return false;
        }
        if (std::memcmp(h, Signature, 8) != 0)
        {
            error = "the header has no Compound File signature";
            return false;
        }
        if (Bytes::U16(h + 28) != 0xfffe)
        {
            error = "the header's byte order mark is not FFFE";
            return false;
        }
        const uint16_t major = Bytes::U16(h + 26), shift = Bytes::U16(h + 30);
        if (!((major == 3 && shift == 9) || (major == 4 && shift == 12)) || Bytes::U16(h + 32) != 6)
        {
            error = "version " + Num(major) + " with sectors of 2^" + Num(shift) + " bytes is not one the format allows";
            return false;
        }
        m_v3    = major == 3;
        m_shift = shift;
        const uint32_t sectorSize = 1u << m_shift;
        const uint32_t numFat = Bytes::U32(h + 44), firstDir = Bytes::U32(h + 48), firstMiniFat = Bytes::U32(h + 60), numMiniFat = Bytes::U32(h + 64);
        const uint32_t firstDifat = Bytes::U32(h + 68), numDifat = Bytes::U32(h + 72);
        m_cutoff = Bytes::U32(h + 56);
        if (m_cutoff != 4096)
        {
            error = "the mini stream cutoff is " + Num(m_cutoff) + ", not 4096";
            return false;
        }
        const uint64_t sectors = size > sectorSize ? (size - sectorSize + sectorSize - 1) / sectorSize : 0;
        m_sectors = static_cast<uint32_t>(std::min<uint64_t>(sectors, MaxRegSect));
        if (numFat == 0 || numFat > m_sectors)
        {
            error = "it declares " + Num(numFat) + " FAT sectors in a file of " + Num(m_sectors) + " sectors";
            return false;
        }

        // 分配表的扇区：头里的前 109 个，其余在 DIFAT 扇区的链上
        std::vector<uint32_t> fatSectors;
        for (uint32_t i = 0; i < 109 && fatSectors.size() < numFat; ++i)
            fatSectors.push_back(Bytes::U32(h + 76 + 4 * i));
        std::vector<uint8_t> sector(sectorSize);
        uint32_t difat = firstDifat, steps = 0;
        while (fatSectors.size() < numFat)
        {
            if (difat >= m_sectors || ++steps > std::min(numDifat, m_sectors))
            {
                error = "the DIFAT chain ends before it lists every FAT sector";
                return false;
            }
            if (!Sector(difat, sector.data(), sectorSize))
            {
                error = "a DIFAT sector is cut short";
                return false;
            }
            const uint32_t per = sectorSize / 4 - 1;
            for (uint32_t i = 0; i < per && fatSectors.size() < numFat; ++i)
                fatSectors.push_back(Bytes::U32(sector.data() + 4 * i));
            difat = Bytes::U32(sector.data() + 4 * per);
        }
        m_fat.clear();
        m_fat.reserve(static_cast<size_t>(numFat) * (sectorSize / 4));
        for (const uint32_t f : fatSectors)
        {
            if (f >= m_sectors || !Sector(f, sector.data(), sectorSize))
            {
                error = "FAT sector " + Num(f) + " is past the end of the file";
                return false;
            }
            for (uint32_t i = 0; i < sectorSize / 4; ++i)
                m_fat.push_back(Bytes::U32(sector.data() + 4 * i));
        }

        // 目录
        std::vector<uint32_t> chain;
        if (!Chain(firstDir, m_fat, m_sectors, chain, error))
        {
            error = "the directory: " + error;
            return false;
        }
        std::vector<uint8_t> dir(chain.size() * static_cast<size_t>(sectorSize));
        for (size_t i = 0; i < chain.size(); ++i)
            if (!Sector(chain[i], dir.data() + i * sectorSize, sectorSize))
            {
                error = "a directory sector is cut short";
                return false;
            }
        const size_t entries = dir.size() / EntrySize;
        if (entries == 0 || dir[66] != 5)
        {
            error = "its first directory entry is not the root";
            return false;
        }
        const auto sizeOf = [&](const uint8_t* e)
        {
            const uint64_t s = Bytes::U64(e + 120);
            // 第 3 版的高 32 位按规范的建议不看（有的旧程序没清零）
            return m_v3 ? s & 0xffffffffu : s;
        };

        // 迷你流（根的链）与迷你分配表
        m_miniSize = sizeOf(dir.data());
        m_miniStream.clear();
        if (m_miniSize > 0)
        {
            if (!Chain(Bytes::U32(dir.data() + 116), m_fat, m_sectors, m_miniStream, error))
            {
                error = "the mini stream: " + error;
                return false;
            }
            if (static_cast<uint64_t>(m_miniStream.size()) * sectorSize < m_miniSize)
            {
                error = "the mini stream is shorter than the root entry says";
                return false;
            }
        }
        m_miniFat.clear();
        if (numMiniFat > 0 && firstMiniFat != EndOfChain)
        {
            if (!Chain(firstMiniFat, m_fat, m_sectors, chain, error))
            {
                error = "the mini FAT: " + error;
                return false;
            }
            for (const uint32_t s : chain)
            {
                if (!Sector(s, sector.data(), sectorSize))
                {
                    error = "a mini FAT sector is cut short";
                    return false;
                }
                for (uint32_t i = 0; i < sectorSize / 4; ++i)
                    m_miniFat.push_back(Bytes::U32(sector.data() + 4 * i));
            }
        }

        // 目录树：每个存储的子项是一棵按名称排的树，这里只要全部走到；每项只能走到一次
        std::vector<bool> seen(entries);
        seen[0] = true;
        std::vector<std::pair<uint32_t, std::string>> todo{ { Bytes::U32(dir.data() + 76), std::string() } };
        while (!todo.empty())
        {
            auto [i, prefix] = std::move(todo.back());
            todo.pop_back();
            if (i == NoStream)
                continue;
            if (i >= entries)
            {
                error = "a directory entry points to entry " + Num(i) + ", past the directory";
                return false;
            }
            if (seen[i])
            {
                error = "the directory tree runs in a loop at entry " + Num(i);
                return false;
            }
            seen[i] = true;
            const uint8_t* e = dir.data() + i * EntrySize;
            todo.emplace_back(Bytes::U32(e + 68), prefix);
            todo.emplace_back(Bytes::U32(e + 72), prefix);
            const size_t units = std::min<size_t>(Bytes::U16(e + 64) / 2, 32);
            std::vector<uint16_t> name;
            for (size_t k = 0; k < units; ++k)
                name.push_back(Bytes::U16(e + 2 * k));
            while (!name.empty() && name.back() == 0)
                name.pop_back();
            const std::string n = Bytes::FromUtf16(name, false).value_or("");
            if (e[66] == 1)
                todo.emplace_back(Bytes::U32(e + 76), prefix + n + "/");
            else if (e[66] == 2)
            {
                Stream s;
                s.path  = prefix + n;
                s.size  = sizeOf(e);
                s.start = Bytes::U32(e + 116);
                s.mini  = s.size < m_cutoff;
                m_streams.push_back(std::move(s));
            }
        }
        return true;
    }

    bool File::Read(const Stream& s, std::vector<uint8_t>& out, std::string& error)
    {
        out.clear();
        if (s.size == 0)
            return true;
        const uint32_t sectorSize = 1u << m_shift;
        out.reserve(static_cast<size_t>(std::min<uint64_t>(s.size, m_source->Size())));
        uint8_t buf[4096];
        uint32_t cur = s.start;
        // 链上每个扇区只能走到一次：坏文件的链可以绕回自己，读出的就成了重复的扇区
        std::vector<bool> seen;
        const auto again = [&](uint32_t n)
        {
            if (seen.size() <= n)
                seen.resize(static_cast<size_t>(n) + 1);
            if (seen[n])
            {
                error = "a sector chain runs in a loop";
                return true;
            }
            seen[n] = true;
            return false;
        };
        if (s.mini)
        {
            // 64 字节的迷你扇区，在迷你流里的位置换成文件里的扇区
            while (out.size() < s.size && cur < m_miniFat.size())
            {
                if (again(cur))
                    return false;
                const uint64_t off = static_cast<uint64_t>(cur) * 64;
                const uint64_t k = off >> m_shift;
                if (off >= m_miniSize || k >= m_miniStream.size())
                    break;
                const size_t len = static_cast<size_t>(std::min<uint64_t>(64, s.size - out.size()));
                if (!Sector(m_miniStream[k], buf, len, static_cast<size_t>(off & (sectorSize - 1))))
                    break;
                out.insert(out.end(), buf, buf + len);
                cur = m_miniFat[cur];
            }
            return true;
        }
        while (out.size() < s.size && cur < m_sectors && cur < m_fat.size())
        {
            if (again(cur))
                return false;
            size_t len = static_cast<size_t>(std::min<uint64_t>(sectorSize, s.size - out.size()));
            // 文件最后一个扇区可能不完整
            const uint64_t off = (static_cast<uint64_t>(cur) + 1) << m_shift;
            if (off >= m_source->Size())
                break;
            len = static_cast<size_t>(std::min<uint64_t>(len, m_source->Size() - off));
            if (!m_source->ReadAt(off, buf, len))
            {
                error = "sector " + Num(cur) + " cannot be read";
                return false;
            }
            out.insert(out.end(), buf, buf + len);
            cur = m_fat[cur];
        }
        return true;
    }
}
