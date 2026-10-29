// 移植自 BIMCraft crates/core/src/units.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Units.h"
#include "Model/Text.h"
#include "I18n/Translator.h"
#include <cmath>
#include <cstdio>
#include <vector>

namespace MiniBIM::Bim::UnitsOf
{
    namespace
    {
        constexpr double kInch = 25.4;
        constexpr double kFoot = 304.8;

        // 公制项目可选的比例 1 : n
        constexpr double kMetricScales[] = { 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0 };
        // 英制项目可选的比例：12" 到 1/32" 对 1 英尺，然后 1" 对 10' 到 1" 对 200'
        constexpr double kImperialScales[] = { 1.0,   2.0,   4.0,   8.0,   12.0,  16.0,  24.0,  32.0,   48.0,   64.0,  96.0,
                                               128.0, 192.0, 256.0, 384.0, 120.0, 240.0, 360.0, 480.0, 600.0, 1200.0, 2400.0 };

        std::string Fixed2(double x)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.2f", x);
            return buf;
        }

        // n 个 1/64 英寸，按图纸的写法：1/8、1 1/2、12
        std::string Fraction(unsigned n)
        {
            unsigned whole = n / 64, num = n % 64, den = 64;
            while (num > 0 && num % 2 == 0)
            {
                num /= 2;
                den /= 2;
            }
            if (num == 0)
                return std::to_string(whole);
            if (whole == 0)
                return std::to_string(num) + "/" + std::to_string(den);
            return std::to_string(whole) + " " + std::to_string(num) + "/" + std::to_string(den);
        }

        std::optional<double> Number(std::string_view s)
        {
            const std::optional<double> x = Text::ParseNumber(s);
            if (x && std::isfinite(*x))
                return x;
            return std::nullopt;
        }

        // 输入的英寸：6、6.5、6 1/2 或 1/2
        std::optional<double> Inches(std::string_view s)
        {
            auto fraction = [](std::string_view f) -> std::optional<double>
            {
                const size_t slash = f.find('/');
                if (slash == std::string_view::npos)
                    return std::nullopt;
                const auto a = Number(f.substr(0, slash)), b = Number(f.substr(slash + 1));
                if (!a || !b || *b == 0.0)
                    return std::nullopt;
                return *a / *b;
            };
            const std::vector<std::string_view> words = Text::SplitWhitespace(s);
            if (words.size() == 2)
            {
                const auto whole = Number(words[0]), part = fraction(words[1]);
                if (!whole || !part)
                    return std::nullopt;
                return *whole + *part;
            }
            if (words.size() == 1)
                return words[0].find('/') != std::string_view::npos ? fraction(words[0]) : Number(words[0]);
            return std::nullopt;
        }

