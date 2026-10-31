// 移植自 BIMCraft crates/ui-egui/src/typeprops.rs、properties.rs（f3bbb33）的类型属性、族类型、材质对话框，MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Commands/Commands.h"
#include "Core/UIContext.h"
#include "GUI/Parameters.h"
#include "I18n/Translator.h"
#include "Style/Theme.hpp"
#include "Widgets/Button.h"
#include "Widgets/ColorPicker.h"
#include "Widgets/ComboBox.h"
#include "Widgets/Controls.h"
#include "Widgets/Dialog.h"
#include "Widgets/Label.h"
#include "Widgets/NumberBox.h"
#include "Widgets/PropertyGrid.h"
#include "Widgets/TextBox.h"
#include "Core/Log.h"
#include "Model/Text.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

// 对话框，照 BIMCraft（同 Revit）：
//   - 类型属性：族（系统族的类别）与类型，旁边"复制…""重命名…"；类型参数的表（按组）；墙、楼板、屋顶、天花板还有层的编辑
//     （功能、材质、厚度，外 / 内或上 / 下移动、删除、插入）；确定、取消、应用。都经 type、rename_type 命令
//   - 族类型：族的类型参数（长度、数、角度、是否、材质；公式算出的只显示），复制、重命名；经 family、rename_type 命令
//   - 材质：选一个材质，新建、删除，名称、颜色、截面与表面图案；每个改动是一条 material 命令
// 打开时编辑的是选中的元素的类型，没选东西时是当前工具要放的类型。确定后选中的元素换成对话框里显示的类型（没选东西时工具改放它）
namespace MiniBIM
{
    namespace
    {
        using namespace MiniGUI;

        // 一行：名称 + 控件
        Node* Row(Node* parent, float gap = 8.0f)
        {
            auto* row = parent->AddChild<Node>();
            LayoutStyle s;
            s.direction  = FlexDirection::Row;
            s.alignItems = Align::Center;
            s.gap        = gap;
            row->SetLayoutStyle(s);
            return row;
        }

        template <class T>
        T* Sized(T* node, float width)
        {
            LayoutStyle& s = node->EditLayoutStyle();
            s.width = width;
            return node;
        }

        Label* Caption(Node* parent, const std::string& text, float width = 70.0f)
        {
            return Sized(parent->AddChild<Label>(text, 13.0f), width);
        }

        const std::pair<Bim::Function, const char*> Functions[] = {
            { Bim::Function::Structure, N_("Structure") }, { Bim::Function::Substrate, N_("Substrate") }, { Bim::Function::Thermal, N_("Thermal") },
            { Bim::Function::Membrane, N_("Membrane") },   { Bim::Function::Finish, N_("Finish") },
        };

        const std::pair<Bim::Pattern, const char*> Patterns[] = {
            { Bim::Pattern::None, N_("None") },              { Bim::Pattern::Solid, N_("Solid Fill") },   { Bim::Pattern::Diagonal, N_("Diagonal") },
            { Bim::Pattern::Crosshatch, N_("Crosshatch") },  { Bim::Pattern::Horizontal, N_("Horizontal") }, { Bim::Pattern::Vertical, N_("Vertical") },
            { Bim::Pattern::Grid, N_("Square Grid") },       { Bim::Pattern::Brick, N_("Brick") },        { Bim::Pattern::Concrete, N_("Concrete") },
            { Bim::Pattern::Insulation, N_("Insulation") },
        };

        // 类型的参数：组、字段、名称（英文原文）
        struct TypeRow { const char* group; const char* key; const char* label; };
        std::vector<TypeRow> TypeRows(const std::string& category)
        {
            if (category == "wall") return { { N_("Construction"), "thickness", N_("Width") } };
            if (category == "floor") return { { "Construction", "thickness", N_("Default Thickness") } };
            if (category == "door") return { { "Dimensions", "width", N_("Width") }, { "Dimensions", "height", N_("Height") } };
            if (category == "window")
                return { { "Dimensions", "width", "Width" }, { "Dimensions", "height", "Height" }, { "Construction", "sill", N_("Default Sill Height") } };
            if (category == "roof") return { { "Construction", "thickness", N_("Thickness") } };
            if (category == "ceiling") return { { "Construction", "thickness", "Thickness" }, { "Construction", "grid", N_("Tile Size") } };
            if (category == "column" || category == "structuralcolumn" || category == "footing" || Bim::IsRunCategory(category))
                return { { "Dimensions", "width", "Width" }, { "Dimensions", "depth", N_("Depth") } };
            return {};
        }

