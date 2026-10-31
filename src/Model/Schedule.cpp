// 移植自 BIMCraft crates/core/src/schedule.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Schedule.h"
#include "Commands/Commands.h"
#include "I18n/Translator.h"
#include "Model/Family.h"
#include "Model/Geom.h"
#include "Model/Language.h"
#include "Model/Model.h"
#include "Model/ModelJson.h"
#include "Model/Text.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

namespace MiniBIM::Bim::Schedules
{
    const std::array<Builtin, 6> Views = { {
        { "room", N_("Room Schedule") },
        { "door", N_("Door Schedule") },
        { "window", N_("Window Schedule") },
        { "wall", N_("Wall Schedule") },
        { "floor", N_("Floor Schedule") },
        { "level", N_("Level Schedule") },
    } };

    namespace
    {
        // 类别自己的字段从哪里来：元素的 JSON（ElementJson）里的一个键（数没有时为 0），它指向的元素的 ID，标高的名称，
        // 米换成毫米，1（计数），是 / 否，门窗的开启方式（项目的语言）
        enum class From
        {
            Key,
            Ref,
            Level,
            Metres,
            Count,
            Flag,
            Operation,
        };

        struct Own
        {
            const char* name;
            Kind        kind;
            From        from;
            const char* key = "";
        };

        constexpr Own ID        = { N_("Id"), Kind::Number, From::Key, "id" };
        constexpr Own TYPE      = { N_("Type"), Kind::Text, From::Key, "type" };
        constexpr Own FAMILY    = { N_("Family"), Kind::Text, From::Key, "family" };
        constexpr Own LEVEL     = { N_("Level"), Kind::Text, From::Level };
        constexpr Own AREA      = { N_("Area"), Kind::Area, From::Key, "area_m2" };
        constexpr Own WIDTH     = { N_("Width"), Kind::Length, From::Key, "width" };
        constexpr Own HEIGHT    = { N_("Height"), Kind::Length, From::Key, "height" };
        constexpr Own LENGTH    = { N_("Length"), Kind::Length, From::Key, "length" };
        constexpr Own THICKNESS = { N_("Thickness"), Kind::Length, From::Key, "thickness" };
        constexpr Own DEPTH     = { N_("Depth"), Kind::Length, From::Key, "depth" };
        constexpr Own SYSTEM    = { N_("System"), Kind::Text, From::Key, "system" };
        constexpr Own OFFSET    = { N_("Offset"), Kind::Length, From::Key, "offset" };
        constexpr Own MARK      = { N_("Mark"), Kind::Text, From::Key, "mark" };
        constexpr Own OPERATION = { N_("Operation"), Kind::Text, From::Operation };
        constexpr Own NAME      = { N_("Name"), Kind::Text, From::Key, "name" };
        constexpr Own HOST      = { N_("Wall"), Kind::Number, From::Ref, "host" };
        constexpr Own COUNT     = { N_("Count"), Kind::Number, From::Count };
        constexpr Own VOLUME    = { N_("Volume"), Kind::Number, From::Key, "volume_m3" };
        constexpr Own DIAMETER  = { N_("Diameter"), Kind::Length, From::Key, "width" };

        struct Category
        {
            const char*      name;
            // 不选字段时显示前几个
            size_t           shown;
            std::vector<Own> fields;
        };

