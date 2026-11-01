// 移植自 BIMCraft crates/ui-egui/src/properties.rs、browser.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Commands/Commands.h"
#include "Core/UIContext.h"
#include "GUI/Parameters.h"
#include "Model/Text.h"
#include "I18n/Translator.h"
#include "Style/Theme.hpp"
#include "Widgets/Button.h"
#include "Widgets/ComboBox.h"
#include "Widgets/Controls.h"
#include "Widgets/Dialog.h"
#include "Widgets/DockSpace.h"
#include "Widgets/Label.h"
#include "Widgets/ListView.h"
#include "Widgets/Panel.h"
#include "Widgets/PropertyGrid.h"
#include "Widgets/TextBox.h"
#include "Widgets/UiLayout.h"
#include "Widgets/ViewportHost.h"
#include <algorithm>

// 停靠区的两块面板，照 BIMCraft（同 Revit）：
//   - 特性：类型选择器，"类别（个数）"，按组列出参数。选中的是一类元素时列出它们的参数，取值都一样的才显示；
//     没选东西时是平面视图（读写它的标高）。每个改动都是一条 modify 命令：检查、失败回滚、可撤销
//   - 项目浏览器：视图（各标高的楼层平面、三维视图）与族（系统族的类型、可载入族的类型），上面的搜索框按词筛选
namespace MiniBIM
{
    namespace
    {
        using Props::Kind;

        // 元素指着的东西（标高）在 JSON 里是 GUID，或是 ID
        std::optional<Bim::Id> IdOf(const Bim::Model& m, const Bim::Json& v)
        {
            if (v.is_number_unsigned())
                return v.get<Bim::Id>();
            if (v.is_string())
                if (const auto g = Bim::Guid::Parse(v.get<std::string>()))
                    return m.elements.IdOf(*g);
            return std::nullopt;
        }

        std::string LevelName(const Bim::Model& m, const Bim::Json& v)
        {
            const auto id = IdOf(m, v);
            const Bim::Level* l = id ? m.GetLevel(*id) : nullptr;
            return l ? l->name : std::string();
        }

        // "族: 类型"
        std::string PairLabel(const std::string& family, const std::string& type) { return family + ": " + type; }

        MiniGUI::LayoutStyle Grow()
        {
            MiniGUI::LayoutStyle s;
            s.grow = 1.0f;
            return s;
        }
    }

    // =========================================================
    // 选择集
    // =========================================================
    void MainFrame::SetSelection(std::vector<Bim::Id> ids)
    {
        m_selection = std::move(ids);
        UpdateStatus();
        RefreshPanels();
        m_commands.NotifyStateChanged();
        if (m_viewport)
            m_viewport->RequestRender();
    }

    void MainFrame::RefreshPanels()
    {
        if (m_panelsPosted || !m_platform)
            return;
        m_panelsPosted = true;
        m_platform->Post([this]
        {
            m_panelsPosted = false;
            BuildProperties();
            BuildBrowser();
            m_platform->RequestRedraw();
        });
    }

    bool MainFrame::PanelShown(const char* name) const
    {
        MiniGUI::DockSpace* dock = MainDock();
        return dock && dock->IsPanelVisible(name);
    }

    void MainFrame::TogglePanel(const char* name)
    {
        if (MiniGUI::DockSpace* dock = MainDock())
        {
            dock->TogglePanel(name);
            SavePanelLayout();
        }
        m_commands.NotifyStateChanged();
    }

    void MainFrame::EditParameter(const std::vector<Bim::Id>& ids, const std::string& key, Bim::Json value)
    {
        Bim::Json p = Props::Nested(key, std::move(value));
        p["ids"] = ids;
        RunCommand("modify", p, N_("Could not change the parameter"));
        RefreshPanels();    // 失败时也重建：输入框回到原来的值
    }

    // =========================================================
    // 特性
    // =========================================================
    std::unique_ptr<MiniGUI::Node> MainFrame::CreateProperties()
    {
        using namespace MiniGUI;
        auto panel = std::make_unique<Panel>(Theme::PanelAlt);
        LayoutStyle s;
        s.direction = FlexDirection::Column;
        s.padding   = Edges::Make(6.0f, 6.0f, 6.0f, 6.0f);
        s.gap       = 6.0f;
        panel->SetLayoutStyle(s);
        m_propsBody = panel.get();
        return panel;
    }

