// 移植自 BIMCraft crates/core/src/cloud.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Cloud.h"
#include "Model/Model.h"
#include "Model/Text.h"
#include "Model/Zlib.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace MiniBIM::Bim
{
    namespace
    {
        using namespace Clouds;

        // 点云的 Grid 一格多宽，毫米
        constexpr double Cell = 250.0;
        constexpr const char* Axes[3] = { "x", "y", "z" };

        int64_t CellOf(double v) { return static_cast<int64_t>(std::floor(v / Cell)); }

        std::string Num(double v) { return Text::Shortest(v); }

        std::string Round(double v)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.0f", v);
            return buf;
        }

        // 一个立方体格子的键
        struct CellKey
        {
            int64_t x, y, z;
            bool operator==(const CellKey&) const = default;
        };
        struct CellHash
        {
            size_t operator()(const CellKey& k) const
            {
                uint64_t h = static_cast<uint64_t>(k.x) * 0x9E3779B97F4A7C15ull;
                h ^= static_cast<uint64_t>(k.y) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
                h ^= static_cast<uint64_t>(k.z) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
                return static_cast<size_t>(h);
            }
        };

        // 第一遍读到的：几个点、多远、颜色与强度的范围
        struct Stats
        {
            size_t count = 0;
            D3     min{ INFINITY, INFINITY, INFINITY };
            D3     max{ -INFINITY, -INFINITY, -INFINITY };
            std::optional<std::pair<double, double>> intensity;
            std::optional<double> rgb;
            bool   fraction = false;

            bool Add(const Raw& raw, double scale, std::string& error)
            {
                for (size_t k = 0; k < 3; ++k)
                {
                    const double v = raw.p[k] * scale;
                    if (!std::isfinite(v))
                    {
                        error = "point " + std::to_string(count + 1) + " isn't a number along " + Axes[k];
                        return false;
                    }
                    if (std::abs(v) > MaxFileCoord)
                    {
                        error = "point " + std::to_string(count + 1) + " lies " + Round(v) + " mm from the file's origin, farther than " + Num(MaxFileCoord) + " mm";
                        return false;
                    }
                    min[k] = std::min(min[k], v);
                    max[k] = std::max(max[k], v);
                }
                if (raw.intensity && std::isfinite(*raw.intensity))
                {
                    const double i = *raw.intensity;
                    const auto [lo, hi] = intensity.value_or(std::pair{ i, i });
                    intensity = std::pair{ std::min(lo, i), std::max(hi, i) };
                }
                if (raw.rgb)
                    for (const double v : *raw.rgb)
                        if (std::isfinite(v))
                        {
                            rgb = std::max(rgb.value_or(0.0), v);
                            fraction |= v != std::trunc(v);
                        }
                ++count;
                return true;
            }
        };

        // 点的颜色（或强度）换成八位 RGB
        struct Shade
        {
            double full = 255.0;
            std::pair<double, double> intensity{ 0.0, 0.0 };

            static uint8_t Byte(double v)
            {
                if (!std::isfinite(v))
                    return Grey;
                return static_cast<uint8_t>(std::clamp(std::round(v * 255.0), 0.0, 255.0));
            }

            Rgb Of(const Raw& raw) const
            {
                if (raw.rgb)
                    return { Byte((*raw.rgb)[0] / full), Byte((*raw.rgb)[1] / full), Byte((*raw.rgb)[2] / full) };
                if (raw.intensity)
                {
                    const auto [lo, hi] = intensity;
                    const uint8_t g = hi > lo ? Byte((*raw.intensity - lo) / (hi - lo)) : Grey;
                    return { g, g, g };
                }
                return { Grey, Grey, Grey };
            }
        };

        // 点怎么着色；文件没有颜色也没有强度时没有
        std::optional<Shade> ShadeOf(const Stats& s)
        {
            if (!s.rgb && !s.intensity)
                return std::nullopt;
            // 十六位、零到一，或八位：看最亮的颜色
            double full = 255.0;
            if (s.rgb && *s.rgb > 255.0)
                full = 65535.0;
            else if (s.rgb && *s.rgb <= 1.0 && s.fraction)
                full = 1.0;
            return Shade{ full, s.intensity.value_or(std::pair{ 0.0, 0.0 }) };
        }

        std::string_view TrimView(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n' || s.front() == '\v' || s.front() == '\f'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n' || s.back() == '\v' || s.back() == '\f'))
                s.remove_suffix(1);
            return s;
        }

        bool ParseNumber(std::string_view w, double& out)
        {
            const auto v = Text::ParseNumber(w);
            if (!v)
                return false;
            out = *v;
            return true;
        }

        bool ParseUnsigned(std::string_view w, uint64_t& out)
        {
            if (!w.empty() && w.front() == '+')
                w.remove_prefix(1);
            const auto r = std::from_chars(w.data(), w.data() + w.size(), out);
            return r.ec == std::errc() && r.ptr == w.data() + w.size() && !w.empty();
        }

        bool IsUtf8(std::string_view s)
        {
            const std::string out = Text::Lossy(s);
            return out == s;
        }

        // 按空格、制表符或逗号分开的 x y z，再是强度、颜色或两样。PTS 里只有一个数的行是接下来那次扫描的点数
        bool ReadText(std::string_view bytes, bool pts, const Each& f, std::string& error)
        {
            size_t n = 0;
            size_t start = 0;
            while (start <= bytes.size())
            {
                const size_t end = bytes.find('\n', start);
                std::string_view raw = bytes.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
                start = end == std::string_view::npos ? bytes.size() + 1 : end + 1;
                ++n;
                if (!IsUtf8(raw))
                {
                    error = "line " + std::to_string(n) + " isn't text";
                    return false;
                }
                const std::string_view line = TrimView(raw);
                if (line.empty() || line.front() == '#' || line.substr(0, 2) == "//")
                    continue;
                std::vector<std::string_view> words;
                size_t i = 0;
                while (i < line.size())
                {
                    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == ',' || line[i] == '\r' || line[i] == '\v' || line[i] == '\f'))
                        ++i;
                    const size_t from = i;
                    while (i < line.size() && !(line[i] == ' ' || line[i] == '\t' || line[i] == ',' || line[i] == '\r' || line[i] == '\v' || line[i] == '\f'))
                        ++i;
                    if (i > from)
                        words.push_back(line.substr(from, i - from));
                }
                uint64_t count = 0;
                if (pts && words.size() == 1 && ParseUnsigned(words[0], count))
                    continue;
                double v[7] = {};
                for (size_t k = 0; k < words.size() && k < 7; ++k)
                {
                    double x = 0.0;
                    if (!ParseNumber(words[k], x))
                    {
                        error = "line " + std::to_string(n) + ": " + std::string(words[k]) + " isn't a number";
                        return false;
                    }
                    if (!std::isfinite(x))
                    {
                        error = "line " + std::to_string(n) + ": " + std::string(words[k]) + " isn't a number MiniBIM can place";
                        return false;
                    }
                    v[k] = x;
                }
                Raw r;
                r.p = { v[0], v[1], v[2] };
                switch (words.size())
                {
                case 3: break;
                case 4: r.intensity = v[3]; break;
                case 6: r.rgb = D3{ v[3], v[4], v[5] }; break;
                case 7: r.intensity = v[3]; r.rgb = D3{ v[4], v[5], v[6] }; break;
                default:
                    error = "line " + std::to_string(n) + ": " + std::to_string(words.size()) +
                            " columns; a point is x y z, then an intensity, a colour (r g b), or both";
                    return false;
                }
                if (!f(r, error))
                    return false;
            }
            return true;
        }

        // PLY 属性的数的类型
        enum class NumType { I8, U8, I16, U16, I32, U32, F32, F64 };

        std::optional<NumType> NumOf(std::string_view name)
        {
            if (name == "char" || name == "int8") return NumType::I8;
            if (name == "uchar" || name == "uint8") return NumType::U8;
            if (name == "short" || name == "int16") return NumType::I16;
            if (name == "ushort" || name == "uint16") return NumType::U16;
            if (name == "int" || name == "int32") return NumType::I32;
            if (name == "uint" || name == "uint32") return NumType::U32;
            if (name == "float" || name == "float32") return NumType::F32;
            if (name == "double" || name == "float64") return NumType::F64;
            return std::nullopt;
        }

        size_t SizeOf(NumType t)
        {
            switch (t)
            {
            case NumType::I8: case NumType::U8: return 1;
            case NumType::I16: case NumType::U16: return 2;
            case NumType::F64: return 8;
            default: return 4;
            }
        }

        bool IsFloat(NumType t) { return t == NumType::F32 || t == NumType::F64; }

        template <class T>
        T Load(const uint8_t* b, bool big)
        {
            uint8_t a[sizeof(T)];
            std::memcpy(a, b, sizeof(T));
            if (big)
                std::reverse(a, a + sizeof(T));
            T v;
            std::memcpy(&v, a, sizeof(T));
            return v;
        }

        // b 开头的值，big 说的字节序；不够时没有
        std::optional<double> ReadNum(NumType t, std::string_view b, size_t at, bool big)
        {
            if (at > b.size() || b.size() - at < SizeOf(t))
                return std::nullopt;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(b.data()) + at;
            switch (t)
            {
            case NumType::I8: return static_cast<double>(static_cast<int8_t>(p[0]));
            case NumType::U8: return static_cast<double>(p[0]);
            case NumType::I16: return static_cast<double>(Load<int16_t>(p, big));
            case NumType::U16: return static_cast<double>(Load<uint16_t>(p, big));
            case NumType::I32: return static_cast<double>(Load<int32_t>(p, big));
            case NumType::U32: return static_cast<double>(Load<uint32_t>(p, big));
            case NumType::F32: return static_cast<double>(Load<float>(p, big));
            default: return Load<double>(p, big);
            }
        }

        // PLY 头里的一种元素：名称、几个、属性（列表的没有固定大小）
        struct PlyElement
        {
            std::string name;
            size_t      count = 0;
            std::vector<std::pair<std::string, std::optional<NumType>>> props;

            // 二进制文件里一个占几个字节；有列表时不定
            std::optional<size_t> Stride() const
            {
                size_t s = 0;
                for (const auto& [_, t] : props)
                {
                    if (!t)
                        return std::nullopt;
                    s += SizeOf(*t);
                }
                return s;
            }

            std::optional<size_t> Find(std::initializer_list<std::string_view> names) const
            {
                for (size_t i = 0; i < props.size(); ++i)
                    for (const std::string_view w : names)
                        if (Text::AsciiLower(props[i].first) == Text::AsciiLower(w))
                            return i;
                return std::nullopt;
            }
        };

        // PLY 头最长几个字节
        constexpr size_t PlyHeader = size_t(1) << 20;

        std::vector<std::string_view> Words(std::string_view line)
        {
            std::vector<std::string_view> out;
            size_t i = 0;
            while (i < line.size())
            {
                while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
                    ++i;
                const size_t from = i;
                while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
                    ++i;
                if (i > from)
                    out.push_back(line.substr(from, i - from));
            }
            return out;
        }

        // Polygon File Format：文本的头，再是它的元素，文本或二进制。只读顶点
        bool ReadPly(std::string_view bytes, const Each& f, std::string& error)
        {
            if (bytes.substr(0, 3) != "ply")
            {
                error = "it isn't a PLY file: it doesn't start with ply";
                return false;
            }
            const std::string_view head = bytes.substr(0, std::min(PlyHeader, bytes.size()));
            const size_t end = head.find("end_header");
            if (end == std::string_view::npos)
            {
                error = "its header has no end_header";
                return false;
            }
            std::string_view body = bytes.substr(end);
            const size_t nl = body.find('\n');
            body = nl == std::string_view::npos ? std::string_view() : body.substr(nl + 1);
            const std::string_view header = head.substr(0, end);
            if (!IsUtf8(header))
            {
                error = "its header isn't text";
                return false;
            }
            std::optional<std::string> form;
            std::vector<PlyElement> elements;
            bool first = true;
            size_t at = 0;
            while (at < header.size())
            {
                size_t e = header.find('\n', at);
                std::string_view line = header.substr(at, e == std::string_view::npos ? std::string_view::npos : e - at);
                at = e == std::string_view::npos ? header.size() : e + 1;
                if (first)
                {
                    first = false;
                    continue;
                }
                line = TrimView(line);
                if (line.empty())
                    continue;
                const auto w = Words(line);
                if (w.size() == 3 && w[0] == "format")
                    form = std::string(w[1]);
                else if (!w.empty() && (w[0] == "comment" || w[0] == "obj_info"))
                    continue;
                else if (w.size() == 3 && w[0] == "element")
                {
                    uint64_t count = 0;
                    if (!ParseUnsigned(w[2], count))
                    {
                        error = "element " + std::string(w[1]) + " " + std::string(w[2]) + ": not a count";
                        return false;
                    }
                    elements.push_back(PlyElement{ std::string(w[1]), static_cast<size_t>(count), {} });
                }
                else if (w.size() == 5 && w[0] == "property" && w[1] == "list")
                {
                    if (elements.empty())
                    {
                        error = "a property before any element";
                        return false;
                    }
                    elements.back().props.emplace_back(std::string(w[4]), std::nullopt);
                }
                else if (w.size() == 3 && w[0] == "property")
                {
                    if (elements.empty())
                    {
                        error = "a property before any element";
                        return false;
                    }
                    const auto t = NumOf(w[1]);
                    if (!t)
                    {
                        error = "property " + std::string(w[2]) + " is of type " + std::string(w[1]) + ", which PLY doesn't have";
                        return false;
                    }
                    elements.back().props.emplace_back(std::string(w[2]), *t);
                }
                else
                {
                    error = "its header says \"" + std::string(line) + "\", which isn't PLY";
                    return false;
                }
            }
            std::optional<bool> big;
            if (!form)
            {
                error = "its header has no format";
                return false;
            }
            if (*form == "binary_little_endian")
                big = false;
            else if (*form == "binary_big_endian")
                big = true;
            else if (*form != "ascii")
            {
                error = "its format is " + *form + ": PLY is ascii, binary_little_endian or binary_big_endian";
                return false;
            }
            const auto vit = std::find_if(elements.begin(), elements.end(), [](const PlyElement& e) { return e.name == "vertex"; });
            if (vit == elements.end())
            {
                error = "it has no vertex element";
                return false;
            }
            const PlyElement& vertex = *vit;
            for (const auto& [_, t] : vertex.props)
                if (!t)
                {
                    error = "its vertices hold a list, which a point can't";
                    return false;
                }
            const std::optional<size_t> axes[3] = { vertex.Find({ "x" }), vertex.Find({ "y" }), vertex.Find({ "z" }) };
            for (size_t k = 0; k < 3; ++k)
                if (!axes[k])
                {
                    error = std::string("its vertices have no ") + Axes[k];
                    return false;
                }
            const auto r = vertex.Find({ "red", "diffuse_red", "r" }), g = vertex.Find({ "green", "diffuse_green", "g" }),
                       b = vertex.Find({ "blue", "diffuse_blue", "b" });
            const std::optional<std::array<size_t, 3>> rgb = r && g && b ? std::optional<std::array<size_t, 3>>({ *r, *g, *b }) : std::nullopt;
            const auto intensity = vertex.Find({ "intensity", "scalar_intensity" });
            // 浮点的颜色从零到一；按八位的范围交出去
            bool floatRgb = false;
            if (rgb)
                for (const size_t i : *rgb)
                    floatRgb = floatRgb || IsFloat(*vertex.props[i].second);
            const auto point = [&](const std::function<std::optional<double>(size_t)>& v) -> std::optional<Raw>
            {
                const double up = floatRgb ? 255.0 : 1.0;
                Raw raw;
                for (size_t k = 0; k < 3; ++k)
                {
                    const auto x = v(*axes[k]);
                    if (!x)
                        return std::nullopt;
                    raw.p[k] = *x;
                }
                if (intensity)
                {
                    const auto x = v(*intensity);
                    if (!x)
                        return std::nullopt;
                    raw.intensity = *x;
                }
                if (rgb)
                {
                    D3 c{};
                    for (size_t k = 0; k < 3; ++k)
                    {
                        const auto x = v((*rgb)[k]);
                        if (!x)
                            return std::nullopt;
                        c[k] = *x * up;
                    }
                    raw.rgb = c;
                }
                return raw;
            };
            const size_t before = static_cast<size_t>(vit - elements.begin());
            if (!big)
            {
                // 文本：空行不算
                size_t pos = 0;
                const auto nextLine = [&]() -> std::optional<std::string_view>
                {
                    while (pos < body.size())
                    {
                        size_t e = body.find('\n', pos);
                        std::string_view l = body.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos);
                        pos = e == std::string_view::npos ? body.size() : e + 1;
                        l = TrimView(l);
                        if (!l.empty())
                            return l;
                    }
                    return std::nullopt;
                };
                for (size_t i = 0; i < before; ++i)
                    for (size_t k = 0; k < elements[i].count; ++k)
                        if (!nextLine())
                        {
                            error = "it's cut short in its " + elements[i].name + " elements";
                            return false;
                        }
                for (size_t n = 0; n < vertex.count; ++n)
                {
                    const auto line = nextLine();
                    if (!line)
                    {
                        error = "it's cut short: " + std::to_string(n) + " vertices of " + std::to_string(vertex.count);
                        return false;
                    }
                    if (!IsUtf8(*line))
                    {
                        error = "vertex " + std::to_string(n + 1) + " isn't text";
                        return false;
                    }
                    std::vector<double> values;
                    for (const std::string_view w : Words(*line))
                    {
                        double x = 0.0;
                        if (!ParseNumber(w, x))
                        {
                            error = "vertex " + std::to_string(n + 1) + ": " + std::string(w) + " isn't a number";
                            return false;
                        }
                        values.push_back(x);
                    }
                    const auto raw = point([&](size_t i) { return i < values.size() ? std::optional<double>(values[i]) : std::nullopt; });
                    if (!raw)
                    {
                        error = "vertex " + std::to_string(n + 1) + " has too few values";
                        return false;
                    }
                    if (!f(*raw, error))
                        return false;
                }
            }
            else
            {
                size_t skip = 0;
                for (size_t i = 0; i < before; ++i)
                {
                    const auto stride = elements[i].Stride();
                    if (!stride)
                    {
                        error = "its " + elements[i].name + " elements, before its vertices, hold a list, which MiniBIM can't step over";
                        return false;
                    }
                    if (*stride != 0 && elements[i].count > (std::numeric_limits<size_t>::max() - skip) / *stride)
                    {
                        error = "it's cut short";
                        return false;
                    }
                    skip += elements[i].count * *stride;
                }
                const size_t stride = vertex.Stride().value_or(0);
                std::vector<size_t> offsets;
                size_t o = 0;
                for (const auto& [_, t] : vertex.props)
                {
                    offsets.push_back(o);
                    o += t ? SizeOf(*t) : 0;
                }
                const bool fits = stride == 0 || vertex.count <= (std::numeric_limits<size_t>::max() - skip) / stride;
                const size_t need = fits ? skip + vertex.count * stride : 0;
                if (!fits || need > body.size() || skip > body.size())
                {
                    error = "it's cut short: " + std::to_string(vertex.count) + " vertices don't fit in it";
                    return false;
                }
                const std::string_view rows = body.substr(skip, need - skip);
                const size_t step = std::max<size_t>(stride, 1);
                for (size_t at2 = 0; at2 + step <= rows.size(); at2 += step)
                {
                    const std::string_view row = rows.substr(at2, step);
                    const auto raw = point([&](size_t i) -> std::optional<double>
                    {
                        if (i >= vertex.props.size() || !vertex.props[i].second)
                            return std::nullopt;
                        return ReadNum(*vertex.props[i].second, row, offsets[i], *big);
                    });
                    if (!raw)
                    {
                        error = "a vertex can't be read";
                        return false;
                    }
                    if (!f(*raw, error))
                        return false;
                }
            }
            return true;
        }

        // 每种 LAS 点格式的记录最少几个字节，格式 0 到 10
        constexpr size_t LasRecord[11] = { 20, 28, 26, 34, 57, 63, 30, 36, 38, 59, 67 };

        // ASPRS LAS 1.0 到 1.4：头，它自己的记录，再是点，每个照头里的比例与偏移
        bool ReadLas(std::string_view bytes, const Each& f, std::string& error)
        {
            if (bytes.substr(0, 4) != "LASF")
            {
                error = "it isn't a LAS file: it doesn't start with LASF";
                return false;
            }
            const auto* b = reinterpret_cast<const uint8_t*>(bytes.data());
            const auto has = [&](size_t i, size_t n) { return i <= bytes.size() && bytes.size() - i >= n; };
            const auto u8 = [&](size_t i) { return b[i]; };
            const auto u16 = [&](size_t i) { return Load<uint16_t>(b + i, false); };
            const auto u32 = [&](size_t i) { return Load<uint32_t>(b + i, false); };
            const auto u64 = [&](size_t i) { return Load<uint64_t>(b + i, false); };
            const auto f64 = [&](size_t i) { return Load<double>(b + i, false); };
            if (bytes.size() < 227)
            {
                error = "its header is cut short";
                return false;
            }
            const uint8_t major = u8(24), minor = u8(25);
            if (major != 1 || minor > 4)
            {
                error = "it's LAS " + std::to_string(major) + "." + std::to_string(minor) + "; MiniBIM reads 1.0 to 1.4";
                return false;
            }
            const size_t header = u16(94);
            if (header < 227 || header > bytes.size())
            {
                error = "its header is cut short";
                return false;
            }
            const size_t start = u32(96);
            const uint8_t format = u8(104);
            if (format & 0xC0)
            {
                error = "its points are compressed (LAZ): decompress it to LAS first (laszip or PDAL)";
                return false;
            }
            if (format > 10)
            {
                error = "its point format " + std::to_string(format) + " isn't one LAS has (0 to 10)";
                return false;
            }
            const size_t need = LasRecord[format];
            const size_t len = u16(105);
            if (len < need)
            {
                error = "its point records are " + std::to_string(len) + " bytes; point format " + std::to_string(format) + " needs " + std::to_string(need);
                return false;
            }
            const uint64_t legacy = u32(107);
            uint64_t count = legacy;
            if (minor >= 4 && header >= 255 && (legacy == 0 || format >= 6))
            {
                if (!has(247, 8))
                {
                    error = "its header is cut short";
                    return false;
                }
                count = u64(247);
            }
            double scale[3], offset[3];
            for (size_t k = 0; k < 3; ++k)
            {
                scale[k]  = f64(131 + 8 * k);
                offset[k] = f64(155 + 8 * k);
                if (!(std::isfinite(scale[k]) && scale[k] != 0.0))
                {
                    error = std::string("its scale along ") + Axes[k] + " is " + Num(scale[k]);
                    return false;
                }
                if (!std::isfinite(offset[k]))
                {
                    error = std::string("its offset along ") + Axes[k] + " is " + (std::isnan(offset[k]) ? "NaN" : Num(offset[k]));
                    return false;
                }
            }
            const bool fits = start >= header && count <= (std::numeric_limits<size_t>::max() - start) / std::max<size_t>(len, 1) &&
                              start + count * len <= bytes.size();
            if (!fits)
            {
                error = "its points are cut short: " + std::to_string(count) + " of " + std::to_string(len) + " bytes from byte " + std::to_string(start) +
                        " don't fit in " + std::to_string(bytes.size()) + " bytes";
                return false;
            }
            std::optional<size_t> colour;
            switch (format)
            {
            case 2: colour = 20; break;
            case 3: case 5: colour = 28; break;
            case 7: case 8: case 10: colour = 30; break;
            default: break;
            }
            for (uint64_t n = 0; n < count; ++n)
            {
                const uint8_t* r = b + start + n * len;
                const auto i32 = [&](size_t i) { return static_cast<double>(Load<int32_t>(r + i, false)); };
                const auto u16in = [&](size_t i) { return static_cast<double>(Load<uint16_t>(r + i, false)); };
                Raw raw;
                raw.p = { i32(0) * scale[0] + offset[0], i32(4) * scale[1] + offset[1], i32(8) * scale[2] + offset[2] };
                raw.intensity = u16in(12);
                if (colour && *colour + 6 <= len)
                    raw.rgb = D3{ u16in(*colour), u16in(*colour + 2), u16in(*colour + 4) };
                if (!f(raw, error))
                    return false;
            }
            return true;
        }

        // 文件的每个点按文件的顺序交给 f
        bool EachPoint(std::string_view bytes, Format format, const Each& f, std::string& error)
        {
            switch (format)
            {
            case Format::Pts: return ReadText(bytes, true, f, error);
            case Format::Xyz: return ReadText(bytes, false, f, error);
            case Format::Ply: return ReadPly(bytes, f, error);
            case Format::Las: return ReadLas(bytes, f, error);
            default: return E57::Read(bytes, f, error);
            }
        }

        std::string FloatBytes(const std::vector<F3>& points)
        {
            std::string out(points.size() * 12, '\0');
            char* p = out.data();
            for (const F3& v : points)
                for (const float x : v)
                {
                    uint32_t u = std::bit_cast<uint32_t>(x);
                    for (int i = 0; i < 4; ++i)
                        *p++ = static_cast<char>((u >> (8 * i)) & 0xFF);
                }
            return out;
        }
    }

    namespace Clouds
    {
        bool FormatFrom(std::string_view ext, Format& out, std::string& error)
        {
            std::string e = Text::AsciiLower(Text::Trim(ext));
            while (!e.empty() && e.front() == '.')
                e.erase(0, 1);
            if (e == "pts") out = Format::Pts;
            else if (e == "xyz" || e == "txt") out = Format::Xyz;
            else if (e == "ply") out = Format::Ply;
            else if (e == "las") out = Format::Las;
            else if (e == "e57") out = Format::E57;
            else if (e == "laz")
            {
                error = "LAZ is compressed LAS, which MiniBIM doesn't read: decompress it to LAS first (laszip or PDAL)";
                return false;
            }
            else if (e == "rcp" || e == "rcs")
            {
                error = "ReCap's RCP and RCS are closed formats: export the scan from ReCap as E57, LAS or PTS";
                return false;
            }
            else
            {
                error = "a ." + e + " file isn't a point cloud MiniBIM reads: PTS, XYZ, PLY, LAS or E57";
                return false;
            }
            return true;
        }

        std::optional<double> Unit(std::string_view name)
        {
            const std::string n = Text::AsciiLower(Text::Trim(name));
            if (n == "mm") return 1.0;
            if (n == "cm") return 10.0;
            if (n == "m") return 1000.0;
            if (n == "in") return 25.4;
            if (n == "ft") return 304.8;
            return std::nullopt;
        }

        std::optional<D3> Scan::Absolute(size_t i) const
        {
            if (i >= points.size())
                return std::nullopt;
            const F3& p = points[i];
            return D3{ origin[0] + p[0], origin[1] + p[1], origin[2] + p[2] };
        }

        bool Merge(std::vector<std::pair<std::string, Scan>> scans, double spacing, Scan& out, std::vector<Station>& stations, std::string& error)
        {
            if (!(std::isfinite(spacing) && spacing >= 0.0))
            {
                error = "a spacing of " + Num(spacing) + " mm isn't one";
                return false;
            }
            D3 lo{ INFINITY, INFINITY, INFINITY }, hi{ -INFINITY, -INFINITY, -INFINITY };
            for (const auto& [_, s] : scans)
                for (const F3& p : s.points)
                    for (size_t k = 0; k < 3; ++k)
                    {
                        const double v = s.origin[k] + p[k];
                        lo[k] = std::min(lo[k], v);
                        hi[k] = std::max(hi[k], v);
                    }
            if (!(std::isfinite(lo[0]) && std::isfinite(lo[1]) && std::isfinite(lo[2])))
            {
                error = "the files hold no points";
                return false;
            }
            for (size_t k = 0; k < 3; ++k)
                if (hi[k] - lo[k] > MaxSpan)
                {
                    error = "together they're " + Round(hi[k] - lo[k]) + " mm wide along " + Axes[k] + "; up to " + Num(MaxSpan) + " mm";
                    return false;
                }
            const D3 origin{ (lo[0] + hi[0]) / 2.0, (lo[1] + hi[1]) / 2.0, (lo[2] + hi[2]) / 2.0 };
            const bool coloured = std::any_of(scans.begin(), scans.end(), [](const auto& s) { return s.second.colours.has_value(); });
            std::unordered_set<CellKey, CellHash> cells;
            Scan merged;
            merged.origin = origin;
            std::vector<Rgb> colours;
            stations.clear();
            for (auto& [source, s] : scans)
            {
                const size_t before = merged.points.size();
                for (size_t i = 0; i < s.points.size(); ++i)
                {
                    const D3 a{ s.origin[0] + s.points[i][0], s.origin[1] + s.points[i][1], s.origin[2] + s.points[i][2] };
                    if (spacing > 0.0)
                    {
                        const CellKey key{ static_cast<int64_t>(std::floor((a[0] - lo[0]) / spacing)), static_cast<int64_t>(std::floor((a[1] - lo[1]) / spacing)),
                                           static_cast<int64_t>(std::floor((a[2] - lo[2]) / spacing)) };
                        if (!cells.insert(key).second)
                            continue;
                    }
                    merged.points.push_back({ static_cast<float>(a[0] - origin[0]), static_cast<float>(a[1] - origin[1]), static_cast<float>(a[2] - origin[2]) });
                    if (coloured)
                        colours.push_back(s.colours && i < s.colours->size() ? (*s.colours)[i] : Rgb{ Grey, Grey, Grey });
                }
                merged.read = merged.read + s.read < merged.read ? std::numeric_limits<size_t>::max() : merged.read + s.read;
                stations.push_back(Station{ std::move(source), merged.points.size() - before });
            }
            if (coloured)
                merged.colours = std::move(colours);
            out = std::move(merged);
            return true;
        }

        bool Read(std::string_view bytes, Format format, const Options& o, Scan& out, std::string& error)
        {
            if (!(std::isfinite(o.scale) && o.scale > 0.0))
            {
                error = "a unit of " + Num(o.scale) + " mm isn't one";
                return false;
            }
            if (!(std::isfinite(o.spacing) && o.spacing >= 0.0))
            {
                error = "a spacing of " + Num(o.spacing) + " mm isn't one";
                return false;
            }
            if (o.cap == 0)
            {
                error = "a cloud keeps one point at least";
                return false;
            }
            if (bytes.size() > MaxBytes)
            {
                error = "it's " + std::to_string(bytes.size()) + " bytes; up to " + std::to_string(MaxBytes);
                return false;
            }
            // 读两遍：一遍量它，一遍留它的点
            Stats stats;
            if (!EachPoint(bytes, format, [&](const Raw& raw, std::string& e) { return stats.Add(raw, o.scale, e); }, error))
                return false;
            if (stats.count == 0)
            {
                error = "it holds no points";
                return false;
            }
            D3 origin{};
            for (size_t k = 0; k < 3; ++k)
            {
                const double wide = stats.max[k] - stats.min[k];
                if (wide > MaxSpan)
                {
                    error = "it's " + Round(wide) + " mm wide along " + Axes[k] + "; up to " + Num(MaxSpan) + " mm";
                    return false;
                }
                origin[k] = (stats.min[k] + stats.max[k]) / 2.0;
            }
            // 每隔几个留一个，不超过上限，再每个间距的立方体留一个
            const size_t stride = std::max<size_t>((stats.count + o.cap - 1) / o.cap, 1);
            const auto shade = ShadeOf(stats);
            std::unordered_set<CellKey, CellHash> cells;
            Scan s;
            s.origin = origin;
            std::vector<Rgb> colours;
            size_t n = 0;
            const bool ok = EachPoint(bytes, format, [&](const Raw& raw, std::string&)
            {
                const size_t at = n++;
                if (at % stride != 0)
                    return true;
                const D3 p{ raw.p[0] * o.scale, raw.p[1] * o.scale, raw.p[2] * o.scale };
                if (o.spacing > 0.0)
                {
                    const CellKey key{ static_cast<int64_t>(std::floor((p[0] - stats.min[0]) / o.spacing)),
                                       static_cast<int64_t>(std::floor((p[1] - stats.min[1]) / o.spacing)),
                                       static_cast<int64_t>(std::floor((p[2] - stats.min[2]) / o.spacing)) };
                    if (!cells.insert(key).second)
                        return true;
                }
                s.points.push_back({ static_cast<float>(p[0] - origin[0]), static_cast<float>(p[1] - origin[1]), static_cast<float>(p[2] - origin[2]) });
                if (shade)
                    colours.push_back(shade->Of(raw));
                return true;
            }, error);
            if (!ok)
                return false;
            if (shade)
                s.colours = std::move(colours);
            s.read = stats.count;
            out = std::move(s);
            return true;
        }

        size_t& Looked()
        {
            thread_local size_t n = 0;
            return n;
        }
    }

    // ── Cloud ────────────────────────────────────────────────────
    struct Cloud::Pack
    {
        std::once_flag once;
        Json           json;
    };

    // 从上往下看，点按它们落在的 Cell 毫米宽的方格分
    struct Cloud::Grid
    {
        std::once_flag once;
        std::unordered_map<int64_t, std::unordered_map<int64_t, std::vector<uint32_t>>> cells;
    };

    bool Cloud::Make(std::string_view source, Scan scan, Cloud& out, std::string& error)
    {
        return Checked(Text::TakeChars(source, MaxText), {}, scan.origin, scan.read, std::move(scan.points), std::move(scan.colours), out, error);
    }

    bool Cloud::OfStations(Scan scan, std::vector<Station> stations, Cloud& out, std::string& error)
    {
        std::string source = stations.empty() ? std::string() : stations.front().source;
        return Checked(std::move(source), std::move(stations), scan.origin, scan.read, std::move(scan.points), std::move(scan.colours), out, error);
    }

    bool Cloud::Checked(std::string source, std::vector<Station> stations, D3 origin, size_t read, std::vector<F3> points, std::optional<std::vector<Rgb>> colours,
                        Cloud& out, std::string& error)
    {
        if (points.empty() || points.size() > Clouds::MaxPoints)
        {
            error = "a cloud holds 1 to " + std::to_string(Clouds::MaxPoints) + " points, not " + std::to_string(points.size());
            return false;
        }
        if (read < points.size())
        {
            error = "a cloud read from " + std::to_string(read) + " points can't keep " + std::to_string(points.size());
            return false;
        }
        for (const double v : origin)
            if (!(std::isfinite(v) && std::abs(v) <= MaxFileCoord))
            {
                error = "a cloud's origin [" + Num(origin[0]) + ", " + Num(origin[1]) + ", " + Num(origin[2]) + "] isn't one";
                return false;
            }
        if (colours && colours->size() != points.size())
        {
            error = "a cloud has a colour for each point or none";
            return false;
        }
        if (stations.size() == 1)
            stations.clear();
        if (stations.size() > MaxStations)
        {
            error = "a cloud comes from up to " + std::to_string(MaxStations) + " files, not " + std::to_string(stations.size());
            return false;
        }
        if (!stations.empty())
        {
            size_t sum = 0;
            bool over = false;
            for (const Station& t : stations)
            {
                over = over || t.points > std::numeric_limits<size_t>::max() - sum;
                sum += t.points;
            }
            if (over || sum != points.size())
            {
                error = "a cloud's stations hold all its points between them";
                return false;
            }
        }
        for (Station& t : stations)
            t.source = Text::TakeChars(t.source, MaxText);
        if (!stations.empty())
            source = stations.front().source;
        F3 lo{ INFINITY, INFINITY, INFINITY }, hi{ -INFINITY, -INFINITY, -INFINITY };
        for (const F3& p : points)
            for (size_t k = 0; k < 3; ++k)
            {
                const float v = p[k];
                if (!(std::isfinite(v) && std::abs(static_cast<double>(v)) <= MaxSpan))
                {
                    error = "a cloud's point lies " + (std::isnan(v) ? std::string("NaN") : Num(v)) + " mm from its middle, past " + Num(MaxSpan);
                    return false;
                }
                lo[k] = std::min(lo[k], v);
                hi[k] = std::max(hi[k], v);
            }
        Cloud c;
        c.m_source   = std::move(source);
        c.m_stations = std::move(stations);
        c.m_origin   = origin;
        c.m_read     = read;
        c.m_points   = std::make_shared<const std::vector<F3>>(std::move(points));
        if (colours)
            c.m_colours = std::make_shared<const std::vector<Rgb>>(std::move(*colours));
        c.m_lo     = lo;
        c.m_hi     = hi;
        c.m_packed = std::make_shared<Pack>();
        c.m_grid   = std::make_shared<Grid>();
        out = std::move(c);
        return true;
    }

    const std::vector<F3>& Cloud::Points() const
    {
        static const std::vector<F3> none;
        return m_points ? *m_points : none;
    }

    std::pair<D3, D3> Cloud::Bounds() const
    {
        return { D3{ m_lo[0], m_lo[1], m_lo[2] }, D3{ m_hi[0], m_hi[1], m_hi[2] } };
    }

    std::optional<F3> Cloud::Nearest(std::array<double, 2> at, double reach, std::pair<double, double> z) const
    {
        if (!(std::isfinite(reach) && reach >= 0.0 && std::isfinite(at[0]) && std::isfinite(at[1]) && std::abs(at[0]) <= MaxFileCoord &&
              std::abs(at[1]) <= MaxFileCoord) || !m_points || !m_grid)
            return std::nullopt;
        // 只看它有的格子：离它远时一个都没有
        const auto [lo, hi] = Bounds();
        const int64_t x0 = CellOf(std::max(at[0] - reach, lo[0])), x1 = CellOf(std::min(at[0] + reach, hi[0]));
        const int64_t y0 = CellOf(std::max(at[1] - reach, lo[1])), y1 = CellOf(std::min(at[1] + reach, hi[1]));
        if (x0 > x1 || y0 > y1)
            return std::nullopt;
        std::call_once(m_grid->once, [&]
        {
            for (size_t i = 0; i < m_points->size(); ++i)
            {
                const F3& p = (*m_points)[i];
                m_grid->cells[CellOf(p[0])][CellOf(p[1])].push_back(static_cast<uint32_t>(i));
            }
        });
        std::optional<std::pair<double, F3>> best;
        for (int64_t x = x0; x <= x1; ++x)
        {
            const auto col = m_grid->cells.find(x);
            if (col == m_grid->cells.end())
                continue;
            for (int64_t y = y0; y <= y1; ++y)
            {
                const auto cell = col->second.find(y);
                if (cell == col->second.end())
                    continue;
                for (const uint32_t i : cell->second)
                {
                    ++Looked();
                    const F3& p = (*m_points)[i];
                    const double d = std::hypot(p[0] - at[0], p[1] - at[1]);
                    if (d <= reach && p[2] >= z.first && p[2] <= z.second && (!best || d < best->first))
                        best = std::pair{ d, p };
                }
            }
        }
        if (!best)
            return std::nullopt;
        return best->second;
    }

    bool Cloud::operator==(const Cloud& o) const
    {
        const bool samePoints = m_points == o.m_points || (m_points && o.m_points && *m_points == *o.m_points);
        const bool sameColours = m_colours == o.m_colours || (m_colours && o.m_colours && *m_colours == *o.m_colours);
        return m_source == o.m_source && m_stations == o.m_stations && m_origin == o.m_origin && m_read == o.m_read && samePoints && sameColours;
    }

    const Json& Cloud::ToJson() const
    {
        static const Json none;
        if (!m_packed || !m_points)
            return none;
        std::call_once(m_packed->once, [&]
        {
            Json j = Json::object();
            j["source"] = m_source;
            if (!m_stations.empty())
            {
                Json st = Json::array();
                for (const Station& t : m_stations)
                    st.push_back(Json{ { "source", t.source }, { "points", t.points } });
                j["stations"] = std::move(st);
            }
            j["origin"] = Json::array({ m_origin[0], m_origin[1], m_origin[2] });
            j["read"]   = m_read;
            j["points"] = Zlib::Pack(FloatBytes(*m_points));
            if (m_colours)
            {
                std::string bytes;
                bytes.reserve(m_colours->size() * 3);
                for (const Rgb& c : *m_colours)
                    bytes.append(reinterpret_cast<const char*>(c.data()), 3);
                j["colours"] = Zlib::Pack(bytes);
            }
            m_packed->json = std::move(j);
        });
        return m_packed->json;
    }

    bool Cloud::FromJson(const Json& j, Cloud& out, std::string& error)
    {
        if (!j.is_object())
        {
            error = "a cloud is an object of its source, origin, read and points";
            return false;
        }
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.key() != "source" && it.key() != "stations" && it.key() != "origin" && it.key() != "read" && it.key() != "points" && it.key() != "colours")
            {
                error = "a cloud has an unknown field `" + it.key() + "`";
                return false;
            }
        const auto field = [&](const char* k) -> const Json*
        {
            const auto it = j.find(k);
            return it == j.end() ? nullptr : &*it;
        };
        const Json* source = field("source");
        const Json* origin = field("origin");
        const Json* read   = field("read");
        const Json* points = field("points");
        if (!source || !source->is_string() || !origin || !read || !points || !points->is_string())
        {
            error = "a cloud needs its source, origin, read and points";
            return false;
        }
        if (!origin->is_array() || origin->size() != 3 || !(*origin)[0].is_number() || !(*origin)[1].is_number() || !(*origin)[2].is_number())
        {
            error = "a cloud's origin is [x, y, z]";
            return false;
        }
        if (!read->is_number_unsigned() && !(read->is_number_integer() && read->get<int64_t>() >= 0))
        {
            error = "a cloud's read is a count of points";
            return false;
        }
        std::vector<Station> stations;
        if (const Json* st = field("stations"); st && !st->is_null())
        {
            if (!st->is_array())
            {
                error = "a cloud's stations are a list of {source, points}";
                return false;
            }
            for (const Json& t : *st)
            {
                if (!t.is_object() || !t.contains("source") || !t["source"].is_string() || !t.contains("points") || !t["points"].is_number_unsigned() ||
                    t.size() != 2)
                {
                    error = "a cloud's stations are a list of {source, points}";
                    return false;
                }
                stations.push_back(Station{ t["source"].get<std::string>(), t["points"].get<size_t>() });
            }
        }
        std::string bytes;
        if (!Zlib::Unpack("a cloud", points->get<std::string>(), Clouds::MaxPoints * 12, bytes, error))
            return false;
        if (bytes.size() % 12 != 0)
        {
            error = "a cloud's points are cut short";
            return false;
        }
        std::vector<F3> pts(bytes.size() / 12);
        for (size_t i = 0; i < pts.size(); ++i)
            for (size_t k = 0; k < 3; ++k)
            {
                uint32_t u = 0;
                for (int b = 0; b < 4; ++b)
                    u |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[i * 12 + k * 4 + b])) << (8 * b);
                pts[i][k] = std::bit_cast<float>(u);
            }
        std::optional<std::vector<Rgb>> colours;
        if (const Json* c = field("colours"); c && !c->is_null())
        {
            if (!c->is_string())
            {
                error = "a cloud's colours are packed bytes";
                return false;
            }
            std::string raw;
            if (!Zlib::Unpack("a cloud", c->get<std::string>(), Clouds::MaxPoints * 3, raw, error))
                return false;
            std::vector<Rgb> cs(raw.size() / 3);
            for (size_t i = 0; i < cs.size(); ++i)
                cs[i] = { static_cast<uint8_t>(raw[i * 3]), static_cast<uint8_t>(raw[i * 3 + 1]), static_cast<uint8_t>(raw[i * 3 + 2]) };
            colours = std::move(cs);
        }
        const D3 o{ (*origin)[0].get<double>(), (*origin)[1].get<double>(), (*origin)[2].get<double>() };
        Cloud cloud;
        if (!Checked(Text::TakeChars(source->get<std::string>(), MaxText), std::move(stations), o, read->get<size_t>(), std::move(pts), std::move(colours), cloud,
                     error))
            return false;
        // 读进来的就是它存的样子：照原样写回
        std::call_once(cloud.m_packed->once, [&] { cloud.m_packed->json = j; });
        out = std::move(cloud);
        return true;
    }
}