        // 各类别自己的字段（BIMCraft 另有楼梯、坡道、栏杆，以及阶段与设计选项的字段，随那些功能移植）
        const std::vector<Category>& OwnTable()
        {
            static const std::vector<Category> table = {
                { "room", 5, { { N_("Number"), Kind::Text, From::Key, "number" }, NAME, LEVEL, AREA, { N_("Perimeter"), Kind::Length, From::Metres, "perimeter_m" }, COUNT } },
                { "door", 6, { MARK, TYPE, WIDTH, HEIGHT, LEVEL, HOST, FAMILY, OPERATION, COUNT } },
                { "window", 7, { MARK, TYPE, WIDTH, HEIGHT, { N_("Sill"), Kind::Length, From::Key, "sill" }, LEVEL, HOST, FAMILY, OPERATION, COUNT } },
                { "wall", 6, { ID, TYPE, LEVEL, LENGTH, HEIGHT, THICKNESS, COUNT } },
                { "floor", 5, { ID, TYPE, LEVEL, AREA, THICKNESS, OFFSET, COUNT, { N_("Structural"), Kind::YesNo, From::Flag, "structural" } } },
                { "ceiling", 5, { ID, TYPE, LEVEL, OFFSET, AREA, THICKNESS, COUNT } },
                { "roof", 4, { ID, TYPE, LEVEL, { N_("Slope"), Kind::Number, From::Key, "slope" }, { N_("Plan Area"), Kind::Area, From::Key, "area_m2" }, OFFSET, COUNT } },
                { "column", 6, { ID, TYPE, LEVEL, WIDTH, DEPTH, HEIGHT, COUNT } },
                { "structuralcolumn", 6, { ID, TYPE, LEVEL, WIDTH, DEPTH, HEIGHT, COUNT } },
                { "footing", 6, { ID, TYPE, LEVEL, WIDTH, DEPTH, { N_("Thickness"), Kind::Length, From::Key, "height" }, COUNT } },
                { "beam", 6, { ID, TYPE, LEVEL, LENGTH, WIDTH, DEPTH, SYSTEM, COUNT } },
                { "stripfooting", 6, { ID, TYPE, LEVEL, LENGTH, WIDTH, DEPTH, SYSTEM, COUNT } },
                { "pipe", 6, { ID, TYPE, LEVEL, SYSTEM, LENGTH, DIAMETER, COUNT } },
                { "conduit", 6, { ID, TYPE, LEVEL, SYSTEM, LENGTH, DIAMETER, COUNT } },
                { "cabletray", 7, { ID, TYPE, LEVEL, SYSTEM, LENGTH, WIDTH, DEPTH, COUNT } },
                { "cable", 6, { ID, TYPE, LEVEL, SYSTEM, LENGTH, DIAMETER, COUNT } },
                { "downconductor", 7, { ID, TYPE, LEVEL, SYSTEM, LENGTH, WIDTH, DEPTH, COUNT } },
                { "groundconductor", 7, { ID, TYPE, LEVEL, SYSTEM, LENGTH, WIDTH, DEPTH, COUNT } },
                { "wire", 6, { ID, TYPE, LEVEL, SYSTEM, LENGTH, DIAMETER, COUNT } },
                { "component", 4, { FAMILY, TYPE, LEVEL, COUNT } },
                { "mass", 5, { NAME, LEVEL, HEIGHT, AREA, VOLUME, { N_("Gross Floor Area"), Kind::Area, From::Key, "gross_floor_area_m2" }, COUNT } },
                // 体量穿过的每个标高一行（Sources）：那里的楼层面积，以及到上一个这样的标高的体积
                { "massfloor", 5, { { N_("Mass"), Kind::Text, From::Key, "mass" }, LEVEL, AREA, { N_("Perimeter"), Kind::Length, From::Metres, "perimeter_m" }, VOLUME, COUNT } },
                { "level", 2, { { N_("Elevation"), Kind::Length, From::Key, "elevation" }, NAME, COUNT } },
            };
            return table;
        }

        // 不做明细表的类别的字段：注释，选择的过滤与标记显示的。不选字段时显示 0 个
        const std::vector<Category>& MoreTable()
        {
            static const std::vector<Category> table = {
                { "line", 0, { { N_("Line Style"), Kind::Text, From::Key, "style" }, LENGTH } },
                { "region", 0, { TYPE, { N_("Line Style"), Kind::Text, From::Key, "line_style" } } },
                { "text", 0, { { N_("Text"), Kind::Text, From::Key, "text" } } },
                { "tag", 0, { { N_("Text"), Kind::Text, From::Key, "text" } } },
                { "dimension", 0, { { N_("Kind"), Kind::Text, From::Key, "kind" } } },
                { "grid", 0, { NAME } },
            };
            return table;
        }

        const Category* OwnOf(std::string_view cat)
        {
            for (const auto* list : { &OwnTable(), &MoreTable() })
                for (const Category& c : *list)
                    if (cat == c.name)
                        return &c;
            return nullptr;
        }

        const char* Holding(Kind k)
        {
            switch (k)
            {
            case Kind::Text:   return "text";
            case Kind::Number: return "a number";
            case Kind::Length: return "a length";
            case Kind::Area:   return "an area";
            case Kind::YesNo:  return "yes or no";
            }
            return "text";
        }

        std::string Joined(const std::vector<std::string>& names)
        {
            std::string out;
            for (const std::string& n : names)
                out += (out.empty() ? "" : ", ") + n;
            return out;
        }

        std::string Show(const Json& v) { return v.dump(-1, ' ', false, Json::error_handler_t::replace); }

        // x 到百万分之一，合计与公式照写的出来
        double Fine(double x)
        {
            const double r = std::round(x * 1e6) / 1e6 + 0.0;
            return std::isfinite(r) ? r : x;
        }

        std::string Cell(const Json& v)
        {
            if (v.is_string())
                return v.get<std::string>();
            if (v.is_null())
                return {};
            if (v.is_number_float())
                return Text::Shortest(v.get<double>());
            return Show(v);
        }

        std::optional<double> Number(const Json& v)
        {
            if (v.is_number())
                return v.get<double>();
            return std::nullopt;
        }

        // 数（以及"2""10"这样的编号文字）按大小在前，再是别的文字，先不分大小写
        int Natural(const Json& a, const Json& b)
        {
            const std::string s = Cell(a), t = Cell(b);
            const auto x = Text::ParseNumber(Text::Trim(s)), y = Text::ParseNumber(Text::Trim(t));
            if (x && y)
                return *x < *y ? -1 : *x > *y ? 1 : 0;
            if (x)
                return -1;
            if (y)
                return 1;
            const std::string ls = Text::AsciiLower(s), lt = Text::AsciiLower(t);
            if (ls != lt)
                return ls < lt ? -1 : 1;
            return s < t ? -1 : s > t ? 1 : 0;
        }

        // 算出的字段的公式，以及它读到的算出字段的，都读有的字段、存数，不绕回自己，连起来不超过 MaxChain 个
        struct Follower
        {
            const std::map<std::string, std::string>&  formulas;
            const std::map<std::string, Kind>&          kinds;
            const std::function<std::string(const std::string&)>& unknown;
            std::vector<std::string>                    path;
            std::map<std::string, size_t>               depths;

