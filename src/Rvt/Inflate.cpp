// MiniBIM 按 RFC 1951（deflate）自己写的解压，随 src/Rvt 以 Apache-2.0 发布，见 src/Rvt/NOTICE
#include "Rvt/Inflate.h"

#include <array>
#include <cstring>

namespace MiniBIM::Rvt::Deflate
{
    namespace
    {
        // 按字节取、从低位起读的位流；读过了结尾时 over 为真（之后读到的是 0）
        struct Bits
        {
            const uint8_t* p;
            size_t         n;
            size_t         pos   = 0;
            uint64_t       buf   = 0;
            int            count = 0;
            bool           over  = false;

            void Fill()
            {
                while (count <= 56 && pos < n)
                {
                    buf |= static_cast<uint64_t>(p[pos++]) << count;
                    count += 8;
                }
            }
            uint32_t Peek(int k)
            {
                if (count < k)
                    Fill();
                return static_cast<uint32_t>(buf & ((1ull << k) - 1));
            }
            void Drop(int k)
            {
                if (count < k)
                {
                    over  = true;
                    buf   = 0;
                    count = 0;
                    return;
                }
                buf >>= k;
                count -= k;
            }
            uint32_t Get(int k)
            {
                if (k == 0)
                    return 0;
                const uint32_t v = Peek(k);
                Drop(k);
                return v;
            }
            // 丢掉当前字节里剩下的位（存储块从整字节开始）
            void Align() { Drop(count % 8); }
            size_t Consumed() const { return pos - static_cast<size_t>(count / 8); }
        };

        constexpr int MaxBits  = 15;
        constexpr int FastBits = 10;

        // 范式 Huffman 码：各长度的码数、按码排好的符号，以及不超过 FastBits 位的码的速查表（反过来的码 → 符号 << 4 | 长度）
        struct Huffman
        {
            std::array<uint16_t, MaxBits + 1> count{};
            std::array<uint16_t, 320>         symbol{};
            std::array<int16_t, 1 << FastBits> fast{};
        };

        // 返回 0 时码是完整的，大于 0 时不完整（还缺多少），小于 0 时超额（不是前缀码）
        int Build(Huffman& h, const uint8_t* lengths, int n)
        {
            h.count.fill(0);
            for (int s = 0; s < n; ++s)
                ++h.count[lengths[s]];
            h.count[0] = 0;
            int left = 1;
            for (int len = 1; len <= MaxBits; ++len)
            {
                left <<= 1;
                left -= h.count[len];
                if (left < 0)
                    return left;
            }
            std::array<uint16_t, MaxBits + 2> offs{};
            for (int len = 1; len < MaxBits; ++len)
                offs[len + 1] = static_cast<uint16_t>(offs[len] + h.count[len]);
            for (int s = 0; s < n; ++s)
                if (lengths[s])
                    h.symbol[offs[lengths[s]]++] = static_cast<uint16_t>(s);
            // 速查表：码按 RFC 1951 3.2.2 分配，位流里是反着的
            h.fast.fill(-1);
            std::array<uint32_t, MaxBits + 1> next{};
            uint32_t code = 0;
            for (int len = 1; len <= MaxBits; ++len)
            {
                code      = (code + h.count[len - 1]) << 1;
                next[len] = code;
            }
            for (int s = 0; s < n; ++s)
            {
                const int len = lengths[s];
                if (!len)
                    continue;
                const uint32_t c = next[len]++;
                if (len > FastBits)
                    continue;
                uint32_t rev = 0;
                for (int i = 0; i < len; ++i)
                    rev |= (c >> i & 1u) << (len - 1 - i);
                for (uint32_t k = rev; k < (1u << FastBits); k += 1u << len)
                    h.fast[k] = static_cast<int16_t>(s << 4 | len);
            }
            return left;
        }

