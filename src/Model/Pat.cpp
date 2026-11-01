// 移植自 BIMCraft crates/core/src/pat.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Pat.h"
#include "Model/Model.h"
#include "Model/Text.h"
#include <cmath>
#include <optional>

namespace MiniBIM::Bim::Pat
{
    namespace
    {
        // 报错引用坏行的字数
        constexpr size_t Echo = 40;

        // line 的前 Echo 个字，截掉时接省略号
        std::string EchoOf(std::string_view line)
        {
            const std::u32string all = Text::Utf32(line);
            std::string out;
            for (size_t i = 0; i < all.size() && i < Echo; ++i)
                Text::AppendUtf8(out, all[i]);
            if (all.size() > Echo)
                out += "\xE2\x80\xA6";
            return out;
        }

        // 留下一个图案：计数，列表没满时写上
        void Leave(Read& out, std::string name, std::string why)
        {
            ++out.leftOutCount;
            if (out.leftOut.size() < MaxLeftOut)
                out.leftOut.emplace_back(std::move(name), std::move(why));
        }

        double Rem360(double a)
        {
            const double r = std::fmod(a, 360.0);
            return r < 0.0 ? r + 360.0 : r;
        }

        // 正在读的图案：名称、图案、哪里不对
        struct Current
        {
            std::string                name;
            FillPattern                pattern;
            std::optional<std::string> wrong;
        };

        void Finish(std::optional<Current>& cur, Read& out)
        {
            if (!cur)
                return;
            std::optional<std::string> why = cur->wrong;
            if (!why && cur->pattern.lines.empty())
                why = "it has no lines";
            std::string error;
            if (!why && !cur->pattern.Check(cur->name, error))
                why = error;
            if (why)
                Leave(out, cur->name, *why);
            else if (std::any_of(out.patterns.begin(), out.patterns.end(), [&](const auto& p) { return p.first == cur->name; }))
                Leave(out, cur->name, "the file has it twice");
            else
                out.patterns.emplace_back(cur->name, std::move(cur->pattern));
            cur.reset();
        }

        // 数：最多六位小数，没有 -0
        std::string Num(double x)
        {
            const double r = std::round(x * 1e6) / 1e6;
            return Text::Shortest(r + 0.0);
        }
    }