            bool Follow(const std::string& name, size_t& out, std::string& error)
            {
                out = 0;
                const auto f = formulas.find(name);
                if (f == formulas.end())
                    return true;
                const std::string tooDeep = "calculated field `" + name + "` reads formulas reading more than " + std::to_string(Fam::MaxChain) + " others in a row";
                if (const auto d = depths.find(name); d != depths.end())
                {
                    if (path.size() + d->second > Fam::MaxChain)
                        return (error = tooDeep, false);
                    out = d->second;
                    return true;
                }
                if (std::find(path.begin(), path.end(), name) != path.end())
                    return (error = "calculated field `" + name + "`'s formula comes back to itself", false);
                if (path.size() >= Fam::MaxChain)
                    return (error = tooDeep, false);
                const std::string who = "calculated field `" + name + "`";
                path.push_back(name);
                std::vector<std::string> names;
                std::string why;
                if (!Fam::Names(f->second, names, why))
                    return (error = who + ": " + why, false);
                size_t below = 0;
                for (const std::string& n : names)
                {
                    const auto k = kinds.find(n);
                    if (k == kinds.end())
                        return (error = who + ": " + unknown(n), false);
                    if (k->second == Kind::Text)
                        return (error = who + ": `" + n + "` is text, and a formula reads numbers", false);
                    size_t d = 0;
                    if (!Follow(n, d, error))
                        return false;
                    below = std::max(below, d);
                }
                path.pop_back();
                if (path.size() + below + 1 > Fam::MaxChain)
                    return (error = tooDeep, false);
                depths[name] = below + 1;
                out = below + 1;
                return true;
            }
        };

        // 一个元素的值，明细表的一行这样算：类别的字段从它的 JSON 来，算出的字段由公式算
        struct Row
        {
            const Model&   m;
            const Element& e;
            // 它是哪个明细表的行的类别：e 的，体量楼层时为体量楼层
            std::string_view cat;
            Json             json;
            const std::map<std::string, std::pair<std::string, Kind>>& formulas;
            std::map<std::string, Json> done;

            Json Get(const std::string& name)
            {
                std::vector<std::string> path;
                return Value(name, path);
            }

            Json Value(const std::string& name, std::vector<std::string>& path)
            {
                if (const auto d = done.find(name); d != done.end())
                    return d->second;
                Json v;
                if (const auto f = formulas.find(name); f != formulas.end())
                {
                    // 检查时已读过；这里有字段没有值时它也没有
                    if (std::find(path.begin(), path.end(), name) != path.end() || path.size() >= Fam::MaxChain)
                        v = nullptr;
                    else
                    {
                        path.push_back(name);
                        double x = 0.0;
                        std::string why;
                        const bool ok = Fam::Eval(
                            f->second.first,
                            [&](const std::string& n, double& out, std::string& err)
                            {
                                const Json got = Value(n, path);
                                if (got.is_boolean())
                                    return (out = got.get<bool>() ? 1.0 : 0.0, true);
                                if (got.is_number())
                                    return (out = got.get<double>(), true);
                                err = "`" + n + "` has no number";
                                return false;
                            },
                            x, why);
                        path.pop_back();
                        if (!ok)
                            v = nullptr;
                        else if (f->second.second == Kind::YesNo)
                            v = x != 0.0;
                        else
                            v = Fine(x);
                    }
                }
                else
                    v = Field(name);
                done[name] = v;
                return v;
            }

