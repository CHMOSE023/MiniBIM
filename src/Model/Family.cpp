// 移植自 BIMCraft crates/core/src/family.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Family.h"
#include "Model/Csg.h"
#include "Model/Model.h"
#include "Model/Text.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

namespace MiniBIM::Bim
{
    namespace Fam
    {
        namespace
        {
            using namespace V3;

            constexpr double kPi  = 3.14159265358979323846;
            constexpr double kTau = 2.0 * kPi;
            // 公式最多嵌套几层
            constexpr size_t MaxDepth = 64;
            // 圆形轮廓画成几条边
            constexpr size_t Segments = 32;
            // 符号的圆弧每段弦转过的度数
            constexpr double ArcStep  = 3.75;

            std::string Num(double x) { return Text::Shortest(x); }

            // ── UTF-8 与字符类别（Rust 的 char 方法）────────────────
            std::u32string Decode(std::string_view s)
            {
                std::u32string out;
                for (size_t i = 0; i < s.size();)
                {
                    const unsigned char c = static_cast<unsigned char>(s[i]);
                    char32_t cp = c;
                    size_t n = 1;
                    if (c >= 0xF0 && i + 3 < s.size())
                    {
                        cp = (c & 0x07u) << 18 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12 |
                             (static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6 | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
                        n = 4;
                    }
                    else if (c >= 0xE0 && i + 2 < s.size())
                    {
                        cp = (c & 0x0Fu) << 12 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6 | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
                        n = 3;
                    }
                    else if (c >= 0xC0 && i + 1 < s.size())
                    {
                        cp = (c & 0x1Fu) << 6 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
                        n = 2;
                    }
                    out.push_back(cp);
                    i += n;
                }
                return out;
            }

            std::string Encode(std::u32string_view s)
            {
                std::string out;
                for (const char32_t cp : s)
                {
                    if (cp < 0x80)
                        out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800)
                    {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    else if (cp < 0x10000)
                    {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    else
                    {
                        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                }
                return out;
            }

            bool IsWhitespace(char32_t c)
            {
                return c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
                       c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
            }

            // Unicode 字母的近似：拉丁、希腊、西里尔、阿拉伯、希伯来、各种东亚文字（项目语言与界面语言用得到的）
            bool IsAlphabetic(char32_t c)
            {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
                    return true;
                if (c < 0xAA)
                    return false;
                if (c == 0xAA || c == 0xB5 || c == 0xBA)
                    return true;
                if (c >= 0xC0 && c <= 0x24F)
                    return c != 0xD7 && c != 0xF7;
                return (c >= 0x370 && c <= 0x3FF && c != 0x37E && c != 0x387) || (c >= 0x400 && c <= 0x52F) || (c >= 0x5D0 && c <= 0x5EA) ||
                       (c >= 0x620 && c <= 0x64A) || (c >= 0x1E00 && c <= 0x1FFF) || (c >= 0x3040 && c <= 0x30FF) ||
                       (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7A3) ||
                       (c >= 0xF900 && c <= 0xFAFF);
            }

            bool IsDigit(char32_t c) { return c >= '0' && c <= '9'; }
            bool IsAlphanumeric(char32_t c) { return IsAlphabetic(c) || IsDigit(c) || (c >= 0x660 && c <= 0x669) || (c >= 0xFF10 && c <= 0xFF19); }

            std::string TrimEnd(const std::u32string& s)
            {
                size_t n = s.size();
                while (n > 0 && IsWhitespace(s[n - 1]))
                    --n;
                return Encode(std::u32string_view(s).substr(0, n));
            }

            // ── 公式 ────────────────────────────────────────────
            struct Node
            {
                enum class K
                {
                    Number,
                    Name,
                    Minus,
                    Chain,  // 和或积：第一项，然后每个运算符与它后面的项，平铺，长了也不嵌套
                    Op,
                    Call,
                };
                K                 k = K::Number;
                double            number = 0.0;
                std::string       name;
                char              op = 0;
                std::vector<Node> kids;
                std::vector<char> ops;
            };

            // 读公式：比较套着和，和套着积，积套着幂（幂右结合）
            struct Reader
            {
                std::u32string chars;
                size_t at = 0;
                size_t depth = 0;
                std::string error;

                std::optional<char32_t> Peek()
                {
                    while (at < chars.size() && IsWhitespace(chars[at]))
                        ++at;
                    if (at < chars.size())
                        return chars[at];
                    return std::nullopt;
                }

                bool Eat(char32_t c)
                {
                    const bool found = Peek() == c;
                    at += found ? 1 : 0;
                    return found;
                }

                // 再深一层；超过 MaxDepth 拒绝，恶意的公式耗不尽栈
                bool Deeper()
                {
                    ++depth;
                    if (depth > MaxDepth)
                    {
                        error = "it's nested too deep (more than " + std::to_string(MaxDepth) + " levels)";
                        return false;
                    }
                    return true;
                }

                bool Compare(Node& out)
                {
                    if (!Deeper())
                        return false;
                    Node a;
                    if (!Sum(a))
                        return false;
                    char op = 0;
                    const auto c = Peek();
                    if (c == U'<')
                    {
                        ++at;
                        op = Eat(U'=') ? 'l' : '<';
                    }
                    else if (c == U'>')
                    {
                        ++at;
                        op = Eat(U'=') ? 'g' : '>';
                    }
                    else if (c == U'=')
                    {
                        ++at;
                        op = '=';
                    }
                    else
                    {
                        --depth;
                        out = std::move(a);
                        return true;
                    }
                    Node b;
                    if (!Sum(b))
                        return false;
                    --depth;
                    out = Node{ Node::K::Op, 0.0, {}, op, {}, {} };
                    out.kids.push_back(std::move(a));
                    out.kids.push_back(std::move(b));
                    return true;
                }

                bool Sum(Node& out)
                {
                    Node first;
                    if (!Product(first))
                        return false;
                    Node chain{ Node::K::Chain, 0.0, {}, 0, {}, {} };
                    for (auto c = Peek(); c == U'+' || c == U'-'; c = Peek())
                    {
                        ++at;
                        Node term;
                        if (!Product(term))
                            return false;
                        chain.ops.push_back(static_cast<char>(*c));
                        chain.kids.push_back(std::move(term));
                    }
                    if (chain.kids.empty())
                        out = std::move(first);
                    else
                    {
                        chain.kids.insert(chain.kids.begin(), std::move(first));
                        out = std::move(chain);
                    }
                    return true;
                }

                bool Product(Node& out)
                {
                    Node first;
                    if (!Power(first))
                        return false;
                    Node chain{ Node::K::Chain, 0.0, {}, 0, {}, {} };
                    for (auto c = Peek(); c == U'*' || c == U'/'; c = Peek())
                    {
                        ++at;
                        Node term;
                        if (!Power(term))
                            return false;
                        chain.ops.push_back(static_cast<char>(*c));
                        chain.kids.push_back(std::move(term));
                    }
                    if (chain.kids.empty())
                        out = std::move(first);
                    else
                    {
                        chain.kids.insert(chain.kids.begin(), std::move(first));
                        out = std::move(chain);
                    }
                    return true;
                }

                bool Power(Node& out)
                {
                    Node a;
                    if (!Unary(a))
                        return false;
                    if (!Eat(U'^'))
                    {
                        out = std::move(a);
                        return true;
                    }
                    if (!Deeper())
                        return false;
                    Node b;
                    if (!Power(b))
                        return false;
                    --depth;
                    out = Node{ Node::K::Op, 0.0, {}, '^', {}, {} };
                    out.kids.push_back(std::move(a));
                    out.kids.push_back(std::move(b));
                    return true;
                }

                bool Unary(Node& out)
                {
                    const auto c = Peek();
                    if (c == U'-' || c == U'+')
                    {
                        ++at;
                        if (!Deeper())
                            return false;
                        Node a;
                        if (!Unary(a))
                            return false;
                        --depth;
                        if (c == U'-')
                        {
                            out = Node{ Node::K::Minus, 0.0, {}, 0, {}, {} };
                            out.kids.push_back(std::move(a));
                        }
                        else
                            out = std::move(a);
                        return true;
                    }
                    return Atom(out);
                }

                bool Atom(Node& out)
                {
                    const auto c = Peek();
                    if (!c)
                    {
                        error = "something's missing at its end";
                        return false;
                    }
                    if (*c == U'(')
                    {
                        ++at;
                        if (!Compare(out))
                            return false;
                        if (!Eat(U')'))
                        {
                            error = "a `(` isn't closed";
                            return false;
                        }
                        return true;
                    }
                    if (IsDigit(*c) || *c == U'.')
                    {
                        const size_t start = at;
                        while (at < chars.size() && (IsDigit(chars[at]) || chars[at] == U'.'))
                            ++at;
                        // 指数：1e6、2.5E-3
                        if (at < chars.size() && (chars[at] == U'e' || chars[at] == U'E'))
                        {
                            const size_t sign = at + 1 < chars.size() && (chars[at + 1] == U'+' || chars[at + 1] == U'-') ? 1 : 0;
                            if (at + 1 + sign < chars.size() && IsDigit(chars[at + 1 + sign]))
                            {
                                at += 1 + sign;
                                while (at < chars.size() && IsDigit(chars[at]))
                                    ++at;
                            }
                        }
                        const std::string text = Encode(std::u32string_view(chars).substr(start, at - start));
                        const auto x = Text::ParseNumber(text);
                        if (!x)
                        {
                            error = "`" + text + "` isn't a number";
                            return false;
                        }
                        out = Node{ Node::K::Number, *x, {}, 0, {}, {} };
                        return true;
                    }
                    // 名称里可以有空格，同 Revit 的：`Leg Height`
                    if (IsAlphabetic(*c) || *c == U'_')
                    {
                        const size_t start = at;
                        while (at < chars.size() && (IsAlphanumeric(chars[at]) || chars[at] == U'_' || chars[at] == U' '))
                            ++at;
                        const std::string name = TrimEnd(chars.substr(start, at - start));
                        if (!Eat(U'('))
                        {
                            out = Node{ Node::K::Name, 0.0, name, 0, {}, {} };
                            return true;
                        }
                        Node call{ Node::K::Call, 0.0, name, 0, {}, {} };
                        if (!Eat(U')'))
                        {
                            for (;;)
                            {
                                Node arg;
                                if (!Compare(arg))
                                    return false;
                                call.kids.push_back(std::move(arg));
                                if (Eat(U')'))
                                    break;
                                if (!Eat(U','))
                                {
                                    error = "`" + name + "(` needs a `,` or a `)`";
                                    return false;
                                }
                            }
                        }
                        out = std::move(call);
                        return true;
                    }
                    error = "`" + Encode(std::u32string(1, *c)) + "` can't start a value there";
                    return false;
                }
            };

            double Truth(bool b) { return b ? 1.0 : 0.0; }

            const char* const Functions =
                "if, and, or, not, min, max, abs, sqrt, round, roundup, rounddown, sin, cos, tan, asin, acos, atan, exp, ln, log and pi()";

            using Getter = std::function<bool(const std::string&, double&, std::string&)>;

            // 节点的值：名称的值由 get 给出；if 只算它走的那一支
            bool Value(const Node& n, const Getter& get, double& out, std::string& error)
            {
                switch (n.k)
                {
                case Node::K::Number:
                    out = n.number;
                    return true;
                case Node::K::Name:
                    return get(n.name, out, error);
                case Node::K::Minus:
                {
                    double a = 0.0;
                    if (!Value(n.kids[0], get, a, error))
                        return false;
                    out = -a;
                    return true;
                }
                case Node::K::Chain:
                {
                    double x = 0.0;
                    if (!Value(n.kids[0], get, x, error))
                        return false;
                    for (size_t i = 1; i < n.kids.size(); ++i)
                    {
                        double y = 0.0;
                        if (!Value(n.kids[i], get, y, error))
                            return false;
                        switch (n.ops[i - 1])
                        {
                        case '+': x = x + y; break;
                        case '-': x = x - y; break;
                        case '*': x = x * y; break;
                        default: x = x / y; break;
                        }
                    }
                    out = x;
                    return true;
                }
                case Node::K::Op:
                {
                    double x = 0.0, y = 0.0;
                    if (!Value(n.kids[0], get, x, error) || !Value(n.kids[1], get, y, error))
                        return false;
                    switch (n.op)
                    {
                    case '+': out = x + y; break;
                    case '-': out = x - y; break;
                    case '*': out = x * y; break;
                    case '/': out = x / y; break;
                    case '^': out = std::pow(x, y); break;
                    case '<': out = Truth(x < y); break;
                    case '>': out = Truth(x > y); break;
                    case 'l': out = Truth(x <= y); break;
                    case 'g': out = Truth(x >= y); break;
                    // 在浮点误差以内相等，同 Revit 的比较
                    default: out = Truth(std::abs(x - y) <= 1e-9 * std::max({ std::abs(x), std::abs(y), 1.0 })); break;
                    }
                    return true;
                }
                case Node::K::Call:
                {
                    const std::string f = Text::AsciiLower(n.name);
                    const auto& args = n.kids;
                    auto one = [&](double& x) -> bool
                    {
                        if (args.size() != 1)
                        {
                            error = "`" + n.name + "` takes one value, not " + std::to_string(args.size());
                            return false;
                        }
                        return Value(args[0], get, x, error);
                    };
                    const auto deg = [](double x) { return x * kPi / 180.0; };
                    const auto toDeg = [](double x) { return x * 180.0 / kPi; };
                    if (f == "if")
                    {
                        if (args.size() != 3)
                        {
                            error = "`" + n.name + "` takes a condition and two values, not " + std::to_string(args.size()) + " values";
                            return false;
                        }
                        double c = 0.0;
                        if (!Value(args[0], get, c, error))
                            return false;
                        return Value(c != 0.0 ? args[1] : args[2], get, out, error);
                    }
                    if (f == "and" || f == "or" || f == "min" || f == "max")
                    {
                        if (args.empty())
                        {
                            error = "`" + n.name + "` takes one value or more";
                            return false;
                        }
                        std::vector<double> xs;
                        for (const Node& a : args)
                        {
                            double x = 0.0;
                            if (!Value(a, get, x, error))
                                return false;
                            xs.push_back(x);
                        }
                        if (f == "and")
                            out = Truth(std::all_of(xs.begin(), xs.end(), [](double x) { return x != 0.0; }));
                        else if (f == "or")
                            out = Truth(std::any_of(xs.begin(), xs.end(), [](double x) { return x != 0.0; }));
                        else if (f == "min")
                        {
                            out = INFINITY;
                            for (const double x : xs)
                                out = std::fmin(out, x);
                        }
                        else
                        {
                            out = -INFINITY;
                            for (const double x : xs)
                                out = std::fmax(out, x);
                        }
                        return true;
                    }
                    if (f == "pi" && args.empty())
                    {
                        out = kPi;
                        return true;
                    }
                    static const char* const unary[] = { "not", "abs", "sqrt", "round", "roundup", "rounddown", "sin", "cos", "tan",
                                                         "asin", "acos", "atan", "exp", "ln", "log" };
                    if (std::find_if(std::begin(unary), std::end(unary), [&](const char* u) { return f == u; }) == std::end(unary))
                    {
                        error = "`" + n.name + "` isn't a function: there are " + Functions;
                        return false;
                    }
                    double x = 0.0;
                    if (!one(x))
                        return false;
                    if (f == "not") out = Truth(x == 0.0);
                    else if (f == "abs") out = std::abs(x);
                    else if (f == "sqrt") out = std::sqrt(x);
                    else if (f == "round") out = std::round(x);
                    else if (f == "roundup") out = std::ceil(x);
                    else if (f == "rounddown") out = std::floor(x);
                    else if (f == "sin") out = std::sin(deg(x));
                    else if (f == "cos") out = std::cos(deg(x));
                    else if (f == "tan") out = std::tan(deg(x));
                    else if (f == "asin") out = toDeg(std::asin(x));
                    else if (f == "acos") out = toDeg(std::acos(x));
                    else if (f == "atan") out = toDeg(std::atan(x));
                    else if (f == "exp") out = std::exp(x);
                    else if (f == "ln") out = std::log(x);
                    else out = std::log10(x);
                    return true;
                }
                }
                return false;
            }

            // 读 formula
            bool Parse(std::string_view formula, Node& out, std::string& error)
            {
                if (Text::Trim(formula).empty())
                {
                    error = "a formula is empty";
                    return false;
                }
                Reader r{ Decode(formula), 0, 0, {} };
                if (!r.Compare(out))
                {
                    error = "formula `" + std::string(formula) + "`: " + r.error;
                    return false;
                }
                if (const auto c = r.Peek())
                {
                    error = "formula `" + std::string(formula) + "`: `" + Encode(std::u32string(1, *c)) + "` follows where it should end";
                    return false;
                }
                return true;
            }

            // 已读成 node 的 formula 算出来：一个数，或说明为什么不是数
            bool Worked(std::string_view formula, const Node& node, const Getter& get, double& out, std::string& error)
            {
                double x = 0.0;
                std::string why;
                if (!Value(node, get, x, why))
                {
                    error = "formula `" + std::string(formula) + "`: " + why;
                    return false;
                }
                if (!std::isfinite(x))
                {
                    error = "formula `" + std::string(formula) + "`: it comes to " + Num(x) + ", which isn't a number";
                    return false;
                }
                out = x;
                return true;
            }

            bool Walk(const Node& n, std::vector<std::string>& out, std::string& error)
            {
                switch (n.k)
                {
                case Node::K::Number: return true;
                case Node::K::Name:
                    if (std::find(out.begin(), out.end(), n.name) == out.end())
                        out.push_back(n.name);
                    return true;
                case Node::K::Minus:
                case Node::K::Chain:
                case Node::K::Op:
                    for (const Node& k : n.kids)
                        if (!Walk(k, out, error))
                            return false;
                    return true;
                case Node::K::Call:
                {
                    const std::string f = Text::AsciiLower(n.name);
                    bool fits = false;
                    if (f == "if")
                        fits = n.kids.size() == 3;
                    else if (f == "and" || f == "or" || f == "min" || f == "max")
                        fits = !n.kids.empty();
                    else if (f == "pi")
                        fits = n.kids.empty();
                    else if (f == "not" || f == "abs" || f == "sqrt" || f == "round" || f == "roundup" || f == "rounddown" || f == "sin" ||
                             f == "cos" || f == "tan" || f == "asin" || f == "acos" || f == "atan" || f == "exp" || f == "ln" || f == "log")
                        fits = n.kids.size() == 1;
                    else
                    {
                        error = "`" + n.name + "` isn't a function: there are " + Functions;
                        return false;
                    }
                    if (!fits)
                    {
                        error = "`" + n.name + "` doesn't take " + std::to_string(n.kids.size()) + " values";
                        return false;
                    }
                    for (const Node& k : n.kids)
                        if (!Walk(k, out, error))
                            return false;
                    return true;
                }
                }
                return true;
            }

            // 已读过的公式：同一个公式在各类型间只读一次
            using ReadCache = std::unordered_map<std::string, std::pair<std::shared_ptr<Node>, std::string>>;

            // e 当作数：它本身，或它的公式在 values 上算出的值
            bool Number(const Expr& e, const Values& values, ReadCache& read, double& out, std::string& error)
            {
                double x = 0.0;
                if (const double* d = std::get_if<double>(&e))
                    x = *d;
                else
                {
                    const std::string& f = std::get<std::string>(e);
                    auto it = read.find(f);
                    if (it == read.end())
                    {
                        auto node = std::make_shared<Node>();
                        std::string why;
                        if (!Parse(f, *node, why))
                            node.reset();
                        it = read.emplace(f, std::pair{ node, why }).first;
                    }
                    if (!it->second.first)
                    {
                        error = it->second.second;
                        return false;
                    }
                    const Getter get = [&](const std::string& n, double& v, std::string& why) -> bool
                    {
                        const auto found = values.find(n);
                        if (found == values.end())
                        {
                            why = "`" + n + "` isn't one of its parameters";
                            return false;
                        }
                        if (const double* d = std::get_if<double>(&found->second))
                        {
                            v = *d;
                            return true;
                        }
                        why = "`" + n + "` is a material, not a number";
                        return false;
                    };
                    if (!Worked(f, *it->second.first, get, x, error))
                        return false;
                }
                if (std::isfinite(x) && std::abs(x) <= MaxCoord)
                {
                    out = x;
                    return true;
                }
                error = Num(x) + " is not a usable number";
                return false;
            }

            // tris 从多低到多高
            std::pair<double, double> Heights(const std::vector<Tri>& tris)
            {
                double lo = INFINITY, hi = -INFINITY;
                for (const Tri& t : tris)
                    for (const P3& p : t)
                    {
                        lo = std::fmin(lo, p[2]);
                        hi = std::fmax(hi, p[2]);
                    }
                return { lo, hi };
            }

            // 草图平面上 p 点、沿扫掠方向 t 处的点；t 为 0 时即 p 在族坐标里的方向
            P3 Embed(SketchPlane plane, P2 p, double t)
            {
                switch (plane)
                {
                case SketchPlane::Xy: return { p.x, p.y, t };
                case SketchPlane::Xz: return { p.x, t, p.y };
                case SketchPlane::Yz: return { t, p.x, p.y };
                }
                return { p.x, p.y, t };
            }

            // 封住轮廓 ring 的三角形，逆时针；轮廓自交时封不住
            bool Cap(const Ring& ring, std::vector<std::array<size_t, 3>>& out, std::string& error)
            {
                const auto pt = [&](size_t i) { return i < ring.size() ? ring[i] : P2{}; };
                out = Geom::Triangulate(ring);
                double covered = 0.0;
                for (const auto& [a, b, c] : out)
                    covered += (pt(b) - pt(a)).Cross(pt(c) - pt(a)) / 2.0;
                const double area = Geom::Area(ring);
                if (std::abs(covered - area) > 1e-6 * area + 1e-3)
                {
                    error = "its profile crosses itself";
                    return false;
                }
                return true;
            }

            // plane 上逆时针的 ring 从 from 扫到 to：先是两端，然后是侧面，每个三角形朝外；以及端面占了几个
            bool Sweep(const Ring& ring, double from, double to, SketchPlane plane, std::vector<Tri>& tris, size_t& caps, std::string& error)
            {
                const auto at = [&](P2 p, double t) { return Embed(plane, p, t); };
                const auto pt = [&](size_t i) { return i < ring.size() ? ring[i] : P2{}; };
                std::vector<std::array<size_t, 3>> capped;
                if (!Cap(ring, capped, error))
                    return false;
                tris.clear();
                tris.reserve(capped.size() * 2 + ring.size() * 2);
                for (const auto& [a, b, c] : capped)
                {
                    tris.push_back(Tri{ at(pt(a), to), at(pt(b), to), at(pt(c), to) });
                    tris.push_back(Tri{ at(pt(a), from), at(pt(c), from), at(pt(b), from) });
                }
                for (const auto& [a, b] : Geom::RingEdges(ring))
                {
                    const P3 a0 = at(a, from), b0 = at(b, from), a1 = at(a, to), b1 = at(b, to);
                    tris.push_back(Tri{ a0, b0, b1 });
                    tris.push_back(Tri{ a0, b1, a1 });
                }
                // 在 x、z 上扫时沿 y 走：坐标轴是其他两种的镜像，三角形要反过来才朝外
                if (plane == SketchPlane::Xz)
                    for (Tri& t : tris)
                        std::swap(t[1], t[2]);
                caps = capped.size() * 2;
                return true;
            }

            // plane 上逆时针的 ring 绕 axis（在 plane 上）从 from 转到 to 度：三角形（都朝外；整圈只有侧面，不到一圈的两端封上），
            // 以及从上往下看的轮廓
            bool Revolve(const Ring& ring, std::array<P2, 2> axis, double from, double to, SketchPlane plane, std::vector<Tri>& tris, Ring& outline,
                         std::string& error)
            {
                const P2 p0 = axis[0], p1 = axis[1];
                if (p0.Dist(p1) < 1e-6)
                {
                    error = "its axis starts and ends at one point: it needs a direction";
                    return false;
                }
                std::vector<std::array<size_t, 3>> caps;
                if (!Cap(ring, caps, error))
                    return false;
                // 每个角点沿轴的距离，以及离轴往左多远（在轴上为 0）
                const P2 along = (p1 - p0).Unit(), left = (p1 - p0).Unit().Perp();
                std::vector<std::pair<double, double>> reach;
                double least = 0.0, most = 0.0;
                for (const P2& c : ring)
                {
                    const double t = (c - p0).Dot(along);
                    double r = (c - p0).Dot(left);
                    if (std::abs(r) < 1e-6)
                        r = 0.0;
                    reach.emplace_back(t, r);
                    least = std::fmin(least, r);
                    most  = std::fmax(most, r);
                }
                if (least < 0.0 && most > 0.0)
                {
                    error = "its profile crosses its axis: draw it to one side of it";
                    return false;
                }
                if (least == 0.0 && most == 0.0)
                {
                    error = "its profile lies along its axis: it needs some width off it";
                    return false;
                }
                // 三维里：沿轴（u）、离轴（n），以及往左的角点转的方向（w）。往右的角点反着转，它的三角形也翻过来
                const P3 o = Embed(plane, p0, 0.0), u = Embed(plane, along, 0.0), n = Embed(plane, left, 0.0);
                const P3 w = Cross(u, n);
                const double turn = to - from;
                const bool full = turn > 360.0 - 1e-9;
                const size_t steps = static_cast<size_t>(std::max(1.0, std::ceil(turn / 360.0 * static_cast<double>(Segments) - 1e-9)));
                const auto angle = [&](size_t k)
                {
                    const double a = (from + turn * static_cast<double>(full ? k % steps : k) / static_cast<double>(steps)) * Fam::kPi / 180.0;
                    return std::pair{ std::sin(a), std::cos(a) };
                };
                const auto at = [&](size_t i, size_t k) -> P3
                {
                    const auto [t, r] = i < reach.size() ? reach[i] : std::pair{ 0.0, 0.0 };
                    const auto [s, c] = angle(k);
                    P3 p = Add(Add(o, Mul(u, t)), Mul(Add(Mul(n, c), Mul(w, s)), r));
                    for (double& x : p)
                        x = std::round(x * 1e9) / 1e9 + 0.0;
                    return p;
                };
                tris.clear();
                const auto push = [&](Tri t)
                {
                    // 轴上的角点转的时候不动，留下没有面积的侧面
                    if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2])
                        tris.push_back(least < 0.0 ? Tri{ t[0], t[2], t[1] } : t);
                };
                if (!full)
                    for (const auto& [a, b, c] : caps)
                    {
                        push(Tri{ at(a, steps), at(b, steps), at(c, steps) });
                        push(Tri{ at(a, 0), at(c, 0), at(b, 0) });
                    }
                for (size_t i = 0; i < ring.size(); ++i)
                {
                    const size_t j = (i + 1) % ring.size();
                    for (size_t k = 0; k < steps; ++k)
                    {
                        const P3 a0 = at(i, k), b0 = at(j, k), a1 = at(i, k + 1), b1 = at(j, k + 1);
                        push(Tri{ a0, b0, b1 });
                        push(Tri{ a0, b1, a1 });
                    }
                }
                // 从上看，绕竖直轴不到一圈的是从轮廓最宽处到最窄处的一段环；其他的取它的凸包（BIMCraft 的简化）
                if (std::abs(u[0]) < 1e-9 && std::abs(u[1]) < 1e-9 && !full)
                {
                    double inner = INFINITY;
                    for (const auto& [t, r] : reach)
                        inner = std::fmin(inner, std::abs(r));
                    // 在轴右边的轮廓沿 -n 向外
                    const double side = least < 0.0 ? -1.0 : 1.0;
                    const auto arc = [&](double r, size_t k)
                    {
                        const auto [s, c] = angle(k);
                        return P2(o[0] + side * r * (n[0] * c + w[0] * s), o[1] + side * r * (n[1] * c + w[1] * s));
                    };
                    outline.clear();
                    for (size_t k = 0; k <= steps; ++k)
                        outline.push_back(arc(std::max(most, -least), k));
                    if (inner > 0.0)
                        for (size_t k = steps + 1; k-- > 0;)
                            outline.push_back(arc(inner, k));
                    else
                        outline.emplace_back(o[0], o[1]);
                    if (Geom::Area(outline) < 0.0)
                        std::reverse(outline.begin(), outline.end());
                }
                else
                {
                    std::vector<P2> pts;
                    for (const Tri& t : tris)
                        for (const P3& p : t)
                            pts.emplace_back(p[0], p[1]);
                    outline = Geom::Hull(std::move(pts));
                }
                return true;
            }

            // 文件带来的三角形形体：从上看的轮廓取它们的凸包
            bool Mesh(const std::vector<Tri>& tris, const std::string& material, Piece& out, std::string& error)
            {
                if (tris.size() > MaxTriangles)
                {
                    error = "it has more than " + std::to_string(MaxTriangles) + " triangles";
                    return false;
                }
                for (const Tri& t : tris)
                    for (const P3& p : t)
                        for (const double x : p)
                            if (!std::isfinite(x) || std::abs(x) > MaxCoord)
                            {
                                error = "a corner of it isn't a usable number";
                                return false;
                            }
                const auto [bottom, top] = Heights(tris);
                std::vector<P2> pts;
                for (const Tri& t : tris)
                    for (const P3& p : t)
                        pts.emplace_back(p[0], p[1]);
                out = Piece{ tris, tris.size(), Geom::Hull(std::move(pts)), bottom, top, material, false };
                return true;
            }
        }

