// Revit 文件读取器的底层（src/Rvt）：复合文档、gzip 与页、BasicFileInfo、Atom 条目、预览图，以及许可的隔离。
// 移植自 BIMCraft crates/rvt/src/tests/mod.rs 与 metadata.rs 的测试（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE。
// 要样例文件（BIMCRAFT_RVT_SAMPLES）的测试没有移植；自己写的 inflate 与复合文档读取另有测试。记录层（R4）的测试在
// RecordTests.cpp 与 DecoderTests.cpp
#include "Check.h"
#include "Fixture.h"
#include "DeflateVectors.inc"

#include "Rvt/Atom.h"
#include "Rvt/Bytes.h"
#include "Rvt/Container.h"
#include "Rvt/Gzip.h"
#include "Rvt/Inflate.h"
#include "Rvt/Info.h"
#include "Rvt/Preview.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define MINIBIM_GETPID _getpid
#else
#include <unistd.h>
#define MINIBIM_GETPID getpid
#endif

using MiniBIM::Rvt::Atom;
using MiniBIM::Rvt::BasicFileInfo;
using MiniBIM::Rvt::DocumentAtom;
using MiniBIM::Rvt::Error;
using MiniBIM::Rvt::ErrorKind;
using MiniBIM::Rvt::Limits;
using MiniBIM::Rvt::MaxAtom;
using MiniBIM::Rvt::Preview;
using MiniBIM::Rvt::ProjectInformation;
using MiniBIM::Rvt::ReadBasicFileInfo;
using MiniBIM::Rvt::ReadPreview;
using MiniBIM::Rvt::Revit;
namespace Deflate = MiniBIM::Rvt::Deflate;
namespace Gzip    = MiniBIM::Rvt::Gzip;
namespace RB      = MiniBIM::Rvt::Bytes;
using Fixture::Bytes;
using Fixture::Streams;
using RvtTest::Check;
using RvtTest::Contains;
using RvtTest::Xorshift;

namespace
{
    // BasicFileInfo 的替身：容器只要这个流在
    const Bytes kBasic{ 'b', 0, 'f', 0, 'i', 0 };

    bool Open(Streams streams, const Limits& limits, Revit& r, Error& e)
    {
        streams.insert(streams.begin(), { "BasicFileInfo", kBasic });
        return Revit::FromBytes(Fixture::Container(streams), limits, r, e);
    }

    // 打开的文件，出错时打印原因
    Revit Must(Streams streams, const Limits& limits = {})
    {
        Revit r;
        Error e;
        if (!Open(std::move(streams), limits, r, e))
            Check(false, "打开合成的文件：" + e.message);
        return r;
    }

    Bytes Read(Revit& r, std::string_view name)
    {
        Bytes out;
        Error e;
        r.ReadStream(name, out, e);
        return out;
    }

    std::string ReadText(const std::filesystem::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        std::stringstream s;
        s << f.rdbuf();
        return s.str();
    }

    std::filesystem::path Root() { return std::filesystem::path(std::u8string(u8"" MINIBIM_SOURCE_DIR)); }

    Bytes Hex(const char* hex)
    {
        Bytes out;
        for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
            out.push_back(static_cast<uint8_t>(std::stoi(std::string(hex + i, 2), nullptr, 16)));
        return out;
    }

    Bytes Text()
    {
        std::string s;
        for (int i = 0; i < 300; ++i)
            s += "line " + std::to_string(i) + ": the quick brown fox jumps over the lazy dog " + std::to_string(i * i) + "\n";
        return Fixture::Of(s);
    }

    // ---------- 自己写的 inflate ----------