            // 类别的一个字段：自己的，或族的参数
            Json Field(const std::string& name) const
            {
                const Json& j = json;
                const auto number = [&](const char* k)
                {
                    const auto it = j.find(k);
                    return it != j.end() && it->is_number() ? it->get<double>() : 0.0;
                };
                if (const Category* own = OwnOf(cat))
                    for (const Own& o : own->fields)
                    {
                        if (name != o.name)
                            continue;
                        switch (o.from)
                        {
                        case From::Key:
                            if (o.kind == Kind::Text)
                            {
                                const auto it = j.find(o.key);
                                if (it == j.end() || (it->is_string() && it->get<std::string>().empty()))
                                    return nullptr;
                                return *it;
                            }
                            return number(o.key);
                        case From::Ref:
                        {
                            Guid g;
                            std::string why;
                            const auto it = j.find(o.key);
                            const auto id = it != j.end() && FromJson(*it, g, why) ? m.elements.IdOf(g) : std::nullopt;
                            return id ? static_cast<double>(*id) : 0.0;
                        }
                        case From::Level:
                        {
                            Guid g;
                            std::string why;
                            const auto it = j.find("level");
                            const Level* l = it != j.end() && FromJson(*it, g, why) ? m.GetLevel(g) : nullptr;
                            return l ? Json(l->name) : Json(nullptr);
                        }
                        case From::Metres:
                            return Fine(number(o.key) * 1000.0);
                        case From::Count:
                            return 1.0;
                        case From::Flag:
                        {
                            const auto it = j.find(o.key);
                            return it != j.end() && it->is_boolean() && it->get<bool>();
                        }
                        case From::Operation:
                        {
                            const auto it = j.find("operation");
                            const auto op = it != j.end() && it->is_string() ? OperationOf::Parse(it->get<std::string>()) : std::nullopt;
                            return op ? Json(LanguageOf::Word(m.language, OperationOf::Label(*op))) : Json(nullptr);
                        }
                        }
                    }
                // 它的类别的项目参数：自己的值；没有时，是否为否（同特性面板）
                if (const auto pp = m.parameters.find(name); pp != m.parameters.end() &&
                    std::find(pp->second.categories.begin(), pp->second.categories.end(), cat) != pp->second.categories.end())
                {
                    const auto* params = e.Params();
                    if (params)
                        if (const auto it = params->find(name); it != params->end())
                            return it->second;
                    return pp->second.kind == ParameterKind::YesNo ? Json(false) : Json(nullptr);
                }
                // 族的参数：构件与族的门窗
                const std::string* family = nullptr;
                if (const Component* cp = e.As<Component>())
                    family = &cp->family;
                else if (const Opening* op = e.AsOpening(); op && op->family)
                    family = &*op->family;
                if (family)
                    if (const auto f = m.families.find(*family); f != m.families.end())
                        for (const Fam::Parameter& p : f->second.parameters)
                        {
                            if (p.name != name)
                                continue;
                            Json v = nullptr;
                            if (const auto vs = j.find("values"); vs != j.end() && vs->is_object())
                                if (const auto it = vs->find(name); it != vs->end())
                                    v = *it;
                            if (p.kind == Fam::Kind::YesNo && v.is_number())
                                return v.get<double>() != 0.0;
                            if (p.kind == Fam::Kind::Material && v.is_string() && v.get<std::string>().empty())
                                return nullptr;
                            return v;
                        }
                return nullptr;
            }
        };

        // 类别 cat 的明细表每行对应什么，各带它的 JSON：类别的一个元素。体量楼层是体量穿过的每个标高，
        // 带那里的楼层面积、周长，以及到上一个这样的标高（或体量顶部）的体积
        struct Source
        {
            Id             id;
            const Element* e;
            Json           json;
        };

        std::vector<Source> Sources(const Model& m, std::string_view cat)
        {
            std::vector<Source> out;
            for (const auto& [id, e] : m.elements.Items())
            {
                if (cat != "massfloor")
                {
                    if (cat == e->Category())
                        out.push_back({ id, e.get(), ElementJson(m, id) });
                    continue;
                }
                const Mass* ms = e->As<Mass>();
                if (!ms)
                    continue;
                for (const Model::MassSlice& s : m.MassSlices(*ms))
                {
                    const Ring outline = m.MassOutline(*ms, s.z);
                    const double area = std::abs(Geom::Area(outline)), perimeter = Geom::Perimeter(outline);
                    Json j = { { "id", id },
                               { "mass", ms->name },
                               { "level", ToJson(m.GuidOf(s.level)) },
                               { "area_m2", std::round(area / 1e4) / 100.0 },
                               { "perimeter_m", std::round(perimeter / 10.0) / 100.0 },
                               { "volume_m3", std::round(m.MassVolume(*ms, s.z, s.z + s.height) / 1e7) / 100.0 } };
                    out.push_back({ id, e.get(), std::move(j) });
                }
            }
            return out;
        }

        // 它选的字段，没选时为类别的
        std::vector<Field> Chosen(const Schedule& sc, std::string_view cat)
        {
            return sc.fields.empty() ? Defaults(cat) : sc.fields;
        }

        Json Sum(const std::vector<const Json*>& values)
        {
            double s = 0.0;
            for (const Json* v : values)
                if (v->is_number())
                    s += v->get<double>();
            return Fine(s);
        }

        // 过滤器 f 是否留下存 kind 的字段的值 v：文字不分大小写；数照明细表在 units 下显示的比（显示相同的相等）；
        // 没有值的只有 has-no-value 与带"不"的留下
        bool Keeps(const Filter& f, const Json& v, std::optional<Kind> kind, Units units)
        {
            const bool empty = v.is_null() || (v.is_string() && v.get<std::string>().empty());
            const std::string a = Text::AsciiLower(Cell(v)), b = Text::AsciiLower(Cell(f.value));
            const auto x = Number(v), y = Number(f.value);
            const bool numbers = x && y && kind && *kind != Kind::Text;
            const auto order = [&]() -> int
            {
                if (numbers)
                {
                    if (Shown(units, *kind, *x) == Shown(units, *kind, *y))
                        return 0;
                    return *x < *y ? -1 : *x > *y ? 1 : 0;
                }
                return Natural(a, b);
            };
            const auto same = [&] { return numbers ? order() == 0 : a == b; };
            switch (f.op)
            {
            case Op::HasValue:      return !empty;
            case Op::HasNoValue:    return empty;
            case Op::NotEquals:     return empty || !same();
            case Op::NotContains:   return empty || a.find(b) == std::string::npos;
            case Op::NotBeginsWith: return empty || !Text::StartsWith(a, b);
            case Op::NotEndsWith:   return empty || !Text::EndsWith(a, b);
            default: break;
            }
            if (empty)
                return false;
            switch (f.op)
            {
            case Op::Equals:         return same();
            case Op::Contains:       return a.find(b) != std::string::npos;
            case Op::BeginsWith:     return Text::StartsWith(a, b);
            case Op::EndsWith:       return Text::EndsWith(a, b);
            case Op::Greater:        return order() > 0;
            case Op::GreaterOrEqual: return order() >= 0;
            case Op::Less:           return order() < 0;
            case Op::LessOrEqual:    return order() <= 0;
            default: return false;
            }
        }