    void MainFrame::BuildProperties()
    {
        using namespace MiniGUI;
        if (!m_propsBody)
            return;
        m_propsBody->RemoveAllChildren();
        const Bim::Model& m = m_session.GetModel();

        // 显示什么：选中的一类元素、几类元素，没选东西时工具要放的（"新建 …"），否则视图
        std::string category;
        const bool placing = m_selection.empty() && ToolCategory();
        std::vector<Bim::Id> ids;
        std::span<const Props::Row> rows;
        std::string heading;
        if (!m_selection.empty())
        {
            std::set<std::string> categories;
            for (const Bim::Id id : m_selection)
                if (const Bim::Element* e = m.elements.Get(id))
                    categories.insert(e->Category());
            if (categories.size() == 1)
            {
                category = *categories.begin();
                ids      = m_selection;
                rows     = Props::RowsOf(category);
                heading  = Format(Tr("{0} ({1})"), { Tr(Props::CategoryName(category, true)), std::to_string(ids.size()) });
            }
            else
                heading = Format(Tr("Multiple Categories ({0})"), { std::to_string(m_selection.size()) });
        }
        else if (placing)
        {
            category = ToolCategory();
            heading  = Format(Tr("New {0}"), { Tr(Props::CategoryName(category, false)) });
        }
        else if (m_planLevel)
        {
            if (const auto id = m.elements.IdOf(PlanKey()))
            {
                ids  = { *id };
                rows = m_planOf ? Props::PlanViewRows() : Props::PlanRows();
                if (const std::string* name = m.ViewName(PlanKey()))
                    heading = Format(Tr("Floor Plan: {0}"), { *name });
            }
        }
        else if (m_sectionOf)
        {
            if (const std::string* name = m.ViewName(*m_sectionOf))
                heading = m.elements.Get(*m_sectionOf)->As<Bim::Elevation>() ? Format(Tr("Elevation: {0}"), { *name }) : Format(Tr("Section: {0}"), { *name });
        }
        else if (m_sheetOf)
        {
            if (const Bim::Sheet* sh = m.GetSheet(*m_sheetOf))
                heading = Format(Tr("Sheet: {0} - {1}"), { sh->number, sh->name });
        }
        else
            heading = "{3D}";

        // ── 类型选择器：类别（或族）在上，下拉列表里是这一类的类型；门窗另有门窗族的"族: 类型"，构件只有族的 ──
        const bool hosted = category == "door" || category == "window";
        std::vector<std::pair<std::string, std::string>> pairs;
        for (const auto& [name, f] : m.families)
        {
            const bool fits = category == "component" ? (placing ? PlacingFits(f.category) : !Bim::Fam::CategoryOf::Hosted(f.category))
                            : hosted && f.category == (category == "door" ? Bim::Fam::Category::Door : Bim::Fam::Category::Window);
            if (fits)
                for (const auto& [type, _] : f.types)
                    pairs.emplace_back(name, type);
        }
        std::vector<std::string> names;
        if (category != "component")
            for (const std::string& t : m.TypeNames(category))
                names.push_back(t);
        const size_t own = names.size();
        for (const auto& [f, t] : pairs)
            names.push_back(PairLabel(f, t));
        // 选中的都是同一个类型（同一个族）时，它在列表里的位置
        std::set<std::string> current;
        for (const Bim::Id id : ids)
        {
            const Bim::Json e = Bim::ElementJson(m, id);
            const std::string type = e.value("type", std::string());
            current.insert(e.contains("family") && e["family"].is_string() ? PairLabel(e["family"].get<std::string>(), type) : type);
        }
        // 工具要放的：选项里选过的，否则项目默认的（构件为第一个族的第一个类型）
        if (placing)
        {
            const auto f = m_toolOpt.openingFamilies.find(category);
            const auto t = m_toolOpt.types.find(category);
            if (category == "component")
            {
                if (const auto what = Placing())
                    current.insert(PairLabel(what->first, what->second));
            }
            else if (f != m_toolOpt.openingFamilies.end() && t != m_toolOpt.types.end())
                current.insert(PairLabel(f->second, t->second));
            else if (t != m_toolOpt.types.end())
                current.insert(t->second);
            else if (const auto d = m.DefaultType(category))
                current.insert(*d);
        }
        const std::string family = placing             ? Tr(Props::CategoryName(category, false))
                                 : m_selection.empty() ? (m_planLevel ? Tr("Floor Plan") : m_sheetOf ? Tr("Sheet") : m_sectionOf ? (m.elements.Get(*m_sectionOf)->As<Bim::Elevation>() ? Tr("Elevation View") : Tr("Section")) : Tr("3D View"))
                                 : category.empty()    ? Tr("Multiple Categories")
                                                       : Tr(Props::CategoryName(category, false));
        m_propsBody->AddChild<Label>(family, 13.0f)->SetBold(true);
        if (!names.empty() && ((!ids.empty() && !m_selection.empty()) || placing))
        {
            int at = -1;
            if (current.size() == 1)
                if (const auto it = std::find(names.begin(), names.end(), *current.begin()); it != names.end())
                    at = static_cast<int>(it - names.begin());
            auto* types = m_propsBody->AddChild<ComboBox>(names, at);
            types->SetTooltip(Tr("Type Selector"));
            types->SetMaxVisibleItems(16);
            types->SetOnChanged([this, names, own, pairs, ids, hosted, placing, category](int i)
            {
                if (i < 0 || static_cast<size_t>(i) >= names.size())
                    return;
                // 选的是工具接下来放的类型
                if (placing)
                {
                    if (category == "component")
                        m_toolOpt.component = pairs[static_cast<size_t>(i)];
                    else if (static_cast<size_t>(i) < own)
                    {
                        m_toolOpt.types[category] = names[static_cast<size_t>(i)];
                        m_toolOpt.openingFamilies.erase(category);
                    }
                    else
                    {
                        m_toolOpt.openingFamilies[category] = pairs[static_cast<size_t>(i) - own].first;
                        m_toolOpt.types[category]           = pairs[static_cast<size_t>(i) - own].second;
                    }
                    if (m_viewport)
                        m_viewport->RequestRender();
                    return;
                }
                Bim::Json p{ { "ids", ids } };
                if (static_cast<size_t>(i) < own)
                {
                    p["type"] = names[static_cast<size_t>(i)];
                    // 项目自己的门窗类型：不再是哪个族的
                    if (hosted)
                        p["family"] = nullptr;
                }
                else
                {
                    const auto& [f, t] = pairs[static_cast<size_t>(i) - own];
                    p["family"] = f;
                    p["type"]   = t;
                }
                m_platform->Post([this, p] { RunCommand("modify", p, N_("Could not change the type")); RefreshPanels(); });
            });
        }
        // "类别（个数）"，右边"编辑类型"
        {
            auto* row = m_propsBody->AddChild<Node>();
            LayoutStyle rs;
            rs.direction  = FlexDirection::Row;
            rs.alignItems = Align::Center;
            row->SetLayoutStyle(rs);
            row->AddChild<Label>(heading, 12.0f, Theme::TextDim)->SetLayoutStyle(Grow());
            if (CurrentTypeSubject())
                row->AddChild<Button>(Tr("Edit Type"), [this] { m_platform->Post([this] { EditType(); }); });
        }
        if (ids.empty() || rows.empty())
            return;

        // ── 参数：选中的几个取值都一样的才显示，不一样的空着 ─────────────
        std::vector<Bim::Json> all;
        for (const Bim::Id id : ids)
            all.push_back(Bim::ElementJson(m, id));
        Bim::Json v = Bim::Json::object();
        auto shared = [&](const std::string& key, Kind kind)
        {
            const Bim::Json one = Props::FieldOf(all.front(), key, kind);
            const bool same = std::all_of(all.begin(), all.end(), [&](const Bim::Json& e) { return Props::FieldOf(e, key, kind) == one; });
            v[key] = same ? one : Bim::Json(nullptr);
        };
        for (const Props::Row& r : rows)
            shared(r.key, r.kind);
        shared("top_level", Kind::Shown);
        shared("enclosed", Kind::Shown);

        std::vector<std::string> levels;
        for (const auto& [_, l] : m.Levels())
            levels.push_back(l->name);
        const Bim::Units units = m.units;
        const bool one = ids.size() == 1;

        auto* grid = m_propsBody->AddChild<PropertyGrid>();
        grid->SetLayoutStyle(Grow());
        grid->SetNameWidth(130.0f);
        std::vector<std::string> groups;
        for (const Props::Row& r : rows)
            if (std::find(groups.begin(), groups.end(), r.group) == groups.end())
                groups.emplace_back(r.group);
        for (const std::string& group : groups)
        {
            Expander* g = grid->AddGroup(Tr(group), !m_collapsedGroups.contains(group));
            g->SetOnToggled([this, group](bool open) { open ? (void)m_collapsedGroups.erase(group) : (void)m_collapsedGroups.insert(group); });
            for (const Props::Row& r : rows)
            {
                if (r.group != group)
                    continue;
                const std::string key = r.key;
                const Bim::Json& val = v[key];
                // 这一个元素没有的长度（直墙的半径）：不列
                if (one && (r.kind == Kind::LengthShown || r.kind == Kind::LengthIf) && val.is_null())
                    continue;
                // 墙顶连着标高时，高度是两个标高定的
                const Kind kind = r.kind == Kind::Height && !v["top_level"].is_null() ? Kind::LengthShown : r.kind;
                const std::string label = Tr(r.label);
                switch (kind)
                {
                // 指着的元素（门窗的主体墙）写它的 ID：命令认的是 ID
                case Kind::Shown:
                {
                    const auto to = val.is_string() ? IdOf(m, val) : std::nullopt;
                    grid->AddProperty<Label>(g, label, to ? std::to_string(*to) : Tr(Props::Text(val)), 13.0f, Theme::TextDim);
                    break;
                }
                case Kind::LengthShown: case Kind::AreaShown: case Kind::MetresShown:
                    grid->AddProperty<Label>(g, label, Props::Shown(kind, val, units), 13.0f, Theme::TextDim);
                    break;
                case Kind::LevelName:
                    grid->AddProperty<Label>(g, label, LevelName(m, val), 13.0f, Theme::TextDim);
                    break;
                case Kind::Flag:
                {
                    auto* box = grid->AddProperty<CheckBox>(g, label, std::string(), val.is_boolean() && val.get<bool>());
                    box->SetOnChanged([this, ids, key](bool on) { m_platform->Post([this, ids, key, on] { EditParameter(ids, key, on); }); });
                    break;
                }
                // Revit 灰显的复选框：附着、分离墙顶墙底改它
                case Kind::Attached:
                {
                    auto* box = grid->AddProperty<CheckBox>(g, label, std::string(), !val.is_null());
                    box->SetEnabled(false);
                    break;
                }
                case Kind::Level:
                {
                    const std::string now = LevelName(m, val);
                    const auto it = std::find(levels.begin(), levels.end(), now);
                    auto* box = grid->AddProperty<ComboBox>(g, label, levels, it == levels.end() ? -1 : static_cast<int>(it - levels.begin()));
                    box->SetOnChanged([this, ids, key, levels](int i)
                    {
                        if (i >= 0)
                            m_platform->Post([this, ids, key, name = levels[static_cast<size_t>(i)]] { EditParameter(ids, key, name); });
                    });
                    break;
                }
                case Kind::TopLevel:
                {
                    std::vector<std::string> options{ Tr("Unconnected") };
                    options.insert(options.end(), levels.begin(), levels.end());
                    int at = 0;
                    if (!val.is_null())
                    {
                        const auto it = std::find(levels.begin(), levels.end(), LevelName(m, val));
                        at = it == levels.end() ? -1 : static_cast<int>(it - levels.begin()) + 1;
                    }
                    auto* box = grid->AddProperty<ComboBox>(g, label, options, at);
                    box->SetOnChanged([this, ids, key, options](int i)
                    {
                        if (i < 0)
                            return;
                        Bim::Json to = i == 0 ? Bim::Json(nullptr) : Bim::Json(options[static_cast<size_t>(i)]);
                        m_platform->Post([this, ids, key, to] { EditParameter(ids, key, to); });
                    });
                    break;
                }
                // run 的对齐：顶、中、底；几个取值不同时空着
                case Kind::Justification:
                {
                    static const std::pair<const char*, const char*> names[] = { { "top", N_("Top") }, { "centre", N_("Centre") }, { "bottom", N_("Bottom") } };
                    std::vector<std::string> options;
                    int at = -1;
                    for (const auto& [k, n] : names)
                    {
                        if (val.is_string() && val.get<std::string>() == k)
                            at = static_cast<int>(options.size());
                        options.push_back(Tr(n));
                    }
                    auto* box = grid->AddProperty<ComboBox>(g, label, options, at);
                    box->SetOnChanged([this, ids, key](int i)
                    {
                        if (i >= 0)
                            m_platform->Post([this, ids, key, name = std::string(names[static_cast<size_t>(i)].first)] { EditParameter(ids, key, name); });
                    });
                    break;
                }
                // 按名称显示；墙的 JSON 略去默认的墙中心线
                case Kind::LocationLine:
                {
                    const auto now = val.is_string() ? Bim::LocationOf::FromName(val.get<std::string>()) : std::optional<Bim::Location>(Bim::Location::Centre);
                    std::vector<std::string> options;
                    int at = -1;
                    for (const Bim::Location l : Bim::LocationOf::All)
                    {
                        if (now == l)
                            at = static_cast<int>(options.size());
                        options.push_back(Tr(Bim::LocationOf::Label(l)));
                    }
                    // 选中的几面墙定位线不一样：空着
                    if (!one && val.is_null())
                        at = -1;
                    auto* box = grid->AddProperty<ComboBox>(g, label, options, at);
                    box->SetOnChanged([this, ids, key](int i)
                    {
                        if (i >= 0)
                            m_platform->Post([this, ids, key, name = std::string(Bim::LocationOf::Name(Bim::LocationOf::All[static_cast<size_t>(i)]))]
                                             { EditParameter(ids, key, name); });
                    });
                    break;
                }
                default:
                {
                    // 写着改，按 Enter 或离开输入框时改一次（文字没变就不改）；几个元素取值不同时空着，不是"没有"
                    const std::string shown = Props::Shown(kind, val, units);
                    auto* box = grid->AddProperty<TextBox>(g, label);
                    box->SetText(shown);
                    if (kind == Kind::Limit && one && r.hint)
                        box->SetPlaceholder(Tr(r.hint));
                    auto typed = std::make_shared<std::string>(shown);
                    auto done  = std::make_shared<std::string>(shown);
                    auto commit = [this, ids, key, kind, units, typed, done]
                    {
                        if (*typed == *done)
                            return;
                        *done = *typed;
                        Bim::Json to;
                        std::string error;
                        if (!Props::Parse(kind, *typed, units, to, error))
                        {
                            m_platform->Post([this, error]
                            {
                                MiniGUI::ShowMessageBox(*m_ui, Tr("Could not change the parameter"), error);
                                RefreshPanels();
                            });
                            return;
                        }
                        m_platform->Post([this, ids, key, to] { EditParameter(ids, key, to); });
                    };
                    box->SetOnChanged([typed](const std::string& t) { *typed = t; });
                    box->SetOnSubmit([commit](const std::string&) { commit(); });
                    box->SetOnFocusChanged([commit](bool focused) { if (!focused) commit(); });
                    break;
                }
                }
            }
        }
        // ── 项目参数：它的类别的（构件的同名族参数除外），几个取值一样才显示；改时 modify {ids, params: {名称: 值}} ──
        if (!m_selection.empty() && !category.empty())
        {
            Expander* g = nullptr;
            const Bim::Element* first = m.elements.Get(ids.front());
            for (const auto& [name, p] : m.parameters)
            {
                if (!first || !m.ParameterOf(*first, name))
                    continue;
                if (!g)
                {
                    const std::string group = "Project Parameters";
                    g = grid->AddGroup(Tr("Project Parameters"), !m_collapsedGroups.contains(group));
                    g->SetOnToggled([this, group](bool open) { open ? (void)m_collapsedGroups.erase(group) : (void)m_collapsedGroups.insert(group); });
                }
                const auto valueOf = [&](const Bim::Json& e) { return e.contains("params") && e["params"].contains(name) ? e["params"][name] : Bim::Json(nullptr); };
                const Bim::Json one0 = valueOf(all.front());
                const bool same = std::all_of(all.begin(), all.end(), [&](const Bim::Json& e) { return valueOf(e) == one0; });
                const Bim::Json val = same ? one0 : Bim::Json(nullptr);
                const auto set = [this, ids, name](Bim::Json to)
                {
                    m_platform->Post([this, ids, name, to]
                    {
                        RunCommand("modify", Bim::Json{ { "ids", ids }, { "params", { { name, to } } } }, N_("Could not change the parameter"));
                        RefreshPanels();
                    });
                };
                if (p.kind == Bim::ParameterKind::YesNo)
                {
                    auto* box = grid->AddProperty<CheckBox>(g, name, std::string(), val.is_boolean() && val.get<bool>());
                    box->SetOnChanged([set](bool on) { set(on); });
                    continue;
                }
                // 文字、数与长度：写着改，按 Enter 或离开时改一次；清空时去掉它的值
                const Kind kind = p.kind == Bim::ParameterKind::Length ? Kind::Length : p.kind == Bim::ParameterKind::Number ? Kind::Number : Kind::Text;
                const std::string shown = val.is_null() ? std::string() : Props::Shown(kind, val, units);
                auto* box = grid->AddProperty<TextBox>(g, name);
                box->SetText(shown);
                auto typed = std::make_shared<std::string>(shown);
                auto done  = std::make_shared<std::string>(shown);
                auto commit = [this, kind, units, typed, done, set]
                {
                    if (*typed == *done)
                        return;
                    *done = *typed;
                    Bim::Json to;
                    std::string error;
                    if (Bim::Text::Trim(*typed).empty())
                        to = nullptr;
                    else if (!Props::Parse(kind, *typed, units, to, error))
                    {
                        m_platform->Post([this, error]
                        {
                            MiniGUI::ShowMessageBox(*m_ui, Tr("Could not change the parameter"), error);
                            RefreshPanels();
                        });
                        return;
                    }
                    set(to);
                };
                box->SetOnChanged([typed](const std::string& t) { *typed = t; });
                box->SetOnSubmit([commit](const std::string&) { commit(); });
                box->SetOnFocusChanged([commit](bool focused) { if (!focused) commit(); });
            }
        }
        // 四周的墙没有围合的房间
        if (one && v["enclosed"] == Bim::Json(false))
        {
            auto* note = m_propsBody->AddChild<Label>(Tr("Not enclosed: the walls around it don't close."), 12.0f, Theme::Danger);
            note->SetWrap(true);
        }
    }