    void TestInflate()
    {
        using Deflate::Status;
        const auto inflate = [](const Bytes& d, Bytes& out, size_t room, size_t& used, std::string& why) { return Deflate::Inflate(d.data(), d.size(), out, room, used, why); };
        const Bytes text = Text();
        for (const auto& [name, hex, want] : { std::tuple{ "动态 Huffman 块", kDynamic, text }, std::tuple{ "中间一个空的存储块", kFlushed, text },
                                                std::tuple{ "没有匹配的动态块", kHuffmanOnly, Bytes(text.begin(), text.begin() + 2000) } })
        {
            const Bytes d = Hex(hex);
            Bytes out;
            size_t used = 0;
            std::string why;
            Check(inflate(d, out, 1 << 20, used, why) == Status::Ok && out == want && used == d.size(), std::string("zlib 压的流解得开：") + name + " " + why);
        }
        const auto crc = [](std::string_view s) { return Deflate::Crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
        Check(crc("123456789") == 0xcbf43926u, "CRC-32 的校验值");
        Check(Deflate::Crc32(reinterpret_cast<const uint8_t*>("6789"), 4, crc("12345")) == crc("123456789"), "CRC-32 能接着算");

        // 自己的压缩器（固定 Huffman、存储块）解回原样；consumed 停在流的末尾，后面的字节不算
        for (const Bytes& data : { Bytes{}, Fixture::Of("a"), text, Fixture::Noise(70'000), Bytes(100'000, 0) })
            for (const bool store : { false, true })
            {
                Bytes d = Fixture::Deflate(data, store);
                const size_t len = d.size();
                d.insert(d.end(), { 1, 2, 3 });
                Bytes out{ 42 };
                size_t used = 0;
                std::string why;
                const bool ok = inflate(d, out, data.size(), used, why) == Status::Ok;
                Check(ok && used == len && out.size() == data.size() + 1 && out[0] == 42 && std::equal(data.begin(), data.end(), out.begin() + 1),
                      "压缩再解开 " + std::to_string(data.size()) + " 字节" + (store ? "（存储块）" : "") + " " + why);
            }

        // 上限正好够与差一个字节；失败时 out 恢复原样
        const Bytes d = Hex(kDynamic);
        Bytes out{ 7, 7 };
        size_t used = 0;
        std::string why;
        Check(inflate(d, out, text.size(), used, why) == Status::Ok && out.size() == text.size() + 2, "上限正好等于解出的大小");
        out = { 7, 7 };
        Check(inflate(d, out, text.size() - 1, used, why) == Status::TooMuch && out == Bytes{ 7, 7 }, "超过上限一个字节：TooMuch，out 不变");
        for (size_t cut : { size_t(0), size_t(1), d.size() / 2, d.size() - 1 })
        {
            out = { 7 };
            const Bytes part(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(cut));
            Check(inflate(part, out, 1 << 20, used, why) == Status::Bad && out == Bytes{ 7 }, "截短到 " + std::to_string(cut) + " 字节的流：" + why);
        }
        Check(inflate(Bytes{ 0x07 }, out, 100, used, why) == Status::Bad, "保留的块类型 3：" + why);
        Check(inflate(Bytes{ 0x01, 5, 0, 0, 0, 'a', 'b', 'c', 'd', 'e' }, out, 100, used, why) == Status::Bad, "存储块的 NLEN 不是 LEN 取反：" + why);
        // 距离超出已解出的部分：固定块，字面 'a'，然后长度 3、距离 2（距离码 1）
        {
            Bytes d2;
            struct
            {
                Bytes& out;
                uint32_t acc = 0;
                int n = 0;
                void Put(uint32_t v, int bits)
                {
                    acc |= v << n;
                    for (n += bits; n >= 8; n -= 8, acc >>= 8)
                        out.push_back(static_cast<uint8_t>(acc));
                }
            } w{ d2 };
            w.Put(1, 1);
            w.Put(1, 2);
            w.Put(0x89, 8);    // 'a'：码 0x30 + 0x61 = 10010001，高位先写，倒过来是 0x89
            w.Put(0x40, 7);    // 257（长度 3）：码 0000001，倒过来是 1000000
            w.Put(0x10, 5);    // 距离码 1（距离 2）：00001，倒过来是 10000
            w.Put(0, 7);       // 256
            w.Put(0, 7);       // 补齐最后一个字节
            Bytes o;
            Check(inflate(d2, o, 100, used, why) == Status::Bad, "距离超出已解出的部分：" + why);
        }
        // 随机字节：不崩，不超出上限
        Xorshift next{ 0x1357'9bdf'2468'ace0ull };
        bool bounded = true;
        for (int round = 0; round < 3000; ++round)
        {
            Bytes junk(1 + next() % 200);
            for (uint8_t& b : junk)
                b = static_cast<uint8_t>(next());
            Bytes o;
            if (inflate(junk, o, 5000, used, why) == Status::Ok)
                bounded = bounded && o.size() <= 5000 && used <= junk.size();
            else
                bounded = bounded && o.empty();
        }
        Check(bounded, "随机字节解不开或在上限内，不崩");
    }

    // ---------- 自己写的复合文档读取 ----------

    void TestCfb()
    {
        const Bytes small = Fixture::Noise(100), big = Fixture::Noise(10'000), mid = Fixture::Noise(4096);
        for (const int version : { 3, 4 })
        {
            const Bytes file = Fixture::Container({ { "BasicFileInfo", kBasic }, { "A/B/c", small }, { "A/d", big }, { "Edge", mid }, { "Empty", {} } }, version);
            Revit r;
            Error e;
            const bool ok = Revit::FromBytes(file, {}, r, e);
            Check(ok, "第 " + std::to_string(version) + " 版的复合文档打得开 " + e.message);
            if (!ok)
                continue;
            Check(r.StreamNames() == std::vector<std::string>{ "A/B/c", "A/d", "BasicFileInfo", "Edge", "Empty" }, "第 " + std::to_string(version) + " 版：嵌套存储里的流按路径列出");
            Check(Read(r, "A/B/c") == small && Read(r, "A/d") == big && Read(r, "Edge") == mid, "第 " + std::to_string(version) + " 版：迷你流、正好 4096 字节与大流读回原样");
            Bytes empty{ 1 };
            Check(r.ReadStream("Empty", empty, e) && empty.empty(), "空的流读出空");
        }

        // 分配表超过头里的 109 个扇区：要走 DIFAT 链
        const Bytes huge = Fixture::Noise(8u << 20);
        {
            const Bytes file = Fixture::Container({ { "BasicFileInfo", kBasic }, { "Huge", huge } });
            Revit r;
            Error e;
            Check(RB::U32(file.data() + 72) > 0 && Revit::FromBytes(file, {}, r, e) && Read(r, "Huge") == huge, "要 DIFAT 扇区的文件读回原样 " + e.message);
        }

        // 目录树绕回自己、扇区链绕回自己：报错，不死循环
        const auto entry = [](const Bytes& file, std::string_view name)
        {
            const Bytes wide = RB::Wide(name);
            for (size_t at = 0; at + 128 <= file.size(); at += 128)
                if (std::equal(wide.begin(), wide.end(), file.begin() + static_cast<std::ptrdiff_t>(at)) && file[at + wide.size()] == 0)
                    return at;
            return std::string::npos;
        };
        {
            Bytes file = Fixture::Container({ { "BasicFileInfo", kBasic }, { "S1", Bytes(10, 1) }, { "S2", Bytes(10, 2) } });
            const size_t dir = (static_cast<size_t>(RB::U32(file.data() + 48)) + 1) * 512;
            const size_t at = entry(file, "S1");
            const uint32_t index = static_cast<uint32_t>((at - dir) / 128);
            std::memcpy(file.data() + at + 72, &index, 4);
            Revit r;
            Error e;
            Check(at != std::string::npos && !Revit::FromBytes(file, {}, r, e) && e.kind == ErrorKind::Damaged && Contains(e.message, "loop"), "目录树有环：" + e.message);
        }
        for (const size_t len : { size_t(5000), size_t(300) })
        {
            Bytes file = Fixture::Container({ { "BasicFileInfo", kBasic }, { "Big", Bytes(len, 1) } });
            const size_t at = entry(file, "Big");
            const uint32_t start = RB::U32(file.data() + at + 116);
            if (len >= 4096)
            {
                const size_t fat = (static_cast<size_t>(RB::U32(file.data() + 76)) + 1) * 512;
                std::memcpy(file.data() + fat + 4 * start, &start, 4);
            }
            else
            {
                const size_t miniFat = (static_cast<size_t>(RB::U32(file.data() + 60)) + 1) * 512;
                std::memcpy(file.data() + miniFat + 4 * start, &start, 4);
            }
            Revit r;
            Error e;
            Bytes out;
            Check(Revit::FromBytes(file, {}, r, e) && !r.ReadStream("Big", out, e) && Contains(e.message, "loop"), std::string("扇区链有环（") + (len >= 4096 ? "分配表" : "迷你分配表") + "）：" + e.message);
        }
    }

    // ---------- 容器 ----------

    void TestContainer()
    {
        {
            Revit r = Must({ { "Global/Latest", Fixture::Of("abc") }, { "Partitions/21", Fixture::Of("defg") } });
            Check(r.StreamNames() == std::vector<std::string>{ "BasicFileInfo", "Global/Latest", "Partitions/21" }, "按路径列出流");
            Check(r.StreamLen("Global/Latest") == 3u && !r.StreamLen("Global/Nope"), "流的长度");
            Check(Read(r, "Partitions/21") == Fixture::Of("defg"), "读回流");
            Bytes out;
            Error e;
            Check(!r.ReadStream("Nope", out, e) && Contains(e.message, "no stream named Nope"), "没有的流：" + e.message);
        }
        for (const auto& [bytes, says] : { std::pair{ Fixture::Of("PK\x03\x04rest of a zip"), "50 4B 03 04" }, std::pair{ Fixture::Of("short"), "73 68 6F 72 74" },
                                            std::pair{ Bytes{}, "is empty" }, std::pair{ Bytes{ 0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1 }, "Compound File" } })
        {
            Revit r;
            Error e;
            Check(!Revit::FromBytes(bytes, {}, r, e) && Contains(e.message, says), "不是复合文档：" + e.message);
        }
        {
            Revit r;
            Error e;
            Check(!Revit::FromBytes(Fixture::Of("PK\x03\x04"), {}, r, e) && Contains(e.message, "D0 CF 11 E0 A1 B1 1A E1"), "报错说出复合文档的签名");
            Check(!Revit::FromBytes(Fixture::Container({ { "WordDocument", Fixture::Of("hello") }, { "1Table", Fixture::Of("x") } }), {}, r, e) && Contains(e.message, "BasicFileInfo"),
                  "不是 Revit 的复合文档：" + e.message);
        }
        {
            const Bytes bytes = Fixture::Container({ { "BasicFileInfo", kBasic } });
            Limits small;
            small.maxFile = bytes.size() - 1;
            Revit r;
            Error e;
            Check(!Revit::FromBytes(bytes, small, r, e) && e.kind == ErrorKind::TooBig, "内存里的文件超过大小上限");
            const std::filesystem::path path = std::filesystem::temp_directory_path() / ("minibim-rvt-" + std::to_string(MINIBIM_GETPID()) + ".rvt");
            {
                std::ofstream f(path, std::ios::binary);
                f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            const auto u8 = path.u8string();
            const std::string utf8(u8.begin(), u8.end());
            Check(Revit::Open(utf8, {}, r, e), "从磁盘打开 " + e.message);
            Check(!Revit::Open(utf8, small, r, e) && Contains(e.message, "limit"), "磁盘上的文件超过大小上限：" + e.message);
            std::error_code ec;
            std::filesystem::remove(path, ec);
            Check(!Revit::Open("/no/such/file.rvt", {}, r, e) && Contains(e.message, "/no/such/file.rvt"), "找不到的文件：" + e.message);
        }
        {
            // 目录项说流有几 GB：不读就拒绝。名字是 UTF-16，项 128 字节，流的大小在 +120
            Bytes bytes = Fixture::Container({ { "BasicFileInfo", kBasic }, { "Big", Bytes(100, 7) } });
            const Bytes name = RB::Wide("Big");
            size_t at = 0;
            while (at + 128 < bytes.size() && !std::equal(name.begin(), name.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at)))
                at += 128;
            const uint64_t giant = 6ull << 30;
            std::memcpy(bytes.data() + at + 120, &giant, 8);
            Revit r;
            Error e;
            Gzip::Inflated got;
            Bytes out;
            Check(Revit::FromBytes(bytes, {}, r, e) && !r.ReadStream("Big", out, e) && e.kind == ErrorKind::TooBig, "目录项说流有几 GB：" + e.message);
            Check(!r.Inflate("Big", got, e) && !r.InflateAll("Big", got, e), "这样的流也不解压");
        }
        {
            const Streams big{ { "Big", Bytes(5000, 1) } };
            Limits l;
            l.maxStream = 4999;
            Revit r = Must(big, l);
            Bytes out;
            Error e;
            Check(!r.ReadStream("Big", out, e) && e.kind == ErrorKind::TooBig, "流超过上限");
            l.maxStream = 5000;
            r = Must(big, l);
            Check(Read(r, "Big").size() == 5000, "流正好等于上限");
            Streams many;
            for (int i = 0; i < 40; ++i)
                many.push_back({ "S" + std::to_string(i), Bytes(4, 0) });
            l = {};
            l.maxStreams = 10;
            Check(!Open(many, l, r, e) && Contains(e.message, "41 streams"), "流太多：" + e.message);
            many.resize(9);
            Check(Open(many, l, r, e), "流的个数正好等于上限");
            many.push_back({ "S9", Bytes(4, 0) });
            Check(!Open(many, l, r, e) && e.kind == ErrorKind::TooBig, "多一个就拒绝");
        }
    }

    // ---------- gzip ----------

    bool Inflated(Revit& r, std::string_view name, Gzip::Inflated& got, Error& e, bool all = false) { return all ? r.InflateAll(name, got, e) : r.Inflate(name, got, e); }

    // 样例文件显示的页的样子，与 StripPages 分开写，好问别的数会怎样：偏移 o 处的字节在 o % page < data 时是数据
    Bytes PagesRemoved(const Bytes& stored, size_t page, size_t data)
    {
        Bytes out;
        for (size_t o = 0; o < stored.size(); ++o)
            if (o % page < data)
                out.push_back(stored[o]);
        return out;
    }

    void TestGzip()
    {
        Error e;
        Gzip::Inflated got;
        {
            const Bytes data = Fixture::Repeat("Revit writes gzip and leaves the trailer out, or not", 20);
            Bytes wrong = Fixture::Global(data);
            wrong[wrong.size() - 8] ^= 0xff;   // CRC 不对
            Revit r = Must({ { "Global/Latest", Fixture::Global(data) }, { "Formats/Latest", Fixture::TruncatedGzip(data) }, { "Global/ElemTable", wrong } });
            Check(r.Inflate("Global/Latest", got, e) && got.bytes == data && got.trailersOk == 1 && got.members == 1, "带尾巴的 gzip 段");
            Check(r.Inflate("Formats/Latest", got, e) && got.bytes == data && got.trailersOk == 0, "没有尾巴的 gzip 段");
            Check(r.Inflate("Global/ElemTable", got, e) && got.bytes == data && got.trailersOk == 0, "CRC 不对：数据照给，报出不符");
        }
        {
            const Bytes zeros(8u << 20, 0);
            const Bytes bomb = Fixture::Global(zeros);
            Check(bomb.size() * 100 < zeros.size(), "8 MiB 的零压成 " + std::to_string(bomb.size()) + " 字节");
            Limits l;
            l.maxInflated = 1 << 20;
            Revit r = Must({ { "Global/Latest", bomb }, { "Partitions/21", Fixture::Partition({ zeros }) } }, l);
            for (const bool all : { false, true })
                Check(!Inflated(r, all ? "Partitions/21" : "Global/Latest", got, e, all) && e.kind == ErrorKind::TooMuch && Contains(e.message, "1048576"), "压缩炸弹停在上限：" + e.message);
            // 上限管整个流：每段都不到，加起来超过
            const Bytes half(700'000, 0);
            r = Must({ { "Partitions/22", Fixture::Partition({ half, half }) } }, l);
            Check(!r.InflateAll("Partitions/22", got, e) && e.kind == ErrorKind::TooMuch, "各段加起来超过上限");
        }
        {
            Bytes g = Fixture::Global(Fixture::Of("hello hello hello hello hello"));
            g.resize(g.size() - 14);
            std::fill(g.begin() + 12, g.end(), 0xff);
            Revit r = Must({ { "Global/Latest", Bytes(300, 5) }, { "Global/History", g } });
            Check(!r.Inflate("Global/Latest", got, e) && Contains(e.message, "Global/Latest") && Contains(e.message, "gzip"), "没有 gzip 的流：" + e.message);
            Check(!r.Inflate("Global/History", got, e) && Contains(e.message, "Global/History"), "坏了的段：" + e.message);
        }
        {
            using Gzip::PagePayload;
            using Gzip::PageStored;
            Check(PageStored == 65'249 && PagePayload == 64'896, "页的大小");
            const Bytes payload = Fixture::Noise(PagePayload * 2 + 1000);
            Bytes stored = Fixture::Paged(payload);
            Check(stored.size() == payload.size() + 2 * (PageStored - PagePayload), "分页存的流的长度");
            Gzip::StripPages(stored);
            Check(stored == payload, "去掉页正好去掉每一整页的尾巴");
            Bytes shortPage = Fixture::Noise(PageStored - 1);
            const Bytes keep = shortPage;
            Gzip::StripPages(shortPage);
            Check(shortPage == keep, "不足一整页的最后一页整个保留");
            Bytes none;
            Gzip::StripPages(none);
            Check(none.empty(), "空的流");
        }
        {
            // 数写在这里、不从库里读，免得常数错了连测试一起错：64,895 是第一页最后一个数据字节，64,896 到 65,248 是它的尾巴
            Bytes stored(3 * 65'249 + 100);
            for (size_t o = 0; o < stored.size(); ++o)
                stored[o] = o % 65'249 < 64'896 ? 1 : 0xee;
            Bytes data = stored;
            Gzip::StripPages(data);
            Check(data.size() == 3 * 64'896 + 100 && std::all_of(data.begin(), data.end(), [](uint8_t b) { return b == 1; }) && data == PagesRemoved(stored, 65'249, 64'896),
                  "页的边界在样例文件放的地方");
            bool differ = true;
            for (const auto& [page, kept] : { std::pair{ 65'250, 64'896 }, std::pair{ 65'248, 64'896 }, std::pair{ 65'249, 64'897 }, std::pair{ 65'249, 64'895 } })
                differ = differ && PagesRemoved(stored, page, kept) != data;
            Check(differ, "差一个字节的页都给出不同的字节");
            bool exact = true;
            for (size_t pages = 0; pages <= 3; ++pages)
                for (const size_t edge : { 0, 1, 2, 64'894, 64'895, 64'896, 64'897, 65'247, 65'248 })
                {
                    const size_t len = pages * 65'249 + edge;
                    Bytes s(len);
                    for (size_t o = 0; o < len; ++o)
                        s[o] = o % 65'249 < 64'896 ? 1 : 0xee;
                    Bytes g = s;
                    Gzip::StripPages(g);
                    const size_t run = static_cast<size_t>(std::count(g.begin(), g.end(), 0xee));
                    exact = exact && g.size() == len - 353 * pages && run == (edge > 64'896 ? edge - 64'896 : 0) && (edge > 64'896 || g == PagesRemoved(s, 65'249, 64'896));
                }
            Check(exact, "每一整页正好去掉 353 个字节，不论长度");
        }
        {
            const Bytes data = Fixture::Noise(150'000);
            const Bytes member = Fixture::Gzip(data);
            Check(member.size() > 2 * Gzip::PagePayload, "一段 gzip 跨过两页");
            Revit r = Must({ { "Formats/Latest", Fixture::Paged(member) }, { "Global/Latest", Fixture::Paged(Fixture::Global(data)) }, { "Other", member } });
            for (const char* name : { "Formats/Latest", "Global/Latest" })
                Check(r.Inflate(name, got, e) && got.bytes == data && got.trailersOk == 1, std::string("分页存的流跨过页的尾巴解开：") + name + " " + e.message);
            Check(r.Inflate("Other", got, e) && got.bytes == data, "不分页的流原样解开");
        }
        {
            const Bytes first = Fixture::Repeat("first chunk ", 50), second = Fixture::Noise(300);
            // 照原样存在段里的、它自己的一段 gzip：是这里的数据，不是流的一段
            const Bytes tricky = Fixture::Concat({ Fixture::Of("before "), Fixture::Gzip(Fixture::Of("hidden")), Fixture::Of(" after") });
            Bytes stream = Fixture::Partition({ first, second });
            stream.resize(stream.size() + 32, 0x77);
            const Bytes stored = Fixture::StoredGzip(tricky);
            stream.insert(stream.end(), stored.begin(), stored.end());
            Revit r = Must({ { "Partitions/21", stream } });
            const Bytes all = Fixture::Concat({ first, second, tricky });
            Check(r.InflateAll("Partitions/21", got, e) && got.bytes == all, "分区的各段按顺序接起来 " + e.message);
            Check(got.ends == std::vector<size_t>{ first.size(), first.size() + second.size(), all.size() }, "每段结束的位置");
            Check(got.members == 3 && got.trailersOk == 2, "段里的假魔数不算一段；照原样存的段没有尾巴");
        }
        {
            const Bytes a = Fixture::Repeat("alpha ", 40), b = Fixture::Repeat("omega ", 40);
            // 截短的段用存储块：固定 Huffman 块几乎任何位序列都解得出，截短后会接着把后面的垃圾解成数据（BIMCraft 用 flate2 的动态块）
            Bytes cut = Fixture::StoredGzip(Fixture::Noise(500));
            cut.resize(60);
            Bytes stream = Fixture::Partition({ a });
            stream.resize(stream.size() + 17, 0x33);
            stream.insert(stream.end(), cut.begin(), cut.end());
            stream.resize(stream.size() + 9, 0x44);
            const Bytes gb = Fixture::Gzip(b);
            stream.insert(stream.end(), gb.begin(), gb.end());
            Revit r = Must({ { "Partitions/21", stream } });
            Check(r.InflateAll("Partitions/21", got, e) && got.bytes == Fixture::Concat({ a, b }) && got.skipped == 1, "段之间的垃圾与截短的段跳过，并计数");
        }
        {
            Limits l;
            l.maxInflated = 1000;
            const auto at = [&](size_t n) { return Must({ { "Global/Latest", Fixture::Global(Bytes(n, 7)) }, { "Partitions/21", Fixture::Partition({ Bytes(n, 7) }) } }, l); };
            Revit r = at(1000), over = at(1001);
            Gzip::Inflated all;
            Check(r.Inflate("Global/Latest", got, e) && r.InflateAll("Partitions/21", all, e) && got.bytes.size() == 1000 && all.bytes.size() == 1000, "上限正好够");
            Check(!over.Inflate("Global/Latest", got, e) && e.kind == ErrorKind::TooMuch, "超过一个字节（第一段）");
            Check(!over.InflateAll("Partitions/21", got, e) && e.kind == ErrorKind::TooMuch, "超过一个字节（全部段）");
            Bytes flagged = Fixture::Global(Fixture::Of("hello hello hello"));
            flagged[8 + 3] = 0x20;   // gzip 的标志字节，置了保留位
            r = Must({ { "Global/Latest", flagged } });
            Check(!r.Inflate("Global/Latest", got, e), "头里有保留标志的不算一段");
        }
    }

    // ---------- 恶意的输入 ----------

    void TestHostile()
    {
        {
            const Bytes data = Fixture::Noise(20'000);
            const Bytes original = Fixture::Container({ { "BasicFileInfo", kBasic },
                                                        { "Global/Latest", Fixture::Global(data) },
                                                        { "Formats/Latest", Fixture::TruncatedGzip(data) },
                                                        { "Partitions/21", Fixture::Partition({ Bytes(data.begin(), data.begin() + 9000), Bytes(data.begin() + 9000, data.end()) }) },
                                                        { "TransmissionData", Bytes(6000, 0x41) } });
            Limits l;
            l.maxFile     = 1 << 20;
            l.maxStreams  = 64;
            l.maxStream   = 1 << 20;
            l.maxInflated = 1 << 20;
            Xorshift next{ 0x9e37'79b9'7f4a'7c15ull };
            int opened = 0;
            bool bounded = true;
            const size_t header = std::min<size_t>(original.size(), 1536);
            for (int round = 0; round < 1500; ++round)
            {
                Bytes bytes = original;
                // 一半的轮次破坏开头的头与表，其余的任何地方
                const size_t span = round % 2 == 0 ? header : bytes.size();
                for (uint64_t k = 0, n = 1 + next() % 4; k < n; ++k)
                {
                    const size_t at = static_cast<size_t>(next() % span);
                    const uint64_t how = next() % 3;
                    bytes[at] = how == 0 ? 0xff : how == 1 ? 0 : static_cast<uint8_t>(next());
                }
                if (round % 17 == 0)
                    bytes.resize(static_cast<size_t>(next() % bytes.size()));
                Revit r;
                Error e;
                if (!Revit::FromBytes(std::move(bytes), l, r, e))
                    continue;
                ++opened;
                for (const std::string& name : r.StreamNames())
                {
                    Bytes stored;
                    if (r.ReadStream(name, stored, e))
                        bounded = bounded && stored.size() <= l.maxStream;
                    Gzip::Inflated got;
                    for (const bool all : { false, true })
                        if (Inflated(r, name, got, e, all))
                            bounded = bounded && got.bytes.size() <= l.maxInflated && (got.ends.empty() ? 0 : got.ends.back()) == got.bytes.size();
                }
            }
            Check(bounded, "随机破坏：读出的都在上限内");
            Check(opened > 100, "随机破坏要走到读取器里，不只是门口：打开了 " + std::to_string(opened) + " 次");
        }
        {
            const Bytes data = Fixture::Noise(30'000);
            const Bytes whole = Fixture::Container({ { "BasicFileInfo", kBasic }, { "Global/Latest", Fixture::Global(data) }, { "Partitions/21", Fixture::Partition({ data }) } });
            Limits l;
            l.maxStream   = 1 << 20;
            l.maxInflated = 1 << 20;
            for (size_t len = 0; len < whole.size(); len += 97)
            {
                Revit r;
                Error e;
                if (!Revit::FromBytes(Bytes(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(len)), l, r, e))
                    continue;
                for (const std::string& name : r.StreamNames())
                {
                    Bytes stored;
                    Gzip::Inflated got;
                    r.ReadStream(name, stored, e);
                    r.Inflate(name, got, e);
                    r.InflateAll(name, got, e);
                }
            }
            Check(true, "在任何地方截短的文件报错或成为小一些的文件，不崩");
        }
    }

    // ---------- BasicFileInfo ----------

    void TestBasicFileInfo()
    {
        for (const bool odd : { false, true })
        {
            Fixture::Basic b;
            b.odd      = odd;
            b.username = "jdoe";
            const Bytes bytes = Fixture::BasicFileInfo(b);
            BasicFileInfo info;
            Error e;
            const std::string tag = odd ? "（奇数位置）" : "（偶数位置）";
            Check(BasicFileInfo::Parse(bytes, info, e), "BasicFileInfo 解得开" + tag + e.message);
            Check(info.release == 2024 && info.build == "20230308_1635(x64)" && info.guid == "badabcab-cff7-4b43-8935-d6520041e7ca", "版本、构建号与 GUID" + tag);
            Check(info.locale == "ENU" && info.username == "jdoe" && info.lastSavePath == "C:\\Users\\someone\\Documents\\house.rvt" && info.worksharing == "Not enabled", "语言、用户、保存路径与工作共享" + tag);
            Check(!info.centralModelPath, "空的值为空" + tag);
            Check(!info.properties.empty() && info.properties.front().first == "Worksharing" && info.properties.back().first == "ClientAppName", "块按文件里的顺序留下每一行" + tag);
            Check(info.Property("IsSingleUserCloudModel") == "False" && info.Property("Author") == "Autodesk Revit", "宽文字里的窄值解开，后面的行完好" + tag);
            Check(info.Property("ClientAppName") == "RevitApplication" && info.Property("Unique Document Increments") == "22", "最后一行以窄的 CR LF 结尾" + tag);
        }
        {
            BasicFileInfo info;
            Error e;
            Check(BasicFileInfo::Parse(Fixture::BasicFileInfo({}), info, e) && !info.username, "没有用户名时为空");
            Fixture::Basic old;
            old.release = 2016;
            old.build   = "20150220_1515(x64)";
            old.legacy  = true;
            Check(BasicFileInfo::Parse(Fixture::BasicFileInfo(old), info, e) && info.release == 2016 && info.build == "20150220_1515(x64)", "老版本从 Revit Build 行读出版本与构建号 " + e.message);
        }
        for (const Bytes& bytes : { Bytes{}, Bytes(100, 0), Fixture::Of("just some text"), Fixture::Noise(5000) })
        {
            BasicFileInfo info;
            Error e;
            Check(!BasicFileInfo::Parse(bytes, info, e) && Contains(e.message, "BasicFileInfo"), "不是 Revit 写的 BasicFileInfo：" + e.message);
        }
        {
            Fixture::Basic bad;
            bad.release = 77;
            BasicFileInfo info;
            Error e;
            Check(!BasicFileInfo::Parse(Fixture::BasicFileInfo(bad), info, e) && Contains(e.message, "release"), "版本不是年份：" + e.message);
        }
        {
            Fixture::Basic b;
            b.username = "jdoe";
            const Bytes bytes = Fixture::BasicFileInfo(b);
            Revit r;
            Error e;
            BasicFileInfo info;
            Check(Revit::FromBytes(Fixture::Container({ { "BasicFileInfo", bytes } }), {}, r, e) && ReadBasicFileInfo(r, info, e) && info.release == 2024, "从容器里读 BasicFileInfo");
            uint64_t seed = 7;
            const auto next = [&]
            {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                return static_cast<size_t>(seed >> 24);
            };
            for (int round = 0; round < 3000; ++round)
            {
                Bytes damaged = bytes;
                for (size_t k = 0, n = 1 + next() % 5; k < n; ++k)
                    damaged[next() % damaged.size()] = static_cast<uint8_t>(next());
                if (next() % 7 == 0)
                    damaged.resize(next() % damaged.size());
                BasicFileInfo ignored;
                BasicFileInfo::Parse(damaged, ignored, e);
            }
            Check(true, "破坏过的 BasicFileInfo 不崩");
        }
    }

    // ---------- Atom 条目 ----------

    const char* kFamilyAtom =
        R"(<?xml version="1.0" encoding="UTF-8"?>
<entry xmlns="http://www.w3.org/2005/Atom" xmlns:A="urn:schemas-autodesk-com:partatom"><title>Chair &amp; Table</title><updated>2023-03-27T12:09:23Z</updated><A:taxonomy><term>adsk:revit</term><label>Autodesk Revit</label></A:taxonomy><category><term>23.40.20.14.17</term><scheme>std:oc1</scheme></category><category><term>Furniture</term></category><link rel="design-2d"><A:design-file><A:title>chair.rfa</A:title><A:product-version>2024</A:product-version></A:design-file></link><A:features><A:feature><A:title>Other</A:title></A:feature></A:features></entry>)";

    void TestAtom()
    {
        Atom atom;
        Error e;
        Check(Atom::Parse(Fixture::Of(kFamilyAtom), atom, e), "族的 Atom 条目解得开 " + e.message);
        Check(atom.title == "Chair & Table", "条目自己的标题，实体解开，不是嵌套的 A:title");
        Check(atom.updated == "2023-03-27T12:09:23Z" && atom.omniclass == "23.40.20.14.17" && atom.productVersion == "2024", "保存时间、OmniClass 与产品版本");
        Check(atom.categories == std::vector<std::string>{ "23.40.20.14.17", "Furniture" }, "类别");
        Check(Atom::Parse(Fixture::Of(R"(<entry><link><A:title>nested</A:title><title>deeper</title></link><title>Real</title></entry>)"), atom, e) && atom.title == "Real", "先出现的嵌套标题不是条目的");
        Check(Atom::Parse(Fixture::Of(R"(<entry xmlns="http://www.w3.org/2005/Atom"><title>T&#233;st</title><category term="22.1.2" scheme="x"/><!-- <title>no</title> --><![CDATA[<title>no</title>]]></entry>)"), atom, e) &&
                  atom.title == "T\xc3\xa9st" && atom.omniclass == "22.1.2",
              "字符引用、属性里的类别、注释与 CDATA");
        for (const std::string_view bad : { std::string_view("<entry><title>cut"), std::string_view("\xff\xfe\x00 not utf8", 13), std::string_view("<entry><a><b>") })
            Check(!Atom::Parse(Fixture::Of(bad), atom, e), "不是 XML：" + e.message);
        std::string deep = "<entry>";
        for (int i = 0; i < 200; ++i)
            deep += "<a>";
        Check(!Atom::Parse(Fixture::Of(deep), atom, e) && Contains(e.message, "deep"), "套得太深：" + e.message);
        Check(!Atom::Parse(Bytes(MaxAtom + 1, ' '), atom, e) && Contains(e.message, "limit"), "太大：" + e.message);

        for (const bool deflate : { true, false })
        {
            const Bytes zip = Fixture::ZipEntry(Fixture::Of(kFamilyAtom), deflate);
            Atom a;
            Check(ProjectInformation(zip, 1 << 20, a, e) && a.title == "Chair & Table", std::string("项目的 Atom 条目是 ZIP 的第一个条目") + (deflate ? "（deflate）" : "（存储）") + " " + e.message);
            bool leaked = false;
            for (const auto* s : { &a.title, &a.updated, &a.productVersion, &a.omniclass })
                leaked = leaked || (*s && Contains(**s, "someone"));
            for (const std::string& c : a.categories)
                leaked = leaked || Contains(c, "someone");
            Check(!leaked, "条目名里保存者的用户文件夹从不读出");
        }
        const Bytes zip = Fixture::ZipEntry(Fixture::Of(kFamilyAtom), true);
        Check(!ProjectInformation(zip, 16, atom, e) && Contains(e.message, "limit"), "解开后超过上限：" + e.message);
        const Bytes stored = Fixture::ZipEntry(Fixture::Of("x"), false);
        for (const Bytes& bad : { Bytes{}, Fixture::Of("PK\x03\x04"), Fixture::Of("not a zip at all"), Bytes(zip.begin(), zip.begin() + 40), Bytes(stored.begin(), stored.begin() + 20) })
            Check(!ProjectInformation(bad, 1 << 20, atom, e), "坏了的 ZIP：" + e.message);
        Bytes method = zip;
        method[8] = 12;
        Check(!ProjectInformation(method, 1 << 20, atom, e) && Contains(e.message, "method 12"), "不认识的压缩方法：" + e.message);

        std::optional<Atom> found;
        Revit r = Must({ { "PartAtom", Fixture::Of(kFamilyAtom) } });
        Check(DocumentAtom(r, found, e) && found && found->omniclass == "23.40.20.14.17", "族的 PartAtom");
        r = Must({ { "ProjectInformation", zip } });
        Check(DocumentAtom(r, found, e) && found && found->title == "Chair & Table", "项目的 ProjectInformation");
        r = Must({});
        Check(DocumentAtom(r, found, e) && !found, "两者都没有时为空");
    }

    // ---------- 预览图 ----------

    void TestPreview()
    {
        const Bytes png = Fixture::Png(40, 30);
        Bytes stream = Fixture::Noise(300);   // 前面的包装，里面没有 PNG 签名
        stream.erase(std::remove(stream.begin(), stream.end(), 0x89), stream.end());
        stream.insert(stream.end(), png.begin(), png.end());
        stream.resize(stream.size() + 20, 9);   // IEND 后面的垃圾
        Preview p;
        Error e;
        Check(Preview::Find(stream, p, e) && p.width == 40 && p.height == 30 && p.png == png, "在包装后面找到 PNG，检查并剪到 IEND " + e.message);
        std::optional<Preview> found;
        Revit r = Must({ { "RevitPreview4.0", stream } });
        Check(ReadPreview(r, found, e) && found && found->png == png, "从容器里读预览图");
        r = Must({});
        Check(ReadPreview(r, found, e) && !found, "没有预览图的流不是错");

        const Bytes good = Fixture::Png(8, 8);
        std::vector<std::pair<Bytes, std::string>> cases{
            { Fixture::Of("no png here"), "no PNG" },
            { Fixture::Png(0, 5), "dimensions" },
            { Fixture::Png(5, 0), "dimensions" },
            { Fixture::Png(100'000, 10), "dimensions" },
            { Bytes(good.begin(), good.end() - 12), "IEND" },
            { Bytes(good.begin(), good.begin() + 20), "cut short" },
        };
        Bytes flipped = good;
        flipped[20] ^= 1;   // IHDR 的数据里
        cases.push_back({ flipped, "checksum" });
        Bytes notIhdr = good;
        std::memcpy(notIhdr.data() + 12, "IDAT", 4);
        cases.push_back({ notIhdr, "IHDR" });
        Bytes hugeChunk = good;
        std::fill(hugeChunk.begin() + 33, hugeChunk.begin() + 37, 0xff);   // 第一个 IDAT 说自己有 4 GiB
        cases.push_back({ hugeChunk, "over the limit" });
        for (const auto& [bytes, says] : cases)
            Check(!Preview::Find(bytes, p, e) && Contains(e.message, says), says + "：" + e.message);
    }

    // ---------- 许可的隔离 ----------

    // 文件开头连续的 // 注释行
    std::string Header(const std::string& text)
    {
        std::string out;
        std::istringstream in(text);
        for (std::string line; std::getline(in, line) && line.starts_with("//");)
            out += line + "\n";
        return out;
    }

    void TestFence()
    {
        constexpr const char* kRvtRsCommit = "b6086d41b9758fd9b92f42f0a47b8345b9c58056";
        const std::filesystem::path root = Root();
        const std::string notice = ReadText(root / "src/Rvt/NOTICE");
        Check(Contains(ReadText(root / "src/Rvt/LICENSE"), "Apache License") && Contains(ReadText(root / "src/Rvt/LICENSE"), "Version 2.0"), "src/Rvt 带着 Apache-2.0 的许可原文");
        Check(Contains(notice, "Copyright 2026 Griffin Long") && Contains(notice, kRvtRsCommit), "NOTICE 有 rvt-rs 的版权行与移植的提交");
        int files = 0;
        for (const auto& dir : { root / "src/Rvt", root / "tests/Rvt" })
            for (const auto& entry : std::filesystem::directory_iterator(dir))
            {
                const auto ext = entry.path().extension();
                if (ext != ".h" && ext != ".cpp")
                    continue;
                ++files;
                const auto u8 = entry.path().filename().u8string();
                const std::string name(u8.begin(), u8.end());
                const std::string header = Header(ReadText(entry.path()));
                Check(Contains(header, "Apache-2.0") && Contains(header, "src/Rvt/NOTICE"), name + "：开头写明 Apache-2.0 与 NOTICE");
                if (Contains(header, "rvt-rs"))
                    Check(Contains(header, kRvtRsCommit) && Contains(header, "src/") && Contains(header, "Changed") && Contains(header, "Copyright 2026 Griffin Long"),
                          name + "：提到 rvt-rs 的写明提交、文件、改过与版权行");
            }
        Check(files >= 16, "查了 " + std::to_string(files) + " 个文件");
        // 核心库与应用不包含读取器的头文件（R6 起宿主经 MINIBIM_WITH_RVT 使用）
        bool clean = true;
        for (const char* dir : { "src/Model", "src/Commands", "src/Ifc", "src/View", "src/Solid", "src/I18n", "src/Core" })
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root / dir))
                if (entry.is_regular_file() && Contains(ReadText(entry.path()), "Rvt/"))
                {
                    const auto u8 = entry.path().u8string();
                    Check(false, "核心库包含了读取器：" + std::string(u8.begin(), u8.end()));
                    clean = false;
                }
        Check(clean, "核心库不包含 Rvt/ 下的头文件");
        Check(!Contains(ReadText(root / "cmake/MiniBIM.cmake"), "src/Rvt"), "核心库的源文件里没有 src/Rvt");
    }
}

int main()
{
    // 不缓冲：崩溃时也看得到崩在哪一项之后
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestInflate();
    TestCfb();
    TestContainer();
    TestGzip();
    TestHostile();
    TestBasicFileInfo();
    TestAtom();
    TestPreview();
    RvtTest::TestSchema();
    RvtTest::TestRecords();
    RvtTest::TestInventory();
    RvtTest::TestDecoders();
    RvtTest::TestSketch();
    RvtTest::TestImport();
    RvtTest::TestWalls();
    RvtTest::TestFloors();
    RvtTest::TestRoofs();
    RvtTest::TestOpenings();
    RvtTest::TestRooms();
    RvtTest::TestColumns();
    TestFence();
    const int failed = RvtTest::g_failed;
    std::printf("\n%s：%d 项失败\n", failed == 0 ? "全部通过" : "有失败", failed);
    return failed == 0 ? 0 : 1;
}