        // 过滤器 flt 对存 kinds 的字段是否成立：有这个字段，按它存的那种值比较，值也是那种
        bool CheckFilter(const Filter& flt, const std::map<std::string, Kind>& kinds, const std::function<std::string(const std::string&)>& unknown,
                         std::string& error)
        {
            const auto it = kinds.find(flt.field);
            if (it == kinds.end())
                return (error = "filter: " + unknown(flt.field), false);
            const Kind k = it->second;
            const std::string who = "filter on `" + flt.field + "`";
            const bool textOp = flt.op == Op::Contains || flt.op == Op::NotContains || flt.op == Op::BeginsWith || flt.op == Op::NotBeginsWith ||
                                flt.op == Op::EndsWith || flt.op == Op::NotEndsWith;
            if (textOp && k != Kind::Text)
                return (error = who + ": it holds " + Holding(k) + ", and only text contains, begins or ends", false);
            if (k == Kind::YesNo && flt.op != Op::Equals && flt.op != Op::NotEquals && flt.op != Op::HasValue && flt.op != Op::HasNoValue)
                return (error = who + ": yes or no only equals or doesn't", false);
            const Json& v = flt.value;
            const auto smallNumber = [&] { return v.is_number() && std::isfinite(v.get<double>()) && std::abs(v.get<double>()) <= MaxCoord; };
            bool fits = false;
            if (flt.op == Op::HasValue || flt.op == Op::HasNoValue)
                fits = v.is_null() || v.is_boolean() || (v.is_string() && Text::CharCount(v.get<std::string>()) <= MaxText) || smallNumber();
            else if (k == Kind::Text)
                fits = v.is_string() && Text::CharCount(v.get<std::string>()) <= MaxText;
            else if (k == Kind::YesNo)
                fits = v.is_boolean();
            else
                fits = smallNumber();
            if (!fits)
            {
                const char* unit = k == Kind::Length ? " (mm)" : k == Kind::Area ? " (m²)" : "";
                return (error = who + ": it compares " + Holding(k) + unit + ", not " + Show(v), false);
            }
            return true;
        }

        std::function<std::string(const std::string&)> Unknown(const std::vector<std::pair<std::string, Kind>>& available, const std::string& cat)
        {
            std::vector<std::string> names;
            for (const auto& [n, _] : available)
                names.push_back(n);
            return [names, cat](const std::string& n) { return "`" + n + "` isn't a field of " + cat + "s; there are: " + Joined(names); };
        }
    }

    namespace KindOf
    {
        const char* Key(Kind k)
        {
            switch (k)
            {
            case Kind::Text:   return "text";
            case Kind::Number: return "number";
            case Kind::Length: return "length";
            case Kind::Area:   return "area";
            case Kind::YesNo:  return "yesno";
            }
            return "number";
        }

        std::optional<Kind> Parse(std::string_view s)
        {
            for (const Kind k : { Kind::Text, Kind::Number, Kind::Length, Kind::Area, Kind::YesNo })
                if (s == Key(k))
                    return k;
            return std::nullopt;
        }
    }

    namespace OpOf
    {
        namespace
        {
            constexpr std::pair<Op, const char*> kOps[] = {
                { Op::Equals, "equals" },
                { Op::NotEquals, "not-equals" },
                { Op::Greater, "greater" },
                { Op::GreaterOrEqual, "greater-or-equal" },
                { Op::Less, "less" },
                { Op::LessOrEqual, "less-or-equal" },
                { Op::Contains, "contains" },
                { Op::NotContains, "not-contains" },
                { Op::BeginsWith, "begins-with" },
                { Op::NotBeginsWith, "not-begins-with" },
                { Op::EndsWith, "ends-with" },
                { Op::NotEndsWith, "not-ends-with" },
                { Op::HasValue, "has-value" },
                { Op::HasNoValue, "has-no-value" },
            };
        }

        const char* Key(Op op)
        {
            for (const auto& [o, k] : kOps)
                if (o == op)
                    return k;
            return "equals";
        }

        std::optional<Op> Parse(std::string_view s)
        {
            for (const auto& [o, k] : kOps)
                if (s == k)
                    return o;
            return std::nullopt;
        }
    }

    std::optional<Builtin> BuiltinOf(const std::string* name, const std::string* category)
    {
        if (name)
        {
            const std::string n = Text::AsciiLower(Text::Trim(*name));
            for (const Builtin& b : Views)
                for (const Language l : LanguageOf::All)
                    if (Text::AsciiLower(LanguageOf::Word(l, b.name)) == n)
                        return b;
            return std::nullopt;
        }
        if (category)
            for (const Builtin& b : Views)
                if (*category == b.category)
                    return b;
        return std::nullopt;
    }

