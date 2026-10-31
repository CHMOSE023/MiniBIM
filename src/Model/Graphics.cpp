// 移植自 BIMCraft crates/core/src/graphics.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Graphics.h"
#include "Model/Text.h"
#include <algorithm>
#include <cctype>

namespace MiniBIM::Bim
{
    namespace
    {
        constexpr uint8_t MaxPen = 16;

        // 只留字母与数字、小写：Hidden Line、hidden_line、HIDDENLINE 一样
        std::string Plain(std::string_view s)
        {
            std::string out;
            for (const char c : s)
                if (std::isalnum(static_cast<unsigned char>(c)))
                    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        std::string Trimmed(std::string_view s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.remove_suffix(1);
            return Text::AsciiLower(s);
        }

        bool StyleOk(const Style& s, const std::string& who, std::string& error)
        {
            if (s.weight && *s.weight > MaxPen)
            {
                error = who + ": a line weight is a pen, 0 (a hairline) to " + std::to_string(MaxPen) + ", not " + std::to_string(*s.weight);
                return false;
            }
            return true;
        }

        template <class T> std::optional<T> Or(const std::optional<T>& a, const std::optional<T>& b) { return a ? a : b; }
    }

    namespace DetailOf
    {
        const char* Name(Detail d)
        {
            switch (d)
            {
            case Detail::Coarse: return "Coarse";
            case Detail::Medium: return "Medium";
            case Detail::Fine: break;
            }
            return "Fine";
        }

        std::optional<Detail> Parse(std::string_view s)
        {
            const std::string t = Trimmed(s);
            for (const Detail d : { Detail::Coarse, Detail::Medium, Detail::Fine })
                if (Text::AsciiLower(Name(d)) == t)
                    return d;
            return std::nullopt;
        }
    }

    namespace VisualStyleOf
    {
        const char* Name(VisualStyle v)
        {
            switch (v)
            {
            case VisualStyle::HiddenLine: return "Hidden Line";
            case VisualStyle::Wireframe: return "Wireframe";
            case VisualStyle::Shaded: break;
            }
            return "Shaded";
        }

        const char* Key(VisualStyle v)
        {
            switch (v)
            {
            case VisualStyle::HiddenLine: return "hidden_line";
            case VisualStyle::Wireframe: return "wireframe";
            case VisualStyle::Shaded: break;
            }
            return "shaded";
        }

        std::optional<VisualStyle> Parse(std::string_view s)
        {
            const std::string p = Plain(s);
            for (const VisualStyle v : { VisualStyle::Shaded, VisualStyle::HiddenLine, VisualStyle::Wireframe })
                if (Plain(Name(v)) == p)
                    return v;
            return std::nullopt;
        }
    }

    namespace DashOf
    {
        const char* Name(Dash d)
        {
            switch (d)
            {
            case Dash::Dashed: return "dashed";
            case Dash::Dotted: return "dotted";
            case Dash::Solid: break;
            }
            return "solid";
        }

        std::optional<Dash> Parse(std::string_view s)
        {
            for (const Dash d : { Dash::Solid, Dash::Dashed, Dash::Dotted })
                if (s == Name(d))
                    return d;
            return std::nullopt;
        }
    }

    Style Style::Over(const Style& base) const
    {
        return Style{ Or(color, base.color),   Or(weight, base.weight),         Or(dash, base.dash),
                      Or(fill, base.fill),     Or(foreground, base.foreground), Or(background, base.background) };
    }

    Override Override::Over(const Override& base) const
    {
        return Override{ hidden || base.hidden, projection.Over(base.projection), cut.Over(base.cut), halftone || base.halftone,
                         transparency > 0 ? transparency : base.transparency };
    }

    bool Override::Check(const std::string& who, std::string& error) const
    {
        if (!StyleOk(projection, who + " projection", error) || !StyleOk(cut, who + " cut", error))
            return false;
        if (transparency > 100)
        {
            error = who + ": transparency is a percent, 0 to 100, not " + std::to_string(transparency);
            return false;
        }
        return true;
    }

    double Graphics::ScaleN(Units units) const
    {
        return scale.value_or(UnitsOf::DefaultScale(units));
    }

    bool Graphics::Hides(std::string_view category) const
    {
        const auto it = categories.find(std::string(category));
        return it != categories.end() && it->second.hidden;
    }

    bool Graphics::Check(const std::map<std::string, FillPattern>& patterns, std::string& error) const
    {
        if (categories.size() > GraphicsOf::MaxEntries || elements.size() > GraphicsOf::MaxEntries)
        {
            error = "it has more than " + std::to_string(GraphicsOf::MaxEntries) + " overrides";
            return false;
        }
        for (const auto& [c, o] : categories)
        {
            if (!GraphicsOf::Drawn(c))
            {
                error = "`" + c + "` isn't a category views draw; there are: " + GraphicsOf::Names();
                return false;
            }
            if (!o.Check("category `" + c + "`", error))
                return false;
        }
        for (const auto& [id, o] : elements)
            if (!o.Check("element " + id.ToString(), error))
                return false;
        const auto patternsOk = [&](const Override& o)
        {
            for (const Style* s : { &o.projection, &o.cut })
                for (const auto* layer : { &s->foreground, &s->background })
                    if (*layer && (*layer)->pattern && !patterns.count(*(*layer)->pattern))
                    {
                        error = "there's no fill pattern `" + *(*layer)->pattern + "`";
                        return false;
                    }
            return true;
        };
        for (const auto& [c, o] : categories)
            if (!patternsOk(o))
                return false;
        for (const auto& [id, o] : elements)
            if (!patternsOk(o))
                return false;
        if (scale && !(*scale >= GraphicsOf::MinScale && *scale <= GraphicsOf::MaxScale))
        {
            error = "its scale 1 : " + Text::Shortest(*scale) + " isn't between 1 : " + Text::Shortest(GraphicsOf::MinScale) + " and 1 : " +
                    Text::Shortest(GraphicsOf::MaxScale);
            return false;
        }
        return true;
    }

    namespace GraphicsOf
    {
        bool Drawn(std::string_view category)
        {
            return std::find(Model.begin(), Model.end(), category) != Model.end() ||
                   std::find(Annotation.begin(), Annotation.end(), category) != Annotation.end();
        }

        std::string Names()
        {
            std::string out;
            for (const char* c : Model)
                out += (out.empty() ? "" : ", ") + std::string(c);
            for (const char* c : Annotation)
                out += ", " + std::string(c);
            return out;
        }
    }
}
