// 移植自 BIMCraft crates/core/src/cloud/e57.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
//
// E57（ASTM E2807）：扫描仪导出的、不属于哪家厂商的文件。文件是 1024 字节一页，每页末尾是校验和；一段 XML 说它有什么，
// 每次扫描（data3D）的点在一段二进制里，分成包，每个字段在自己的流里按位打包。照公开的规范读；文件里的每次扫描都带进来，
// 按它的姿态放。不核对校验和：说谎的页读出说谎的点，但不会崩溃
#include "Model/Cloud.h"
#include "Model/Text.h"
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace MiniBIM::Bim::Clouds::E57
{
    namespace
    {
        // 一页的字节，其中放数据的（后四个是校验和）
        constexpr uint64_t Page = 1024;
        constexpr uint64_t Data = Page - 4;
        // XML 最多嵌套多深、最多有几个元素
        constexpr size_t MaxDepth = 64;
        constexpr size_t MaxNodes = size_t(1) << 20;

        uint64_t U64(const uint8_t* b)
        {
            uint64_t v = 0;
            for (int i = 7; i >= 0; --i)
                v = (v << 8) | b[i];
            return v;
        }

        // 前面的扫描用掉的：文件的点数预算还剩多少，以及读过的段
        struct Taken
        {
            uint64_t                     points = 0;
            std::unordered_set<uint64_t> sections;
        };

        // E57 排的文件：逻辑字节，每页 1024 字节里 1020 个
        struct Paged
        {
            std::string_view bytes;

            // 一个物理偏移的逻辑位置；不能落在校验和上
            bool Logical(uint64_t physical, uint64_t& out, std::string& error) const
            {
                if (physical % Page >= Data)
                {
                    error = "offset " + std::to_string(physical) + " falls on a page's checksum";
                    return false;
                }
                out = physical / Page * Data + physical % Page;
                return true;
            }

            // 从逻辑位置 at 起的 n 个逻辑字节接在 out 后面
            bool Take(uint64_t at, size_t n, std::string& out, std::string& error) const
            {
                while (n > 0)
                {
                    const uint64_t inPage   = at % Data;
                    const uint64_t physical = at / Data * Page + inPage;
                    const size_t   here     = static_cast<size_t>(std::min<uint64_t>(Data - inPage, n));
                    if (physical > bytes.size() || bytes.size() - physical < here)
                    {
                        error = "it's cut short: byte " + std::to_string(physical) + " is past its " + std::to_string(bytes.size()) + " bytes";
                        return false;
                    }
                    out.append(bytes.substr(static_cast<size_t>(physical), here));
                    at += here;
                    n -= here;
                }
                return true;
            }
        };

        bool ParseInteger(std::string_view v, int64_t& out)
        {
            v = Text::Trim(v);
            if (!v.empty() && v.front() == '+')
                v.remove_prefix(1);
            if (v.empty() || v.front() == '+')
                return false;
            const auto r = std::from_chars(v.data(), v.data() + v.size(), out);
            return r.ec == std::errc() && r.ptr == v.data() + v.size();
        }

        // XML 的一个元素：名称、属性、里面有什么
        struct Node
        {
            std::string                                      name;
            std::vector<std::pair<std::string, std::string>> attrs;
            std::vector<Node>                                children;
            std::string                                      text;

            const Node* Child(std::string_view n) const
            {
                for (const Node& c : children)
                    if (c.name == n)
                        return &c;
                return nullptr;
            }

            const std::string* Attr(std::string_view n) const
            {
                for (const auto& [k, v] : attrs)
                    if (k == n)
                        return &v;
                return nullptr;
            }

            // 属性 n 的数；没有时为 fallback
            bool Number(std::string_view n, double fallback, double& out, std::string& error) const
            {
                const std::string* v = Attr(n);
                if (!v)
                {
                    out = fallback;
                    return true;
                }
                const auto x = Text::ParseNumber(Text::Trim(*v));
                if (!x)
                {
                    error = "<" + name + ">'s " + std::string(n) + " isn't a number: " + *v;
                    return false;
                }
                out = *x;
                return true;
            }

            bool Integer(std::string_view n, int64_t fallback, int64_t& out, std::string& error) const
            {
                const std::string* v = Attr(n);
                if (!v)
                {
                    out = fallback;
                    return true;
                }
                if (!ParseInteger(*v, out))
                {
                    error = "<" + name + ">'s " + std::string(n) + " isn't an integer: " + *v;
                    return false;
                }
                return true;
            }

            // 子元素里的数，如 <w type="Float">1</w>
            std::optional<double> Value(std::string_view n) const
            {
                const Node* c = Child(n);
                return c ? Text::ParseNumber(Text::Trim(c->text)) : std::nullopt;
            }
        };

        std::string Unescape(std::string_view s)
        {
            std::string out(s);
            if (out.find('&') == std::string::npos)
                return out;
            out = Text::ReplaceAll(std::move(out), "&lt;", "<");
            out = Text::ReplaceAll(std::move(out), "&gt;", ">");
            out = Text::ReplaceAll(std::move(out), "&quot;", "\"");
            out = Text::ReplaceAll(std::move(out), "&apos;", "'");
            return Text::ReplaceAll(std::move(out), "&amp;", "&");
        }

        // 标签在哪里结束：引号外的 >
        std::optional<size_t> TagEnd(std::string_view s)
        {
            char quote = 0;
            for (size_t i = 0; i < s.size(); ++i)
            {
                const char c = s[i];
                if (!quote && (c == '"' || c == '\''))
                    quote = c;
                else if (quote && c == quote)
                    quote = 0;
                else if (!quote && c == '>')
                    return i;
            }
            return std::nullopt;
        }

        bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

        std::string_view TrimStart(std::string_view s)
        {
            while (!s.empty() && IsSpace(s.front()))
                s.remove_prefix(1);
            return s;
        }

        // 标签里的元素：名称，再是 key="value"
        std::optional<Node> Element(std::string_view tag)
        {
            tag = Text::Trim(tag);
            size_t nameEnd = 0;
            while (nameEnd < tag.size() && !IsSpace(tag[nameEnd]))
                ++nameEnd;
            if (nameEnd == 0)
                return std::nullopt;
            Node node;
            node.name = std::string(tag.substr(0, nameEnd));
            std::string_view rest = TrimStart(tag.substr(nameEnd));
            while (!rest.empty())
            {
                const size_t eq = rest.find('=');
                if (eq == std::string_view::npos)
                    return std::nullopt;
                const std::string_view key = Text::Trim(rest.substr(0, eq));
                const std::string_view after = TrimStart(rest.substr(eq + 1));
                if (after.empty() || (after.front() != '"' && after.front() != '\''))
                    return std::nullopt;
                const size_t close = after.find(after.front(), 1);
                if (close == std::string_view::npos)
                    return std::nullopt;
                node.attrs.emplace_back(std::string(key), Unescape(after.substr(1, close - 1)));
                rest = TrimStart(after.substr(close + 1));
            }
            return node;
        }

        // 一个小的 XML 读法，够 E57 用：元素、属性、文字与 CDATA；声明与注释跳过
        bool Parse(std::string_view s, Node& out, std::string& error)
        {
            const auto bad = [&](const std::string& what)
            {
                error = "its XML is broken: " + what;
                return false;
            };
            std::vector<Node> stack(1);
            size_t nodes = 0;
            std::string_view rest = s;
            while (!rest.empty())
            {
                const size_t open = rest.find('<');
                if (open == std::string_view::npos)
                {
                    stack.back().text += Unescape(rest);
                    break;
                }
                stack.back().text += Unescape(rest.substr(0, open));
                rest = rest.substr(open);
                if (Text::StartsWith(rest, "<![CDATA["))
                {
                    const std::string_view r = rest.substr(9);
                    const size_t end = r.find("]]>");
                    if (end == std::string_view::npos)
                        return bad("a CDATA isn't closed");
                    stack.back().text += r.substr(0, end);
                    rest = r.substr(end + 3);
                }
                else if (Text::StartsWith(rest, "<!--"))
                {
                    const std::string_view r = rest.substr(4);
                    const size_t end = r.find("-->");
                    if (end == std::string_view::npos)
                        return bad("a comment isn't closed");
                    rest = r.substr(end + 3);
                }
                else if (Text::StartsWith(rest, "<?") || Text::StartsWith(rest, "<!"))
                {
                    const size_t end = rest.find('>');
                    if (end == std::string_view::npos)
                        return bad("a declaration isn't closed");
                    rest = rest.substr(end + 1);
                }
                else if (Text::StartsWith(rest, "</"))
                {
                    const std::string_view r = rest.substr(2);
                    const size_t end = r.find('>');
                    if (end == std::string_view::npos)
                        return bad("an end tag isn't closed");
                    const std::string name(Text::Trim(r.substr(0, end)));
                    if (stack.size() < 2)
                        return bad("</" + name + "> closes nothing");
                    Node node = std::move(stack.back());
                    stack.pop_back();
                    if (node.name != name)
                        return bad("<" + node.name + "> is closed by </" + name + ">");
                    stack.back().children.push_back(std::move(node));
                    rest = r.substr(end + 1);
                }
                else
                {
                    const std::string_view r = rest.substr(1);
                    const auto end = TagEnd(r);
                    if (!end)
                        return bad("a tag isn't closed");
                    std::string_view tag = r.substr(0, *end);
                    const bool empty = !tag.empty() && tag.back() == '/';
                    if (empty)
                        tag.remove_suffix(1);
                    if (++nodes > MaxNodes)
                    {
                        error = "its XML holds more than " + std::to_string(MaxNodes) + " elements";
                        return false;
                    }
                    auto node = Element(tag);
                    if (!node)
                        return bad("<" + std::string(tag) + ">");
                    if (empty)
                        stack.back().children.push_back(std::move(*node));
                    else
                    {
                        if (stack.size() > MaxDepth)
                        {
                            error = "its XML nests deeper than " + std::to_string(MaxDepth);
                            return false;
                        }
                        stack.push_back(std::move(*node));
                    }
                    rest = r.substr(*end + 1);
                }
            }
            if (stack.size() != 1)
                return bad("<" + stack.back().name + "> isn't closed");
            if (stack.front().children.empty())
                return bad("it has no root element");
            out = std::move(stack.front().children.front());
            return true;
        }

        // 一个字段的值是怎么打包的：IEEE 浮点（32 或 64 位），或 min 之上 bits 位的整数，乘 scale 加 offset（普通 Integer 为 1 与 0）
        struct Pack
        {
            bool     isFloat = false;
            bool     isDouble = false;
            int64_t  min = 0, max = 0;
            uint32_t bitCount = 0;
            double   scale = 1.0, offset = 0.0;

            static bool Of(const Node& node, Pack& out, std::string& error)
            {
                const auto integer = [&](double scale, double offset)
                {
                    int64_t lo = 0, hi = 0;
                    if (!node.Integer("minimum", std::numeric_limits<int64_t>::min(), lo, error) ||
                        !node.Integer("maximum", std::numeric_limits<int64_t>::max(), hi, error))
                        return false;
                    if (hi < lo)
                    {
                        error = "<" + node.name + ">'s maximum " + std::to_string(hi) + " is under its minimum " + std::to_string(lo);
                        return false;
                    }
                    // 从最小到最大每个值都放得下的位数
                    const uint64_t range = static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo);
                    out = Pack{ false, false, lo, hi, static_cast<uint32_t>(std::bit_width(range)), scale, offset };
                    return true;
                };
                const std::string* type = node.Attr("type");
                if (type && *type == "Float")
                {
                    const std::string* precision = node.Attr("precision");
                    out = Pack{ true, !(precision && *precision == "single") };
                    return true;
                }
                if (type && *type == "Integer")
                    return integer(1.0, 0.0);
                if (type && *type == "ScaledInteger")
                {
                    double scale = 1.0, offset = 0.0;
                    if (!node.Number("scale", 1.0, scale, error) || !node.Number("offset", 0.0, offset, error))
                        return false;
                    if (!(std::isfinite(scale) && scale != 0.0 && std::isfinite(offset)))
                    {
                        error = "<" + node.name + ">'s scale " + Text::Shortest(scale) + " and offset " + Text::Shortest(offset) + " aren't usable";
                        return false;
                    }
                    return integer(scale, offset);
                }
                error = "its field <" + node.name + "> is a " + (type ? *type : std::string("thing of no type")) + ", which MiniBIM doesn't read";
                return false;
            }

            uint32_t Bits() const { return isFloat ? (isDouble ? 64 : 32) : bitCount; }

            double Decode(uint64_t raw) const
            {
                if (isFloat)
                    return isDouble ? std::bit_cast<double>(raw) : static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(raw)));
                // min + raw 在 int64 里放得下时精确地加（Rust 用 i128）
                double sum = 0.0;
                if (raw <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) &&
                    (min < 0 || static_cast<int64_t>(raw) <= std::numeric_limits<int64_t>::max() - min))
                    sum = static_cast<double>(min + static_cast<int64_t>(raw));
                else
                    sum = static_cast<double>(min) + static_cast<double>(raw);
                return sum * scale + offset;
            }
        };

        // 一个字段的流：还没读的字节，以及下一个值从它们的第几位开始
        struct Stream
        {
            Pack        pack;
            std::string bytes;
            size_t      bit = 0;

            bool Ready() const
            {
                const size_t bits = pack.Bits();
                return bits == 0 || bit + bits <= bytes.size() * 8;
            }

            // 下一个值，如果它的位都到了
            std::optional<double> Next()
            {
                const size_t bits = pack.Bits();
                if (bits == 0)
                    return pack.Decode(0);
                if (bit + bits > bytes.size() * 8)
                    return std::nullopt;
                const size_t first = bit / 8;
                uint8_t word[16] = {};
                const size_t take = std::min<size_t>(bytes.size() - first, 16);
                std::memcpy(word, bytes.data() + first, take);
                const uint64_t lo = U64(word), hi = U64(word + 8);
                const unsigned s = static_cast<unsigned>(bit % 8);
                const uint64_t all = s ? (lo >> s) | (hi << (64 - s)) : lo;
                const uint64_t raw = bits == 64 ? all : all & ((uint64_t(1) << bits) - 1);
                bit += bits;
                return pack.Decode(raw);
            }

            // 接上一个包的字节；攒了超过 ahead 的流是永远在等别的流
            bool Feed(std::string_view more, size_t ahead, std::string& error)
            {
                bytes.erase(0, bit / 8);
                bit %= 8;
                bytes.append(more);
                if (bytes.size() > ahead)
                {
                    error = "a stream of its points runs " + std::to_string(bytes.size()) + " bytes ahead of the others; up to " + std::to_string(ahead);
                    return false;
                }
                return true;
            }
        };

        // 点由哪些流组成
        struct Fields
        {
            std::optional<std::array<size_t, 3>> cartesian, spherical, rgb;
            std::optional<size_t>                invalid, intensity;
        };

        // 扫描自己的坐标在文件坐标里的哪里：先转，再移
        struct Pose
        {
            double m[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            D3     t{};

            static bool Of(const Node& scan, Pose& pose, std::string& error)
            {
                pose = Pose{};
                const Node* node = scan.Child("pose");
                if (!node)
                    return true;
                if (const Node* r = node->Child("rotation"))
                {
                    double q[4];
                    const char* keys[4] = { "w", "x", "y", "z" };
                    double n = 0.0;
                    for (int i = 0; i < 4; ++i)
                    {
                        q[i] = r->Value(keys[i]).value_or(0.0);
                        n += q[i] * q[i];
                    }
                    n = std::sqrt(n);
                    if (!(std::isfinite(n) && n > 1e-9))
                    {
                        error = "its pose's rotation isn't a rotation";
                        return false;
                    }
                    const double w = q[0] / n, x = q[1] / n, y = q[2] / n, z = q[3] / n;
                    const double m[3][3] = {
                        { 1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - w * z), 2.0 * (x * z + w * y) },
                        { 2.0 * (x * y + w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - w * x) },
                        { 2.0 * (x * z - w * y), 2.0 * (y * z + w * x), 1.0 - 2.0 * (x * x + y * y) },
                    };
                    std::memcpy(pose.m, m, sizeof(m));
                }
                if (const Node* t = node->Child("translation"))
                {
                    pose.t = { t->Value("x").value_or(0.0), t->Value("y").value_or(0.0), t->Value("z").value_or(0.0) };
                    for (const double v : pose.t)
                        if (!std::isfinite(v))
                        {
                            error = "its pose's translation isn't a place";
                            return false;
                        }
                }
                return true;
            }

            D3 Apply(const D3& p) const
            {
                D3 out{};
                for (int i = 0; i < 3; ++i)
                    out[i] = m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + t[i];
                return out;
            }
        };

        // 扫描的颜色换成八位，范围取它的 colorLimits（没有时取字段的）
        struct Colour
        {
            D3 lo{ 0.0, 0.0, 0.0 };
            D3 hi{ 255.0, 255.0, 255.0 };

            static Colour Of(const Node& scan, const std::vector<Stream>& streams, const std::optional<std::array<size_t, 3>>& rgb)
            {
                Colour c;
                const Node* limits = scan.Child("colorLimits");
                const char* names[3] = { "Red", "Green", "Blue" };
                for (size_t k = 0; k < 3; ++k)
                {
                    std::optional<std::pair<double, double>> given;
                    if (limits)
                    {
                        const auto a = limits->Value(std::string("color") + names[k] + "Minimum");
                        const auto b = limits->Value(std::string("color") + names[k] + "Maximum");
                        if (a && b)
                            given = std::pair{ *a, *b };
                    }
                    std::optional<std::pair<double, double>> field;
                    if (rgb && (*rgb)[k] < streams.size())
                    {
                        const Pack& p = streams[(*rgb)[k]].pack;
                        if (!p.isFloat)
                            field = std::pair{ static_cast<double>(p.min) * p.scale + p.offset, static_cast<double>(p.max) * p.scale + p.offset };
                    }
                    const auto use = given ? given : field;
                    if (use && use->second > use->first)
                    {
                        c.lo[k] = use->first;
                        c.hi[k] = use->second;
                    }
                }
                return c;
            }

            D3 OfValues(const D3& v) const
            {
                D3 out{};
                for (size_t k = 0; k < 3; ++k)
                    out[k] = std::round((v[k] - lo[k]) / (hi[k] - lo[k]) * 255.0);
                return out;
            }
        };

        // 数据包的各段交给它们的流
        bool FeedPacket(std::string_view packet, std::vector<Stream>& streams, size_t ahead, std::string& error)
        {
            const auto u16At = [&](size_t i) -> std::optional<size_t>
            {
                if (i + 2 > packet.size())
                    return std::nullopt;
                return static_cast<size_t>(static_cast<uint8_t>(packet[i])) | (static_cast<size_t>(static_cast<uint8_t>(packet[i + 1])) << 8);
            };
            const auto n = u16At(4);
            if (!n)
            {
                error = "a data packet is cut short";
                return false;
            }
            if (*n != streams.size())
            {
                error = "a data packet has " + std::to_string(*n) + " streams; its prototype has " + std::to_string(streams.size());
                return false;
            }
            size_t at = 6 + 2 * *n;
            for (size_t k = 0; k < streams.size(); ++k)
            {
                const auto len = u16At(6 + 2 * k);
                if (!len || at > packet.size() || packet.size() - at < *len)
                {
                    error = "a data packet is cut short";
                    return false;
                }
                if (!streams[k].Feed(packet.substr(at, *len), ahead, error))
                    return false;
                at += *len;
            }
            return true;
        }

        // 下一个点的值，每个流一个，如果每个流都有；不是全有时一个都不取
        bool Pull(std::vector<Stream>& streams, std::vector<double>& values)
        {
            for (const Stream& s : streams)
                if (!s.Ready())
                    return false;
            for (size_t i = 0; i < streams.size(); ++i)
                values[i] = streams[i].Next().value_or(std::numeric_limits<double>::quiet_NaN());
            return true;
        }

        std::optional<Raw> Point(const std::vector<double>& v, const Fields& fields, const Pose& pose, const Colour& colour)
        {
            // 1：只有方向没有距离；2：根本没有点
            if (fields.invalid && v[*fields.invalid] != 0.0)
                return std::nullopt;
            D3 p{};
            if (fields.cartesian)
                p = { v[(*fields.cartesian)[0]], v[(*fields.cartesian)[1]], v[(*fields.cartesian)[2]] };
            else if (fields.spherical)
            {
                const double r = v[(*fields.spherical)[0]], az = v[(*fields.spherical)[1]], el = v[(*fields.spherical)[2]];
                p = { r * std::cos(el) * std::cos(az), r * std::cos(el) * std::sin(az), r * std::sin(el) };
            }
            else
                return std::nullopt;
            Raw raw;
            raw.p = pose.Apply(p);
            if (fields.intensity)
                raw.intensity = v[*fields.intensity];
            if (fields.rgb)
                raw.rgb = colour.OfValues({ v[(*fields.rgb)[0]], v[(*fields.rgb)[1]], v[(*fields.rgb)[2]] });
            return raw;
        }

        bool ScanPoints(const Paged& file, const Node& scan, const Each& f, const Caps& caps, Taken& taken, std::string& error)
        {
            const Node* points = scan.Child("points");
            if (!points)
            {
                error = "it has no points";
                return false;
            }
            const std::string* type = points->Attr("type");
            if (!type || *type != "CompressedVector")
            {
                error = "its points aren't a CompressedVector";
                return false;
            }
            if (const Node* codecs = points->Child("codecs"); codecs && !codecs->children.empty())
            {
                error = "its points are packed by a codec MiniBIM doesn't know; E57 writes bit-packed ones";
                return false;
            }
            int64_t recordCount = 0, fileOffset = 0;
            if (!points->Integer("recordCount", -1, recordCount, error))
                return false;
            if (recordCount < 0)
            {
                error = "it doesn't say how many points it has";
                return false;
            }
            if (!points->Integer("fileOffset", -1, fileOffset, error))
                return false;
            if (fileOffset < 0)
            {
                error = "it doesn't say where its points are";
                return false;
            }
            const uint64_t count = static_cast<uint64_t>(recordCount), at = static_cast<uint64_t>(fileOffset);
            const Node* prototype = points->Child("prototype");
            if (!prototype)
            {
                error = "its points have no prototype";
                return false;
            }
            if (prototype->children.size() > caps.fields)
            {
                error = "its points have " + std::to_string(prototype->children.size()) + " fields; MiniBIM reads up to " + std::to_string(caps.fields);
                return false;
            }
            std::vector<Stream> streams;
            std::vector<std::string_view> names;
            for (const Node& field : prototype->children)
            {
                Stream s;
                if (!Pack::Of(field, s.pack, error))
                    return false;
                streams.push_back(std::move(s));
                names.push_back(field.name);
            }
            const auto find = [&](std::string_view n) -> std::optional<size_t>
            {
                for (size_t i = 0; i < names.size(); ++i)
                    if (names[i] == n)
                        return i;
                return std::nullopt;
            };
            const auto three = [&](std::string_view a, std::string_view b, std::string_view c) -> std::optional<std::array<size_t, 3>>
            {
                const auto x = find(a), y = find(b), z = find(c);
                if (!x || !y || !z)
                    return std::nullopt;
                return std::array<size_t, 3>{ *x, *y, *z };
            };
            Fields fields;
            fields.cartesian = three("cartesianX", "cartesianY", "cartesianZ");
            fields.spherical = three("sphericalRange", "sphericalAzimuth", "sphericalElevation");
            fields.invalid   = find(fields.cartesian ? "cartesianInvalidState" : "sphericalInvalidState");
            fields.intensity = find("intensity");
            fields.rgb       = three("colorRed", "colorGreen", "colorBlue");
            if (!fields.cartesian && !fields.spherical)
            {
                error = "its points have neither cartesian nor spherical coordinates";
                return false;
            }
            if (count == 0)
                return true;
            if (std::all_of(streams.begin(), streams.end(), [](const Stream& s) { return s.pack.Bits() == 0; }))
            {
                error = "its points hold no data";
                return false;
            }
            if (taken.points < count)
            {
                error = "the scans of a file hold up to " + std::to_string(caps.points) + " points together; thin the survey where it was made";
                return false;
            }
            taken.points -= count;
            if (!taken.sections.insert(at).second)
            {
                error = "its points at offset " + std::to_string(at) + " are the section another scan reads; each scan keeps its own";
                return false;
            }
            Pose pose;
            if (!Pose::Of(scan, pose, error))
                return false;
            const Colour colour = Colour::Of(scan, streams, fields.rgb);

            // 段头，再从第一个数据包起的包
            uint64_t start = 0;
            if (!file.Logical(at, start, error))
                return false;
            std::string head;
            if (!file.Take(start, 32, head, error))
                return false;
            const auto word = [&](size_t i) { return i + 8 <= head.size() ? U64(reinterpret_cast<const uint8_t*>(head.data()) + i) : uint64_t(0); };
            if (head.empty() || head[0] != 1)
            {
                error = "its points at offset " + std::to_string(at) + " aren't a compressed vector section";
                return false;
            }
            const uint64_t length = word(8);
            const uint64_t end = start > std::numeric_limits<uint64_t>::max() - length ? std::numeric_limits<uint64_t>::max() : start + length;
            // 点占的位不会少于各字段宽度之和：段里放不下的点数是谎话，在拆开任何包之前就说
            uint64_t bits = 0;
            for (const Stream& s : streams)
                bits += s.pack.Bits();
            const auto satMul = [](uint64_t a, uint64_t b) { return b != 0 && a > std::numeric_limits<uint64_t>::max() / b ? std::numeric_limits<uint64_t>::max() : a * b; };
            const uint64_t need = satMul(count, bits), room = satMul(length > 32 ? length - 32 : 0, 8);
            if (need > room)
            {
                error = "it's cut short: its " + std::to_string(count) + " points take " + std::to_string(need / 8 + (need % 8 != 0)) + " bytes and its section holds " +
                        std::to_string(room / 8);
                return false;
            }
            uint64_t next = 0;
            if (!file.Logical(word(16), next, error))
                return false;
            uint64_t left = count;
            std::string packet;
            std::vector<double> values(streams.size(), 0.0);
            while (left > 0)
            {
                if (next >= end)
                {
                    error = "it's cut short: " + std::to_string(left) + " of its " + std::to_string(count) + " points are missing";
                    return false;
                }
                packet.clear();
                if (!file.Take(next, 4, packet, error))
                    return false;
                const uint64_t size = static_cast<uint64_t>(static_cast<uint8_t>(packet[2]) | (static_cast<uint8_t>(packet[3]) << 8)) + 1;
                switch (static_cast<uint8_t>(packet[0]))
                {
                case 1:
                    if (!file.Take(next + 4, static_cast<size_t>(size > 4 ? size - 4 : 0), packet, error) || !FeedPacket(packet, streams, caps.ahead, error))
                        return false;
                    // 字段都到了的每个点
                    while (left > 0 && Pull(streams, values))
                    {
                        --left;
                        if (const auto raw = Point(values, fields, pose, colour))
                            if (!f(*raw, error))
                                return false;
                    }
                    break;
                case 0:
                case 2:
                    break;
                default:
                    error = "a packet of type " + std::to_string(static_cast<uint8_t>(packet[0])) + ", which E57 doesn't have";
                    return false;
                }
                next += size;
            }
            return true;
        }
    }

    bool Read(std::string_view bytes, const Each& f, std::string& error, const Caps& caps)
    {
        if (!Text::StartsWith(bytes, "ASTM-E57"))
        {
            error = "it isn't an E57 file: it doesn't start with ASTM-E57";
            return false;
        }
        const auto* b = reinterpret_cast<const uint8_t*>(bytes.data());
        const auto has = [&](size_t i, size_t n) { return i + n <= bytes.size(); };
        if (!has(8, 4) || !has(40, 8) || !has(24, 16))
        {
            error = "its header is cut short";
            return false;
        }
        const uint32_t major = static_cast<uint32_t>(b[8]) | (static_cast<uint32_t>(b[9]) << 8) | (static_cast<uint32_t>(b[10]) << 16) |
                               (static_cast<uint32_t>(b[11]) << 24);
        if (major != 1)
        {
            error = "it's E57 version " + std::to_string(major) + "; MiniBIM reads version 1";
            return false;
        }
        const uint64_t page = U64(b + 40);
        if (page != Page)
        {
            error = "its pages are " + std::to_string(page) + " bytes; E57's are " + std::to_string(Page);
            return false;
        }
        const Paged file{ bytes };
        const uint64_t xmlAt = U64(b + 24), xmlLen = U64(b + 32);
        if (xmlLen > bytes.size())
        {
            error = "its XML is longer than the file";
            return false;
        }
        std::string xml;
        uint64_t logical = 0;
        if (!file.Logical(xmlAt, logical, error) || !file.Take(logical, static_cast<size_t>(xmlLen), xml, error))
            return false;
        Node root;
        if (!Parse(Text::Lossy(xml), root, error))
            return false;
        const Node* data3d = root.Child("data3D");
        if (!data3d)
        {
            error = "it holds no scans (no data3D)";
            return false;
        }
        if (data3d->children.size() > caps.scans)
        {
            error = "it holds " + std::to_string(data3d->children.size()) + " scans; MiniBIM reads up to " + std::to_string(caps.scans);
            return false;
        }
        Taken taken;
        taken.points = caps.points;
        for (size_t i = 0; i < data3d->children.size(); ++i)
            if (!ScanPoints(file, data3d->children[i], f, caps, taken, error))
            {
                error = "scan " + std::to_string(i + 1) + ": " + error;
                return false;
            }
        return true;
    }
}