        // ── 名称 ──────────────────────────────────────────────────
        namespace CategoryOf
        {
            namespace
            {
                constexpr std::pair<Category, const char*> kNames[] = {
                    { Category::Furniture, "furniture" },     { Category::Casework, "casework" },
                    { Category::Plumbing, "plumbing" },       { Category::Drain, "drain" },
                    { Category::PipeFitting, "pipefitting" }, { Category::PipeAccessory, "pipeaccessory" },
                    { Category::Equipment, "equipment" },     { Category::Outlet, "outlet" },
                    { Category::Switch, "switch" },           { Category::AirTerminal, "airterminal" },
                    { Category::Earthing, "earthing" },       { Category::InspectionBox, "inspectionbox" },
                    { Category::Lighting, "lighting" },       { Category::Generic, "generic" },
                    { Category::DataOutlet, "dataoutlet" },   { Category::VoiceOutlet, "voiceoutlet" },
                    { Category::PatchPanel, "patchpanel" },   { Category::Rack, "rack" },
                    { Category::Door, "door" },               { Category::Window, "window" },
                };
            }

            const char* Name(Category c)
            {
                for (const auto& [k, n] : kNames)
                    if (k == c)
                        return n;
                return "generic";
            }

            std::optional<Category> FromName(std::string_view name)
            {
                for (const auto& [k, n] : kNames)
                    if (name == n)
                        return k;
                return std::nullopt;
            }
        }

