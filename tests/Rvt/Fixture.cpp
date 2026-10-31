// 移植自 BIMCraft crates/rvt/src/fixture.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// 复合文档与 deflate 的写法是 MiniBIM 按 MS-CFB 与 RFC 1951 自己写的（BIMCraft 用 cfb 与 flate2）
#include "Fixture.h"

#include "Rvt/Gzip.h"
#include "Rvt/Inflate.h"

#include <algorithm>
#include <functional>
#include <map>

namespace Fixture
{
    namespace
    {
        constexpr uint32_t EndOfChain = 0xfffffffe, FreeSect = 0xffffffff, FatSect = 0xfffffffd, DifSect = 0xfffffffc, NoStream = 0xffffffff;

        void Le16(Bytes& out, uint32_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); }
        void Le32(Bytes& out, uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); }
        void Be32(Bytes& out, uint32_t v) { for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); }
        void Put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }

        // 按 deflate 的位序（低位先）写
        struct Bits
        {
            Bytes&   out;
            uint32_t acc = 0;
            int      n   = 0;
            void Put(uint32_t v, int bits)
            {
                acc |= v << n;
                n += bits;
                while (n >= 8)
                {
                    out.push_back(static_cast<uint8_t>(acc));
                    acc >>= 8;
                    n -= 8;
                }
            }
            void Align()
            {
                if (n > 0)
                    out.push_back(static_cast<uint8_t>(acc));
                acc = 0;
                n   = 0;
            }
        };

        // Huffman 码高位先写，所以倒过来
        uint32_t Rev(uint32_t code, int len)
        {
            uint32_t r = 0;
            for (int i = 0; i < len; ++i)
                r |= ((code >> i) & 1) << (len - 1 - i);
            return r;
        }

        void Symbol(Bits& w, uint32_t v)
        {
            if (v < 144)
                w.Put(Rev(0x30 + v, 8), 8);
            else if (v < 256)
                w.Put(Rev(0x190 + v - 144, 9), 9);
            else if (v < 280)
                w.Put(Rev(v - 256, 7), 7);
            else
                w.Put(Rev(0xc0 + v - 280, 8), 8);
        }

        constexpr uint16_t LenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        constexpr uint8_t  LenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        constexpr uint16_t DistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        constexpr uint8_t  DistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

        void Match(Bits& w, uint32_t len, uint32_t dist)
        {
            int i = 28;
            while (LenBase[i] > len)
                --i;
            Symbol(w, 257 + static_cast<uint32_t>(i));
            w.Put(len - LenBase[i], LenExtra[i]);
            int j = 29;
            while (DistBase[j] > dist)
                --j;
            w.Put(Rev(static_cast<uint32_t>(j), 5), 5);
            w.Put(dist - DistBase[j], DistExtra[j]);
        }

        Bytes Header() { return { 0x1f, 0x8b, 0x08, 0, 0, 0, 0, 0, 0, 0x0b }; }

        void Append(Bytes& out, std::span<const uint8_t> more) { out.insert(out.end(), more.begin(), more.end()); }

        std::vector<uint8_t> Wide(std::string_view text)
        {
            // 测试里的文字都是 ASCII
            Bytes out;
            for (const char c : text)
                Le16(out, static_cast<uint8_t>(c));
            return out;
        }
    }

    Bytes Of(std::string_view s) { return Bytes(s.begin(), s.end()); }

    Bytes Repeat(std::string_view s, size_t times)
    {
        Bytes out;
        for (size_t i = 0; i < times; ++i)
            out.insert(out.end(), s.begin(), s.end());
        return out;
    }

    Bytes Concat(std::initializer_list<Bytes> parts)
    {
        Bytes out;
        for (const Bytes& p : parts)
            Append(out, p);
        return out;
    }

    Bytes Noise(size_t len)
    {
        uint64_t x = 0x2545'f491'4f6c'dd1dull;
        Bytes out(len);
        for (uint8_t& b : out)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            b = static_cast<uint8_t>(x >> 24);
        }
        return out;
    }

    Bytes Deflate(std::span<const uint8_t> data, bool store)
    {
        Bytes out;
        Bits w{ out };
        if (store)
        {
            size_t at = 0;
            do
            {
                const size_t n = std::min<size_t>(65'535, data.size() - at);
                w.Put(at + n == data.size() ? 1 : 0, 1);
                w.Put(0, 2);
                w.Align();
                Le16(out, static_cast<uint32_t>(n));
                Le16(out, static_cast<uint32_t>(~n & 0xffff));
                out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + n));
                at += n;
            } while (at < data.size());
            return out;
        }
        w.Put(1, 1);
        w.Put(1, 2);
        std::vector<int64_t> last(1 << 15, -1);
        const auto hash = [&](size_t i) { return ((data[i] << 10) ^ (data[i + 1] << 5) ^ data[i + 2]) & 0x7fff; };
        for (size_t i = 0; i < data.size();)
        {
            size_t len = 0, dist = 0;
            if (i + 3 <= data.size())
            {
                const int h = hash(i);
                const int64_t cand = last[h];
                last[h] = static_cast<int64_t>(i);
                if (cand >= 0 && i - static_cast<size_t>(cand) <= 32'768)
                {
                    const size_t from = static_cast<size_t>(cand);
                    const size_t most = std::min<size_t>(258, data.size() - i);
                    while (len < most && data[from + len] == data[i + len])
                        ++len;
                    dist = i - from;
                }
            }
            if (len >= 3)
            {
                Match(w, static_cast<uint32_t>(len), static_cast<uint32_t>(dist));
                for (size_t k = i + 1; k < i + len && k + 3 <= data.size(); ++k)
                    last[hash(k)] = static_cast<int64_t>(k);
                i += len;
            }
            else
                Symbol(w, data[i++]);
        }
        Symbol(w, 256);
        w.Align();
        return out;
    }

    Bytes Gzip(std::span<const uint8_t> data)
    {
        Bytes out = TruncatedGzip(data);
        Le32(out, MiniBIM::Rvt::Deflate::Crc32(data.data(), data.size()));
        Le32(out, static_cast<uint32_t>(data.size()));
        return out;
    }

    Bytes StoredGzip(std::span<const uint8_t> data)
    {
        Bytes out = Header();
        Append(out, Deflate(data, true));
        return out;
    }

    Bytes TruncatedGzip(std::span<const uint8_t> data)
    {
        Bytes out = Header();
        Append(out, Deflate(data));
        return out;
    }

    Bytes Global(std::span<const uint8_t> data)
    {
        Bytes out{ 1, 0, 0, 0, 0, 0, 0, 0 };
        Append(out, Gzip(data));
        return out;
    }

    Bytes Partition(const std::vector<Bytes>& chunks)
    {
        Bytes out{ 9, 0, 0, 0, 0, 0, 0, 0 };
        out.resize(44, 0x5a);
        for (size_t i = 0; i < chunks.size(); ++i)
        {
            Append(out, Gzip(chunks[i]));
            if (i + 1 < chunks.size())
                out.resize(out.size() + 32, 0x77);
        }
        return out;
    }

    Bytes Paged(std::span<const uint8_t> payload)
    {
        using MiniBIM::Rvt::Gzip::PagePayload;
        using MiniBIM::Rvt::Gzip::PageStored;
        Bytes out;
        for (size_t at = 0; at < payload.size(); at += PagePayload)
        {
            const size_t n = std::min(PagePayload, payload.size() - at);
            Append(out, payload.subspan(at, n));
            if (n == PagePayload)
                out.resize(out.size() + PageStored - PagePayload, 0xa5);
        }
        return out;
    }

    Bytes Container(const Streams& streams, int version)
    {
        const uint32_t shift = version == 4 ? 12 : 9;
        const size_t   ss    = size_t(1) << shift;
        struct Entry
        {
            std::string           name;
            uint8_t               type  = 0;
            uint32_t              right = NoStream, child = NoStream, start = EndOfChain;
            uint64_t              size  = 0;
            std::vector<uint32_t> kids;
            const Bytes*          data  = nullptr;
        };
        std::vector<Entry> e(1);
        e[0].name = "Root Entry";
        e[0].type = 5;
        std::map<std::string, uint32_t> storages{ { "", 0 } };
        for (const auto& [path, data] : streams)
        {
            uint32_t parent = 0;
            std::string prefix;
            size_t from = 0;
            for (size_t slash; (slash = path.find('/', from)) != std::string::npos; from = slash + 1)
            {
                prefix += path.substr(from, slash - from) + "/";
                auto it = storages.find(prefix);
                if (it == storages.end())
                {
                    Entry s;
                    s.name = path.substr(from, slash - from);
                    s.type = 1;
                    e.push_back(std::move(s));
                    const uint32_t index = static_cast<uint32_t>(e.size() - 1);
                    e[parent].kids.push_back(index);
                    it = storages.emplace(prefix, index).first;
                }
                parent = it->second;
            }
            Entry s;
            s.name = path.substr(from);
            s.type = 2;
            s.size = data.size();
            s.data = &data;
            e.push_back(std::move(s));
            e[parent].kids.push_back(static_cast<uint32_t>(e.size() - 1));
        }
        // 各存储的子项连成一串右兄弟（读取只要走得到，不看次序）
        for (Entry& x : e)
            if (!x.kids.empty())
            {
                x.child = x.kids[0];
                for (size_t k = 0; k + 1 < x.kids.size(); ++k)
                    e[x.kids[k]].right = x.kids[k + 1];
            }

        Bytes body;
        std::vector<uint32_t> fat;
        const auto alloc = [&](std::span<const uint8_t> bytes) -> uint32_t
        {
            if (bytes.empty())
                return EndOfChain;
            const size_t n = (bytes.size() + ss - 1) / ss;
            const uint32_t first = static_cast<uint32_t>(fat.size());
            for (size_t k = 0; k < n; ++k)
                fat.push_back(k + 1 < n ? first + static_cast<uint32_t>(k) + 1 : EndOfChain);
            Append(body, bytes);
            body.resize(body.size() + n * ss - bytes.size(), 0);
            return first;
        };
        // 大的流在扇区里，小于 4096 字节的在迷你流里
        Bytes mini;
        std::vector<uint32_t> miniFat;
        for (Entry& x : e)
        {
            if (x.type != 2 || x.size == 0)
                continue;
            if (x.size >= 4096)
            {
                x.start = alloc(*x.data);
                continue;
            }
            const size_t n = (x.data->size() + 63) / 64;
            x.start = static_cast<uint32_t>(miniFat.size());
            for (size_t k = 0; k < n; ++k)
                miniFat.push_back(k + 1 < n ? x.start + static_cast<uint32_t>(k) + 1 : EndOfChain);
            Append(mini, *x.data);
            mini.resize(mini.size() + n * 64 - x.data->size(), 0);
        }
        e[0].start = alloc(mini);
        e[0].size  = mini.size();
        Bytes miniFatBytes;
        for (const uint32_t v : miniFat)
            Le32(miniFatBytes, v);
        while (miniFatBytes.size() % ss)
            Le32(miniFatBytes, FreeSect);
        const uint32_t firstMiniFat = alloc(miniFatBytes);
        const uint32_t numMiniFat   = static_cast<uint32_t>(miniFatBytes.size() / ss);

        Bytes dir;
        for (const Entry& x : e)
        {
            uint8_t d[128] = {};
            for (size_t k = 0; k < x.name.size() && k < 31; ++k)
                d[2 * k] = static_cast<uint8_t>(x.name[k]);
            d[64] = static_cast<uint8_t>((std::min<size_t>(x.name.size(), 31) + 1) * 2);
            d[66] = x.type;
            d[67] = 1;
            Put32(d + 68, NoStream);
            Put32(d + 72, x.right);
            Put32(d + 76, x.child);
            Put32(d + 116, x.start);
            Put32(d + 120, static_cast<uint32_t>(x.size));
            Put32(d + 124, static_cast<uint32_t>(x.size >> 32));
            dir.insert(dir.end(), d, d + 128);
        }
        while (dir.size() % ss)
        {
            uint8_t d[128] = {};
            Put32(d + 68, NoStream);
            Put32(d + 72, NoStream);
            Put32(d + 76, NoStream);
            dir.insert(dir.end(), d, d + 128);
        }
        const uint32_t firstDir = alloc(dir);
        const uint32_t numDir   = static_cast<uint32_t>(dir.size() / ss);

        // 分配表本身与 DIFAT 也占扇区：算到不再变
        const uint32_t per = static_cast<uint32_t>(ss / 4), base = static_cast<uint32_t>(fat.size());
        uint32_t nf = 0, nd = 0;
        for (;;)
        {
            const uint32_t total = base + nf + nd;
            const uint32_t nf2 = (total + per - 1) / per;
            const uint32_t nd2 = nf2 > 109 ? (nf2 - 109 + per - 2) / (per - 1) : 0;
            if (nf2 == nf && nd2 == nd)
                break;
            nf = nf2;
            nd = nd2;
        }
        for (uint32_t k = 0; k < nf; ++k)
            fat.push_back(FatSect);
        for (uint32_t k = 0; k < nd; ++k)
            fat.push_back(DifSect);
        fat.resize(static_cast<size_t>(nf) * per, FreeSect);
        for (const uint32_t v : fat)
            Le32(body, v);
        for (uint32_t k = 0; k < nd; ++k)
        {
            for (uint32_t i = 0; i < per - 1; ++i)
            {
                const uint32_t f = 109 + k * (per - 1) + i;
                Le32(body, f < nf ? base + f : FreeSect);
            }
            Le32(body, k + 1 < nd ? base + nf + k + 1 : EndOfChain);
        }

        Bytes out(512, 0);
        const uint8_t signature[8] = { 0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1 };
        std::copy(signature, signature + 8, out.begin());
        uint8_t* h = out.data();
        h[24] = 0x3e;
        h[26] = static_cast<uint8_t>(version == 4 ? 4 : 3);
        h[28] = 0xfe;
        h[29] = 0xff;
        h[30] = static_cast<uint8_t>(shift);
        h[32] = 6;
        Put32(h + 40, version == 4 ? numDir : 0);
        Put32(h + 44, nf);
        Put32(h + 48, firstDir);
        Put32(h + 56, 4096);
        Put32(h + 60, firstMiniFat);
        Put32(h + 64, numMiniFat);
        Put32(h + 68, nd > 0 ? base + nf : EndOfChain);
        Put32(h + 72, nd);
        for (uint32_t i = 0; i < 109; ++i)
            Put32(h + 76 + 4 * i, i < nf ? base + i : FreeSect);
        out.resize(ss, 0);
        Append(out, body);
        return out;
    }

    Bytes BasicFileInfo(const Basic& b)
    {
        Bytes out{ 0x0e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        for (const std::string& field : { std::to_string(b.release), b.build, b.path })
        {
            Le32(out, static_cast<uint32_t>(field.size()));
            Append(out, Wide(field));
        }
        if (b.odd)
            out.push_back(1);
        std::string buildLine;
        if (b.legacy)
        {
            std::string build = b.build;
            if (build.ends_with("(x64)"))
                build.resize(build.size() - 5);
            buildLine = "Revit Build: Autodesk Revit " + std::to_string(b.release) + " (Build: " + build + ")";
        }
        else
            buildLine = "Format: " + std::to_string(b.release);
        std::vector<Bytes> lines{ Wide("Worksharing: Not enabled"), Wide("Username: " + b.username), Wide("Central Model Path: "), Wide(buildLine) };
        if (!b.legacy)
            lines.push_back(Wide("Build: " + b.build));
        lines.push_back(Wide("Last Save Path: " + b.path));
        lines.push_back(Wide("Open Workset Default: 3"));
        lines.push_back(Wide("Locale when saved: ENU"));
        lines.push_back(Wide("Unique Document GUID: " + b.guid));
        lines.push_back(Wide("Unique Document Increments: 22"));
        // 宽文字里的窄值：一个 NUL、ASCII、一个 NUL
        Bytes cloud = Wide("IsSingleUserCloudModel: ");
        Append(cloud, Of(std::string_view("\0False\0", 7)));
        lines.push_back(cloud);
        lines.push_back(Wide("Author: Autodesk Revit"));
        lines.push_back(Wide("ClientAppName: RevitApplication"));
        for (size_t i = 0; i < lines.size(); ++i)
        {
            Append(out, lines[i]);
            // 每行以 UTF-16 的 CR LF 结尾，流的最后一行以窄的 CR LF
            if (i + 1 == lines.size())
                Append(out, Bytes{ 0x0d, 0x0a });
            else
                Append(out, Bytes{ 0x0d, 0, 0x0a, 0 });
        }
        return out;
    }

    Bytes Schema(const std::vector<Class>& classes)
    {
        Bytes out;
        uint16_t nextTag = 12;
        const std::function<void(const Class&)> one = [&](const Class& c)
        {
            ++nextTag;
            Le16(out, static_cast<uint32_t>(c.name.size()));
            Append(out, Of(c.name));
            if (c.base)
            {
                Le16(out, 0x8000u | nextTag);
                Le16(out, 0);
                one(*c.base);
            }
            else
                Le16(out, 0);
            Le32(out, 1);
            Le32(out, static_cast<uint32_t>(c.fields.size()));
            for (const std::string& field : c.fields)
            {
                Le32(out, static_cast<uint32_t>(field.size()));
                Append(out, Of(field));
                Le32(out, 5);
            }
            Le32(out, 0);
        };
        for (const Class& c : classes)
        {
            Le16(out, 0);
            one(c);
        }
        Le16(out, 0);
        return out;
    }

    Bytes ZipEntry(std::span<const uint8_t> body, bool deflate)
    {
        const std::string_view name = "C:\\Users\\someone\\AppData\\Local\\Temp\\Revit0000.project.xml";
        const Bytes data = deflate ? Deflate(body) : Bytes(body.begin(), body.end());
        Bytes out = Of("PK\x03\x04");
        Le16(out, 20);
        Le16(out, 2);
        Le16(out, deflate ? 8 : 0);
        out.resize(out.size() + 8, 0);
        Le32(out, static_cast<uint32_t>(data.size()));
        Le32(out, static_cast<uint32_t>(body.size()));
        Le16(out, static_cast<uint32_t>(name.size()));
        Le16(out, 0);
        Append(out, Of(name));
        Append(out, data);
        Append(out, Of("PK\x05\x06"));
        return out;
    }

    Bytes Png(uint32_t width, uint32_t height)
    {
        using MiniBIM::Rvt::Deflate::Crc32;
        const auto chunk = [](Bytes& out, const char* kind, const Bytes& data)
        {
            Be32(out, static_cast<uint32_t>(data.size()));
            const size_t at = out.size();
            out.insert(out.end(), kind, kind + 4);
            Append(out, data);
            Be32(out, Crc32(out.data() + at, 4 + data.size()));
        };
        Bytes ihdr;
        Be32(ihdr, width);
        Be32(ihdr, height);
        Append(ihdr, Bytes{ 8, 2, 0, 0, 0 });
        Bytes raw;
        for (uint32_t y = 0; y < std::min<uint32_t>(height, 64); ++y)
        {
            raw.push_back(0);
            raw.resize(raw.size() + std::min<uint32_t>(width, 64) * 3, 0);
        }
        // zlib：两个字节的头、deflate 流、Adler-32（高位在前）
        Bytes idat{ 0x78, 0x01 };
        Append(idat, Deflate(raw));
        uint32_t a = 1, s = 0;
        for (const uint8_t v : raw)
        {
            a = (a + v) % 65'521;
            s = (s + a) % 65'521;
        }
        Be32(idat, s << 16 | a);
        Bytes out{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
        chunk(out, "IHDR", ihdr);
        chunk(out, "IDAT", idat);
        chunk(out, "IEND", {});
        return out;
    }
}