    std::optional<Id> Named(const Model& m, const std::string& name)
    {
        const auto twin = BuiltinOf(&name, nullptr);
        for (const auto& [id, e] : m.elements.Items())
            if (const Schedule* sc = e->As<Schedule>())
            {
                if (twin)
                {
                    const auto b = BuiltinOf(&sc->name, nullptr);
                    if (b && std::string_view(b->name) == twin->name)
                        return id;
                }
                else if (sc->name == name)
                    return id;
            }
        return std::nullopt;
    }

    std::pair<std::optional<Id>, Schedule> Standard(const Model& m, const std::string& cat)
    {
        const char* name = "";
        for (const Builtin& b : Views)
            if (cat == b.category)
                name = b.name;
        if (const auto id = Named(m, name))
            if (const Schedule* sc = m.elements.Get(*id)->As<Schedule>())
                return { id, *sc };
        Schedule sc;
        sc.name = LanguageOf::Word(m.language, name);
        sc.of   = cat;
        return { std::nullopt, sc };
    }

    std::vector<std::string> Categories()
    {
        std::vector<std::string> out;
        for (const Category& c : OwnTable())
            out.push_back(c.name);
        return out;
    }

    bool IsCategory(std::string_view cat)
    {
        for (const Category& c : OwnTable())
            if (cat == c.name)
                return true;
        return false;
    }

    bool OwnField(std::string_view cat, std::string_view name)
    {
        if (const Category* c = OwnOf(cat))
            for (const Own& o : c->fields)
                if (name == o.name)
                    return true;
        return false;
    }

    std::vector<std::pair<std::string, Kind>> Fields(const Model& m, std::string_view cat)
    {
        std::vector<std::pair<std::string, Kind>> out;
        std::set<std::string> seen;
        const auto add = [&](const std::string& n, Kind k)
        {
            if (seen.insert(n).second)
                out.emplace_back(n, k);
        };
        if (const Category* c = OwnOf(cat))
            for (const Own& o : c->fields)
                add(o.name, o.kind);
        // 它的类别的项目参数
        for (const auto& [n, p] : m.parameters)
            if (std::find(p.categories.begin(), p.categories.end(), cat) != p.categories.end())
                add(n, p.kind == ParameterKind::Text     ? Kind::Text
                     : p.kind == ParameterKind::Number   ? Kind::Number
                     : p.kind == ParameterKind::Length   ? Kind::Length
                                                         : Kind::YesNo);
        if (cat == "component")
            for (const auto& [_, f] : m.families)
                for (const Fam::Parameter& p : f.parameters)
                    add(p.name, p.kind == Fam::Kind::Length                                ? Kind::Length
                                : p.kind == Fam::Kind::YesNo                               ? Kind::YesNo
                                : p.kind == Fam::Kind::Material                            ? Kind::Text
                                                                                           : Kind::Number);
        return out;
    }

    std::optional<std::string> FieldText(const Model& m, Id id, const std::string& name)
    {
        const Element* e = m.elements.Get(id);
        if (!e)
            return std::nullopt;
        const std::string cat = e->Category();
        std::optional<Kind> kind;
        for (const auto& [n, k] : Fields(m, cat))
            if (n == name)
                kind = k;
        if (!kind)
            return std::nullopt;
        const std::map<std::string, std::pair<std::string, Kind>> formulas;
        Row row{ m, *e, cat, ElementJson(m, id), formulas, {} };
        const Json v = row.Get(name);
        if (v.is_null())
            return std::nullopt;
        return Shown(m.units, *kind, v);
    }

    std::vector<Field> Defaults(std::string_view cat)
    {
        std::vector<Field> out;
        if (const Category* c = OwnOf(cat))
            for (size_t i = 0; i < c->shown && i < c->fields.size(); ++i)
                out.push_back(Field{ c->fields[i].name });
        return out;
    }