        namespace KindOf
        {
            const char* Name(Kind k)
            {
                switch (k)
                {
                case Kind::Length: return "length";
                case Kind::Number: return "number";
                case Kind::Angle: return "angle";
                case Kind::YesNo: return "yesno";
                case Kind::Material: return "material";
                }
                return "length";
            }

            std::optional<Kind> FromName(std::string_view name)
            {
                for (const Kind k : { Kind::Length, Kind::Number, Kind::Angle, Kind::YesNo, Kind::Material })
                    if (name == Name(k))
                        return k;
                return std::nullopt;
            }
        }

        bool Form::operator==(const Form& o) const
        {
            const bool sameMesh = mesh == o.mesh || (!HasMesh() && !o.HasMesh()) || (HasMesh() && o.HasMesh() && *mesh == *o.mesh);
            return id == o.id && guid == o.guid && plane == o.plane && profile == o.profile && circle == o.circle && from == o.from && to == o.to &&
                   axis == o.axis && isVoid == o.isVoid && material == o.material && visible == o.visible && sameMesh;
        }

        // ── 公式（对外）────────────────────────────────────────────
        bool Names(std::string_view formula, std::vector<std::string>& out, std::string& error)
        {
            Node node;
            if (!Parse(formula, node, error))
                return false;
            out.clear();
            std::string why;
            if (!Walk(node, out, why))
            {
                error = "formula `" + std::string(formula) + "`: " + why;
                return false;
            }
            return true;
        }

