// 移植自 BIMCraft crates/ui-egui/src/ 的 lineweights.rs、objectstyles.rs、linestyles.rs、fillpatterns.rs、purge.rs 与 lib.rs 的项目参数（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Commands/Commands.h"
#include "GUI/Parameters.h"
#include "I18n/Translator.h"
#include "Model/ModelJson.h"
#include "Model/Text.h"
#include "Style/Theme.hpp"
#include "Widgets/Button.h"
#include "Widgets/ColorPicker.h"
#include "Widgets/ComboBox.h"
#include "Widgets/Controls.h"
#include "Widgets/Dialog.h"
#include "Widgets/Label.h"
#include "Widgets/NumberBox.h"
#include "Widgets/ScrollView.h"
#include "Widgets/TextBox.h"
#include <algorithm>
#include <cmath>

// 管理选项卡的项目设置，每个对话框的改动都是一条模型命令（失败时原因显示在对话框里）：
//   - 项目参数：左边项目的参数（新建、删除），右边选中的那个的名称、种类与类别
//   - 线宽：模型的笔在选中的比例下多宽、注释的笔多宽（纸上毫米），可加、删比例
//   - 对象样式：每个模型类别看到的线与剖到的线用几号笔
//   - 线样式：左边项目的线样式（新建、删除），右边笔号、颜色与线型图案
//   - 填充图案：左边项目的图案（新建、删除、导入 / 导出 PAT），右边制图或模型、各族线的角度与间距
//   - 清除未使用：勾选清除哪些，列出会去掉的，再清除
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

        Label* Caption(Node* parent, const std::string& text, float width = 110.0f)
        {
            return Sized(parent->AddChild<Label>(text, 13.0f), width);
        }

        // 类别给人看的名称：界面认得的翻译，否则原样
        std::string CategoryLabel(const std::string& c, bool several)
        {
            const char* n = Props::CategoryName(c, several);
            return *n ? Tr(n) : c;
        }

        // 左边一列：名称按钮（选中的高亮），之后是新建、删除
        Node* NameList(Node* body, const std::string& title, const std::vector<std::string>& names, const std::string& selected,
                       std::function<void(const std::string&)> pick)
        {
            auto* left = body->AddChild<Node>();
            LayoutStyle& ls = left->EditLayoutStyle();
            ls.direction = FlexDirection::Column;
            ls.gap       = 4.0f;
            ls.width     = 200.0f;
            left->AddChild<Label>(title, 13.0f, Theme::TextDim);
            auto* scroll = left->AddChild<ScrollView>();
            scroll->EditLayoutStyle().height = 300.0f;
            Node* list = scroll->SetContent<Node>();
            list->EditLayoutStyle().gap = 2.0f;
            for (const std::string& n : names)
            {
                auto* b = list->AddChild<Button>(n, [pick, n] { pick(n); });
                b->SetStyle(n == selected ? ButtonStyle::Primary() : ButtonStyle::Flat());
            }
            return left;
        }

        // 右边一列
        Node* Side(Node* body)
        {
            auto* right = body->AddChild<Node>();
            LayoutStyle& rs = right->EditLayoutStyle();
            rs.direction = FlexDirection::Column;
            rs.gap       = 6.0f;
            rs.grow      = 1.0f;
            return right;
        }

        // 左右两栏的对话框内容
        void TwoColumns(Node* body, float width)
        {
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction  = FlexDirection::Row;
            bs.alignItems = Align::Start;
            bs.gap        = 12.0f;
            bs.width      = width;
        }

        // 名称框：按 Enter 或离开时改名（rename 收到去掉首尾空白、与原名不同的新名）
        void RenameBox(Node* parent, const std::string& name, std::function<void(const std::string&)> rename)
        {
            Node* row = Row(parent);
            Caption(row, Tr("Name"));
            auto* box = Sized(row->AddChild<TextBox>(name), 240.0f);
            auto typed = std::make_shared<std::string>(name);
            auto go = [name, rename](const std::string& t)
            {
                const std::string to(Bim::Text::Trim(t));
                if (!to.empty() && to != name)
                    rename(to);
            };
            box->SetOnChanged([typed](const std::string& s) { *typed = s; });
            box->SetOnSubmit(go);
            box->SetOnFocusChanged([typed, go](bool focused) { if (!focused) go(*typed); });
        }

        // 不与 taken 重名的"<base> n"
        template <class Map>
        std::string Fresh(const Map& taken, const std::string& base)
        {
            for (int i = 1;; ++i)
            {
                const std::string n = base + " " + std::to_string(i);
                if (!taken.count(n))
                    return n;
            }
        }

        // 笔号的选项：0 是细线
        std::vector<std::string> PenLabels()
        {
            std::vector<std::string> out{ Tr("0 (hairline)") };
            for (int i = 1; i <= Bim::Pens::Max; ++i)
                out.push_back(std::to_string(i));
            return out;
        }

        // 清除未使用的种类：命令里的写法、界面上的说法
        const std::pair<const char*, const char*> PurgeKinds[] = {
            { "type", N_("Types") }, { "family", N_("Families") }, { "material", N_("Materials") },
            { "filter", N_("View Filters") }, { "template", N_("View Templates") },
        };

        // 清除会去掉的一项的种类（Kind::Label）给人看的说法
        std::string KindLabel(const std::string& label)
        {
            if (label == "material")
                return Tr("Material");
            if (label == "family")
                return Tr("Family");
            if (label == "family type")
                return Tr("Family Type");
            if (label == "view filter")
                return Tr("View Filter");
            if (label == "view template")
                return Tr("View Template");
            if (label.size() > 5 && label.ends_with(" type"))
                return Format(Tr("{0} Type"), { CategoryLabel(label.substr(0, label.size() - 5), false) });
            return label;
        }
    }

    // =========================================================
    // 项目参数
    // =========================================================
    void MainFrame::OpenProjectParameters()
    {
        struct State
        {
            std::string selected;
            std::string error;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        if (!m_session.GetModel().parameters.empty())
            d->selected = m_session.GetModel().parameters.begin()->first;
        auto dialog = std::make_unique<Dialog>(Tr("Project Parameters"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        // 一条 parameter 命令，成功时选中 pick
        auto run = [this, d, rebuild](Bim::Json p, std::optional<std::string> pick)
        {
            if (RunQuiet("parameter", p, d->error))
            {
                d->error.clear();
                if (pick)
                    d->selected = *pick;
            }
            rebuild();
        };
        *build = [this, d, rebuild, run]
        {
            const Bim::Model& m = m_session.GetModel();
            Node* body = d->dialog->GetBody();
            TwoColumns(body, 720.0f);
            std::vector<std::string> names;
            for (const auto& [n, _] : m.parameters)
                names.push_back(n);
            Node* left = NameList(body, Tr("Parameters"), names, d->selected, [d, rebuild](const std::string& n) { d->selected = n, d->error.clear(), rebuild(); });
            Node* row = Row(left, 4.0f);
            row->AddChild<Button>(Tr("New"), [this, run]
            {
                const std::string n = Fresh(m_session.GetModel().parameters, Tr("Parameter"));
                run(Bim::Json{ { "name", n }, { "categories", Bim::Json::array({ "wall" }) } }, n);
            });
            row->AddChild<Button>(Tr("Delete"), [this, d, run]
            {
                const auto& ps = m_session.GetModel().parameters;
                std::optional<std::string> next = std::string();
                for (const auto& [n, _] : ps)
                    if (n != d->selected)
                    {
                        next = n;
                        break;
                    }
                run(Bim::Json{ { "name", d->selected }, { "delete", true } }, next);
            })->SetEnabled(m.parameters.count(d->selected) > 0);

            Node* right = Side(body);
            const auto it = m.parameters.find(d->selected);
            if (it == m.parameters.end())
            {
                right->AddChild<Label>(Tr("A project parameter is a value each element of its categories can hold, shown in Properties and in schedules. Make one with New."), 13.0f, Theme::TextDim)->SetWrap(true);
            }
            else
            {
                const std::string name = it->first;
                const Bim::ProjectParameter p = it->second;
                RenameBox(right, name, [run, name](const std::string& to) { run(Bim::Json{ { "name", name }, { "rename", to } }, to); });
                row = Row(right);
                Caption(row, Tr("Type of Parameter"));
                // 与 ParameterKindOf::All 同序
                const std::vector<std::string> kinds{ TrC("parameter type", "Text"), TrC("parameter type", "Number"),
                                                      TrC("parameter type", "Length"), TrC("parameter type", "Yes/No") };
                int at = 0;
                for (size_t i = 0; i < Bim::ParameterKindOf::All.size(); ++i)
                    if (Bim::ParameterKindOf::All[i] == p.kind)
                        at = static_cast<int>(i);
                Sized(row->AddChild<ComboBox>(kinds, at), 160.0f)->SetOnChanged([run, name](int i)
                {
                    if (i >= 0)
                        run(Bim::Json{ { "name", name }, { "kind", Bim::ParameterKindOf::Name(Bim::ParameterKindOf::All[static_cast<size_t>(i)]) } }, std::nullopt);
                });
                right->AddChild<Label>(Tr("Categories"), 13.0f, Theme::TextDim);
                Node* cols = Row(right, 8.0f);
                cols->EditLayoutStyle().alignItems = Align::Start;
                std::vector<Node*> columns;
                for (int i = 0; i < 3; ++i)
                {
                    auto* col = cols->AddChild<Node>();
                    col->EditLayoutStyle().gap   = 2.0f;
                    col->EditLayoutStyle().width = 160.0f;
                    columns.push_back(col);
                }
                const size_t count = Bim::ParameterCategories.size();
                const size_t per = (count + 2) / 3;
                for (size_t i = 0; i < count; ++i)
                {
                    const std::string c = Bim::ParameterCategories[i];
                    const bool on = std::find(p.categories.begin(), p.categories.end(), c) != p.categories.end();
                    auto* cb = columns[i / per]->AddChild<CheckBox>(CategoryLabel(c, true), on);
                    cb->SetOnChanged([run, name, p, c](bool now)
                    {
                        std::vector<std::string> cats = p.categories;
                        if (now)
                            cats.push_back(c);
                        else
                            std::erase(cats, c);
                        run(Bim::Json{ { "name", name }, { "categories", cats } }, std::nullopt);
                    });
                }
            }
            if (!d->error.empty())
                right->AddChild<Label>(d->error, 12.0f, Theme::Danger)->SetWrap(true);
        };
        d->dialog->AddButton(Tr("Close"), [d] { d->dialog->Close(); }, DialogButtonRole::Default);
        d->dialog->SetOnClosed([build] { *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }

    // =========================================================
    // 线宽
    // =========================================================
    void MainFrame::OpenLineWeights()
    {
        struct State
        {
            double      scale = 100.0;      // 模型笔正在看的比例
            double      added = 50.0;       // 要加的比例
            std::string error;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        auto dialog = std::make_unique<Dialog>(Tr("Line Weights"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        auto run = [this, d, rebuild](const Bim::Json& p)
        {
            if (RunQuiet("line_weights", p, d->error))
                d->error.clear();
            rebuild();
        };
        *build = [this, d, run]
        {
            Bim::Json now;
            std::string why;
            if (!RunQuiet("line_weights", Bim::Json::object(), why, &now))
                return;
            Node* body = d->dialog->GetBody();
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction = FlexDirection::Column;
            bs.gap       = 6.0f;
            bs.width     = 460.0f;
            const Bim::Json model = now["model"];
            const Bim::Json annotation = now["annotation"];
            // 正在看的比例：没有了就看最接近 1 : 100 的
            size_t at = 0;
            for (size_t i = 0; i < model.size(); ++i)
                if (std::abs(std::log(model[i]["scale"].get<double>() / d->scale)) < std::abs(std::log(model[at]["scale"].get<double>() / d->scale)))
                    at = i;
            d->scale = model[at]["scale"].get<double>();

            body->AddChild<Label>(Tr("What each pen draws on paper, in mm. The model's pens draw at the scale nearest a view's own; annotations' pens at any scale."), 12.0f, Theme::TextDim)->SetWrap(true);
            Node* row = Row(body);
            Caption(row, Tr("Scale"));
            std::vector<std::string> scales;
            for (const auto& s : model)
                scales.push_back(s["name"].get<std::string>());
            Sized(row->AddChild<ComboBox>(scales, static_cast<int>(at)), 120.0f)->SetOnChanged([d, model](int i)
            {
                if (i >= 0)
                    d->scale = model[static_cast<size_t>(i)]["scale"].get<double>();
            });
            row->AddChild<Button>(Tr("Remove"), [model, at, run]
            {
                Bim::Json rest = Bim::Json::array();
                for (size_t i = 0; i < model.size(); ++i)
                    if (i != at)
                        rest.push_back(Bim::Json{ { "scale", model[i]["scale"] }, { "widths", model[i]["widths"] } });
                run(Bim::Json{ { "model", rest } });
            })->SetEnabled(model.size() > 1);
            row = Row(body);
            Caption(row, Tr("New scale 1 :"));
            auto* added = Sized(row->AddChild<NumberBox>(d->added, 1.0, 100000.0, 10.0, 0), 90.0f);
            added->SetOnChanged([d](double v) { d->added = v; });
            row->AddChild<Button>(Tr("Add"), [d, model, at, run]
            {
                Bim::Json all = Bim::Json::array();
                for (const auto& s : model)
                    all.push_back(Bim::Json{ { "scale", s["scale"] }, { "widths", s["widths"] } });
                all.push_back(Bim::Json{ { "scale", d->added }, { "widths", model[at]["widths"] } });
                d->scale = d->added;
                run(Bim::Json{ { "model", all } });
            });

            // 笔 1 到 16：模型的（在这个比例下）与注释的
            Node* head = Row(body);
            Caption(head, Tr("Pen"), 60.0f);
            Caption(head, Format(Tr("Model at {0}"), { scales[at] }), 150.0f);
            Caption(head, Tr("Annotation"), 150.0f);
            auto* scroll = body->AddChild<ScrollView>();
            scroll->EditLayoutStyle().height = 300.0f;
            Node* list = scroll->SetContent<Node>();
            list->EditLayoutStyle().gap = 2.0f;
            for (size_t pen = 0; pen < static_cast<size_t>(Bim::Pens::Max); ++pen)
            {
                row = Row(list);
                Caption(row, std::to_string(pen + 1), 60.0f);
                auto* mw = Sized(row->AddChild<NumberBox>(model[at]["widths"][pen].get<double>(), Bim::Pens::MinWidth, Bim::Pens::MaxWidth, 0.05, 2), 120.0f);
                mw->SetOnChanged([model, at, pen, run](double v)
                {
                    Bim::Json all = Bim::Json::array();
                    for (const auto& s : model)
                        all.push_back(Bim::Json{ { "scale", s["scale"] }, { "widths", s["widths"] } });
                    all[at]["widths"][pen] = v;
                    run(Bim::Json{ { "model", all } });
                });
                Sized(row->AddChild<Label>("", 13.0f), 22.0f);
                auto* aw = Sized(row->AddChild<NumberBox>(annotation[pen].get<double>(), Bim::Pens::MinWidth, Bim::Pens::MaxWidth, 0.05, 2), 120.0f);
                aw->SetOnChanged([annotation, pen, run](double v)
                {
                    Bim::Json a = annotation;
                    a[pen] = v;
                    run(Bim::Json{ { "annotation", a } });
                });
            }
            body->AddChild<Button>(Tr("Reset"), [run] { run(Bim::Json{ { "reset", true } }); })->SetEnabled(now["own"].get<bool>());
            if (!d->error.empty())
                body->AddChild<Label>(d->error, 12.0f, Theme::Danger)->SetWrap(true);
        };
        d->dialog->AddButton(Tr("Close"), [d] { d->dialog->Close(); }, DialogButtonRole::Default);
        d->dialog->SetOnClosed([build] { *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }

    // =========================================================
    // 对象样式
    // =========================================================
    void MainFrame::OpenObjectStyles()
    {
        struct State
        {
            std::string error;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        auto dialog = std::make_unique<Dialog>(Tr("Object Styles"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        auto run = [this, d, rebuild](const Bim::Json& p)
        {
            if (RunQuiet("object_styles", p, d->error))
                d->error.clear();
            rebuild();
        };
        *build = [this, d, run]
        {
            Bim::Json now;
            std::string why;
            if (!RunQuiet("object_styles", Bim::Json::object(), why, &now))
                return;
            Node* body = d->dialog->GetBody();
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction = FlexDirection::Column;
            bs.gap       = 6.0f;
            bs.width     = 480.0f;
            body->AddChild<Label>(Tr("The pen each category's lines draw in, in every plan and section, where it is seen and where a view cuts it. A view's Visibility/Graphics overrides them."), 12.0f, Theme::TextDim)->SetWrap(true);
            Node* head = Row(body);
            Caption(head, Tr("Category"), 180.0f);
            Caption(head, TrC("object style", "Projection"), 130.0f);
            Caption(head, TrC("object style", "Cut"), 130.0f);
            auto* scroll = body->AddChild<ScrollView>();
            scroll->EditLayoutStyle().height = 360.0f;
            Node* list = scroll->SetContent<Node>();
            list->EditLayoutStyle().gap = 2.0f;
            const std::vector<std::string> pens = PenLabels();
            bool own = false;
            for (const auto& c : now["categories"])
            {
                const std::string cat = c["category"].get<std::string>();
                own = own || c["own"].get<bool>();
                // MiniBIM 还没有的类别（管井、楼梯、坡道、栏杆）不列，同可见性 / 图形
                if (!*Props::CategoryName(cat, true))
                    continue;
                Node* row = Row(list);
                auto* l = Caption(row, CategoryLabel(cat, true), 180.0f);
                l->SetBold(c["own"].get<bool>());
                for (const char* key : { "projection", "cut" })
                {
                    const std::string k = key;
                    auto* box = Sized(row->AddChild<ComboBox>(pens, c[k].get<int>()), 120.0f);
                    box->SetMaxVisibleItems(17);
                    box->SetOnChanged([run, cat, k](int i)
                    {
                        if (i >= 0)
                            run(Bim::Json{ { "categories", { { cat, { { k, i } } } } } });
                    });
                }
            }
            body->AddChild<Button>(Tr("Reset"), [run] { run(Bim::Json{ { "reset", true } }); })->SetEnabled(own);
            if (!d->error.empty())
                body->AddChild<Label>(d->error, 12.0f, Theme::Danger)->SetWrap(true);
        };
        d->dialog->AddButton(Tr("Close"), [d] { d->dialog->Close(); }, DialogButtonRole::Default);
        d->dialog->SetOnClosed([build] { *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }

    // =========================================================
    // 线样式
    // =========================================================
    void MainFrame::OpenLineStyles()
    {
        struct State
        {
            std::string selected;
            std::string error;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        if (!m_session.GetModel().lineStyles.empty())
            d->selected = m_session.GetModel().lineStyles.begin()->first;
        auto dialog = std::make_unique<Dialog>(Tr("Line Styles"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        auto run = [this, d, rebuild](Bim::Json p, std::optional<std::string> pick)
        {
            if (RunQuiet("line_style", p, d->error))
            {
                d->error.clear();
                if (pick)
                    d->selected = *pick;
            }
            rebuild();
        };
        *build = [this, d, rebuild, run]
        {
            const Bim::Model& m = m_session.GetModel();
            Node* body = d->dialog->GetBody();
            TwoColumns(body, 600.0f);
            std::vector<std::string> names;
            for (const auto& [n, _] : m.lineStyles)
                names.push_back(n);
            Node* left = NameList(body, Tr("Line Styles"), names, d->selected, [d, rebuild](const std::string& n) { d->selected = n, d->error.clear(), rebuild(); });
            Node* row = Row(left, 4.0f);
            row->AddChild<Button>(Tr("New"), [this, run]
            {
                const std::string n = Fresh(m_session.GetModel().lineStyles, Tr("Line Style"));
                run(Bim::Json{ { "name", n }, { "pen", 1 } }, n);
            });
            row->AddChild<Button>(Tr("Delete"), [this, d, run]
            {
                std::optional<std::string> next = std::string();
                for (const auto& [n, _] : m_session.GetModel().lineStyles)
                    if (n != d->selected)
                    {
                        next = n;
                        break;
                    }
                run(Bim::Json{ { "name", d->selected }, { "delete", true } }, next);
            })->SetEnabled(m.lineStyles.count(d->selected) > 0);

            Node* right = Side(body);
            const auto it = m.lineStyles.find(d->selected);
            if (it != m.lineStyles.end())
            {
                const std::string name = it->first;
                const Bim::LineStyle st = it->second;
                RenameBox(right, name, [run, name](const std::string& to) { run(Bim::Json{ { "name", name }, { "rename", to } }, to); });
                row = Row(right);
                Caption(row, Tr("Line Weight"));
                auto* pen = Sized(row->AddChild<ComboBox>(PenLabels(), st.pen), 120.0f);
                pen->SetMaxVisibleItems(17);
                pen->SetOnChanged([run, name](int i) { if (i >= 0) run(Bim::Json{ { "name", name }, { "pen", i } }, std::nullopt); });
                row = Row(right);
                Caption(row, Tr("Color"));
                auto* colour = Sized(row->AddChild<ColorButton>(RGBA(st.color[0], st.color[1], st.color[2])), 80.0f);
                colour->SetOnChanged([run, name](Color32 v)
                {
                    run(Bim::Json{ { "name", name }, { "color", Bim::Json::array({ v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF }) } }, std::nullopt);
                });
                row = Row(right);
                Caption(row, Tr("Line Pattern"));
                std::vector<std::string> patterns{ Tr(Bim::Lines::Solid) }, keys{ Bim::Lines::Solid };
                int at = 0;
                for (const auto& [n, _] : m.linePatterns)
                {
                    if (st.pattern == n)
                        at = static_cast<int>(keys.size());
                    patterns.push_back(n);
                    keys.push_back(n);
                }
                auto* pat = Sized(row->AddChild<ComboBox>(patterns, at), 200.0f);
                pat->SetMaxVisibleItems(16);
                pat->SetOnChanged([run, name, keys](int i) { if (i >= 0) run(Bim::Json{ { "name", name }, { "pattern", keys[static_cast<size_t>(i)] } }, std::nullopt); });
            }
            if (!d->error.empty())
                right->AddChild<Label>(d->error, 12.0f, Theme::Danger)->SetWrap(true);
        };
        d->dialog->AddButton(Tr("Close"), [d] { d->dialog->Close(); }, DialogButtonRole::Default);
        d->dialog->SetOnClosed([build] { *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }

    // =========================================================
    // 填充图案
    // =========================================================
    void MainFrame::OpenFillPatterns()
    {
        struct State
        {
            std::string selected;
            std::string error;
            std::string note;       // 导入、导出之后的说明
            bool        closed = false;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        if (!m_session.GetModel().fillPatterns.empty())
            d->selected = m_session.GetModel().fillPatterns.begin()->first;
        auto dialog = std::make_unique<Dialog>(Tr("Fill Patterns"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        auto run = [this, d, rebuild](Bim::Json p, std::optional<std::string> pick)
        {
            if (RunQuiet("fill_pattern", p, d->error))
            {
                d->error.clear();
                if (pick)
                    d->selected = *pick;
            }
            rebuild();
        };
        *build = [this, d, rebuild, run]
        {
            const Bim::Model& m = m_session.GetModel();
            Node* body = d->dialog->GetBody();
            TwoColumns(body, 680.0f);
            std::vector<std::string> names;
            for (const auto& [n, _] : m.fillPatterns)
                names.push_back(n);
            Node* left = NameList(body, Tr("Fill Patterns"), names, d->selected, [d, rebuild](const std::string& n) { d->selected = n, d->error.clear(), rebuild(); });
            Node* row = Row(left, 4.0f);
            row->AddChild<Button>(Tr("New"), [this, run]
            {
                const std::string n = Fresh(m_session.GetModel().fillPatterns, Tr("Fill Pattern"));
                run(Bim::Json{ { "name", n }, { "target", "drafting" }, { "lines", Bim::Json::parse(R"([{"angle": 45, "spacing": 3}])") } }, n);
            });
            row->AddChild<Button>(Tr("Delete"), [this, d, run]
            {
                std::optional<std::string> next = std::string();
                for (const auto& [n, _] : m_session.GetModel().fillPatterns)
                    if (n != d->selected)
                    {
                        next = n;
                        break;
                    }
                run(Bim::Json{ { "name", d->selected }, { "delete", true } }, next);
            })->SetEnabled(m.fillPatterns.count(d->selected) > 0);
            row = Row(left, 4.0f);
            row->AddChild<Button>(Tr("Import..."), [this, d, rebuild]
            {
                m_platform->PickFile(FileKind::FillPatterns, [this, d, rebuild](const std::string& path)
                {
                    if (path.empty() || d->closed)
                        return;
                    Bim::Json r;
                    if (RunQuiet("import_fill_patterns", Bim::Json{ { "path", path } }, d->error, &r))
                    {
                        const size_t n = r["imported"].size(), out = r["left_out_count"].get<size_t>();
                        d->error.clear();
                        d->note = Format(TrN("Imported {0} fill pattern.", "Imported {0} fill patterns.", static_cast<long long>(n)), { std::to_string(n) });
                        if (out > 0)
                            d->note += " " + Format(TrN("Left out {0} that MiniBIM can't use.", "Left out {0} that MiniBIM can't use.", static_cast<long long>(out)),
                                                    { std::to_string(out) });
                        if (n > 0)
                            d->selected = r["imported"][0].get<std::string>();
                    }
                    rebuild();
                });
            });
            row->AddChild<Button>(Tr("Export..."), [this, d, rebuild]
            {
                AskSavePath(SaveKind::FillPatterns, ProjectStem() + ".pat", [this, d, rebuild](const std::string& path)
                {
                    if (d->closed)
                        return;
                    if (RunQuiet("export_fill_patterns", Bim::Json{ { "path", path } }, d->error))
                    {
                        d->error.clear();
                        d->note = Tr("Exported the fill patterns.");
                        m_platform->OnDocumentSaved(path);
                    }
                    rebuild();
                });
            });

            Node* right = Side(body);
            const auto it = m.fillPatterns.find(d->selected);
            if (it != m.fillPatterns.end())
            {
                const std::string name = it->first;
                const Bim::FillPattern fp = it->second;
                RenameBox(right, name, [run, name](const std::string& to) { run(Bim::Json{ { "name", name }, { "rename", to } }, to); });
                row = Row(right);
                Caption(row, Tr("Pattern Type"));
                Sized(row->AddChild<ComboBox>(std::vector<std::string>{ Tr("Drafting"), Tr("Model") }, fp.model ? 1 : 0), 140.0f)->SetOnChanged([run, name](int i)
                {
                    if (i >= 0)
                        run(Bim::Json{ { "name", name }, { "target", i == 1 ? "model" : "drafting" } }, std::nullopt);
                });
                right->AddChild<Label>(fp.model ? Tr("Model patterns' lengths are mm in the building.") : Tr("Drafting patterns' lengths are mm on paper, the same at any scale."),
                                       12.0f, Theme::TextDim)->SetWrap(true);
                // 各族线：角度与间距；有虚线、偏移的照原样留着
                const auto save = [run, name](std::vector<Bim::HatchLine> lines)
                {
                    Bim::Json all = Bim::Json::array();
                    for (const auto& l : lines)
                        all.push_back(Bim::ToJson(l));
                    run(Bim::Json{ { "name", name }, { "lines", all } }, std::nullopt);
                };
                if (fp.IsSolid())
                    right->AddChild<Label>(Tr("Solid fill"), 13.0f);
                else
                {
                    Node* head = Row(right);
                    Caption(head, Tr("Angle"), 100.0f);
                    Caption(head, Tr("Spacing"), 100.0f);
                }
                for (size_t i = 0; i < fp.lines.size(); ++i)
                {
                    row = Row(right);
                    const Bim::HatchLine l = fp.lines[i];
                    Sized(row->AddChild<NumberBox>(l.angle, -360.0, 360.0, 15.0, 2), 90.0f)->SetOnChanged([fp, i, save](double v)
                    {
                        auto lines = fp.lines;
                        lines[i].angle = v;
                        save(lines);
                    });
                    Sized(row->AddChild<NumberBox>(l.spacing, -100000.0, 100000.0, 0.5, 3), 90.0f)->SetOnChanged([fp, i, save](double v)
                    {
                        auto lines = fp.lines;
                        lines[i].spacing = v;
                        save(lines);
                    });
                    if (!l.dashes.empty())
                        row->AddChild<Label>(Tr("dashed"), 12.0f, Theme::TextDim);
                    row->AddChild<Button>(Tr("Remove"), [fp, i, save]
                    {
                        auto lines = fp.lines;
                        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
                        save(lines);
                    })->SetStyle(ButtonStyle::Flat());
                }
                right->AddChild<Button>(Tr("Add Lines"), [fp, save]
                {
                    auto lines = fp.lines;
                    Bim::HatchLine l;
                    l.angle   = lines.empty() ? 45.0 : lines.back().angle + 90.0;
                    l.spacing = lines.empty() ? 3.0 : lines.back().spacing;
                    lines.push_back(l);
                    save(lines);
                });
            }
            if (!d->note.empty())
                right->AddChild<Label>(d->note, 12.0f, Theme::TextDim)->SetWrap(true);
            if (!d->error.empty())
                right->AddChild<Label>(d->error, 12.0f, Theme::Danger)->SetWrap(true);
        };
        d->dialog->AddButton(Tr("Close"), [d] { d->dialog->Close(); }, DialogButtonRole::Default);
        d->dialog->SetOnClosed([d, build] { d->closed = true; *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }

    // =========================================================
    // 清除未使用
    // =========================================================
    void MainFrame::OpenPurge()
    {
        struct State
        {
            std::vector<std::string> kinds{ "type", "family" };
            std::string              error;
            std::string              note;
            Dialog*                  dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        auto dialog = std::make_unique<Dialog>(Tr("Purge Unused"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        *build = [this, d, rebuild]
        {
            Node* body = d->dialog->GetBody();
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction = FlexDirection::Column;
            bs.gap       = 6.0f;
            bs.width     = 440.0f;
            body->AddChild<Label>(Tr("Takes out what nothing in the project uses, and then what that leaves unused. A category keeps one type, and a family one type."), 12.0f, Theme::TextDim)->SetWrap(true);
            // 种类：每行三个
            Node* row = nullptr;
            for (size_t i = 0; i < std::size(PurgeKinds); ++i)
            {
                if (i % 3 == 0)
                    row = Row(body, 4.0f);
                const std::string k = PurgeKinds[i].first;
                const bool on = std::find(d->kinds.begin(), d->kinds.end(), k) != d->kinds.end();
                Sized(row->AddChild<CheckBox>(Tr(PurgeKinds[i].second), on), 140.0f)->SetOnChanged([d, rebuild, k](bool now)
                {
                    std::erase(d->kinds, k);
                    if (now)
                        d->kinds.push_back(k);
                    d->note.clear();
                    rebuild();
                });
            }
            Bim::Json r;
            std::string why;
            const bool listed = !d->kinds.empty() && RunQuiet("purge", Bim::Json{ { "kinds", d->kinds }, { "dry_run", true } }, why, &r);
            const size_t n = listed ? r["count"].get<size_t>() : 0;
            auto* scroll = body->AddChild<ScrollView>();
            scroll->EditLayoutStyle().height = 260.0f;
            Node* list = scroll->SetContent<Node>();
            list->EditLayoutStyle().gap = 2.0f;
            if (n == 0)
                list->AddChild<Label>(Tr("Nothing of these is unused."), 13.0f, Theme::TextDim);
            for (size_t i = 0; i < n; ++i)
            {
                Node* line = Row(list);
                Caption(line, KindLabel(r["removed"][i]["kind"].get<std::string>()), 160.0f)->SetEllipsis(true);
                line->AddChild<Label>(r["removed"][i]["name"].get<std::string>(), 13.0f)->SetEllipsis(true);
            }
            body->AddChild<Button>(Format(TrN("Purge {0} Item", "Purge {0} Items", static_cast<long long>(n)), { std::to_string(n) }), [this, d, rebuild, n]
            {
                if (RunQuiet("purge", Bim::Json{ { "kinds", d->kinds } }, d->error))
                {
                    d->error.clear();
                    d->note = Format(TrN("Purged {0} item.", "Purged {0} items.", static_cast<long long>(n)), { std::to_string(n) });
                }
                rebuild();
            })->SetEnabled(n > 0);
            if (!d->note.empty())
                body->AddChild<Label>(d->note, 12.0f, Theme::TextDim);
            if (!d->error.empty())
                body->AddChild<Label>(d->error, 12.0f, Theme::Danger)->SetWrap(true);
        };
        d->dialog->AddButton(Tr("Close"), [d] { d->dialog->Close(); }, DialogButtonRole::Default);
        d->dialog->SetOnClosed([build] { *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }
}