    bool ReadText(std::string_view text, Read& out, std::string& error)
    {
        out = Read{};
        if (text.size() > MaxText)
        {
            error = "the file is " + std::to_string(text.size()) + " bytes, more than " + std::to_string(MaxText);
            return false;
        }
        double scale = 1.0;
        // 见过的图案，好坏都算；过了 MaxPatterns 的只数（unread），不读
        size_t met = 0, unread = 0;
        std::optional<Current> current;
        size_t from = 0;
        while (from <= text.size())
        {
            size_t end = text.find('\n', from);
            if (end == std::string_view::npos)
                end = text.size();
            std::string_view line = Text::Trim(text.substr(from, end - from));
            const bool last = end == text.size();
            from = end + 1;
            // 开头的 BOM
            while (Text::StartsWith(line, "\xEF\xBB\xBF"))
                line.remove_prefix(3);
            if (unread > 0)
            {
                unread += Text::StartsWith(line, "*") ? 1 : 0;
                if (last)
                    break;
                continue;
            }
            if (Text::StartsWith(line, ";%"))
            {
                const std::string_view setting = line.substr(2);
                const size_t eq = setting.find('=');
                const std::string key   = Text::AsciiUpper(Text::Trim(setting.substr(0, eq)));
                const std::string value = eq == std::string_view::npos ? std::string() : Text::AsciiUpper(Text::Trim(setting.substr(eq + 1)));
                if (key == "UNITS")
                    scale = value == "INCH" || value == "INCHES" ? 25.4 : 1.0;
                else if (key == "TYPE" && current)
                    current->pattern.model = value == "MODEL";
            }
            else if (line.empty() || Text::StartsWith(line, ";"))
            {
            }
            else if (Text::StartsWith(line, "*"))
            {
                Finish(current, out);
                if (met >= MaxPatterns)
                    unread = 1;
                else
                {
                    ++met;
                    const std::string_view header = line.substr(1);
                    const std::string_view head = Text::Trim(header.substr(0, header.find(',')));
                    // 名称里的控制字符去掉，最多 MaxText 个字
                    std::string name;
                    size_t count = 0;
                    for (const char32_t ch : Text::Utf32(head))
                    {
                        if (ch < 0x20 || (ch >= 0x7F && ch < 0xA0))
                            continue;
                        if (count++ >= Bim::MaxText)
                            break;
                        Text::AppendUtf8(name, ch);
                    }
                    current = Current{ name.empty() ? std::string("Unnamed") : name, {}, std::nullopt };
                }
            }
            // 一族线的一行：几个数，后面的注释去掉
            else if (current && !current->wrong)
            {
                const std::string_view body = line.substr(0, line.find(';'));
                // 五个数和一族线的虚实段，再多一段看是不是太多：更长的行在那里截断，不读
                std::vector<std::string_view> fields;
                size_t at = 0;
                while (fields.size() < 5 + Fills::MaxDashes + 1)
                {
                    const size_t comma = body.find(',', at);
                    fields.push_back(body.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at));
                    if (comma == std::string_view::npos)
                        break;
                    at = comma + 1;
                }
                FillPattern& pattern = current->pattern;
                if (fields.size() > 5 + Fills::MaxDashes)
                    current->wrong = "a family has more than " + std::to_string(Fills::MaxDashes) + " dashes";
                else
                {
                    std::vector<double> n;
                    bool ok = fields.size() >= 5;
                    for (const std::string_view f : fields)
                    {
                        const auto x = Text::ParseNumber(f);
                        if (!x || !std::isfinite(*x))
                        {
                            ok = false;
                            break;
                        }
                        n.push_back(*x);
                    }
                    if (!ok)
                        current->wrong = "`" + EchoOf(Text::Trim(body)) + "` isn't a line of numbers: angle, x, y, shift, offset and dashes";
                    else if (pattern.lines.size() >= Fills::MaxFamilies)
                        current->wrong = "more than " + std::to_string(Fills::MaxFamilies) + " line families";
                    else
                    {
                        // 沿这族线自己的方向的下一条：沿着错开 shift、横过去 offset；反方向横过去的，同样的线倒着走
                        const double shift  = n[4] < 0.0 ? -n[3] : n[3];
                        const double offset = n[4] < 0.0 ? -n[4] : n[4];
                        if (offset == 0.0)
                            current->wrong = "a family's lines lie on each other: its offset is 0";
                        // 全是虚的什么都不画（FillPattern::Check 也拒绝，这里说清楚）
                        else if (n.size() > 5 && std::all_of(n.begin() + 5, n.end(), [](double d) { return d < 0.0; }))
                            current->wrong = "a family's dashes are all spaces: it draws nothing";
                        else
                        {
                            HatchLine h;
                            h.angle   = Rem360(n[0]);
                            h.origin  = P2(n[1] * scale, n[2] * scale);
                            h.shift   = shift * scale;
                            h.spacing = offset * scale;
                            for (size_t i = 5; i < n.size(); ++i)
                                h.dashes.push_back(n[i] * scale);
                            pattern.lines.push_back(std::move(h));
                        }
                    }
                }
            }
            if (last)
                break;
        }
        Finish(current, out);
        if (unread > 0)
        {
            out.leftOutCount += unread;
            out.leftOut.emplace_back(std::to_string(unread) + " more",
                                     "the file holds more than " + std::to_string(MaxPatterns) + " patterns; the rest weren't read");
        }
        return true;
    }

    std::string Decode(std::string_view bytes)
    {
        const std::u32string all = Text::Utf32(bytes);
        if (std::find(all.begin(), all.end(), U'�') == all.end() || Text::Lossy(bytes) == bytes)
            return std::string(bytes);
        std::string out;
        for (const char b : bytes)
            Text::AppendUtf8(out, static_cast<unsigned char>(b));
        return out;
    }

    std::string Write(const std::vector<std::pair<std::string, const FillPattern*>>& patterns)
    {
        std::string out = ";%UNITS=MM\n;%VERSION=3.0\n;Fill patterns from MiniBIM\n";
        for (const auto& [name, p] : patterns)
        {
            if (p->IsSolid())
                continue;
            // 名称占一行，逗号会结束它
            std::string clean;
            for (const char32_t ch : Text::Utf32(name))
                Text::AppendUtf8(clean, ch == U',' || ch < 0x20 || (ch >= 0x7F && ch < 0xA0) ? U' ' : ch);
            out += "*" + clean + "\n";
            out += p->model ? ";%TYPE=MODEL\n" : ";%TYPE=DRAFTING\n";
            for (const HatchLine& l : p->lines)
            {
                std::string row = Num(l.angle) + "," + Num(l.origin.x) + "," + Num(l.origin.y) + "," + Num(l.shift) + "," + Num(l.spacing);
                for (const double d : l.dashes)
                    row += "," + Num(d);
                out += row + "\n";
            }
        }
        return out;
    }
}