        double TypeValue(const Bim::Model& m, const std::string& category, const std::string& name, const std::string& key)
        {
            auto get = [&](const auto& table, auto f) { const auto it = table.find(name); return it == table.end() ? 0.0 : f(it->second); };
            if (category == "wall") return get(m.types.wall, [](const auto& t) { return t.thickness; });
            if (category == "floor") return get(m.types.floor, [](const auto& t) { return t.thickness; });
            if (category == "roof") return get(m.types.roof, [](const auto& t) { return t.thickness; });
            if (category == "ceiling") return get(m.types.ceiling, [&](const auto& t) { return key == "grid" ? t.grid : t.thickness; });
            if (category == "column" || category == "structuralcolumn" || category == "footing")
                return get(category == "column" ? m.types.column : category == "footing" ? m.types.footing : m.types.structuralColumn,
                           [&](const auto& t) { return key == "width" ? t.width : t.depth; });
            if (Bim::IsRunCategory(category))
            {
                const auto runs = m.types.runs.find(category);
                return runs == m.types.runs.end() ? 0.0 : get(runs->second, [&](const auto& t) { return key == "width" ? t.width : t.depth; });
            }
            const auto& openings = category == "door" ? m.types.door : m.types.window;
            return get(openings, [&](const auto& t) { return key == "width" ? t.width : key == "height" ? t.height : t.sill; });
        }

        bool Layered(const std::string& category) { return category == "wall" || category == "floor" || category == "roof" || category == "ceiling"; }

        struct LayerRow
        {
            Bim::Function function = Bim::Function::Structure;
            std::string   material;
            std::string   thickness;
            bool operator==(const LayerRow&) const = default;
        };

        enum class Naming { None, Duplicate, Rename };

        // 类型属性对话框的状态：显示的类型，参数与层照输入框里的文字
        struct TypeState
        {
            std::string category;
            std::string name;
            std::vector<Bim::Id> elements;
            std::vector<std::pair<TypeRow, std::string>> fields;
            std::vector<LayerRow> layers, layersWere;
            // run 的类型：截面是否圆的（圆的没有进深，宽度是直径）、用库里哪种材质（空为没有）
            std::optional<std::pair<bool, std::string>> shape;
            Naming      naming = Naming::None;
            std::string naminText;
            std::string error;
            Dialog*     dialog = nullptr;
        };

        void LoadType(TypeState& d, const Bim::Model& m, const std::string& name)
        {
            d.name = name;
            d.fields.clear();
            for (const TypeRow& r : TypeRows(d.category))
                d.fields.emplace_back(r, Bim::UnitsOf::Length(m.units, TypeValue(m, d.category, name, r.key)));
            d.layers.clear();
            for (const Bim::Model::Layered& t : m.LayeredTypes())
                if (t.category == d.category && *t.name == name)
                {
                    for (const Bim::Layer& l : *t.layers)
                        d.layers.push_back({ l.function, l.material, Bim::UnitsOf::Length(m.units, l.thickness) });
                    // 没有层的：一层结构，与它一样厚
                    if (d.layers.empty())
                        d.layers.push_back({ Bim::Function::Structure, {}, Bim::UnitsOf::Length(m.units, t.thickness) });
                }
            d.layersWere = d.layers;
            d.shape.reset();
            if (Bim::IsRunCategory(d.category))
            {
                const Bim::RunType* t = nullptr;
                if (const auto runs = m.types.runs.find(d.category); runs != m.types.runs.end())
                    if (const auto it = runs->second.find(name); it != runs->second.end())
                        t = &it->second;
                d.shape = std::pair{ t && t->round, t ? t->material : std::string() };
            }
            d.naming = Naming::None;
            d.error.clear();
        }

        // type 命令的参数：改了层时（或复制时）带上层，厚度由层定
        bool TypeParams(const TypeState& d, const std::string& name, Bim::Units units, bool copy, Bim::Json& p, std::string& error)
        {
            p = Bim::Json{ { "category", d.category }, { "name", name } };
            const bool layered = Layered(d.category) && (copy || d.layers != d.layersWere);
            if (layered)
            {
                Bim::Json layers = Bim::Json::array();
                for (const LayerRow& l : d.layers)
                {
                    const auto t = Bim::UnitsOf::Parse(units, l.thickness);
                    if (!t)
                    {
                        error = Format(Tr("{0}: `{1}` is not a length"), { Tr("Thickness"), l.thickness });
                        return false;
                    }
                    layers.push_back(Bim::Json{ { "function", Bim::FunctionOf::Name(l.function) }, { "material", l.material }, { "thickness", *t } });
                }
                p["layers"] = std::move(layers);
            }
            if (d.shape)
            {
                p["round"]    = d.shape->first;
                p["material"] = d.shape->second;
            }
            for (const auto& [r, typed] : d.fields)
            {
                if (layered && std::string(r.key) == "thickness")
                    continue;
                // 圆的进深即直径
                if (d.shape && d.shape->first && std::string(r.key) == "depth")
                    continue;
                const auto v = Bim::UnitsOf::Parse(units, typed);
                if (!v)
                {
                    error = Format(Tr("{0}: `{1}` is not a length"), { Tr(r.label), typed });
                    return false;
                }
                p[r.key] = *v;
            }
            return true;
        }