        bool Eval(std::string_view formula, const std::function<bool(const std::string&, double&, std::string&)>& get, double& out,
                  std::string& error)
        {
            Node node;
            return Parse(formula, node, error) && Worked(formula, node, get, out, error);
        }

        bool Nameable(std::string_view name)
        {
            const std::u32string s = Decode(name);
            if (s.empty() || !(IsAlphabetic(s.front()) || s.front() == U'_'))
                return false;
            for (const char32_t c : s)
                if (!(IsAlphanumeric(c) || c == U'_' || c == U' '))
                    return false;
            return s.back() != U' ' && s.size() <= MaxText;
        }

        bool Read(const Parameter& p, const Json& v, Val& out, std::string& error)
        {
            const auto bad = [&](const char* what)
            {
                error = "parameter `" + p.name + "` holds " + what + ", not " + v.dump();
                return false;
            };
            switch (p.kind)
            {
            case Kind::Material:
                if (v.is_string())
                    out = v.get<std::string>();
                else if (v.is_null())
                    out = std::string();
                else
                    return bad("a material's name");
                return true;
            case Kind::YesNo:
                if (v.is_boolean())
                    out = Truth(v.get<bool>());
                else if (v.is_null())
                    out = 0.0;
                else if (v.is_number() && std::isfinite(v.get<double>()))
                    out = Truth(v.get<double>() != 0.0);
                else
                    return bad("yes or no");
                return true;
            case Kind::Length:
            case Kind::Number:
            case Kind::Angle:
                if (v.is_null())
                    out = 0.0;
                else if (v.is_number() && std::isfinite(v.get<double>()))
                    out = v.get<double>();
                else
                    return bad("a number");
                return true;
            }
            return bad("a number");
        }
    }