    bool Check(const Model& m, const Schedule& sc, std::string& error)
    {
        const std::string cat = NormalizeCategory(sc.of);
        if (!IsCategory(cat))
            return (error = "there's no schedule of `" + sc.of + "`; there are schedules of: " + Joined(Categories()), false);
        if (const auto b = BuiltinOf(&sc.name, nullptr); b && cat != b->category)
            return (error = "`" + sc.name + "` is the " + b->category + "s' " + b->name + " every project has; call a schedule of " + cat +
                            "s something else",
                    false);
        for (const auto& [what, n] : { std::pair<const char*, size_t>{ "fields", sc.fields.size() }, { "filters", sc.filters.size() }, { "sort fields", sc.sort.size() } })
            if (n > MaxFields)
                return (error = "it has " + std::to_string(n) + " " + what + "; a schedule has up to " + std::to_string(MaxFields), false);
        const auto available = Fields(m, cat);
        std::map<std::string, Kind> kinds(available.begin(), available.end());
        const auto unknown = Unknown(available, cat);
        const std::vector<Field> fields = Chosen(sc, cat);
        std::set<std::string> names;
        for (const Field& f : fields)
        {
            if (!names.insert(f.name).second)
                return (error = "field `" + f.name + "` is in it twice", false);
            if (f.heading && Text::CharCount(*f.heading) > MaxText)
                return (error = "field `" + f.name + "`: a heading is up to " + std::to_string(MaxText) + " characters", false);
            if (!f.formula)
            {
                if (!kinds.count(f.name))
                    return (error = unknown(f.name), false);
                if (f.kind)
                    return (error = "field `" + f.name + "`: only a calculated field says what it holds; give it a formula", false);
                continue;
            }
            const std::string who = "calculated field `" + f.name + "`";
            if (!Fam::Nameable(f.name))
                return (error = who + ": a name starts with a letter and holds letters, digits, `_` and spaces, up to " + std::to_string(MaxText), false);
            if (kinds.count(f.name))
                return (error = who + ": " + cat + "s have a field of that name; call it something else", false);
            if (f.kind == Kind::Text)
                return (error = who + ": a formula comes to a number, a length, an area or yes or no", false);
            if (Text::CharCount(*f.formula) > Fam::MaxFormula)
                return (error = who + ": a formula is up to " + std::to_string(Fam::MaxFormula) + " characters", false);
        }
        std::map<std::string, std::string> formulas;
        for (const Field& f : fields)
            if (f.formula)
            {
                formulas[f.name] = *f.formula;
                kinds[f.name]    = f.kind.value_or(Kind::Number);
            }
        Follower follower{ formulas, kinds, unknown, {}, {} };
        for (const auto& [name, _] : formulas)
        {
            size_t depth = 0;
            if (!follower.Follow(name, depth, error))
                return false;
        }
        for (const Field& f : fields)
            if (f.total)
            {
                const Kind k = kinds.count(f.name) ? kinds[f.name] : Kind::Number;
                if (k != Kind::Number && k != Kind::Length && k != Kind::Area)
                    return (error = "field `" + f.name + "` holds " + Holding(k) + "; only numbers, lengths and areas total", false);
            }
        for (const Filter& flt : sc.filters)
            if (!CheckFilter(flt, kinds, unknown, error))
                return false;
        for (const Sort& s : sc.sort)
            if (!kinds.count(s.field))
                return (error = "sorting: " + unknown(s.field), false);
        return true;
    }

    bool CheckRules(const Model& m, const std::vector<std::string>& cats, const std::vector<Filter>& rules, std::string& error)
    {
        if (rules.size() > MaxFields)
            return (error = "it has " + std::to_string(rules.size()) + " rules; a filter has up to " + std::to_string(MaxFields), false);
        for (const std::string& cat : cats)
        {
            const auto available = Fields(m, cat);
            const std::map<std::string, Kind> kinds(available.begin(), available.end());
            const auto unknown = Unknown(available, cat);
            for (const Filter& flt : rules)
                if (!CheckFilter(flt, kinds, unknown, error))
                    return false;
        }
        return true;
    }

    std::set<Id> Picks(const Model& m, const std::vector<std::string>& cats, const std::vector<Filter>& rules)
    {
        std::map<std::string, std::map<std::string, Kind>> kinds;
        for (const std::string& c : cats)
        {
            const auto f = Fields(m, c);
            kinds[c] = std::map<std::string, Kind>(f.begin(), f.end());
        }
        const std::map<std::string, std::pair<std::string, Kind>> formulas;
        std::set<Id> out;
        for (const auto& [id, e] : m.elements.Items())
        {
            const auto of = kinds.find(e->Category());
            if (of == kinds.end())
                continue;
            if (rules.empty())
            {
                out.insert(id);
                continue;
            }
            Row row{ m, *e, e->Category(), ElementJson(m, id), formulas, {} };
            const bool all = std::all_of(rules.begin(), rules.end(), [&](const Filter& f)
            {
                const auto k = of->second.find(f.field);
                return Keeps(f, row.Get(f.field), k == of->second.end() ? std::nullopt : std::optional<Kind>(k->second), m.units);
            });
            if (all)
                out.insert(id);
        }
        return out;
    }

