// 移植自 BIMCraft crates/core/src/model.rs 的项目参数、pen.rs 与 styles.rs 的项目设置（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Model.h"
#include "Model/Massing.h"
#include "Model/Text.h"
#include <algorithm>
#include <cmath>

// 管理选项卡的项目设置（第 5f 步）：项目参数、线宽、对象样式
namespace MiniBIM::Bim
{
    namespace
    {
        std::string CategoryList()
        {
            std::string out;
            for (const char* c : ParameterCategories)
                out += (out.empty() ? "" : ", ") + std::string(c);
            return out;
        }
    }

    namespace ParameterKindOf
    {
        const char* Name(ParameterKind k)
        {
            switch (k)
            {
            case ParameterKind::Text:   return "text";
            case ParameterKind::Number: return "number";
            case ParameterKind::Length: return "length";
            case ParameterKind::YesNo:  return "yesno";
            }
            return "text";
        }

        std::optional<ParameterKind> Parse(std::string_view s)
        {
            for (const ParameterKind k : All)
                if (s == Name(k))
                    return k;
            return std::nullopt;
        }
    }

    bool ProjectParameter::Holds(const Json& v) const
    {
        switch (kind)
        {
        case ParameterKind::Text:
            return v.is_string() && Text::CharCount(v.get<std::string>()) <= MaxText;
        case ParameterKind::Number:
        case ParameterKind::Length:
            return v.is_number() && std::isfinite(v.get<double>()) && std::abs(v.get<double>()) <= MaxCoord;
        case ParameterKind::YesNo:
            return v.is_boolean();
        }
        return false;
    }

    LineWeights Model::LineWeightsOf() const { return lineWeights ? *lineWeights : LineWeights::Default(units); }

    ObjectStyle Model::ObjectStyleOf(std::string_view category) const
    {
        const auto it = objectStyles.find(std::string(category));
        return it != objectStyles.end() ? it->second : ObjectStyles::Default(category);
    }

    const ProjectParameter* Model::ParameterOf(const Element& e, const std::string& name) const
    {
        // 构件的同名族参数是它的族的
        if (const auto fo = e.FamilyOf())
            if (const auto f = families.find(fo->first); f != families.end() &&
                std::any_of(f->second.parameters.begin(), f->second.parameters.end(), [&](const Fam::Parameter& p) { return p.name == name; }))
                return nullptr;
        const auto it = parameters.find(name);
        if (it == parameters.end())
            return nullptr;
        const std::string cat = e.Category();
        const auto& cs = it->second.categories;
        return std::find(cs.begin(), cs.end(), cat) != cs.end() ? &it->second : nullptr;
    }

    void Model::RekeyParams(const std::function<std::optional<std::string>(const std::string&)>& key)
    {
        std::vector<Id> ids;
        for (const auto& [id, e] : elements.Items())
            if (const auto* ps = e->Params(); ps && std::any_of(ps->begin(), ps->end(), [&](const auto& kv) { return key(kv.first).has_value(); }))
                ids.push_back(id);
        for (const Id id : ids)
        {
            std::map<std::string, Json>& params = *elements.GetMut(id)->ParamsMut();
            std::map<std::string, Json> out;
            for (auto& [k, v] : params)
            {
                const auto to = key(k);
                out[to ? *to : k] = std::move(v);
            }
            params = std::move(out);
        }
    }

    bool Model::CheckManage(std::string& error) const
    {
        if (parameters.size() > MaxProjectParameters)
        {
            error = "the project has " + std::to_string(parameters.size()) + " parameters; it holds up to " + std::to_string(MaxProjectParameters);
            return false;
        }
        for (const auto& [n, p] : parameters)
        {
            const std::string who = "parameter `" + n + "`";
            if (!Fam::Nameable(n))
            {
                error = who + ": a name starts with a letter and holds letters, digits, `_` and spaces, up to " + std::to_string(MaxText);
                return false;
            }
            if (Massing::Reserved(n))
            {
                error = who + ": elements hold their phase, their design option, their paint and their mass under that name; name it otherwise";
                return false;
            }
            if (p.categories.empty())
            {
                error = who + ": it needs a category: one of " + CategoryList();
                return false;
            }
            for (const std::string& c : p.categories)
            {
                if (std::find(ParameterCategories.begin(), ParameterCategories.end(), c) == ParameterCategories.end())
                {
                    error = who + ": `" + c + "` isn't a category it can be for; there are " + CategoryList();
                    return false;
                }
                if (Schedules::OwnField(c, n))
                {
                    error = who + ": " + c + "s have a `" + n + "` of their own; name it otherwise";
                    return false;
                }
            }
            if (std::find(p.categories.begin(), p.categories.end(), "component") != p.categories.end())
                for (const auto& [f, fam] : families)
                    if (std::any_of(fam.parameters.begin(), fam.parameters.end(), [&](const Fam::Parameter& q) { return q.name == n; }))
                    {
                        error = who + ": family `" + f + "` has a parameter of that name; name it otherwise";
                        return false;
                    }
        }
        if (lineWeights && !lineWeights->Check(error))
            return false;
        for (const auto& [c, s] : objectStyles)
        {
            if (!ObjectStyles::Known(c))
            {
                std::string all;
                for (const char* k : GraphicsOf::Model)
                    all += (all.empty() ? "" : ", ") + std::string(k);
                error = "`" + c + "` isn't a model category; Object Styles has: " + all;
                return false;
            }
            if (!ObjectStyles::Check(s, c, error))
                return false;
        }
        return true;
    }
}