        // 族类型对话框的状态
        struct FamilyState
        {
            std::string category;     // component、door、window
            std::string family;
            std::string name;
            std::vector<Bim::Id> elements;
            struct Field { std::string name; Bim::Fam::Kind kind; std::string typed; bool computed; };
            std::vector<Field> fields;
            Naming      naming = Naming::None;
            std::string naminText;
            std::string error;
            Dialog*     dialog = nullptr;
        };

        void LoadFamily(FamilyState& d, const Bim::Model& m, const std::string& name)
        {
            d.name = name;
            d.fields.clear();
            const auto f = m.families.find(d.family);
            if (f != m.families.end())
            {
                Bim::Fam::Values values;
                std::string why;
                f->second.Values(name, {}, values, why);
                for (const Bim::Fam::Parameter& p : f->second.parameters)
                {
                    if (p.instance)
                        continue;
                    std::string typed;
                    if (const auto v = values.find(p.name); v != values.end())
                    {
                        if (const double* x = std::get_if<double>(&v->second))
                            typed = p.kind == Bim::Fam::Kind::Length ? Bim::UnitsOf::Length(m.units, *x)
                                  : p.kind == Bim::Fam::Kind::YesNo  ? (*x != 0.0 ? "1" : "0")
                                                                      : Props::Text(std::round(*x * 1e6) / 1e6);
                        else
                            typed = std::get<std::string>(v->second);
                    }
                    d.fields.push_back({ p.name, p.kind, typed, p.formula.has_value() });
                }
            }
            d.naming = Naming::None;
            d.error.clear();
        }

        bool FamilyParams(const FamilyState& d, const std::string& name, Bim::Units units, Bim::Json& p, std::string& error)
        {
            Bim::Json set = Bim::Json::object();
            for (const auto& f : d.fields)
            {
                if (f.computed)
                    continue;
                Bim::Json v;
                switch (f.kind)
                {
                case Bim::Fam::Kind::Length:
                {
                    const auto x = Bim::UnitsOf::Parse(units, f.typed);
                    if (!x)
                    {
                        error = Format(Tr("{0}: `{1}` is not a length"), { f.name, f.typed });
                        return false;
                    }
                    v = *x;
                    break;
                }
                case Bim::Fam::Kind::Number: case Bim::Fam::Kind::Angle:
                    if (!Props::Parse(Props::Kind::Number, f.typed, units, v, error))
                    {
                        error = Format(Tr("{0}: `{1}` is not a number"), { f.name, f.typed });
                        return false;
                    }
                    break;
                case Bim::Fam::Kind::YesNo:
                    v = f.typed == "1";
                    break;
                case Bim::Fam::Kind::Material:
                    v = f.typed;
                    break;
                }
                set[f.name] = std::move(v);
            }
            p = Bim::Json{ { "name", d.family }, { "types", Bim::Json{ { name, std::move(set) } } } };
            return true;
        }
    }

    // =========================================================
    // 要编辑的类型
    // =========================================================
    std::optional<MainFrame::TypeSubject> MainFrame::CurrentTypeSubject() const
    {
        const Bim::Model& m = m_session.GetModel();
        auto typed = [](const std::string& c)
        {
            return c == "wall" || c == "floor" || c == "roof" || c == "ceiling" || c == "door" || c == "window" || c == "component" ||
                   c == "column" || c == "structuralcolumn" || c == "footing" || Bim::IsRunCategory(c);
        };
        TypeSubject t;
        if (!m_selection.empty())
        {
            std::set<std::pair<std::string, std::string>> pairs;   // (族, 类型)
            std::set<std::string> categories;
            for (const Bim::Id id : m_selection)
            {
                const Bim::Json e = Bim::ElementJson(m, id);
                categories.insert(e.value("category", std::string()));
                pairs.insert({ e.contains("family") && e["family"].is_string() ? e["family"].get<std::string>() : std::string(), e.value("type", std::string()) });
            }
            if (categories.size() != 1 || !typed(*categories.begin()) || pairs.size() != 1)
                return std::nullopt;
            t.category = *categories.begin();
            if (!pairs.begin()->first.empty())
                t.family = pairs.begin()->first;
            t.name     = pairs.begin()->second;
            t.elements = m_selection;
            return t;
        }
        const char* c = ToolCategory();
        if (!c || !typed(c))
            return std::nullopt;
        t.category = c;
        if (t.category == "component")
        {
            const auto what = Placing();
            if (!what)
                return std::nullopt;
            t.family = what->first;
            t.name   = what->second;
            return t;
        }
        if (const auto f = m_toolOpt.openingFamilies.find(t.category); f != m_toolOpt.openingFamilies.end())
            t.family = f->second;
        if (const auto ty = m_toolOpt.types.find(t.category); ty != m_toolOpt.types.end())
            t.name = ty->second;
        else if (const auto d = m.DefaultType(t.category))
            t.name = *d;
        else
            return std::nullopt;
        return t;
    }