    bool TableOf(const Model& m, const Schedule& sc, Table& out, std::string& error)
    {
        const std::string cat = NormalizeCategory(sc.of);
        if (!IsCategory(cat))
            return (error = "no schedule of `" + sc.of + "`; there are schedules of: " + Joined(Categories()), false);
        const auto available = Fields(m, cat);
        const std::vector<Field> fields = Chosen(sc, cat);
        std::map<std::string, std::pair<std::string, Kind>> formulas;
        for (const Field& f : fields)
            if (f.formula)
                formulas[f.name] = { *f.formula, f.kind.value_or(Kind::Number) };
        const auto availableKind = [&](const std::string& n) -> std::optional<Kind>
        {
            for (const auto& [a, k] : available)
                if (a == n)
                    return k;
            return std::nullopt;
        };
        out = Table{};
        for (const Field& f : fields)
            out.columns.push_back({ f.heading.value_or(f.name), f.formula ? f.kind.value_or(Kind::Number) : availableKind(f.name).value_or(Kind::Text) });
        const auto kindOf = [&](const std::string& n) -> std::optional<Kind>
        {
            if (const auto it = formulas.find(n); it != formulas.end())
                return it->second.second;
            return availableKind(n);
        };
        std::vector<std::pair<std::string, bool>> sort;
        if (sc.sort.empty() && !sc.grouped && !fields.empty())
            sort.emplace_back(fields.front().name, false);
        else
            for (const Sort& s : sc.sort)
                sort.emplace_back(s.field, s.descending);
        // 过滤器留下的每个元素：它的格子、排序字段的值、它自己
        struct Line
        {
            std::vector<Json> cells, keys;
            Id                id;
        };
        std::vector<Line> rows;
        for (Source& src : Sources(m, cat))
        {
            Row row{ m, *src.e, cat, std::move(src.json), formulas, {} };
            const bool kept = std::all_of(sc.filters.begin(), sc.filters.end(),
                                          [&](const Filter& f) { return Keeps(f, row.Get(f.field), kindOf(f.field), m.units); });
            if (!kept)
                continue;
            Line line{ {}, {}, src.id };
            for (const Field& f : fields)
                line.cells.push_back(row.Get(f.name));
            for (const auto& [n, _] : sort)
                line.keys.push_back(row.Get(n));
            rows.push_back(std::move(line));
        }
        std::stable_sort(rows.begin(), rows.end(), [&](const Line& a, const Line& b)
        {
            for (size_t i = 0; i < sort.size(); ++i)
            {
                const int o = Natural(a.keys[i], b.keys[i]);
                if (o != 0)
                    return sort[i].second ? o > 0 : o < 0;
            }
            return false;
        });
        const auto summed = [](const Field& f) { return f.total || (!f.formula && f.name == "Count"); };
        if (std::any_of(fields.begin(), fields.end(), [](const Field& f) { return f.total; }))
        {
            std::vector<Json> totals;
            for (size_t i = 0; i < fields.size(); ++i)
            {
                if (!fields[i].total)
                {
                    totals.push_back(nullptr);
                    continue;
                }
                std::vector<const Json*> column;
                for (const Line& r : rows)
                    column.push_back(&r.cells[i]);
                totals.push_back(Sum(column));
            }
            out.totals = std::move(totals);
        }
        if (!sc.grouped)
        {
            for (Line& r : rows)
            {
                out.rows.push_back(std::move(r.cells));
                out.ids.push_back({ r.id });
            }
            return true;
        }
        // 成组：排序字段的值相同的连着的行合成一行，计数与合计的相加，值不同的字段空着
        std::vector<std::pair<std::vector<const Line*>, std::vector<Id>>> groups;
        const Line* last = nullptr;
        for (const Line& r : rows)
        {
            bool same = last && !groups.empty();
            if (same)
                for (size_t i = 0; i < r.keys.size(); ++i)
                    if (Natural(last->keys[i], r.keys[i]) != 0)
                        same = false;
            if (same)
            {
                groups.back().first.push_back(&r);
                groups.back().second.push_back(r.id);
            }
            else
                groups.push_back({ { &r }, { r.id } });
            last = &r;
        }
        for (const auto& [g, ids] : groups)
        {
            std::vector<Json> cells;
            for (size_t i = 0; i < fields.size(); ++i)
            {
                std::vector<const Json*> column;
                for (const Line* r : g)
                    column.push_back(&r->cells[i]);
                if (summed(fields[i]))
                    cells.push_back(Sum(column));
                else
                {
                    const Json first = column.empty() ? Json(nullptr) : *column.front();
                    const bool all = std::all_of(column.begin(), column.end(), [&](const Json* v) { return Natural(*v, first) == 0; });
                    cells.push_back(all ? first : Json(nullptr));
                }
            }
            out.rows.push_back(std::move(cells));
            out.ids.push_back(ids);
        }
        return true;
    }

    std::string Heading(Units units, const Column& column)
    {
        if (column.kind == Kind::Area)
            return column.heading + " (" + UnitsOf::Area(units, 0.0).second + ")";
        return column.heading;
    }

    std::string Shown(Units units, Kind kind, const Json& v)
    {
        if (v.is_boolean())
            return v.get<bool>() ? "Yes" : "No";
        if (v.is_number())
        {
            const double x = v.get<double>();
            if (kind == Kind::Length)
                return UnitsOf::Length(units, x);
            if (kind == Kind::Area)
            {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.2f", UnitsOf::Area(units, x * 1e6).first);
                return buf;
            }
            return Text::Shortest(std::round(x * 100.0) / 100.0 + 0.0);
        }
        return Cell(v);
    }

    int NaturalText(const std::string& a, const std::string& b)
    {
        return Natural(a, b);
    }

    std::string ToCsv(const Table& t)
    {
        const auto quote = [](std::string s)
        {
            if (s.find_first_of(",\"\n\r") == std::string::npos)
                return s;
            return "\"" + Text::ReplaceAll(s, "\"", "\"\"") + "\"";
        };
        const auto text = [](std::string s)
        {
            if (!s.empty() && std::string_view("=+-@\t\r").find(s.front()) != std::string_view::npos)
                return "'" + s;
            return s;
        };
        std::string out;
        for (size_t i = 0; i < t.columns.size(); ++i)
            out += (i ? "," : "") + quote(text(Heading(Units::Millimetres, t.columns[i])));
        out += '\n';
        const auto line = [&](const std::vector<Json>& row)
        {
            for (size_t i = 0; i < row.size(); ++i)
                out += (i ? "," : "") + quote(row[i].is_string() ? text(Cell(row[i])) : Cell(row[i]));
            out += '\n';
        };
        for (const auto& row : t.rows)
            line(row);
        if (t.totals)
            line(*t.totals);
        return out;
    }
}
