// 移植自 BIMCraft crates/ui-egui/src/lib.rs 的明细表视图、properties.rs 的新建明细表、filter.rs 的视图过滤器（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Commands/Commands.h"
#include "Core/Log.h"
#include "GUI/Parameters.h"
#include "I18n/Translator.h"
#include "Model/ModelJson.h"
#include "Model/Schedule.h"
#include "Model/Text.h"
#include "Style/Theme.hpp"
#include "Widgets/Button.h"
#include "Widgets/ComboBox.h"
#include "Widgets/Controls.h"
#include "Widgets/Dialog.h"
#include "Widgets/Label.h"
#include "Widgets/ScrollView.h"
#include "Widgets/TextBox.h"
#include <algorithm>

// 视图选项卡的明细表、视图过滤器与视图样板：
//   - 明细表：一个对话框里的表（标题、各行、合计），单击一行的第一格选中它列出的元素；内置的六个与项目自己的在项目浏览器的"明细表/数量"下
//   - 新建明细表：类别与名称，建好后打开
//   - 视图过滤器：左边项目的过滤器（新建、删除），右边选中的那个的类别与规则（字段、比较、值）；每个改动是一条 filter 命令
//   - 由当前视图创建样板：名称，经 template {from}
namespace MiniBIM
{
    namespace
    {
        using namespace MiniGUI;

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
            node->EditLayoutStyle().width = width;
            return node;
        }

        // 类别给人看的名称（复数）：界面认得的翻译，否则原样
        std::string CategoryLabel(const std::string& c)
        {
            const char* n = Props::CategoryName(c, true);
            return *n ? Tr(n) : c;
        }

