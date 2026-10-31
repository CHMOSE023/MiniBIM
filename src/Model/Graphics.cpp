// 移植自 BIMCraft crates/core/src/graphics.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Graphics.h"
#include "Model/Model.h"
#include "Model/Text.h"
#include <algorithm>
#include <cctype>
#include <set>

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

    Graphics Graphics::Resolved(const Model& m) const
    {
        Graphics g = *this;
        if (!templateName)
            return g;
        const auto t = m.viewTemplates.find(*templateName);
        if (t == m.viewTemplates.end())
            return g;
        const auto controls = [&](Group x) { return t->second.controls.count(x) != 0; };
        const Graphics& tg = t->second.graphics;
        if (controls(Group::Categories))
            g.categories = tg.categories;
        if (controls(Group::Filters))
            g.filters = tg.filters;
        if (controls(Group::Phase))
        {
            g.phase       = tg.phase;
            g.phaseFilter = tg.phaseFilter;
        }
        if (controls(Group::Options))
            g.options = tg.options;
        if (controls(Group::Scale))
            g.scale = tg.scale;
        if (controls(Group::Detail))
            g.detail = tg.detail;
        if (controls(Group::Style))
            g.style = tg.style;
        return g;
    }

    const std::string* Graphics::ControlledBy(const Model& m, Group group) const
    {
        if (!templateName)
            return nullptr;
        const auto t = m.viewTemplates.find(*templateName);
        return t != m.viewTemplates.end() && t->second.controls.count(group) ? &*templateName : nullptr;
    }

    namespace
    {
        // " (there are: a, b)"，没有时为空
        template <typename Map>
        std::string Known(const Map& map)
        {
            std::string out;
            for (const auto& [name, _] : map)
                out += (out.empty() ? " (there are: " : ", ") + name;
            return out.empty() ? out : out + ")";
        }
    }

    bool Graphics::Check(const Model& m, std::string& error) const
    {
        const auto& patterns = m.fillPatterns;
        if (categories.size() > GraphicsOf::MaxEntries || elements.size() > GraphicsOf::MaxEntries)
        {
            error = "it has more than " + std::to_string(GraphicsOf::MaxEntries) + " overrides";
            return false;
        }
        if (filters.size() > GraphicsOf::MaxApplied)
        {
            error = "it applies " + std::to_string(filters.size()) + " filters; a view applies up to " + std::to_string(GraphicsOf::MaxApplied);
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
        if (layers.size() > Cads::MaxCads || std::any_of(layers.begin(), layers.end(), [](const auto& l) { return l.second.size() > Cads::MaxHidden; }))
        {
            error = "it hides layers of more than " + std::to_string(Cads::MaxCads) + " CAD drawings, or more than " + std::to_string(Cads::MaxHidden) +
                    " of one";
            return false;
        }
        for (const auto& [cad, names] : layers)
        {
            const auto tooLong = [](const std::string& n) { return Text::CharCount(n) > MaxText; };
            const std::string* bad = tooLong(cad) ? &cad : nullptr;
            for (const std::string& n : names)
                if (!bad && tooLong(n))
                    bad = &n;
            if (bad)
            {
                error = "the layer or drawing `" + *bad + "` is longer than " + std::to_string(MaxText) + " characters";
                return false;
            }
        }
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
        std::set<std::string> seen;
        for (const Applied& a : filters)
        {
            if (!m.viewFilters.count(a.filter))
            {
                error = "there's no view filter `" + a.filter + "`" + Known(m.viewFilters);
                return false;
            }
            if (!seen.insert(a.filter).second)
            {
                error = "filter `" + a.filter + "` is applied twice";
                return false;
            }
            if (!a.over.Check("filter `" + a.filter + "`", error) || !patternsOk(a.over))
                return false;
        }
        if (templateName && !m.viewTemplates.count(*templateName))
        {
            error = "there's no view template `" + *templateName + "`" + Known(m.viewTemplates);
            return false;
        }
        if (scale && !(*scale >= GraphicsOf::MinScale && *scale <= GraphicsOf::MaxScale))
        {
            error = "its scale 1 : " + Text::Shortest(*scale) + " isn't between 1 : " + Text::Shortest(GraphicsOf::MinScale) + " and 1 : " +
                    Text::Shortest(GraphicsOf::MaxScale);
            return false;
        }
        return true;
    }

    namespace GroupOf
    {
        const char* Key(Group g)
        {
            switch (g)
            {
            case Group::Categories: return "categories";
            case Group::Filters:    return "filters";
            case Group::Range:      return "range";
            case Group::Phase:      return "phase";
            case Group::Options:    return "options";
            case Group::Scale:      return "scale";
            case Group::Detail:     return "detail";
            case Group::Style:      return "style";
            }
            return "categories";
        }

        std::optional<Group> Parse(std::string_view s)
        {
            for (const Group g : All)
                if (s == Key(g))
                    return g;
            return std::nullopt;
        }
    }

    namespace GraphicsOf
    {
        bool Drawn(std::string_view category)
        {
            return std::find(Model.begin(), Model.end(), category) != Model.end() ||
                   std::find(Annotation.begin(), Annotation.end(), category) != Annotation.end() ||
                   std::find(Imported.begin(), Imported.end(), category) != Imported.end();
        }

        std::string Names()
        {
            std::string out;
            for (const char* c : Model)
                out += (out.empty() ? "" : ", ") + std::string(c);
            for (const char* c : Annotation)
                out += ", " + std::string(c);
            for (const char* c : Imported)
                out += ", " + std::string(c);
            return out;
        }
    }
}