        // Parse 不舍入的结果
        std::optional<double> ParseExact(Units u, std::string_view text)
        {
            const std::string t = Text::AsciiLower(Text::Trim(text));
            const std::string_view tv = t;
            double mm = 0.0;
            if (Text::EndsWith(tv, "mm"))
            {
                const auto n = Number(tv.substr(0, tv.size() - 2));
                if (!n) return std::nullopt;
                mm = *n;
            }
            else if (Text::EndsWith(tv, "cm"))
            {
                const auto n = Number(tv.substr(0, tv.size() - 2));
                if (!n) return std::nullopt;
                mm = *n * 10.0;
            }
            else if (Text::EndsWith(tv, "m"))
            {
                const auto n = Number(tv.substr(0, tv.size() - 1));
                if (!n) return std::nullopt;
                mm = *n * 1000.0;
            }
            else if (tv.find_first_of("'\"") != std::string_view::npos || Text::EndsWith(tv, "ft") || Text::EndsWith(tv, "in"))
            {
                const std::string r = Text::ReplaceAll(Text::ReplaceAll(t, "ft", "'"), "in", "\"");
                const size_t q = r.find('\'');
                const std::string_view feet = q == std::string::npos ? std::string_view() : std::string_view(r).substr(0, q);
                std::string_view rest = q == std::string::npos ? std::string_view(r) : std::string_view(r).substr(q + 1);
                const bool negative = Text::StartsWith(Text::Trim(feet), "-") || (Text::Trim(feet).empty() && Text::StartsWith(Text::Trim(rest), "-"));
                double feetValue = 0.0;
                if (!Text::Trim(feet).empty())
                {
                    const auto f = Number(feet);
                    if (!f) return std::nullopt;
                    feetValue = std::abs(*f);
                }
                rest = Text::Trim(rest);
                while (!rest.empty() && rest.front() == '-')
                    rest.remove_prefix(1);
                rest = Text::Trim(rest);
                while (!rest.empty() && rest.back() == '"')
                    rest.remove_suffix(1);
                double inches = 0.0;
                if (!Text::Trim(rest).empty())
                {
                    const auto i = Inches(rest);
                    if (!i) return std::nullopt;
                    inches = *i;
                }
                mm = (feetValue * kFoot + inches * kInch) * (negative ? -1.0 : 1.0);
            }
            else
            {
                std::optional<double> n;
                switch (u)
                {
                case Units::Millimetres: n = Number(tv); break;
                case Units::Centimetres: n = Number(tv); if (n) *n *= 10.0; break;
                case Units::Metres:      n = Number(tv); if (n) *n *= 1000.0; break;
                case Units::Inches:
                    n = !tv.empty() && tv.front() == '-' ? Inches(tv.substr(1)) : Inches(tv);
                    if (n && !tv.empty() && tv.front() == '-') *n = -*n;
                    if (n) *n *= kInch;
                    break;
                case Units::Feet:
                case Units::FeetInches:  n = Number(tv); if (n) *n *= kFoot; break;
                }
                if (!n) return std::nullopt;
                mm = *n;
            }
            if (!std::isfinite(mm))
                return std::nullopt;
            return mm;
        }
    }

    const char* Name(Units u)
    {
        switch (u)
        {
        case Units::Millimetres: return "mm";
        case Units::Centimetres: return "cm";
        case Units::Metres:      return "m";
        case Units::Inches:      return "in";
        case Units::Feet:        return "ft";
        case Units::FeetInches:  return "ft-in";
        }
        return "mm";
    }

    const char* Label(Units u)
    {
        switch (u)
        {
        case Units::Millimetres: return N_("Millimetres");
        case Units::Centimetres: return N_("Centimetres");
        case Units::Metres:      return N_("Metres");
        case Units::Inches:      return N_("Inches");
        case Units::Feet:        return N_("Feet");
        case Units::FeetInches:  return N_("Feet and inches");
        }
        return "";
    }

    std::optional<Units> FromName(std::string_view name)
    {
        const std::string n = Text::AsciiLower(Text::Trim(name));
        for (Units u : All)
            if (n == Name(u))
                return u;
        return std::nullopt;
    }

    bool Imperial(Units u)
    {
        return u == Units::Inches || u == Units::Feet || u == Units::FeetInches;
    }

    std::string Length(Units u, double mm)
    {
        switch (u)
        {
        case Units::Millimetres: return Text::Shortest(std::round(mm) + 0.0);
        case Units::Centimetres: return Text::Shortest(std::round(mm) / 10.0 + 0.0);
        // 先舍入，舍入成 0 的不显示负号
        case Units::Metres:      return Fixed2(std::round(mm / 10.0) / 100.0 + 0.0);
        case Units::Feet:        return Fixed2(std::round(mm / kFoot * 100.0) / 100.0 + 0.0) + "'";
        case Units::Inches:
        case Units::FeetInches:
        {
            // 以 1/16 英寸计
            const double n = std::round(std::abs(mm) / kInch * 16.0);
            const char* sign = mm < 0.0 && n > 0.0 ? "-" : "";
            double feet = 0.0, rest = n;
            if (u == Units::FeetInches)
            {
                feet = std::floor(n / 192.0);
                rest = std::fmod(n, 192.0);
            }
            const double whole = std::floor(rest / 16.0), part = std::fmod(rest, 16.0);
            // 1/16 约到最简
            unsigned top = static_cast<unsigned>(part), bottom = 16;
            while (top > 0 && top % 2 == 0)
            {
                top /= 2;
                bottom /= 2;
            }
            std::string inches;
            if (top == 0)
                inches = Text::Shortest(whole) + "\"";
            else if (whole == 0.0 && u == Units::Inches)
                inches = std::to_string(top) + "/" + std::to_string(bottom) + "\"";
            else
                inches = Text::Shortest(whole) + " " + std::to_string(top) + "/" + std::to_string(bottom) + "\"";
            if (u == Units::FeetInches)
                return std::string(sign) + Text::Shortest(feet) + "'-" + inches;
            return std::string(sign) + inches;
        }
        }
        return {};
    }

    std::string WithUnit(Units u, double mm)
    {
        switch (u)
        {
        case Units::Millimetres:
        case Units::Centimetres:
        case Units::Metres:
            return Length(u, mm) + " " + Name(u);
        default:
            return Length(u, mm);
        }
    }

    std::pair<double, const char*> Area(Units u, double mm2)
    {
        if (Imperial(u))
            return { mm2 / (kFoot * kFoot), "ft²" };
        return { mm2 / 1e6, "m²" };
    }

    std::span<const double> Scales(Units u)
    {
        if (Imperial(u))
            return kImperialScales;
        return kMetricScales;
    }

    double DefaultScale(Units u)
    {
        return Imperial(u) ? 96.0 : 100.0;
    }

    std::string Scale(Units u, double n)
    {
        if (Imperial(u))
        {
            const double sixtyFourths = 12.0 * 64.0 / n;
            if (n <= 12.0 * 64.0 && std::abs(sixtyFourths - std::round(sixtyFourths)) < 1e-9)
                return Fraction(static_cast<unsigned>(std::round(sixtyFourths))) + "\" = 1'-0\"";
            const double feet = n / 12.0;
            if (feet >= 10.0 && std::abs(feet - std::round(feet)) < 1e-9)
                return "1\" = " + Text::Shortest(std::round(feet)) + "'-0\"";
        }
        return "1 : " + Text::Shortest(n);
    }

    std::optional<double> ParseScale(Units u, std::string_view text)
    {
        const std::string_view t = Text::Trim(text);
        double n = 0.0;
        if (const size_t eq = t.find('='); eq != std::string_view::npos)
        {
            const auto model = ParseExact(u, t.substr(eq + 1));
            if (!model) return std::nullopt;
            const auto paper = ParseExact(u, t.substr(0, eq));
            if (!paper) return std::nullopt;
            n = *model / *paper;
        }
        else if (const size_t colon = t.find(':'); colon != std::string_view::npos)
        {
            if (Number(t.substr(0, colon)) != 1.0)
                return std::nullopt;
            const auto x = Number(t.substr(colon + 1));
            if (!x) return std::nullopt;
            n = *x;
        }
        else
        {
            const auto x = Number(t);
            if (!x) return std::nullopt;
            n = *x;
        }
        if (!(std::isfinite(n) && n > 0.0))
            return std::nullopt;
        return std::round(n * 1e6) / 1e6;
    }

    std::optional<double> Parse(Units u, std::string_view text)
    {
        // 到微米：输入的精确值保持精确
        const auto exact = ParseExact(u, text);
        if (!exact)
            return std::nullopt;
        const double mm = std::round(*exact * 1e3) / 1e3;
        if (!std::isfinite(mm))
            return std::nullopt;
        return mm + 0.0;
    }
}