    // =========================================================
    // 项目浏览器
    // =========================================================
    std::unique_ptr<MiniGUI::Node> MainFrame::CreateBrowser()
    {
        using namespace MiniGUI;
        auto panel = std::make_unique<Panel>(Theme::PanelAlt);
        LayoutStyle s;
        s.direction = FlexDirection::Column;
        s.padding   = Edges::Make(6.0f, 6.0f, 6.0f, 6.0f);
        s.gap       = 6.0f;
        panel->SetLayoutStyle(s);

        auto* find = panel->AddChild<TextBox>();
        find->SetPlaceholder(Tr("Search"));
        find->SetOnChanged([this](const std::string& text)
        {
            m_browserFind = text.substr(0, 64);
            BuildBrowser();
        });

        auto* tree = panel->AddChild<TreeView>();
        tree->SetLayoutStyle(Grow());
        tree->SetOnItemActivated([this](TreeItem* item)
        {
            const uint64_t at = item ? item->GetUserData() : 0;
            if (at > 0 && at <= m_browserActs.size())
                m_platform->Post([act = m_browserActs[at - 1]] { act(); });
        });
        // 右键：类型与族的"类型属性""删除"
        tree->SetItemContextMenu([this](TreeItem* item)
        {
            std::vector<MenuItem> items;
            const uint64_t at = item ? item->GetUserData() : 0;
            if (at == 0 || at > m_browserEntries.size() || m_browserEntries[at - 1].is_null())
                return items;
            const Bim::Json entry = m_browserEntries[at - 1];
            if (entry.contains("type"))
                items.emplace_back(Tr("Type Properties..."), [this, act = m_browserActs[at - 1]] { m_platform->Post(act); });
            items.emplace_back(Tr("Delete"), [this, entry] { m_platform->Post([this, entry] { RunCommand("delete_type", entry, N_("Could not delete")); }); });
            return items;
        });
        m_browserTree = tree;
        m_browserBody = panel.get();
        return panel;
    }