        // 过滤器的比较，界面上的说法
        const std::pair<Bim::Schedules::Op, const char*> Ops[] = {
            { Bim::Schedules::Op::Equals, N_("equals") },
            { Bim::Schedules::Op::NotEquals, N_("does not equal") },
            { Bim::Schedules::Op::Greater, N_("is greater than") },
            { Bim::Schedules::Op::GreaterOrEqual, N_("is greater than or equal to") },
            { Bim::Schedules::Op::Less, N_("is less than") },
            { Bim::Schedules::Op::LessOrEqual, N_("is less than or equal to") },
            { Bim::Schedules::Op::Contains, N_("contains") },
            { Bim::Schedules::Op::NotContains, N_("does not contain") },
            { Bim::Schedules::Op::BeginsWith, N_("begins with") },
            { Bim::Schedules::Op::NotBeginsWith, N_("does not begin with") },
            { Bim::Schedules::Op::EndsWith, N_("ends with") },
            { Bim::Schedules::Op::NotEndsWith, N_("does not end with") },
            { Bim::Schedules::Op::HasValue, N_("has a value") },
            { Bim::Schedules::Op::HasNoValue, N_("has no value") },
        };
    }

    void MainFrame::OpenSchedule(const Bim::Json& which)
    {
        struct State
        {
            Bim::Json   which;          // schedule 命令认它的：{name} 或 {category}
            std::string error;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        d->which = which;
        Bim::Json reply;
        std::string error;
        if (!RunQuiet("schedule", which, error, &reply))
        {
            LOG_WARN("打开明细表失败：%s", error.c_str());
            return;
        }
        auto dialog = std::make_unique<Dialog>(Format(Tr("Schedule: {0}"), { reply["name"].get<std::string>() }));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        *build = [this, d]
        {
            const Bim::Model& m = m_session.GetModel();
            Node* body = d->dialog->GetBody();
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction = FlexDirection::Column;
            bs.gap       = 8.0f;
            bs.width     = 760.0f;
            Bim::Json r;
            std::string why;
            if (!RunQuiet("schedule", d->which, why, &r))
            {
                body->AddChild<Label>(why, 12.0f, Theme::Danger)->SetWrap(true);
                return;
            }
            // 表照项目的单位显示；标题与是 / 否翻译
            const auto id = r["id"].is_number() ? std::optional<Bim::Id>(r["id"].get<Bim::Id>()) : std::nullopt;
            const Bim::Schedule* sc = id ? m.elements.Get(*id)->As<Bim::Schedule>() : nullptr;
            const Bim::Schedule standard = sc ? *sc : Bim::Schedules::Standard(m, r["category"].get<std::string>()).second;
            Bim::Schedules::Table t;
            if (!Bim::Schedules::TableOf(m, standard, t, why))
            {
                body->AddChild<Label>(why, 12.0f, Theme::Danger)->SetWrap(true);
                return;
            }
            if (t.rows.empty())
            {
                body->AddChild<Label>(Format(Tr("No rows yet: the {0} is empty."), { r["name"].get<std::string>() }), 13.0f, Theme::TextDim);
                return;
            }
            const float width = std::max(90.0f, 720.0f / static_cast<float>(std::max<size_t>(t.columns.size(), 1)));
            auto said = [&](const Bim::Schedules::Column& c, const Bim::Json& v)
            {
                const std::string text = Bim::Schedules::Shown(m.units, c.kind, v);
                return v.is_boolean() ? Tr(text) : text;
            };
            Node* head = Row(body, 4.0f);
            for (const Bim::Schedules::Column& c : t.columns)
            {
                auto* l = Sized(head->AddChild<Label>(Bim::Schedules::Heading(m.units, { Tr(c.heading), c.kind }), 13.0f), width);
                l->SetBold(true);
                l->SetEllipsis(true);
            }
            auto* scroll = body->AddChild<ScrollView>();
            scroll->EditLayoutStyle().height = 340.0f;
            Node* list = scroll->SetContent<Node>();
            list->EditLayoutStyle().gap = 2.0f;
            for (size_t i = 0; i < t.rows.size(); ++i)
            {
                Node* row = Row(list, 4.0f);
                const std::vector<Bim::Id> ids = t.ids[i];
                for (size_t k = 0; k < t.rows[i].size(); ++k)
                {
                    const std::string text = said(t.columns[k], t.rows[i][k]);
                    if (k == 0)
                    {
                        // 第一格单击：选中这一行列出的元素
                        auto* b = Sized(row->AddChild<Button>(text.empty() ? std::string("·") : text, [this, ids] { SetSelection(ids); }), width);
                        b->SetStyle(ButtonStyle::Flat());
                        continue;
                    }
                    Sized(row->AddChild<Label>(text, 13.0f), width)->SetEllipsis(true);
                }
            }
            if (t.totals)
            {
                Node* row = Row(body, 4.0f);
                for (size_t k = 0; k < t.totals->size(); ++k)
                {
                    const Bim::Json& v = (*t.totals)[k];
                    auto* l = Sized(row->AddChild<Label>(v.is_null() ? (k == 0 ? Tr("Total") : std::string()) : said(t.columns[k], v), 13.0f), width);
                    l->SetBold(true);
                }
            }
            body->AddChild<Label>(Tr("Click a row's first cell to select what it lists."), 12.0f, Theme::TextDim);
        };
        d->dialog->AddButton(Tr("Close"), [d] { d->dialog->Close(); }, DialogButtonRole::Default);
        d->dialog->SetOnClosed([build] { *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }

    void MainFrame::OpenNewSchedule()
    {
        struct State
        {
            int         category = 0;
            std::string name;
            bool        named = false;  // 名称改过：换类别时不再跟着改
            std::string error;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        const std::vector<std::string> cats = Bim::Schedules::Categories();
        std::vector<std::string> labels;
        for (const std::string& c : cats)
            labels.push_back(CategoryLabel(c));
        // 默认名用类别的单数（"房间明细表"）；与内置明细表同名时编号，内置的名称留给内置的那张
        const auto defaultName = [this, cats](int i)
        {
            const char* one = Props::CategoryName(cats[i], false);
            const std::string base = Format(Tr("{0} Schedule"), { *one ? Tr(one) : cats[i] });
            return m_session.GetModel().FreeViewName(base, Bim::Schedules::BuiltinOf(&base, nullptr).has_value());
        };
        d->name = defaultName(0);
        auto dialog = std::make_unique<Dialog>(Tr("New Schedule"));
        d->dialog = dialog.get();
        Node* body = d->dialog->GetBody();
        LayoutStyle& bs = body->EditLayoutStyle();
        bs.direction = FlexDirection::Column;
        bs.gap       = 8.0f;
        bs.width     = 420.0f;
        Node* row = Row(body);
        Sized(row->AddChild<Label>(Tr("Category"), 13.0f), 90.0f);
        auto* combo = Sized(row->AddChild<ComboBox>(labels, 0), 280.0f);
        row = Row(body);
        Sized(row->AddChild<Label>(Tr("Name"), 13.0f), 90.0f);
        auto* box = Sized(row->AddChild<TextBox>(d->name), 280.0f);
        auto* message = body->AddChild<Label>("", 12.0f, Theme::Danger);
        message->SetWrap(true);
        combo->SetOnChanged([d, box, defaultName](int i)
        {
            d->category = i;
            if (!d->named && i >= 0)
            {
                d->name = defaultName(i);
                box->SetText(d->name);
            }
        });
        box->SetOnChanged([d](const std::string& s) { d->name = s, d->named = true; });
        d->dialog->AddButton(Tr("Cancel"), [d] { d->dialog->Close(); });
        d->dialog->AddButton(Tr("Create"), [this, d, cats, message]
        {
            const std::string name(Bim::Text::Trim(d->name));
            if (name.empty() || d->category < 0)
                return;
            Bim::Json r;
            if (!RunQuiet("schedule", Bim::Json{ { "name", name }, { "category", cats[d->category] } }, d->error, &r))
            {
                message->SetText(d->error);
                return;
            }
            d->dialog->Close();
            const Bim::Json which{ { "name", r["name"] } };
            m_platform->Post([this, which] { OpenSchedule(which); });
        }, DialogButtonRole::Default);
        m_ui->OpenPopup(std::move(dialog));
    }

    void MainFrame::OpenViewFilters()
    {
        struct State
        {
            std::string selected;       // 正在编辑的过滤器
            std::string error;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        if (!m_session.GetModel().viewFilters.empty())
            d->selected = m_session.GetModel().viewFilters.begin()->first;
        auto dialog = std::make_unique<Dialog>(Tr("View Filters"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        // 选中的过滤器改成 f：一条 filter 命令（整个类别与规则）
        auto save = [this, d, rebuild](const Bim::ViewFilter& f)
        {
            Bim::Json rules = Bim::Json::array();
            for (const auto& r : f.rules)
                rules.push_back(Bim::ToJson(r));
            if (RunQuiet("filter", Bim::Json{ { "name", d->selected }, { "categories", f.categories }, { "rules", rules } }, d->error))
                d->error.clear();
            rebuild();
        };
        *build = [this, d, rebuild, save]
        {
            const Bim::Model& m = m_session.GetModel();
            Node* body = d->dialog->GetBody();
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction  = FlexDirection::Row;
            bs.alignItems = Align::Start;
            bs.gap        = 12.0f;
            bs.width      = 780.0f;

            // 左边：过滤器，新建、删除
            auto* left = body->AddChild<Node>();
            LayoutStyle& ls = left->EditLayoutStyle();
            ls.direction = FlexDirection::Column;
            ls.gap       = 4.0f;
            ls.width     = 200.0f;
            left->AddChild<Label>(Tr("Filters"), 13.0f, Theme::TextDim);
            for (const auto& [name, _] : m.viewFilters)
            {
                auto* b = left->AddChild<Button>(name, [d, rebuild, n = name] { d->selected = n, d->error.clear(), rebuild(); });
                b->SetStyle(name == d->selected ? ButtonStyle::Primary() : ButtonStyle::Flat());
            }
            Node* row = Row(left, 4.0f);
            row->AddChild<Button>(Tr("New"), [this, d, rebuild]
            {
                std::string name;
                for (int i = 1;; ++i)
                {
                    name = Format(Tr("Filter {0}"), { std::to_string(i) });
                    if (!m_session.GetModel().viewFilters.count(name))
                        break;
                }
                if (RunQuiet("filter", Bim::Json{ { "name", name }, { "categories", Bim::Json::array({ "wall" }) } }, d->error))
                    d->selected = name, d->error.clear();
                rebuild();
            });
            row->AddChild<Button>(Tr("Delete"), [this, d, rebuild]
            {
                if (d->selected.empty())
                    return;
                if (RunQuiet("filter", Bim::Json{ { "name", d->selected }, { "delete", true } }, d->error))
                {
                    const auto& filters = m_session.GetModel().viewFilters;
                    d->selected = filters.empty() ? std::string() : filters.begin()->first;
                    d->error.clear();
                }
                rebuild();
            });

            // 右边：选中的过滤器的类别与规则
            auto* right = body->AddChild<Node>();
            LayoutStyle& rs = right->EditLayoutStyle();
            rs.direction = FlexDirection::Column;
            rs.gap       = 6.0f;
            rs.grow      = 1.0f;
            const auto it = m.viewFilters.find(d->selected);
            if (it == m.viewFilters.end())
            {
                right->AddChild<Label>(Tr("Make a filter with New, then choose its categories and rules."), 13.0f, Theme::TextDim)->SetWrap(true);
                return;
            }
            const Bim::ViewFilter f = it->second;
            right->AddChild<Label>(Tr("Categories"), 13.0f, Theme::TextDim);
            Node* cols = Row(right, 8.0f);
            cols->EditLayoutStyle().alignItems = Align::Start;
            const std::vector<std::string> cats = Bim::Schedules::Categories();
            std::vector<Node*> columns;
            for (int i = 0; i < 3; ++i)
            {
                auto* col = cols->AddChild<Node>();
                col->EditLayoutStyle().gap   = 2.0f;
                col->EditLayoutStyle().width = 175.0f;
                columns.push_back(col);
            }
            const size_t per = (cats.size() + 2) / 3;
            for (size_t i = 0; i < cats.size(); ++i)
            {
                const std::string c = cats[i];
                const bool on = std::find(f.categories.begin(), f.categories.end(), c) != f.categories.end();
                auto* cb = columns[i / per]->AddChild<CheckBox>(CategoryLabel(c), on);
                cb->SetOnChanged([f, c, save](bool now)
                {
                    Bim::ViewFilter g = f;
                    if (now)
                        g.categories.push_back(c);
                    else
                        std::erase(g.categories, c);
                    save(g);
                });
            }
            // 规则的字段：选中的每个类别都有的
            std::vector<std::pair<std::string, Bim::Schedules::Kind>> fields;
            for (const std::string& c : f.categories)
            {
                const auto mine = Bim::Schedules::Fields(m, c);
                if (&c == &f.categories.front())
                    fields = mine;
                else
                    std::erase_if(fields, [&](const auto& x) { return std::none_of(mine.begin(), mine.end(), [&](const auto& y) { return y.first == x.first; }); });
            }
            std::vector<std::string> fieldLabels, opLabels;
            for (const auto& [n, _] : fields)
                fieldLabels.push_back(Tr(n));
            for (const auto& [_, label] : Ops)
                opLabels.push_back(Tr(label));
            right->AddChild<Label>(Tr("Rules: elements of these categories that meet every rule"), 13.0f, Theme::TextDim);
            for (size_t i = 0; i < f.rules.size(); ++i)
            {
                const Bim::Schedules::Filter& r = f.rules[i];
                Node* line = Row(right, 6.0f);
                const auto fi = std::find_if(fields.begin(), fields.end(), [&](const auto& x) { return x.first == r.field; });
                auto* field = Sized(line->AddChild<ComboBox>(fieldLabels, fi == fields.end() ? -1 : static_cast<int>(fi - fields.begin())), 160.0f);
                field->SetOnChanged([f, i, fields, save](int k)
                {
                    if (k < 0)
                        return;
                    Bim::ViewFilter g = f;
                    g.rules[i].field = fields[k].first;
                    save(g);
                });
                const auto oi = std::find_if(std::begin(Ops), std::end(Ops), [&](const auto& x) { return x.first == r.op; });
                auto* op = Sized(line->AddChild<ComboBox>(opLabels, static_cast<int>(oi - std::begin(Ops))), 190.0f);
                op->SetOnChanged([f, i, save](int k)
                {
                    if (k < 0)
                        return;
                    Bim::ViewFilter g = f;
                    g.rules[i].op = Ops[k].first;
                    save(g);
                });
                // 值：字段存数时读成数，是 / 否读 yes、no，别的为文字
                const Bim::Schedules::Kind kind = fi == fields.end() ? Bim::Schedules::Kind::Text : fi->second;
                const std::string shown = r.value.is_null() ? std::string() : r.value.is_string() ? r.value.get<std::string>() : r.value.dump();
                auto* value = Sized(line->AddChild<TextBox>(shown), 140.0f);
                value->SetOnChanged([f, i, kind, save](const std::string& text)
                {
                    Bim::ViewFilter g = f;
                    const std::string t(Bim::Text::Trim(text));
                    Bim::Json v = t;
                    if (kind == Bim::Schedules::Kind::YesNo)
                        v = Bim::Text::AsciiLower(t) == "yes" || t == "1" || Bim::Text::AsciiLower(t) == "true";
                    else if (kind != Bim::Schedules::Kind::Text)
                    {
                        const auto n = Bim::Text::ParseNumber(t);
                        if (!n)
                            return;
                        v = *n;
                    }
                    g.rules[i].value = v;
                    save(g);
                });
                line->AddChild<Button>(Tr("Remove"), [f, i, save]
                {
                    Bim::ViewFilter g = f;
                    g.rules.erase(g.rules.begin() + static_cast<std::ptrdiff_t>(i));
                    save(g);
                })->SetStyle(ButtonStyle::Flat());
            }
            if (!fields.empty())
                right->AddChild<Button>(Tr("Add Rule"), [f, fields, save]
                {
                    Bim::ViewFilter g = f;
                    g.rules.push_back(Bim::Schedules::Filter{ fields.front().first, Bim::Schedules::Op::HasValue, nullptr });
                    save(g);
                });
            const auto picks = Bim::Schedules::Picks(m, f.categories, f.rules);
            right->AddChild<Label>(Format(TrN("It picks out {0} element.", "It picks out {0} elements.", static_cast<long long>(picks.size())),
                                          { std::to_string(picks.size()) }),
                                   12.0f, Theme::TextDim);
            if (!d->error.empty())
                right->AddChild<Label>(d->error, 12.0f, Theme::Danger)->SetWrap(true);
        };
        d->dialog->AddButton(Tr("Close"), [d] { d->dialog->Close(); }, DialogButtonRole::Default);
        d->dialog->SetOnClosed([build] { *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }

    void MainFrame::OpenCreateTemplate()
    {
        struct State
        {
            std::string name;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        const Bim::Model& m = m_session.GetModel();
        const std::string* view = m_planLevel ? m.ViewName(PlanKey()) : nullptr;
        for (int i = 1;; ++i)
        {
            d->name = Format(Tr("View Template {0}"), { std::to_string(i) });
            if (!m.viewTemplates.count(d->name))
                break;
        }
        auto dialog = std::make_unique<Dialog>(Tr("Create Template from Current View"));
        d->dialog = dialog.get();
        Node* body = d->dialog->GetBody();
        LayoutStyle& bs = body->EditLayoutStyle();
        bs.direction = FlexDirection::Column;
        bs.gap       = 8.0f;
        bs.width     = 420.0f;
        body->AddChild<Label>(Format(Tr("A view template with {0}'s visibility/graphics, scale, detail level and view range."),
                                     { view ? *view : std::string(Bim::View3D::DefaultName) }),
                              12.0f, Theme::TextDim)
            ->SetWrap(true);
        Node* row = Row(body);
        Sized(row->AddChild<Label>(Tr("Name"), 13.0f), 70.0f);
        auto* box = Sized(row->AddChild<TextBox>(d->name), 300.0f);
        box->SetOnChanged([d](const std::string& s) { d->name = s; });
        auto* message = body->AddChild<Label>("", 12.0f, Theme::Danger);
        message->SetWrap(true);
        d->dialog->AddButton(Tr("Cancel"), [d] { d->dialog->Close(); });
        d->dialog->AddButton(Tr("Create"), [this, d, message]
        {
            const std::string name(Bim::Text::Trim(d->name));
            if (name.empty())
                return;
            const Bim::Json from = m_planLevel ? Bim::ToJson(PlanKey()) : Bim::Json(Bim::View3D::DefaultName);
            std::string error;
            // 默认三维视图还没建出来时先建（图形不变的 graphics 不建）：由一个空的样板起步
            const Bim::Json p = m_planLevel || m_session.GetModel().Default3D() ? Bim::Json{ { "name", name }, { "from", from } }
                                                                               : Bim::Json{ { "name", name }, { "controls", Bim::Json::array({ "categories", "filters", "style" }) } };
            if (!RunQuiet("template", p, error))
            {
                message->SetText(error);
                return;
            }
            d->dialog->Close();
        }, DialogButtonRole::Default);
        m_ui->OpenPopup(std::move(dialog));
    }
}