    void MainFrame::EditType()
    {
        if (const auto t = CurrentTypeSubject())
        {
            if (t->family)
                OpenFamilyDialog(*t);
            else
                OpenTypeDialog(*t);
        }
    }

    bool MainFrame::RunQuiet(const char* id, const Bim::Json& params, std::string& error, Bim::Json* result)
    {
        Bim::Json out;
        error.clear();
        const bool ok = m_session.Execute(id, params, out, error);
        if (!ok)
            LOG_WARN("命令 %s 失败：%s", id, error.c_str());
        else if (result)
            *result = std::move(out);
        OnModelChanged();
        return ok;
    }

    // =========================================================
    // 类型属性
    // =========================================================
    void MainFrame::OpenTypeDialog(const TypeSubject& t)
    {
        auto d = std::make_shared<TypeState>();
        d->category = t.category;
        d->elements = t.elements;
        LoadType(*d, m_session.GetModel(), t.name);

        auto dialog = std::make_unique<Dialog>(Tr("Type Properties"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        // 应用：按输入改类型；成功时重读
        auto apply = [this, d]
        {
            const Bim::Units units = m_session.GetModel().units;
            Bim::Json p;
            if (!TypeParams(*d, d->name, units, false, p, d->error) || !RunQuiet("type", p, d->error))
                return false;
            LoadType(*d, m_session.GetModel(), d->name);
            return true;
        };
        *build = [this, d, rebuild]
        {
            const Bim::Model& m = m_session.GetModel();
            Node* body = d->dialog->GetBody();
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction = FlexDirection::Column;
            bs.gap       = 6.0f;
            bs.width     = 600.0f;

            // ── 族与类型，复制、重命名 ──
            Node* row = Row(body);
            Caption(row, Tr("Family:"));
            row->AddChild<Label>(Format(Tr("System Family: {0}"), { Tr(Props::CategoryName(d->category, true)) }), 13.0f);
            row = Row(body);
            Caption(row, Tr("Type:"));
            const std::vector<std::string> names = m.TypeNames(d->category);
            const auto at = std::find(names.begin(), names.end(), d->name);
            auto* types = Sized(row->AddChild<ComboBox>(names, at == names.end() ? -1 : static_cast<int>(at - names.begin())), 300.0f);
            types->SetOnChanged([this, d, names, rebuild](int i)
            {
                if (i >= 0)
                {
                    LoadType(*d, m_session.GetModel(), names[static_cast<size_t>(i)]);
                    rebuild();
                }
            });
            Sized(row->AddChild<Button>(Tr("Duplicate..."), [d, rebuild]
            {
                d->naming    = d->naming == Naming::Duplicate ? Naming::None : Naming::Duplicate;
                d->naminText = d->name + " 2";
                rebuild();
            }), 100.0f);
            Sized(row->AddChild<Button>(Tr("Rename..."), [d, rebuild]
            {
                d->naming    = d->naming == Naming::Rename ? Naming::None : Naming::Rename;
                d->naminText = d->name;
                rebuild();
            }), 100.0f);
            if (d->naming != Naming::None)
            {
                row = Row(body);
                Caption(row, Tr("Name:"));
                auto* box = Sized(row->AddChild<TextBox>(d->naminText), 300.0f);
                box->SetOnChanged([d](const std::string& s) { d->naminText = s; });
                Sized(row->AddChild<Button>(d->naming == Naming::Duplicate ? Tr("Create") : Tr("Rename"), [this, d, rebuild]
                {
                    const std::string to(Bim::Text::Trim(d->naminText));
                    if (to.empty())
                        return;
                    const Bim::Model& mm = m_session.GetModel();
                    if (d->naming == Naming::Rename)
                    {
                        if (RunQuiet("rename_type", Bim::Json{ { "category", d->category }, { "type", d->name }, { "name", to } }, d->error))
                        {
                            if (m_toolOpt.types.count(d->category) && m_toolOpt.types[d->category] == d->name)
                                m_toolOpt.types[d->category] = to;
                            LoadType(*d, m_session.GetModel(), to);
                        }
                    }
                    else
                    {
                        Bim::Json p;
                        // 复制的类型带上层（与原来的一样，或改过的）
                        if (TypeParams(*d, to, mm.units, true, p, d->error) && RunQuiet("type", p, d->error))
                            LoadType(*d, m_session.GetModel(), p["name"].get<std::string>());
                    }
                    rebuild();
                }), 90.0f);
            }

            // ── 类型参数 ──
            body->AddChild<Label>(Tr("Type Parameters"), 13.0f)->SetBold(true);
            auto* grid = body->AddChild<PropertyGrid>();
            grid->EditLayoutStyle().height = 120.0f;
            grid->SetNameWidth(220.0f);
            std::vector<std::string> groups;
            for (const auto& [r, _] : d->fields)
                if (std::find(groups.begin(), groups.end(), r.group) == groups.end())
                    groups.emplace_back(r.group);
            const bool layered = Layered(d->category);
            for (const std::string& g : groups)
            {
                Expander* e = grid->AddGroup(Tr(g));
                for (auto& [r, typed] : d->fields)
                {
                    if (r.group != g)
                        continue;
                    // 复合的：厚度由层定；圆的 run 没有进深
                    if ((layered && std::string(r.key) == "thickness") || (d->shape && d->shape->first && std::string(r.key) == "depth"))
                    {
                        grid->AddProperty<Label>(e, Tr(r.label), typed, 13.0f, Theme::TextDim);
                        continue;
                    }
                    auto* box = grid->AddProperty<TextBox>(e, Tr(r.label), typed);
                    box->SetPlaceholder(Bim::UnitsOf::Name(m.units));
                    std::string* into = &typed;
                    box->SetOnChanged([into](const std::string& s) { *into = s; });
                }
            }

            // ── run 的截面：圆的、材质 ──
            if (d->shape)
            {
                row = Row(body);
                auto* round = row->AddChild<CheckBox>(Tr("Round"), d->shape->first);
                round->SetOnChanged([d, rebuild](bool on)
                {
                    d->shape->first = on;
                    rebuild();
                });
                Caption(row, Tr("Material:"));
                std::vector<std::string> materials{ Tr("(none)") };
                for (const auto& [n, _] : m.materials)
                    materials.push_back(n);
                const auto mi = std::find(materials.begin() + 1, materials.end(), d->shape->second);
                auto* mat = Sized(row->AddChild<ComboBox>(materials, d->shape->second.empty() || mi == materials.end() ? 0 : static_cast<int>(mi - materials.begin())), 200.0f);
                mat->SetMaxVisibleItems(14);
                mat->SetOnChanged([d, materials](int i) { if (i >= 0) d->shape->second = i == 0 ? std::string() : materials[static_cast<size_t>(i)]; });
            }

            // ── 层：墙从外到内，楼板、屋顶、天花板从上到下 ──
            if (layered)
            {
                const bool wall = d->category == "wall";
                body->AddChild<Label>(Tr("Layers"), 13.0f)->SetBold(true);
                row = Row(body);
                Sized(row->AddChild<Label>(Tr("Function"), 12.0f, Theme::TextDim), 120.0f);
                Sized(row->AddChild<Label>(Tr("Material"), 12.0f, Theme::TextDim), 170.0f);
                Sized(row->AddChild<Label>(Tr("Thickness"), 12.0f, Theme::TextDim), 80.0f);
                std::vector<std::string> functions, materials{ Tr("(none)") };
                for (const auto& [_, n] : Functions)
                    functions.push_back(Tr(n));
                for (const auto& [n, _] : m.materials)
                    materials.push_back(n);
                for (size_t k = 0; k < d->layers.size(); ++k)
                {
                    LayerRow& l = d->layers[k];
                    row = Row(body, 6.0f);
                    int fi = 0;
                    for (size_t i = 0; i < std::size(Functions); ++i)
                        if (Functions[i].first == l.function)
                            fi = static_cast<int>(i);
                    auto* fn = Sized(row->AddChild<ComboBox>(functions, fi), 120.0f);
                    fn->SetOnChanged([d, k](int i) { if (i >= 0) d->layers[k].function = Functions[static_cast<size_t>(i)].first; });
                    const auto mi = std::find(materials.begin() + 1, materials.end(), l.material);
                    auto* mat = Sized(row->AddChild<ComboBox>(materials, l.material.empty() || mi == materials.end() ? 0 : static_cast<int>(mi - materials.begin())), 170.0f);
                    mat->SetMaxVisibleItems(14);
                    mat->SetOnChanged([d, k, materials](int i) { if (i >= 0) d->layers[k].material = i == 0 ? std::string() : materials[static_cast<size_t>(i)]; });
                    auto* th = Sized(row->AddChild<TextBox>(l.thickness), 80.0f);
                    th->SetOnChanged([d, k](const std::string& s) { d->layers[k].thickness = s; });
                    auto move = [d, k, rebuild](int by)
                    {
                        const size_t to = k + static_cast<size_t>(by);
                        if (by == 0)
                            d->layers.erase(d->layers.begin() + static_cast<long>(k));
                        else
                            std::swap(d->layers[k], d->layers[to]);
                        rebuild();
                    };
                    row->AddChild<Button>(Tr(wall ? N_("Out") : N_("Up")), [move] { move(-1); })->SetEnabled(k > 0);
                    row->AddChild<Button>(Tr(wall ? N_("In") : N_("Down")), [move] { move(1); })->SetEnabled(k + 1 < d->layers.size());
                    row->AddChild<Button>(Tr("Delete"), [move] { move(0); })->SetEnabled(d->layers.size() > 1);
                }
                // 外侧（上面）插一层 10 毫米的面层
                auto* insert = body->AddChild<Button>(Tr("Insert Layer"), [this, d, rebuild]
                {
                    d->layers.insert(d->layers.begin(), { Bim::Function::Finish, {}, Bim::UnitsOf::Length(m_session.GetModel().units, 10.0) });
                    rebuild();
                });
                insert->EditLayoutStyle().alignSelf = Align::Start;
            }
            if (!d->error.empty())
                body->AddChild<Label>(d->error, 12.0f, Theme::Danger)->SetWrap(true);
        };

        Dialog* raw = d->dialog;
        auto okAction = [this, d, apply, rebuild]
        {
            // 改不成时留着，显示原因
            if (!apply())
            {
                rebuild();
                return;
            }
            // 选中的元素换成这个类型；没选东西时工具改放它
            if (d->elements.empty())
            {
                m_toolOpt.types[d->category] = d->name;
                m_toolOpt.openingFamilies.erase(d->category);
                RefreshPanels();
            }
            else
            {
                const Bim::Model& m = m_session.GetModel();
                const bool change = std::any_of(d->elements.begin(), d->elements.end(),
                                                [&](Bim::Id id) { return Bim::ElementJson(m, id).value("type", std::string()) != d->name; });
                if (change)
                {
                    Bim::Json p{ { "ids", d->elements }, { "type", d->name } };
                    if (d->category == "door" || d->category == "window")
                        p["family"] = nullptr;
                    RunCommand("modify", p, N_("Could not change the type"));
                }
            }
            d->dialog->Close();
        };
        raw->AddButton(Tr("OK"), okAction, DialogButtonRole::Default);
        raw->AddButton(Tr("Cancel"), [d] { d->dialog->Close(); }, DialogButtonRole::Cancel);
        raw->AddButton(Tr("Apply"), [apply, rebuild] { apply(); rebuild(); });
        raw->SetOnClosed([build] { *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }

    // =========================================================
    // 族类型
    // =========================================================
    void MainFrame::OpenFamilyDialog(const TypeSubject& t)
    {
        auto d = std::make_shared<FamilyState>();
        d->category = t.category;
        d->family   = *t.family;
        d->elements = t.elements;
        LoadFamily(*d, m_session.GetModel(), t.name);

        auto dialog = std::make_unique<Dialog>(Tr("Type Properties"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        auto apply = [this, d]
        {
            Bim::Json p;
            if (!FamilyParams(*d, d->name, m_session.GetModel().units, p, d->error) || !RunQuiet("family", p, d->error))
                return false;
            LoadFamily(*d, m_session.GetModel(), d->name);
            return true;
        };
        *build = [this, d, rebuild]
        {
            const Bim::Model& m = m_session.GetModel();
            Node* body = d->dialog->GetBody();
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction = FlexDirection::Column;
            bs.gap       = 6.0f;
            bs.width     = 600.0f;

            Node* row = Row(body);
            Caption(row, Tr("Family:"));
            row->AddChild<Label>(d->family, 13.0f);
            row = Row(body);
            Caption(row, Tr("Type:"));
            std::vector<std::string> names;
            if (const auto f = m.families.find(d->family); f != m.families.end())
                for (const auto& [n, _] : f->second.types)
                    names.push_back(n);
            const auto at = std::find(names.begin(), names.end(), d->name);
            auto* types = Sized(row->AddChild<ComboBox>(names, at == names.end() ? -1 : static_cast<int>(at - names.begin())), 300.0f);
            types->SetOnChanged([this, d, names, rebuild](int i)
            {
                if (i >= 0)
                {
                    LoadFamily(*d, m_session.GetModel(), names[static_cast<size_t>(i)]);
                    rebuild();
                }
            });
            Sized(row->AddChild<Button>(Tr("Duplicate..."), [d, rebuild]
            {
                d->naming    = d->naming == Naming::Duplicate ? Naming::None : Naming::Duplicate;
                d->naminText = d->name + " 2";
                rebuild();
            }), 100.0f);
            Sized(row->AddChild<Button>(Tr("Rename..."), [d, rebuild]
            {
                d->naming    = d->naming == Naming::Rename ? Naming::None : Naming::Rename;
                d->naminText = d->name;
                rebuild();
            }), 100.0f);
            if (d->naming != Naming::None)
            {
                row = Row(body);
                Caption(row, Tr("Name:"));
                auto* box = Sized(row->AddChild<TextBox>(d->naminText), 300.0f);
                box->SetOnChanged([d](const std::string& s) { d->naminText = s; });
                Sized(row->AddChild<Button>(d->naming == Naming::Duplicate ? Tr("Create") : Tr("Rename"), [this, d, rebuild]
                {
                    const std::string to(Bim::Text::Trim(d->naminText));
                    if (to.empty())
                        return;
                    if (d->naming == Naming::Rename)
                    {
                        if (RunQuiet("rename_type", Bim::Json{ { "family", d->family }, { "type", d->name }, { "name", to } }, d->error))
                        {
                            if (m_toolOpt.component == std::pair{ d->family, d->name })
                                m_toolOpt.component = std::pair{ d->family, to };
                            LoadFamily(*d, m_session.GetModel(), to);
                        }
                    }
                    else
                    {
                        Bim::Json p;
                        if (FamilyParams(*d, to, m_session.GetModel().units, p, d->error) && RunQuiet("family", p, d->error))
                            LoadFamily(*d, m_session.GetModel(), to);
                    }
                    rebuild();
                }), 90.0f);
            }

            body->AddChild<Label>(Tr("Type Parameters"), 13.0f)->SetBold(true);
            auto* grid = body->AddChild<PropertyGrid>();
            grid->EditLayoutStyle().height = 260.0f;
            grid->SetNameWidth(220.0f);
            std::vector<std::string> materials;
            for (const auto& [n, _] : m.materials)
                materials.push_back(n);
            // 按 Revit 的组：材质和装饰、尺寸标注、其他
            auto groupOf = [](Bim::Fam::Kind k)
            {
                return k == Bim::Fam::Kind::Material ? N_("Materials and Finishes")
                     : k == Bim::Fam::Kind::Length || k == Bim::Fam::Kind::Angle ? "Dimensions" : "Other";
            };
            for (const char* g : { "Materials and Finishes", "Dimensions", "Other" })
            {
                Expander* e = nullptr;
                for (auto& f : d->fields)
                {
                    if (std::string(groupOf(f.kind)) != g)
                        continue;
                    if (!e)
                        e = grid->AddGroup(Tr(g));
                    std::string* into = &f.typed;
                    if (f.computed)
                        grid->AddProperty<Label>(e, f.name, f.typed, 13.0f, Theme::TextDim);
                    else if (f.kind == Bim::Fam::Kind::YesNo)
                    {
                        auto* box = grid->AddProperty<CheckBox>(e, f.name, std::string(), f.typed == "1");
                        box->SetOnChanged([into](bool on) { *into = on ? "1" : "0"; });
                    }
                    else if (f.kind == Bim::Fam::Kind::Material)
                    {
                        const auto mi = std::find(materials.begin(), materials.end(), f.typed);
                        auto* box = grid->AddProperty<ComboBox>(e, f.name, materials, mi == materials.end() ? -1 : static_cast<int>(mi - materials.begin()));
                        box->SetOnChanged([into, materials](int i) { if (i >= 0) *into = materials[static_cast<size_t>(i)]; });
                    }
                    else
                    {
                        auto* box = grid->AddProperty<TextBox>(e, f.name, f.typed);
                        box->SetPlaceholder(f.kind == Bim::Fam::Kind::Length ? Bim::UnitsOf::Name(m.units) : f.kind == Bim::Fam::Kind::Angle ? "°" : "");
                        box->SetOnChanged([into](const std::string& s) { *into = s; });
                    }
                }
            }
            if (!d->error.empty())
                body->AddChild<Label>(d->error, 12.0f, Theme::Danger)->SetWrap(true);
        };

        Dialog* raw = d->dialog;
        raw->AddButton(Tr("OK"), [this, d, apply, rebuild]
        {
            if (!apply())
            {
                rebuild();
                return;
            }
            if (d->elements.empty())
            {
                if (d->category == "component")
                    m_toolOpt.component = std::pair{ d->family, d->name };
                else
                {
                    m_toolOpt.openingFamilies[d->category] = d->family;
                    m_toolOpt.types[d->category]           = d->name;
                }
                RefreshPanels();
            }
            else
            {
                const Bim::Model& m = m_session.GetModel();
                if (std::any_of(d->elements.begin(), d->elements.end(),
                                [&](Bim::Id id) { return Bim::ElementJson(m, id).value("type", std::string()) != d->name; }))
                    RunCommand("modify", Bim::Json{ { "ids", d->elements }, { "family", d->family }, { "type", d->name } }, N_("Could not change the type"));
            }
            d->dialog->Close();
        }, DialogButtonRole::Default);
        raw->AddButton(Tr("Cancel"), [d] { d->dialog->Close(); }, DialogButtonRole::Cancel);
        raw->AddButton(Tr("Apply"), [apply, rebuild] { apply(); rebuild(); });
        raw->SetOnClosed([build] { *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }

    // =========================================================
    // 材质
    // =========================================================
    void MainFrame::OpenMaterials()
    {
        struct State
        {
            std::string picked;
            std::string name;
            std::string error;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        const Bim::Model& m0 = m_session.GetModel();
        d->picked = m0.materials.empty() ? std::string() : m0.materials.begin()->first;
        d->name   = d->picked;

        auto dialog = std::make_unique<Dialog>(Tr("Materials"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        // 一条 material 命令，之后重建
        auto run = [this, d, rebuild](Bim::Json p, std::optional<std::string> pick)
        {
            if (RunQuiet("material", p, d->error) && pick)
            {
                d->picked = *pick;
                d->name   = *pick;
            }
            rebuild();
        };
        *build = [this, d, rebuild, run]
        {
            const Bim::Model& m = m_session.GetModel();
            Node* body = d->dialog->GetBody();
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction = FlexDirection::Column;
            bs.gap       = 8.0f;
            bs.width     = 460.0f;

            std::vector<std::string> names;
            for (const auto& [n, _] : m.materials)
                names.push_back(n);
            Node* row = Row(body);
            Caption(row, Tr("Material:"), 110.0f);
            const auto at = std::find(names.begin(), names.end(), d->picked);
            auto* pick = Sized(row->AddChild<ComboBox>(names, at == names.end() ? -1 : static_cast<int>(at - names.begin())), 200.0f);
            pick->SetMaxVisibleItems(16);
            pick->SetOnChanged([d, names, rebuild](int i)
            {
                if (i < 0)
                    return;
                d->picked = d->name = names[static_cast<size_t>(i)];
                d->error.clear();
                rebuild();
            });
            row->AddChild<Button>(Tr("New"), [this, d, run]
            {
                const Bim::Model& mm = m_session.GetModel();
                std::string fresh;
                for (int n = 1; fresh.empty() || mm.materials.count(fresh); ++n)
                    fresh = Tr("Material") + " " + std::to_string(n);
                run(Bim::Json{ { "name", fresh } }, fresh);
            });
            row->AddChild<Button>(Tr("Delete"), [this, d, run]
            {
                const Bim::Model& mm = m_session.GetModel();
                std::optional<std::string> next;
                for (const auto& [n, _] : mm.materials)
                    if (n != d->picked)
                    {
                        next = n;
                        break;
                    }
                run(Bim::Json{ { "name", d->picked }, { "delete", true } }, next);
            })->SetEnabled(m.materials.count(d->picked) > 0);

            const auto mat = m.materials.find(d->picked);
            if (mat != m.materials.end())
            {
                // 名称：按 Enter 或离开时改名
                row = Row(body);
                Caption(row, Tr("Name"), 110.0f);
                auto* name = Sized(row->AddChild<TextBox>(d->name), 260.0f);
                auto rename = [d, run](const std::string& typed)
                {
                    const std::string to(Bim::Text::Trim(typed));
                    if (!to.empty() && to != d->picked)
                        run(Bim::Json{ { "name", d->picked }, { "rename", to } }, to);
                };
                name->SetOnChanged([d](const std::string& s) { d->name = s; });
                name->SetOnSubmit(rename);
                name->SetOnFocusChanged([d, rename](bool focused) { if (!focused) rename(d->name); });

                row = Row(body);
                Caption(row, Tr("Color"), 110.0f);
                const auto& c = mat->second.color;
                auto* colour = Sized(row->AddChild<ColorButton>(RGBA(c[0], c[1], c[2])), 80.0f);
                colour->SetOnChanged([d, run](Color32 v)
                {
                    run(Bim::Json{ { "name", d->picked }, { "color", Bim::Json::array({ v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF }) } }, std::nullopt);
                });

                // 三维视图里透过它能看到身后多少，百分数
                row = Row(body);
                Caption(row, Tr("Transparency"), 110.0f);
                auto* clear = Sized(row->AddChild<NumberBox>(mat->second.transparency, 0.0, 100.0, 10.0, 0), 80.0f);
                clear->SetOnChanged([d, run](double v)
                {
                    run(Bim::Json{ { "name", d->picked }, { "transparency", static_cast<int>(std::lround(v)) } }, std::nullopt);
                });
                row->AddChild<Label>("%", 13.0f);

                std::vector<std::string> patterns;
                for (const auto& [_, n] : Patterns)
                    patterns.push_back(Tr(n));
                for (const auto& [key, label, now] : { std::tuple{ "cut", N_("Cut Pattern"), mat->second.cut }, std::tuple{ "surface", N_("Surface Pattern"), mat->second.surface } })
                {
                    row = Row(body);
                    Caption(row, Tr(label), 110.0f);
                    int pi = 0;
                    for (size_t i = 0; i < std::size(Patterns); ++i)
                        if (Patterns[i].first == now)
                            pi = static_cast<int>(i);
                    auto* box = Sized(row->AddChild<ComboBox>(patterns, pi), 200.0f);
                    const std::string k = key;
                    box->SetOnChanged([d, run, k](int i)
                    {
                        if (i >= 0)
                            run(Bim::Json{ { "name", d->picked }, { k, Bim::PatternOf::Name(Patterns[static_cast<size_t>(i)].first) } }, std::nullopt);
                    });
                }
            }
            if (!d->error.empty())
                body->AddChild<Label>(d->error, 12.0f, Theme::Danger)->SetWrap(true);
        };
        Dialog* raw = d->dialog;
        raw->AddButton(Tr("Close"), [d] { d->dialog->Close(); }, DialogButtonRole::Default);
        raw->SetOnClosed([build] { *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }
}