    void MainFrame::BuildBrowser()
    {
        using namespace MiniGUI;
        if (!m_browserTree)
            return;
        const Bim::Model& m = m_session.GetModel();

        // 树先搭成这样，再按搜索的词筛：每个词要么出现在它的名称里，要么出现在它所在的组的名称里
        struct Entry
        {
            std::string label;
            std::function<void()> act;      // 双击做什么（组为空）
            bool current = false;           // 正在显示的视图
            bool open    = false;           // 组默认展开着没有
            std::vector<Entry> kids;
            Bim::Json entry;                // 类型或族：右键能删除（delete_type 的参数）
        };
        auto group = [](std::string label, bool open, std::vector<Entry> kids) { return Entry{ std::move(label), {}, false, open, std::move(kids) }; };

        std::vector<Entry> plans;
        for (const auto& [id, l] : m.Levels())
        {
            const Bim::Guid g = m.GuidOf(id);
            const bool on = m_planLevel && !m_planOf && m.Same(*m_planLevel, g);
            plans.push_back(Entry{ l->name, [this, g] { ShowPlan(g); }, on, false, {} });
            // 它的平面视图跟在后面，相关视图挂在父视图下
            for (const auto& [vid, v] : m.PlanViews(g))
            {
                if (v->parent && !m.Same(*v->parent, g))
                    continue;
                const Bim::Guid vg = m.GuidOf(vid);
                Entry view{ v->name, [this, vg] { ShowPlanView(vg); }, m_planOf && m.Same(*m_planOf, vg), false, {} };
                for (const auto& [did, dv] : m.PlanViews(g))
                    if (dv->parent && m.Same(*dv->parent, vg))
                    {
                        const Bim::Guid dg = m.GuidOf(did);
                        view.kids.push_back(Entry{ dv->name, [this, dg] { ShowPlanView(dg); }, m_planOf && m.Same(*m_planOf, dg), false, {} });
                    }
                view.open = !view.kids.empty();
                if (v->parent)
                    plans.back().kids.push_back(std::move(view));
                else
                    plans.push_back(std::move(view));
            }
        }
        std::vector<Entry> views;
        views.push_back(group(Tr("Floor Plans"), true, std::move(plans)));
        views.push_back(group(Tr("3D Views"), true, { Entry{ "{3D}", [this] { ShowModel3D(); }, !m_planLevel && !m_sectionOf && !m_sheetOf, false, {} } }));
        // 剖面与立面，按元素顺序
        std::vector<Entry> sections, elevations;
        for (const auto& [id, e] : m.elements.Items())
        {
            const Bim::Guid g = m.GuidOf(id);
            if (const Bim::Section* sv = e->As<Bim::Section>())
                sections.push_back(Entry{ sv->name, [this, g] { ShowSection(g); }, m_sectionOf && m.Same(*m_sectionOf, g), false, {} });
            else if (const Bim::Elevation* ev = e->As<Bim::Elevation>())
                elevations.push_back(Entry{ ev->name, [this, g] { ShowSection(g); }, m_sectionOf && m.Same(*m_sectionOf, g), false, {} });
        }
        if (!elevations.empty())
            views.push_back(group(Tr("Elevations"), true, std::move(elevations)));
        if (!sections.empty())
            views.push_back(group(Tr("Sections"), true, std::move(sections)));
        // 明细表：每个项目都有的六个（项目改过的是它自己的），再是项目建的
        std::vector<Entry> schedules;
        for (const Bim::Schedules::Builtin& b : Bim::Schedules::Views)
        {
            const auto [id, sc] = Bim::Schedules::Standard(m, b.category);
            const Bim::Json which = id ? Bim::Json{ { "name", sc.name } } : Bim::Json{ { "category", b.category } };
            schedules.push_back(Entry{ sc.name, [this, which] { OpenSchedule(which); }, false, false, {} });
        }
        for (const auto& [id, e] : m.elements.Items())
            if (const Bim::Schedule* sc = e->As<Bim::Schedule>(); sc && !Bim::Schedules::BuiltinOf(&sc->name, nullptr))
            {
                const Bim::Json which{ { "name", sc->name } };
                schedules.push_back(Entry{ sc->name, [this, which] { OpenSchedule(which); }, false, false, {} });
            }
        views.push_back(group(Tr("Schedules/Quantities"), false, std::move(schedules)));

        // 族：系统族（墙、楼板……）的类型，再是可载入族的类型
        std::vector<Entry> families;
        // 双击类型：类型属性；右键：类型属性、删除
        std::vector<const char*> systemFamilies{ "wall", "floor", "roof", "ceiling", "door", "window", "column", "structuralcolumn", "footing" };
        systemFamilies.insert(systemFamilies.end(), std::begin(Bim::RunCategories), std::end(Bim::RunCategories));
        for (const char* c : systemFamilies)
        {
            std::vector<Entry> types;
            for (const std::string& t : m.TypeNames(c))
                types.push_back(Entry{ t, [this, c, t] { OpenTypeDialog(TypeSubject{ c, t, std::nullopt, {} }); }, false, false, {},
                                       Bim::Json{ { "category", c }, { "type", t } } });
            if (!types.empty())
                families.push_back(group(Tr(Props::CategoryName(c, true)), false, std::move(types)));
        }
        for (const auto& [name, f] : m.families)
        {
            std::vector<Entry> types;
            const std::string category = f.category == Bim::Fam::Category::Door ? "door" : f.category == Bim::Fam::Category::Window ? "window" : "component";
            for (const auto& [t, _] : f.types)
                types.push_back(Entry{ t, [this, category, name, t] { OpenFamilyDialog(TypeSubject{ category, t, name, {} }); }, false, false, {},
                                       Bim::Json{ { "family", name }, { "type", t } } });
            Entry fam = group(name, false, std::move(types));
            fam.entry = Bim::Json{ { "family", name } };
            families.push_back(std::move(fam));
        }
        std::vector<Entry> top;
        top.push_back(group(Tr("Views (all)"), true, std::move(views)));
        // 图纸：编号与名称，按编号；下面是放在它上面的视图（双击显示那个视图）
        std::vector<std::pair<std::string, Entry>> sheets;
        for (const auto& [id, e] : m.elements.Items())
            if (const Bim::Sheet* sh = e->As<Bim::Sheet>())
            {
                const Bim::Guid g = m.GuidOf(id);
                Entry sheet{ sh->number + " - " + sh->name, [this, g] { ShowSheet(g); }, m_sheetOf && m.Same(*m_sheetOf, g), false, {} };
                for (const auto& [vid, x] : m.elements.Items())
                    if (const Bim::Viewport* vp = x->As<Bim::Viewport>(); vp && m.Same(vp->sheet, g))
                        if (const std::string* name = m.ViewName(vp->view))
                        {
                            const Bim::Guid view = m.GuidOf(vp->view);
                            const bool plan = m.PlanLevel(view).has_value(), section = m.GetSection(view) != nullptr;
                            const bool table = m.elements.Get(view)->As<Bim::Schedule>() != nullptr;
                            const Bim::Json which{ { "name", *name } };
                            sheet.kids.push_back(Entry{ *name, [this, view, plan, section, table, which]
                            {
                                if (plan && m_session.GetModel().GetPlanView(view))
                                    ShowPlanView(view);
                                else if (plan)
                                    ShowPlan(view);
                                else if (section)
                                    ShowSection(view);
                                else if (table)
                                    OpenSchedule(which);
                                else
                                    ShowModel3D();
                            }, false, false, {} });
                        }
                sheets.emplace_back(sh->number, std::move(sheet));
            }
        std::sort(sheets.begin(), sheets.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<Entry> sheetEntries;
        for (auto& [n, entry] : sheets)
            sheetEntries.push_back(std::move(entry));
        top.push_back(group(Tr("Sheets (all)"), true, std::move(sheetEntries)));
        top.push_back(group(Tr("Families"), false, std::move(families)));
        // 链接的模型：双击选中它；没读到的注明
        std::vector<Entry> links;
        for (const auto& [id, e] : m.elements.Items())
            if (const Bim::Link* l = e->As<Bim::Link>())
            {
                const std::string label = m.loaded.count(l->path) ? l->name : Format(Tr("{0} (not loaded)"), { l->name });
                links.push_back(Entry{ label, [this, id] { SetSelection({ id }); }, false, false, {} });
            }
        if (!links.empty())
            top.push_back(group(Tr("Linked Models"), true, std::move(links)));

        // 记下现在各分支展开着没有（按路径），重建后照旧
        std::function<void(const TreeItem*, const std::string&)> remember = [&](const TreeItem* item, const std::string& path)
        {
            for (const auto& kid : item->GetChildren())
                if (kid->HasChildren())
                {
                    const std::string p = path + "/" + kid->GetText();
                    m_branchOpen[p] = kid->IsExpanded();
                    remember(kid.get(), p);
                }
        };
        const std::vector<std::string> words = Props::Words(m_browserFind);
        if (words.empty())
            remember(m_browserTree->GetRootItem(), "");

        m_browserTree->Clear();
        m_browserActs.clear();
        m_browserEntries.clear();
        TreeItem* selected = nullptr;
        // 一项显示出来，当它的名称配上了还没配上的每个词，或者它是组、里面有一项显示出来
        std::function<bool(const Entry&, const std::vector<std::string>&)> shows = [&](const Entry& e, const std::vector<std::string>& left)
        {
            std::vector<std::string> rest;
            for (const std::string& w : left)
                if (!Props::Matches(e.label, w))
                    rest.push_back(w);
            if (rest.empty())
                return true;
            return std::any_of(e.kids.begin(), e.kids.end(), [&](const Entry& k) { return shows(k, rest); });
        };
        std::function<void(TreeItem*, const std::vector<Entry>&, const std::vector<std::string>&, const std::string&)> add =
            [&](TreeItem* parent, const std::vector<Entry>& entries, const std::vector<std::string>& left, const std::string& path)
        {
            for (const Entry& e : entries)
            {
                if (!shows(e, left))
                    continue;
                TreeItem* item = m_browserTree->AddItem(parent, e.label);
                if (e.act || !e.entry.is_null())
                {
                    m_browserActs.push_back(e.act ? e.act : std::function<void()>([] {}));
                    m_browserEntries.push_back(e.entry);
                    item->SetUserData(m_browserActs.size());
                }
                if (e.current)
                    selected = item;
                std::vector<std::string> rest;
                for (const std::string& w : left)
                    if (!Props::Matches(e.label, w))
                        rest.push_back(w);
                // 组配上了的词，里面的项不必再配
                const std::string p = path + "/" + e.label;
                add(item, e.kids, rest, p);
                if (!e.kids.empty())
                {
                    const auto it = m_branchOpen.find(p);
                    item->SetExpanded(!words.empty() || (it == m_branchOpen.end() ? e.open : it->second));
                }
            }
        };
        add(nullptr, top, words, "");
        if (selected)
            m_browserTree->SetSelected(selected);
    }
}