        // 下一个符号；码不在表里时为 -1
        int Decode(Bits& b, const Huffman& h)
        {
            const int16_t f = h.fast[b.Peek(FastBits)];
            if (f >= 0)
            {
                b.Drop(f & 15);
                return f >> 4;
            }
            int code = 0, first = 0, index = 0;
            for (int len = 1; len <= MaxBits; ++len)
            {
                code |= static_cast<int>(b.Get(1));
                const int c = h.count[len];
                if (code - c < first)
                    return h.symbol[index + (code - first)];
                index += c;
                first += c;
                first <<= 1;
                code <<= 1;
                if (b.over)
                    return -1;
            }
            return -1;
        }

        constexpr uint16_t LengthBase[29]  = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        constexpr uint8_t  LengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        constexpr uint16_t DistBase[30]    = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        constexpr uint8_t  DistExtra[30]   = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

        // 固定 Huffman 码（RFC 1951 3.2.6），只建一次
        const std::pair<Huffman, Huffman>& Fixed()
        {
            static const std::pair<Huffman, Huffman> fixed = []
            {
                std::pair<Huffman, Huffman> f;
                uint8_t lengths[288];
                for (int s = 0; s < 288; ++s)
                    lengths[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
                Build(f.first, lengths, 288);
                uint8_t dist[30];
                std::memset(dist, 5, sizeof dist);
                Build(f.second, dist, 30);
                return f;
            }();
            return fixed;
        }

        struct Inflater
        {
            Bits                  bits;
            std::vector<uint8_t>& out;
            size_t                start, room;
            std::string           reason;
            bool                  tooMuch = false;

            bool Bad(const char* why)
            {
                reason = why;
                return false;
            }
            bool Room(size_t more)
            {
                if (out.size() - start + more > room)
                {
                    tooMuch = true;
                    return false;
                }
                return true;
            }

            bool Stored()
            {
                bits.Align();
                const uint32_t len = bits.Get(16), nlen = bits.Get(16);
                if (bits.over)
                    return Bad("the deflate stream is cut short");
                if ((len ^ 0xffffu) != nlen)
                    return Bad("a stored block's length does not match its complement");
                if (!Room(len))
                    return false;
                uint32_t left = len;
                // 位缓冲里已经有的整字节先取
                while (left > 0 && bits.count >= 8)
                {
                    out.push_back(static_cast<uint8_t>(bits.Get(8)));
                    --left;
                }
                if (bits.n - bits.pos < left)
                    return Bad("the deflate stream is cut short");
                out.insert(out.end(), bits.p + bits.pos, bits.p + bits.pos + left);
                bits.pos += left;
                return true;
            }

            bool Codes(const Huffman& lit, const Huffman& dist)
            {
                for (;;)
                {
                    const int sym = Decode(bits, lit);
                    if (bits.over)
                        return Bad("the deflate stream is cut short");
                    if (sym < 0)
                        return Bad("a code is not in the block's Huffman table");
                    if (sym < 256)
                    {
                        if (!Room(1))
                            return false;
                        out.push_back(static_cast<uint8_t>(sym));
                        continue;
                    }
                    if (sym == 256)
                        return true;
                    const int l = sym - 257;
                    if (l >= 29)
                        return Bad("an invalid length symbol");
                    const size_t len = LengthBase[l] + bits.Get(LengthExtra[l]);
                    const int d = Decode(bits, dist);
                    if (bits.over)
                        return Bad("the deflate stream is cut short");
                    if (d < 0 || d >= 30)
                        return Bad("an invalid distance symbol");
                    const size_t back = DistBase[d] + bits.Get(DistExtra[d]);
                    if (bits.over)
                        return Bad("the deflate stream is cut short");
                    if (back > out.size() - start)
                        return Bad("a distance too far back");
                    if (!Room(len))
                        return false;
                    size_t from = out.size() - back;
                    for (size_t i = 0; i < len; ++i)
                        out.push_back(out[from++]);
                }
            }

            bool Dynamic()
            {
                const int nlen = static_cast<int>(bits.Get(5)) + 257, ndist = static_cast<int>(bits.Get(5)) + 1, ncode = static_cast<int>(bits.Get(4)) + 4;
                if (nlen > 286 || ndist > 30)
                    return Bad("a dynamic block declares too many codes");
                static constexpr uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                uint8_t lengths[320] = {};
                for (int i = 0; i < ncode; ++i)
                    lengths[order[i]] = static_cast<uint8_t>(bits.Get(3));
                if (bits.over)
                    return Bad("the deflate stream is cut short");
                Huffman lencode, distcode;
                if (Build(lencode, lengths, 19) != 0)
                    return Bad("a dynamic block's code-length code is not complete");
                std::memset(lengths, 0, sizeof lengths);
                int index = 0;
                while (index < nlen + ndist)
                {
                    int sym = Decode(bits, lencode);
                    if (bits.over)
                        return Bad("the deflate stream is cut short");
                    if (sym < 0)
                        return Bad("a code-length code is not in the table");
                    if (sym < 16)
                    {
                        lengths[index++] = static_cast<uint8_t>(sym);
                        continue;
                    }
                    uint8_t len = 0;
                    int repeat = 0;
                    if (sym == 16)
                    {
                        if (index == 0)
                            return Bad("a repeat with no length before it");
                        len    = lengths[index - 1];
                        repeat = 3 + static_cast<int>(bits.Get(2));
                    }
                    else if (sym == 17)
                        repeat = 3 + static_cast<int>(bits.Get(3));
                    else
                        repeat = 11 + static_cast<int>(bits.Get(7));
                    if (index + repeat > nlen + ndist)
                        return Bad("code lengths repeat past their end");
                    while (repeat--)
                        lengths[index++] = len;
                }
                if (lengths[256] == 0)
                    return Bad("a dynamic block has no end-of-block code");
                // 不完整的码只允许是只有一个长 1 位的码的那种（同 zlib 的 puff）
                int err = Build(lencode, lengths, nlen);
                if (err < 0 || (err > 0 && !(lencode.count[1] == 1 && Used(lengths, nlen) == 1)))
                    return Bad("a dynamic block's literal/length code is not complete");
                err = Build(distcode, lengths + nlen, ndist);
                if (err < 0 || (err > 0 && !(distcode.count[1] == 1 && Used(lengths + nlen, ndist) == 1)))
                    return Bad("a dynamic block's distance code is not complete");
                return Codes(lencode, distcode);
            }

            static int Used(const uint8_t* lengths, int n)
            {
                int used = 0;
                for (int i = 0; i < n; ++i)
                    used += lengths[i] != 0;
                return used;
            }

            bool Run()
            {
                for (;;)
                {
                    const uint32_t last = bits.Get(1), type = bits.Get(2);
                    if (bits.over)
                        return Bad("the deflate stream is cut short");
                    bool ok = false;
                    if (type == 0)
                        ok = Stored();
                    else if (type == 1)
                        ok = Codes(Fixed().first, Fixed().second);
                    else if (type == 2)
                        ok = Dynamic();
                    else
                        return Bad("an invalid block type");
                    if (!ok)
                        return false;
                    if (last)
                        return true;
                }
            }
        };

        std::array<uint32_t, 256> CrcTable()
        {
            std::array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }
    }

    Status Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t room, size_t& consumed, std::string& reason)
    {
        Inflater f{ Bits{ data, size }, out, out.size(), room, {} };
        if (f.Run())
        {
            consumed = f.bits.Consumed();
            return Status::Ok;
        }
        out.resize(f.start);
        if (f.tooMuch)
            return Status::TooMuch;
        reason = f.reason;
        return Status::Bad;
    }

    uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc)
    {
        static const std::array<uint32_t, 256> table = CrcTable();
        crc = ~crc;
        for (size_t i = 0; i < size; ++i)
            crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
        return ~crc;
    }
}
