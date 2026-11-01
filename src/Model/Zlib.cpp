#include "Model/Zlib.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace MiniBIM::Bim::Zlib
{
    namespace
    {
        // 长度码 257 起的基数与额外位数
        constexpr std::array<uint16_t, 29> LenBase  = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        constexpr std::array<uint8_t, 29>  LenExtra = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        // 距离码的基数与额外位数
        constexpr std::array<uint16_t, 30> DistBase  = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                                         4097, 6145, 8193, 12289, 16385, 24577 };
        constexpr std::array<uint8_t, 30>  DistExtra = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

        uint32_t Adler32(std::string_view s)
        {
            uint32_t a = 1, b = 0;
            size_t i = 0;
            while (i < s.size())
            {
                // 5552 字节之内 b 不会溢出
                const size_t end = std::min(s.size(), i + 5552);
                for (; i < end; ++i)
                {
                    a += static_cast<unsigned char>(s[i]);
                    b += a;
                }
                a %= 65521;
                b %= 65521;
            }
            return (b << 16) | a;
        }

        // ── 压缩 ─────────────────────────────────────────────
        class BitWriter
        {
        public:
            explicit BitWriter(std::string& out) : m_out(out) {}

            // 低位先出
            void Bits(uint32_t v, int n)
            {
                m_acc |= static_cast<uint64_t>(v) << m_n;
                m_n += n;
                while (m_n >= 8)
                {
                    m_out.push_back(static_cast<char>(m_acc & 0xFF));
                    m_acc >>= 8;
                    m_n -= 8;
                }
            }
            // 哈夫曼码高位先出
            void Code(uint32_t code, int n)
            {
                uint32_t r = 0;
                for (int i = 0; i < n; ++i)
                    r |= ((code >> i) & 1u) << (n - 1 - i);
                Bits(r, n);
            }
            void Flush()
            {
                if (m_n > 0)
                    m_out.push_back(static_cast<char>(m_acc & 0xFF));
                m_acc = 0;
                m_n   = 0;
            }

        private:
            std::string& m_out;
            uint64_t     m_acc = 0;
            int          m_n   = 0;
        };

        // 固定码的字面量与长度码
        void Symbol(BitWriter& w, int sym)
        {
            if (sym < 144)
                w.Code(0x30 + sym, 8);
            else if (sym < 256)
                w.Code(0x190 + sym - 144, 9);
            else if (sym < 280)
                w.Code(sym - 256, 7);
            else
                w.Code(0xC0 + sym - 280, 8);
        }

        void Match(BitWriter& w, int len, int dist)
        {
            int l = 28;
            while (LenBase[l] > len)
                --l;
            Symbol(w, 257 + l);
            if (LenExtra[l])
                w.Bits(len - LenBase[l], LenExtra[l]);
            int d = 29;
            while (DistBase[d] > dist)
                --d;
            w.Code(d, 5);
            if (DistExtra[d])
                w.Bits(dist - DistBase[d], DistExtra[d]);
        }

        // ── 解压 ─────────────────────────────────────────────
        // 规范哈夫曼码：每种码长几个码，按码排好的符号
        struct Huffman
        {
            std::array<uint16_t, 16> count{};
            std::vector<uint16_t>    symbol;
        };

        // lengths 的码长建表；码长超额（不是前缀码）时为 false。不完整的码可以（只有一个距离码的流）
        bool Build(Huffman& h, const uint8_t* lengths, int n)
        {
            h.count.fill(0);
            for (int i = 0; i < n; ++i)
                ++h.count[lengths[i]];
            if (h.count[0] == n)
            {
                h.symbol.clear();
                return true;
            }
            int left = 1;
            for (int len = 1; len < 16; ++len)
            {
                left = left * 2 - h.count[len];
                if (left < 0)
                    return false;
            }
            std::array<uint16_t, 16> offs{};
            for (int len = 1; len < 15; ++len)
                offs[len + 1] = offs[len] + h.count[len];
            h.symbol.assign(n, 0);
            for (int i = 0; i < n; ++i)
                if (lengths[i])
                    h.symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
            return true;
        }

        class Inflater
        {
        public:
            Inflater(std::string_view in, size_t most, std::string& out) : m_in(in), m_most(most), m_out(out) {}

            std::string why;
            // 解出的超过上限
            bool tooMuch = false;

            bool Run()
            {
                bool last = false;
                while (!last)
                {
                    uint32_t v = 0;
                    if (!Bits(1, v))
                        return false;
                    last = v != 0;
                    uint32_t type = 0;
                    if (!Bits(2, type))
                        return false;
                    bool ok = false;
                    if (type == 0)
                        ok = Stored();
                    else if (type == 1)
                        ok = Fixed();
                    else if (type == 2)
                        ok = Dynamic();
                    else
                        why = "a block of an unknown type";
                    if (!ok)
                        return false;
                }
                return true;
            }

            // 流在哪里结束（补齐到整字节）
            size_t At() const { return m_pos; }

        private:
            bool Bits(int n, uint32_t& out)
            {
                while (m_n < n)
                {
                    if (m_pos >= m_in.size())
                    {
                        why = "the data ends early";
                        return false;
                    }
                    m_acc |= static_cast<uint32_t>(static_cast<unsigned char>(m_in[m_pos++])) << m_n;
                    m_n += 8;
                }
                out = m_acc & ((1u << n) - 1u);
                m_acc >>= n;
                m_n -= n;
                return true;
            }

            bool Decode(const Huffman& h, int& sym)
            {
                int code = 0, first = 0, index = 0;
                for (int len = 1; len < 16; ++len)
                {
                    uint32_t b = 0;
                    if (!Bits(1, b))
                        return false;
                    code |= static_cast<int>(b);
                    const int count = h.count[len];
                    if (code - count < first)
                    {
                        sym = h.symbol[index + (code - first)];
                        return true;
                    }
                    index += count;
                    first += count;
                    first <<= 1;
                    code <<= 1;
                }
                why = "a code that isn't in its table";
                return false;
            }

            bool Put(char c)
            {
                if (m_out.size() >= m_most)
                {
                    why = "too much";
                    tooMuch = true;
                    return false;
                }
                m_out.push_back(c);
                return true;
            }

            bool Stored()
            {
                m_acc = 0;
                m_n   = 0;
                if (m_pos + 4 > m_in.size())
                {
                    why = "the data ends early";
                    return false;
                }
                const auto byte = [&](size_t i) { return static_cast<uint32_t>(static_cast<unsigned char>(m_in[i])); };
                const uint32_t len = byte(m_pos) | (byte(m_pos + 1) << 8), nlen = byte(m_pos + 2) | (byte(m_pos + 3) << 8);
                m_pos += 4;
                if (len != (~nlen & 0xFFFF))
                {
                    why = "a stored block whose length doesn't check";
                    return false;
                }
                if (m_pos + len > m_in.size())
                {
                    why = "the data ends early";
                    return false;
                }
                if (m_out.size() + len > m_most)
                {
                    why = "too much";
                    tooMuch = true;
                    return false;
                }
                m_out.append(m_in.substr(m_pos, len));
                m_pos += len;
                return true;
            }

            bool Codes(const Huffman& lit, const Huffman& dist)
            {
                while (true)
                {
                    int sym = 0;
                    if (!Decode(lit, sym))
                        return false;
                    if (sym < 256)
                    {
                        if (!Put(static_cast<char>(sym)))
                            return false;
                        continue;
                    }
                    if (sym == 256)
                        return true;
                    sym -= 257;
                    if (sym >= 29)
                    {
                        why = "a length code past the table";
                        return false;
                    }
                    uint32_t e = 0;
                    if (!Bits(LenExtra[sym], e))
                        return false;
                    const size_t len = LenBase[sym] + e;
                    int ds = 0;
                    if (!Decode(dist, ds))
                        return false;
                    if (ds >= 30)
                    {
                        why = "a distance code past the table";
                        return false;
                    }
                    if (!Bits(DistExtra[ds], e))
                        return false;
                    const size_t d = DistBase[ds] + e;
                    if (d > m_out.size())
                    {
                        why = "a distance back past the start";
                        return false;
                    }
                    if (m_out.size() + len > m_most)
                    {
                        why = "too much";
                        tooMuch = true;
                        return false;
                    }
                    const size_t from = m_out.size() - d;
                    for (size_t i = 0; i < len; ++i)
                        m_out.push_back(m_out[from + i]);
                }
            }

            bool Fixed()
            {
                static const std::pair<Huffman, Huffman> tables = []
                {
                    std::array<uint8_t, 288> l{};
                    for (int i = 0; i < 288; ++i)
                        l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                    std::array<uint8_t, 30> d{};
                    d.fill(5);
                    std::pair<Huffman, Huffman> t;
                    Build(t.first, l.data(), 288);
                    Build(t.second, d.data(), 30);
                    return t;
                }();
                return Codes(tables.first, tables.second);
            }

            bool Dynamic()
            {
                uint32_t hlit = 0, hdist = 0, hclen = 0;
                if (!Bits(5, hlit) || !Bits(5, hdist) || !Bits(4, hclen))
                    return false;
                const int nlen = static_cast<int>(hlit) + 257, ndist = static_cast<int>(hdist) + 1, ncode = static_cast<int>(hclen) + 4;
                if (nlen > 286 || ndist > 30)
                {
                    why = "a block with too many codes";
                    return false;
                }
                static constexpr std::array<uint8_t, 19> order = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                std::array<uint8_t, 320> lengths{};
                for (int i = 0; i < ncode; ++i)
                {
                    uint32_t v = 0;
                    if (!Bits(3, v))
                        return false;
                    lengths[order[i]] = static_cast<uint8_t>(v);
                }
                Huffman lencode;
                if (!Build(lencode, lengths.data(), 19) || lencode.symbol.empty())
                {
                    why = "a block whose code lengths' code isn't one";
                    return false;
                }
                lengths.fill(0);
                int index = 0;
                while (index < nlen + ndist)
                {
                    int sym = 0;
                    if (!Decode(lencode, sym))
                        return false;
                    if (sym < 16)
                    {
                        lengths[index++] = static_cast<uint8_t>(sym);
                        continue;
                    }
                    uint8_t len = 0;
                    uint32_t rep = 0;
                    if (sym == 16)
                    {
                        if (index == 0)
                        {
                            why = "a repeat with nothing before it";
                            return false;
                        }
                        len = lengths[index - 1];
                        if (!Bits(2, rep))
                            return false;
                        rep += 3;
                    }
                    else if (sym == 17)
                    {
                        if (!Bits(3, rep))
                            return false;
                        rep += 3;
                    }
                    else
                    {
                        if (!Bits(7, rep))
                            return false;
                        rep += 11;
                    }
                    if (index + static_cast<int>(rep) > nlen + ndist)
                    {
                        why = "code lengths past the block's";
                        return false;
                    }
                    while (rep--)
                        lengths[index++] = len;
                }
                if (lengths[256] == 0)
                {
                    why = "a block with no end code";
                    return false;
                }
                Huffman lit, dist;
                if (!Build(lit, lengths.data(), nlen) || !Build(dist, lengths.data() + nlen, ndist))
                {
                    why = "a block whose codes aren't a prefix code";
                    return false;
                }
                return Codes(lit, dist);
            }

            std::string_view m_in;
            size_t           m_pos  = 0;
            uint32_t         m_acc  = 0;
            int              m_n    = 0;
            size_t           m_most = 0;
            std::string&     m_out;
        };

        constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    }

    std::string Deflate(std::string_view in)
    {
        std::string out;
        out.reserve(in.size() / 3 + 64);
        out.push_back(static_cast<char>(0x78));
        out.push_back(static_cast<char>(0x01));
        BitWriter w(out);
        // 一整块，固定的码
        w.Bits(1, 1);
        w.Bits(1, 2);
        constexpr size_t Window = 32768, HashSize = 1 << 15, MaxChain = 32;
        constexpr int MaxLen = 258;
        std::vector<int32_t> head(HashSize, -1), prev(Window, -1);
        const auto hash = [&](size_t i)
        {
            const uint32_t v = static_cast<unsigned char>(in[i]) | (static_cast<unsigned char>(in[i + 1]) << 8) | (static_cast<unsigned char>(in[i + 2]) << 16);
            return (v * 2654435761u) >> 17;
        };
        const auto insert = [&](size_t i)
        {
            if (i + 3 > in.size())
                return;
            const uint32_t h = hash(i);
            prev[i % Window] = head[h];
            head[h] = static_cast<int32_t>(i);
        };
        size_t i = 0;
        while (i < in.size())
        {
            int best = 0;
            size_t bestDist = 0;
            if (i + 3 <= in.size())
            {
                int32_t cand = head[hash(i)];
                const int most = static_cast<int>(std::min<size_t>(MaxLen, in.size() - i));
                for (size_t chain = 0; cand >= 0 && chain < MaxChain; ++chain)
                {
                    const size_t c = static_cast<size_t>(cand);
                    if (i - c > Window - 1)
                        break;
                    int len = 0;
                    while (len < most && in[c + len] == in[i + len])
                        ++len;
                    if (len > best)
                    {
                        best = len;
                        bestDist = i - c;
                        if (len == most)
                            break;
                    }
                    const int32_t next = prev[c % Window];
                    if (next >= cand)
                        break;
                    cand = next;
                }
            }
            if (best >= 3)
            {
                Match(w, best, static_cast<int>(bestDist));
                for (int k = 0; k < best; ++k)
                    insert(i + k);
                i += best;
            }
            else
            {
                Symbol(w, static_cast<unsigned char>(in[i]));
                insert(i);
                ++i;
            }
        }
        Symbol(w, 256);
        w.Flush();
        const uint32_t a = Adler32(in);
        for (int s = 24; s >= 0; s -= 8)
            out.push_back(static_cast<char>((a >> s) & 0xFF));
        return out;
    }

    bool Inflate(std::string_view z, size_t most, std::string& out, std::string& error)
    {
        out.clear();
        if (z.size() < 6)
        {
            error = "it isn't zlib: it's too short";
            return false;
        }
        const unsigned cmf = static_cast<unsigned char>(z[0]), flg = static_cast<unsigned char>(z[1]);
        if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || (cmf * 256 + flg) % 31 != 0 || (flg & 0x20))
        {
            error = "it isn't zlib: its header isn't one";
            return false;
        }
        Inflater inf(z.substr(2), most, out);
        if (!inf.Run())
        {
            error = inf.tooMuch ? "it holds more than " + std::to_string(most) + " bytes" : "it isn't zlib: " + inf.why;
            out.clear();
            return false;
        }
        const size_t at = 2 + inf.At();
        if (at + 4 > z.size())
        {
            error = "it isn't zlib: its check is missing";
            out.clear();
            return false;
        }
        uint32_t want = 0;
        for (size_t k = 0; k < 4; ++k)
            want = (want << 8) | static_cast<unsigned char>(z[at + k]);
        if (want != Adler32(out))
        {
            error = "it isn't zlib: its check doesn't match";
            out.clear();
            return false;
        }
        return true;
    }

    std::string Base64(std::string_view in)
    {
        std::string out;
        out.reserve((in.size() + 2) / 3 * 4);
        size_t i = 0;
        for (; i + 3 <= in.size(); i += 3)
        {
            const uint32_t v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
            out.push_back(Alphabet[(v >> 18) & 63]);
            out.push_back(Alphabet[(v >> 12) & 63]);
            out.push_back(Alphabet[(v >> 6) & 63]);
            out.push_back(Alphabet[v & 63]);
        }
        if (i < in.size())
        {
            uint32_t v = static_cast<unsigned char>(in[i]) << 16;
            if (i + 1 < in.size())
                v |= static_cast<unsigned char>(in[i + 1]) << 8;
            out.push_back(Alphabet[(v >> 18) & 63]);
            out.push_back(Alphabet[(v >> 12) & 63]);
            out.push_back(i + 1 < in.size() ? Alphabet[(v >> 6) & 63] : '=');
            out.push_back('=');
        }
        return out;
    }

    std::optional<std::string> Unbase64(std::string_view text)
    {
        std::string out;
        out.reserve(text.size() / 4 * 3);
        uint32_t acc = 0;
        int n = 0;
        bool padded = false;
        for (const char c : text)
        {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
                continue;
            if (c == '=')
            {
                padded = true;
                continue;
            }
            if (padded)
                return std::nullopt;
            int v;
            if (c >= 'A' && c <= 'Z')
                v = c - 'A';
            else if (c >= 'a' && c <= 'z')
                v = c - 'a' + 26;
            else if (c >= '0' && c <= '9')
                v = c - '0' + 52;
            else if (c == '+')
                v = 62;
            else if (c == '/')
                v = 63;
            else
                return std::nullopt;
            acc = (acc << 6) | static_cast<uint32_t>(v);
            if (++n == 4)
            {
                out.push_back(static_cast<char>((acc >> 16) & 0xFF));
                out.push_back(static_cast<char>((acc >> 8) & 0xFF));
                out.push_back(static_cast<char>(acc & 0xFF));
                acc = 0;
                n   = 0;
            }
        }
        if (n == 1)
            return std::nullopt;
        if (n == 2)
            out.push_back(static_cast<char>((acc >> 4) & 0xFF));
        else if (n == 3)
        {
            out.push_back(static_cast<char>((acc >> 10) & 0xFF));
            out.push_back(static_cast<char>((acc >> 2) & 0xFF));
        }
        return out;
    }

    std::string Pack(std::string_view bytes) { return Base64(Deflate(bytes)); }

    bool Unpack(std::string_view what, std::string_view text, size_t most, std::string& out, std::string& error)
    {
        const auto packed = Unbase64(text);
        if (!packed)
        {
            error = std::string(what) + "'s data isn't base64";
            return false;
        }
        std::string why;
        if (!Inflate(*packed, most, out, why))
        {
            error = why.rfind("it holds", 0) == 0 ? std::string(what) + "'s data holds more than " + std::to_string(most) + " bytes"
                                                   : std::string(what) + "'s data isn't zlib: " + why.substr(why.find(':') + 2);
            return false;
        }
        return true;
    }
}
