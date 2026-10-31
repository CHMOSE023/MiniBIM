// 移植自 BIMCraft crates/core/src/pen.rs、styles.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Pens.h"
#include "Model/Graphics.h"
#include "Model/Text.h"
#include <algorithm>
#include <cmath>

namespace MiniBIM::Bim
{
    namespace
    {
        // 笔 1 到 16 的一行 → 笔 0 到 16（笔 0 是发丝线）
        std::array<double, 17> WithHairline(const PenRow& row)
        {
            std::array<double, 17> all{};
            std::copy(row.begin(), row.end(), all.begin() + 1);
            return all;
        }
    }

    LineWeights LineWeights::Default(Units units)
    {
        static constexpr std::array<double, 6> Metric{ 10, 20, 50, 100, 200, 500 }, Imperial{ 12, 24, 48, 96, 192, 384 };
        const auto row = [](double k)
        {
            PenRow r{};
            for (size_t i = 0; i < r.size(); ++i)
                r[i] = std::clamp(std::round(Pens::Widths[i + 1] * k * 100.0) / 100.0, Pens::MinWidth, Pens::MaxWidth);
            return r;
        };
        LineWeights lw;
        for (const double n : UnitsOf::Imperial(units) ? Imperial : Metric)
            lw.model.push_back(ScalePens{ n, row(std::pow(100.0 / n, 0.25)) });
        lw.annotation = row(1.0);
        return lw;
    }

    std::array<double, 17> LineWeights::ModelAt(double n) const
    {
        const ScalePens* best = nullptr;
        for (const ScalePens& s : model)
        {
            if (!best)
            {
                best = &s;
                continue;
            }
            const double off = std::abs(std::log(s.scale / n)), bestOff = std::abs(std::log(best->scale / n));
            if (off < bestOff || (off == bestOff && s.scale < best->scale))
                best = &s;
        }
        return best ? WithHairline(best->widths) : Pens::Widths;
    }

    std::array<double, 17> LineWeights::Annotation() const { return WithHairline(annotation); }

    const ScalePens* LineWeights::Column(double n) const
    {
        const auto it = std::find_if(model.begin(), model.end(), [&](const ScalePens& s) { return s.scale == n; });
        return it == model.end() ? nullptr : &*it;
    }

    bool LineWeights::Check(std::string& error) const
    {
        if (model.empty() || model.size() > Pens::MaxScales)
        {
            error = "the model's line weights hold 1 to " + std::to_string(Pens::MaxScales) + " scales, not " + std::to_string(model.size());
            return false;
        }
        for (size_t i = 0; i < model.size(); ++i)
        {
            const double n = model[i].scale;
            if (!(std::isfinite(n) && n >= GraphicsOf::MinScale && n <= GraphicsOf::MaxScale))
            {
                error = "line weights at 1 : " + Text::Shortest(n) + ": a scale is 1 : 1 to 1 : 100000";
                return false;
            }
            for (size_t j = 0; j < i; ++j)
                if (model[j].scale == n)
                {
                    error = "line weights at 1 : " + Text::Shortest(n) + " are given twice";
                    return false;
                }
        }
        const auto rowOk = [&](const PenRow& row, const std::string& what)
        {
            for (size_t i = 0; i < row.size(); ++i)
                if (!(std::isfinite(row[i]) && row[i] >= Pens::MinWidth && row[i] <= Pens::MaxWidth))
                {
                    error = Text::ReplaceAll(what, "{}", std::to_string(i + 1)) + " is " + Text::Shortest(row[i]) +
                            " mm wide: a pen is 0.01 to 20 mm on paper";
                    return false;
                }
            return true;
        };
        for (const ScalePens& s : model)
            if (!rowOk(s.widths, "model pen {} at 1 : " + Text::Shortest(s.scale)))
                return false;
        return rowOk(annotation, "annotation pen {}");
    }

    void LineWeights::Sort()
    {
        std::stable_sort(model.begin(), model.end(), [](const ScalePens& a, const ScalePens& b) { return a.scale < b.scale; });
    }

    namespace ObjectStyles
    {
        ObjectStyle Default(std::string_view category)
        {
            if (category == "mass")
                return { 4, 4 };
            if (category == "door" || category == "window")
                return { 4, 6 };
            if (category == "railing")
                return { 6, 10 };
            return { 4, 10 };
        }

        bool Known(std::string_view category)
        {
            return std::find(GraphicsOf::Model.begin(), GraphicsOf::Model.end(), category) != GraphicsOf::Model.end();
        }

        bool Check(const ObjectStyle& s, const std::string& category, std::string& error)
        {
            for (const auto& [what, p] : { std::pair{ "projection", s.projection }, std::pair{ "cut", s.cut } })
                if (p > Pens::Max)
                {
                    error = "object style `" + category + "` " + what + ": a line weight is a pen, 0 (a hairline) to 16, not " + std::to_string(p);
                    return false;
                }
            return true;
        }
    }
}