    // ── Family ───────────────────────────────────────────────────
    namespace Fam
    {
    namespace
    {

        std::vector<const Expr*> FormExprs(const Form& f)
        {
            std::vector<const Expr*> out;
            for (const auto& p : f.profile)
                for (const Expr& e : p)
                    out.push_back(&e);
            out.push_back(&f.from);
            out.push_back(&f.to);
            if (f.visible)
                out.push_back(&*f.visible);
            if (f.circle)
            {
                out.push_back(&f.circle->centre[0]);
                out.push_back(&f.circle->centre[1]);
                out.push_back(&f.circle->radius);
            }
            if (f.axis)
                for (const auto& c : *f.axis)
                    for (const Expr& e : c)
                        out.push_back(&e);
            return out;
        }

        bool LongFormula(const Expr& e)
        {
            const std::string* f = std::get_if<std::string>(&e);
            return f && Text::CharCount(*f) > MaxFormula;
        }

        // 参数 name 的值，以及它取值的那些，放进 done；path 是正在算的公式，查出绕回自己的
        bool Resolve(const Family& fam, const std::string& name, const std::map<std::string, Json>& set, const std::map<std::string, Json>& instance,
                     Values& done, std::vector<std::string>& path, Val& out, std::string& error)
        {
            if (const auto it = done.find(name); it != done.end())
            {
                out = it->second;
                return true;
            }
            const auto p = std::find_if(fam.parameters.begin(), fam.parameters.end(), [&](const Parameter& q) { return q.name == name; });
            if (p == fam.parameters.end())
            {
                error = "`" + name + "` isn't one of its parameters";
                return false;
            }
            if (std::find(path.begin(), path.end(), name) != path.end())
            {
                error = "`" + name + "`'s formula comes back to itself";
                return false;
            }
            Val v;
            if (p->formula)
            {
                if (path.size() >= MaxChain)
                {
                    error = "`" + name + "` takes from formulas taking from more than " + std::to_string(MaxChain) + " others in a row: nested too deep";
                    return false;
                }
                path.push_back(name);
                double x = 0.0;
                std::string why;
                const bool ok = Eval(*p->formula,
                                     [&](const std::string& n, double& value, std::string& e) -> bool
                                     {
                                         Val got;
                                         if (!Resolve(fam, n, set, instance, done, path, got, e))
                                             return false;
                                         if (const double* d = std::get_if<double>(&got))
                                         {
                                             value = *d;
                                             return true;
                                         }
                                         e = "`" + n + "` is a material, not a number";
                                         return false;
                                     },
                                     x, why);
                path.pop_back();
                if (!ok)
                {
                    error = "parameter `" + name + "`: " + why;
                    return false;
                }
                v = x;
            }
            else
            {
                const auto& from = p->instance ? instance : set;
                const auto it = from.find(name);
                if (!Read(*p, it != from.end() ? it->second : p->value, v, error))
                    return false;
            }
            done[name] = v;
            out = v;
            return true;
        }

        // 可见形体按 values 定尺寸的实体，减去空心形体切掉的；read 是已读过的公式，cuts 是切割还能花多少
        bool Build(const Family& fam, const Values& values, ReadCache& read, Csg::Meter& cuts, std::vector<Piece>& out, std::string& error)
        {
            out.clear();
            struct Hole
            {
                size_t           form;
                size_t           before;
                std::vector<Tri> tris;
            };
            std::vector<Hole> voids;
            for (size_t i = 0; i < fam.forms.size(); ++i)
            {
                const Form& form = fam.forms[i];
                const std::string what = "form " + std::to_string(i + 1);
                std::string why;
                const auto said = [&]
                {
                    error = what + ": " + why;
                    return false;
                };
                double x = 0.0;
                if (form.visible)
                {
                    if (!Number(*form.visible, values, read, x, why))
                        return said();
                    if (x == 0.0)
                        continue;
                }
                std::string material = form.material;
                if (const auto it = values.find(form.material); it != values.end())
                    if (const std::string* m = std::get_if<std::string>(&it->second))
                        material = *m;
                Piece piece;
                if (form.HasMesh())
                {
                    if (!Mesh(*form.mesh, material, piece, why))
                        return said();
                }
                else
                {
                    Ring ring;
                    if (form.circle)
                    {
                        double cx = 0.0, cy = 0.0, r = 0.0;
                        if (!Number(form.circle->centre[0], values, read, cx, why) || !Number(form.circle->centre[1], values, read, cy, why) ||
                            !Number(form.circle->radius, values, read, r, why))
                            return said();
                        if (r <= 0.0)
                        {
                            why = "its circle's radius is " + Num(r) + "; it needs one above zero";
                            return said();
                        }
                        for (size_t k = 0; k < Segments; ++k)
                        {
                            const double a = static_cast<double>(k) * kTau / static_cast<double>(Segments);
                            ring.emplace_back(cx + r * std::cos(a), cy + r * std::sin(a));
                        }
                    }
                    else if (form.profile.size() > MaxProfile)
                    {
                        why = "its profile has more than " + std::to_string(MaxProfile) + " corners";
                        return said();
                    }
                    else
                        for (const auto& [a, b] : form.profile)
                        {
                            double px = 0.0, py = 0.0;
                            if (!Number(a, values, read, px, why) || !Number(b, values, read, py, why))
                                return said();
                            ring.emplace_back(px, py);
                        }
                    double from = 0.0, to = 0.0;
                    if (!Number(form.from, values, read, from, why) || !Number(form.to, values, read, to, why))
                        return said();
                    if (ring.size() < 3 || std::abs(Geom::Area(ring)) < 1.0)
                    {
                        why = "its profile has no area: it needs three corners or more, or a circle";
                        return said();
                    }
                    if (to <= from)
                    {
                        error = what + " sweeps nothing: from " + Num(from) + " to " + Num(to) + (form.axis ? " degrees" : "");
                        return false;
                    }
                    if (Geom::Area(ring) < 0.0)
                        std::reverse(ring.begin(), ring.end());
                    if (form.axis)
                    {
                        if (to - from > 360.0)
                        {
                            error = what + " turns more than all the way round: from " + Num(from) + " to " + Num(to) + " degrees, 360 at most";
                            return false;
                        }
                        std::array<P2, 2> axis;
                        for (size_t k = 0; k < 2; ++k)
                        {
                            double ax = 0.0, ay = 0.0;
                            if (!Number((*form.axis)[k][0], values, read, ax, why) || !Number((*form.axis)[k][1], values, read, ay, why))
                                return said();
                            axis[k] = P2(ax, ay);
                        }
                        std::vector<Tri> tris;
                        Ring outline;
                        if (!Revolve(ring, axis, from, to, form.plane, tris, outline, why))
                            return said();
                        const auto [bottom, top] = Heights(tris);
                        piece = Piece{ tris, tris.size(), std::move(outline), bottom, top, material, false };
                    }
                    else
                    {
                        std::vector<Tri> tris;
                        size_t caps = 0;
                        if (!Sweep(ring, from, to, form.plane, tris, caps, why))
                            return said();
                        double a0 = INFINITY, a1 = -INFINITY, b0 = INFINITY, b1 = -INFINITY;
                        for (const P2& p : ring)
                        {
                            a0 = std::fmin(a0, p.x);
                            a1 = std::fmax(a1, p.x);
                            b0 = std::fmin(b0, p.y);
                            b1 = std::fmax(b1, p.y);
                        }
                        Ring outline;
                        double bottom = from, top = to;
                        switch (form.plane)
                        {
                        case SketchPlane::Xy: outline = ring; break;
                        case SketchPlane::Xz:
                            outline = { P2(a0, from), P2(a1, from), P2(a1, to), P2(a0, to) };
                            bottom = b0;
                            top = b1;
                            break;
                        case SketchPlane::Yz:
                            outline = { P2(from, a0), P2(to, a0), P2(to, a1), P2(from, a1) };
                            bottom = b0;
                            top = b1;
                            break;
                        }
                        piece = Piece{ std::move(tris), caps, std::move(outline), bottom, top, material, form.circle.has_value() };
                    }
                }
                if (form.isVoid)
                    voids.push_back(Hole{ i, out.size(), std::move(piece.tris) });
                else
                    out.push_back(std::move(piece));
            }
            // 每个空心形体切掉列在它前面、它碰到的实体。切过的实体不再是一个圆的曲面，从上看的轮廓照旧
            //（BIMCraft 的简化：凹口和洞在平面图上不显示）
            for (const Hole& h : voids)
                for (size_t k = 0; k < h.before && k < out.size(); ++k)
                {
                    Piece& piece = out[k];
                    std::vector<Tri> tris;
                    std::string why;
                    if (!Csg::Subtract(piece.tris, h.tris, cuts, tris, why))
                    {
                        error = "form " + std::to_string(h.form + 1) + ": " + why;
                        return false;
                    }
                    if (tris != piece.tris)
                    {
                        const auto [bottom, top] = Heights(tris);
                        piece.caps   = tris.size();
                        piece.tris   = std::move(tris);
                        piece.bottom = bottom;
                        piece.top    = top;
                        piece.round  = false;
                    }
                }
            out.erase(std::remove_if(out.begin(), out.end(), [](const Piece& p) { return p.tris.empty(); }), out.end());
            return true;
        }
    }
    }

    namespace CategoryOf = Fam::CategoryOf;
    using Fam::Form;
    using Fam::Parameter;
    using Fam::Piece;
    using Fam::Symbol;
    using Fam::Val;

    void Family::MaterialRefsMut(const std::function<void(std::string&)>& f)
    {
        std::set<std::string> params;
        for (const Parameter& p : parameters)
            if (p.kind == Fam::Kind::Material)
                params.insert(p.name);
        for (Parameter& p : parameters)
            if (p.kind == Fam::Kind::Material && p.value.is_string())
            {
                std::string m = p.value.get<std::string>();
                f(m);
                p.value = m;
            }
        for (auto& [_, set] : types)
            for (auto& [k, v] : set)
                if (params.count(k) && v.is_string())
                {
                    std::string m = v.get<std::string>();
                    f(m);
                    v = m;
                }
        for (Form& form : forms)
            if (!form.material.empty() && !params.count(form.material))
                f(form.material);
    }

    bool Family::MaterialParameter(std::string_view name) const
    {
        return std::any_of(parameters.begin(), parameters.end(), [&](const Parameter& p) { return p.name == name && p.kind == Fam::Kind::Material; });
    }

    std::vector<std::string> Family::Materials() const
    {
        std::vector<std::string> out;
        for (const Parameter& p : parameters)
        {
            if (p.kind != Fam::Kind::Material)
                continue;
            if (p.value.is_string())
                out.push_back(p.value.get<std::string>());
            for (const auto& [_, set] : types)
                if (const auto it = set.find(p.name); it != set.end() && it->second.is_string())
                    out.push_back(it->second.get<std::string>());
        }
        for (const Form& f : forms)
            if (!MaterialParameter(f.material))
                out.push_back(f.material);
        out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& m) { return m.empty(); }), out.end());
        return out;
    }

    void Family::RenameMaterial(const std::string& from, const std::string& to)
    {
        std::vector<std::string> params;
        for (const Parameter& p : parameters)
            if (p.kind == Fam::Kind::Material)
                params.push_back(p.name);
        const auto swap = [&](Json& v)
        {
            if (v.is_string() && v.get<std::string>() == from)
                v = to;
        };
        for (Parameter& p : parameters)
            if (p.kind == Fam::Kind::Material)
                swap(p.value);
        for (auto& [_, set] : types)
            for (auto& [k, v] : set)
                if (std::find(params.begin(), params.end(), k) != params.end())
                    swap(v);
        for (Form& f : forms)
            if (f.material == from && std::find(params.begin(), params.end(), f.material) == params.end())
                f.material = to;
    }

    bool Family::Values(const std::string& typeName, const std::map<std::string, Json>& instance, Fam::Values& out, std::string& error) const
    {
        return ValuesIn(typeName, instance, Fam::DefaultWall, out, error);
    }

    bool Family::ValuesIn(const std::string& typeName, const std::map<std::string, Json>& instance, double wall, Fam::Values& out,
                          std::string& error) const
    {
        const auto set = types.find(typeName);
        if (set == types.end())
        {
            std::string names;
            for (const auto& [n, _] : types)
                names += (names.empty() ? "" : ", ") + n;
            error = "type `" + typeName + "` doesn't exist (there are: " + names + ")";
            return false;
        }
        Fam::Values done;
        if (CategoryOf::Hosted(category))
            done[Fam::Wall] = wall;
        for (const Parameter& p : parameters)
        {
            std::vector<std::string> path;
            Val v;
            if (!Fam::Resolve(*this, p.name, set->second, instance, done, path, v, error))
                return false;
        }
        out = std::move(done);
        return true;
    }

    bool Family::Check(const std::function<bool(const std::string&)>& hasMaterial, std::string& error) const
    {
        for (const auto& [what, n, most] : { std::tuple{ "parameters", parameters.size(), Fam::MaxParameters }, std::tuple{ "forms", forms.size(), Fam::MaxForms },
                                             std::tuple{ "symbols", symbols.size(), Fam::MaxSymbols }, std::tuple{ "types", types.size(), Fam::MaxTypes } })
            if (n > most)
            {
                error = "it has more than " + std::to_string(most) + " " + what;
                return false;
            }
        if (types.empty())
        {
            error = "it needs a type: add one to `types`";
            return false;
        }
        if (CategoryOf::Hosted(category))
        {
            for (const char* name : { Fam::Width, Fam::Height })
                if (!std::any_of(parameters.begin(), parameters.end(), [&](const Parameter& p) { return p.name == name && p.kind == Fam::Kind::Length; }))
                {
                    error = std::string("a door or window family needs a length parameter `") + name + "`: the size of the opening it makes";
                    return false;
                }
            if (std::any_of(parameters.begin(), parameters.end(), [](const Parameter& p) { return p.name == Fam::Wall; }))
            {
                error = std::string("`") + Fam::Wall + "` is the wall's, which a door or a window reads; name the parameter otherwise";
                return false;
            }
        }
        else if (!symbols.empty())
        {
            error = "symbols are the lines a door's or a window's plan draws; this family isn't one";
            return false;
        }
        const auto materialOk = [&](const std::string& who, const std::string& m)
        {
            if (m.empty() || hasMaterial(m))
                return true;
            error = who + ": material `" + m + "` isn't in the library; add it with `material`";
            return false;
        };
        std::set<std::string> names;
        for (const Parameter& p : parameters)
        {
            const std::string who = "parameter `" + p.name + "`";
            if (!Fam::Nameable(p.name))
            {
                error = who + ": a name starts with a letter and holds letters, digits, `_` and spaces, up to " + std::to_string(MaxText);
                return false;
            }
            if (!names.insert(p.name).second)
            {
                error = who + " is named twice";
                return false;
            }
            if (p.formula && p.kind == Fam::Kind::Material)
            {
                error = who + ": a material takes no formula";
                return false;
            }
            if (p.formula && Text::CharCount(*p.formula) > Fam::MaxFormula)
            {
                error = who + ": its formula is longer than " + std::to_string(Fam::MaxFormula) + " characters";
                return false;
            }
            Val v;
            if (!Fam::Read(p, p.value, v, error))
                return false;
            if (const std::string* m = std::get_if<std::string>(&v); m && !materialOk(who, *m))
                return false;
        }
        for (const auto& [t, set] : types)
        {
            const std::string who = "type `" + t + "`";
            if (Text::Trim(t).empty() || Text::CharCount(t) > MaxText)
            {
                error = who + ": a type needs a name, up to " + std::to_string(MaxText) + " characters";
                return false;
            }
            for (const auto& [k, v] : set)
            {
                const auto p = std::find_if(parameters.begin(), parameters.end(), [&](const Parameter& q) { return q.name == k; });
                if (p == parameters.end())
                {
                    error = who + ": `" + k + "` isn't one of its parameters";
                    return false;
                }
                if (p->instance)
                {
                    error = who + ": `" + k + "` is an instance parameter; each component sets its own";
                    return false;
                }
                if (p->formula)
                {
                    error = who + ": `" + k + "` is worked out by its formula";
                    return false;
                }
                Val val;
                if (!Fam::Read(*p, v, val, error))
                    return false;
                if (const std::string* m = std::get_if<std::string>(&val); m && !materialOk(who, *m))
                    return false;
            }
        }
        for (size_t i = 0; i < forms.size(); ++i)
        {
            const Form& form = forms[i];
            const std::string who = "form " + std::to_string(i + 1);
            const int ways = (form.profile.empty() ? 0 : 1) + (form.circle ? 1 : 0) + (form.HasMesh() ? 1 : 0);
            if (ways != 1)
            {
                error = who + ": it takes a `profile`, a `circle` or a `mesh`, one of them";
                return false;
            }
            if (form.axis && form.HasMesh())
            {
                error = who + ": a `mesh` is as it is; an `axis` turns a `profile` or a `circle`";
                return false;
            }
            if (!std::any_of(parameters.begin(), parameters.end(), [&](const Parameter& p) { return p.name == form.material && p.kind == Fam::Kind::Material; }) &&
                !materialOk(who, form.material))
                return false;
            for (const Fam::Expr* e : Fam::FormExprs(form))
                if (Fam::LongFormula(*e))
                {
                    error = who + ": a formula of it is longer than " + std::to_string(Fam::MaxFormula) + " characters";
                    return false;
                }
        }
        for (const Symbol& y : symbols)
        {
            std::vector<const Fam::Expr*> exprs;
            for (const auto& p : y.line)
                for (const Fam::Expr& e : p)
                    exprs.push_back(&e);
            if (y.visible)
                exprs.push_back(&*y.visible);
            if (y.arc)
                for (const Fam::Expr* e : std::initializer_list<const Fam::Expr*>{ &y.arc->centre[0], &y.arc->centre[1], &y.arc->radius, &y.arc->from, &y.arc->to })
                    exprs.push_back(e);
            for (const Fam::Expr* e : exprs)
                if (Fam::LongFormula(*e))
                {
                    error = "a formula of a symbol is longer than " + std::to_string(Fam::MaxFormula) + " characters";
                    return false;
                }
        }
        // 各类型共用读过的公式；超过族能建的就拒绝
        Fam::ReadCache read;
        Csg::Meter cuts;
        size_t built = 0;
        for (const auto& [t, _] : types)
        {
            Fam::Values v;
            std::vector<Ring> strokes;
            std::vector<Piece> pieces;
            std::string why;
            if (!Values(t, {}, v, why) || !Strokes(v, strokes, why) || !Fam::Build(*this, v, read, cuts, pieces, why))
            {
                error = "type `" + t + "`: " + why;
                return false;
            }
            for (const Piece& p : pieces)
                built += p.tris.size();
            if (built > Fam::MaxBuilt)
            {
                error = "its types build more than " + std::to_string(Fam::MaxBuilt) + " triangles together";
                return false;
            }
        }
        return true;
    }

    bool Family::Pieces(const Fam::Values& values, std::vector<Fam::Piece>& out, std::string& error) const
    {
        Fam::ReadCache read;
        Csg::Meter cuts;
        return Fam::Build(*this, values, read, cuts, out, error);
    }

    bool Family::Strokes(const Fam::Values& values, std::vector<Ring>& out, std::string& error) const
    {
        Fam::ReadCache read;
        out.clear();
        for (size_t i = 0; i < symbols.size(); ++i)
        {
            const Symbol& sym = symbols[i];
            std::string why;
            const auto said = [&]
            {
                error = "symbol " + std::to_string(i + 1) + ": " + why;
                return false;
            };
            if (sym.visible)
            {
                double x = 0.0;
                if (!Fam::Number(*sym.visible, values, read, x, why))
                    return said();
                if (x == 0.0)
                    continue;
            }
            const int ways = (sym.line.empty() ? 0 : 1) + (sym.arc ? 1 : 0);
            if (ways != 1 || sym.line.size() > Fam::MaxProfile)
            {
                why = "it takes a `line` of up to " + std::to_string(Fam::MaxProfile) + " points, or an `arc`, one of them";
                return said();
            }
            Ring pts;
            for (const auto& [a, b] : sym.line)
            {
                double x = 0.0, y = 0.0;
                if (!Fam::Number(a, values, read, x, why) || !Fam::Number(b, values, read, y, why))
                    return said();
                pts.emplace_back(x, y);
            }
            if (sym.arc)
            {
                double cx = 0.0, cy = 0.0, r = 0.0, from = 0.0, to = 0.0;
                if (!Fam::Number(sym.arc->centre[0], values, read, cx, why) || !Fam::Number(sym.arc->centre[1], values, read, cy, why) ||
                    !Fam::Number(sym.arc->radius, values, read, r, why) || !Fam::Number(sym.arc->from, values, read, from, why) ||
                    !Fam::Number(sym.arc->to, values, read, to, why))
                    return said();
                if (r <= 0.0 || std::abs(to - from) > 360.0 || to == from)
                {
                    why = "its arc has radius " + Fam::Num(r) + " and runs from " + Fam::Num(from) + " to " + Fam::Num(to) +
                          " degrees; it needs a radius above zero and up to a full turn";
                    return said();
                }
                const size_t steps = static_cast<size_t>(std::max(1.0, std::ceil(std::abs(to - from) / Fam::ArcStep)));
                for (size_t k = 0; k <= steps; ++k)
                {
                    const double a = (from + (to - from) * static_cast<double>(k) / static_cast<double>(steps)) * Fam::kPi / 180.0;
                    pts.emplace_back(cx + r * std::cos(a), cy + r * std::sin(a));
                }
            }
            out.push_back(std::move(pts));
        }
        return true;
    }
}
