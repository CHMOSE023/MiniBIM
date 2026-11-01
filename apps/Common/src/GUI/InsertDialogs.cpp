// 移植自 BIMCraft crates/ui-egui/src/ 的 links.rs、cad.rs、clouds.rs 与 browser.rs、context.rs 的链接部分（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "I18n/Translator.h"
#include "Style/Theme.hpp"
#include "Model/ModelJson.h"
#include "Model/Text.h"
#include "Widgets/Button.h"
#include "Widgets/ComboBox.h"
#include "Widgets/Controls.h"
#include "Widgets/Dialog.h"
#include "Widgets/Label.h"
#include "Widgets/ScrollView.h"
#include "Widgets/TextBox.h"
#include <array>
#include <filesystem>

// 插入选项卡：
//   - 链接模型：选一个项目或 IFC 文件链接进来；在项目文件夹里的存相对路径，两者一起挪
//   - 管理链接：每个链接的名称、文件与读到没有，再读或去掉；全部再读
//   - 从文件插入：选一个项目，它的元素全部放进这个项目，之后选中放进来的
//   - 导入 CAD、链接 CAD：选一个 DXF 文件，再在对话框里说怎样带进来（名称、颜色、图层、单位、定位、仅当前视图，同 Revit 的导入 CAD 格式），
//     运行 import_cad；放在显示的平面的标高上（或只在这个平面里），之后选中它。带不进来时原因留在对话框里
//   - 点云：选一个扫描文件，再在对话框里说怎样带进来（名称、单位、间距、定位，同 Revit 的链接点云），运行 point_cloud；
//     放在显示的平面的标高上，之后选中它。一次扫描的几个测站的文件只经 point_cloud 命令（path 给一个列表）
namespace MiniBIM
{
    namespace
    {
        using namespace MiniGUI;

        std::filesystem::path ToPath(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

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

        // 图纸的单位，照 import_cad 的写法：auto 读文件的
        constexpr std::array<const char*, 6> Units = { "auto", "mm", "cm", "m", "in", "ft" };
        // 颜色怎样带进来，照 import_cad 的写法，以及它们的名称
        constexpr std::array<std::pair<const char*, const char*>, 3> Colors = {
            std::pair{ "preserve", N_("Preserve") }, std::pair{ "invert", N_("Invert") }, std::pair{ "black", N_("Black and White") } };
        // 扫描坐标的单位，照 point_cloud 的写法
        constexpr std::array<const char*, 5> CloudUnits = { "m", "mm", "cm", "in", "ft" };
    }

    void MainFrame::InsertPointCloud()
    {
        m_platform->PickFile(FileKind::PointCloud, [this](const std::string& path)
        {
            if (!path.empty())
                m_platform->Post([this, path] { OpenPointCloud(path); });
        });
    }

    void MainFrame::OpenPointCloud(const std::string& path)
    {
        struct State
        {
            std::string path;
            std::string name;
            int         units = 0;
            // 每个这么宽（毫米）的立方体留一个点，照打的；空着全留
            std::string spacing;
            // 在文件坐标说的地方（原点到原点），或中心放在原点上（中心到中心）
            bool        byFile = false;
            std::string error;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        d->path = path;
        const std::u8string stem = ToPath(path).stem().u8string();
        d->name = std::string(stem.begin(), stem.end());
        auto dialog = std::make_unique<Dialog>(Tr("Point Cloud"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        *build = [d]
        {
            Node* body = d->dialog->GetBody();
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction = FlexDirection::Column;
            bs.gap       = 6.0f;
            bs.width     = 430.0f;
            const std::u8string file = ToPath(d->path).filename().u8string();
            body->AddChild<Label>(std::string(file.begin(), file.end()), 13.0f, Theme::TextDim);
            Node* row = Row(body);
            Sized(row->AddChild<Label>(Tr("Name"), 13.0f), 160.0f);
            Sized(row->AddChild<TextBox>(d->name), 250.0f)->SetOnChanged([d](const std::string& s) { d->name = s; });
            row = Row(body);
            Sized(row->AddChild<Label>(Tr("Units"), 13.0f), 160.0f);
            std::vector<std::string> units(CloudUnits.begin(), CloudUnits.end());
            Sized(row->AddChild<ComboBox>(units, d->units), 250.0f)->SetOnChanged([d](int i) { d->units = std::max(i, 0); });
            row = Row(body);
            Sized(row->AddChild<Label>(Tr("Spacing (mm)"), 13.0f), 160.0f);
            auto* spacing = Sized(row->AddChild<TextBox>(d->spacing), 250.0f);
            spacing->SetPlaceholder(Tr("all the points"));
            spacing->SetOnChanged([d](const std::string& s) { d->spacing = s; });
            body->AddChild<Label>(Tr("Positioning"), 13.0f);
            // 两个勾选框当单选用：点一个时另一个跟着变
            auto* centre = body->AddChild<CheckBox>(Tr("Auto - Center to Center"), !d->byFile);
            centre->SetTooltip(Tr("Its middle on the project's origin and its foot on the level"));
            auto* origin = body->AddChild<CheckBox>(Tr("Auto - Origin to Origin"), d->byFile);
            origin->SetTooltip(Tr("Where its file's coordinates put it, as the survey placed it"));
            centre->SetOnChanged([d, origin](bool on) { d->byFile = !on; origin->SetChecked(!on); });
            origin->SetOnChanged([d, centre](bool on) { d->byFile = on; centre->SetChecked(!on); });
            if (!d->error.empty())
                body->AddChild<Label>(d->error, 12.0f, Theme::Danger)->SetWrap(true);
        };
        // 运行 point_cloud：放在显示的平面的标高上，选中它。拒绝时对话框开着，原因写在里面
        d->dialog->AddButton(Tr("OK"), [this, d, rebuild]
        {
            if (Bim::Text::Trim(d->name).empty())
                return;
            Bim::Json p{ { "path", d->path },
                         { "name", std::string(Bim::Text::Trim(d->name)) },
                         { "units", CloudUnits[static_cast<size_t>(d->units)] },
                         { "origin", d->byFile ? "file" : "centre" } };
            if (!Bim::Text::Trim(d->spacing).empty())
            {
                const auto s = Bim::Text::ParseNumber(d->spacing);
                if (!s)
                {
                    d->error = Format(Tr("`{0}` is not a number"), { std::string(Bim::Text::Trim(d->spacing)) });
                    rebuild();
                    return;
                }
                p["spacing"] = *s;
            }
            if (m_planLevel)
                p["level"] = Bim::ToJson(*m_planLevel);
            Bim::Json r;
            if (!RunQuiet("point_cloud", p, d->error, &r))
            {
                rebuild();
                return;
            }
            d->dialog->Close();
            SetSelection({ r["id"].get<Bim::Id>() });
        }, DialogButtonRole::Default);
        d->dialog->AddButton(Tr("Cancel"), [d] { d->dialog->Close(); }, DialogButtonRole::Cancel);
        d->dialog->SetOnClosed([build] { *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }

    void MainFrame::ImportCad(bool link)
    {
        m_platform->PickFile(FileKind::Dxf, [this, link](const std::string& path)
        {
            if (!path.empty())
                m_platform->Post([this, path, link] { OpenImportCad(path, link); });
        });
    }

    void MainFrame::OpenImportCad(const std::string& path, bool link)
    {
        struct State
        {
            std::string path;
            bool        link = false;
            std::string name;
            int         colors    = 0;
            bool        allLayers = false;
            int         units     = 0;
            // 它的原点在项目的原点（原点到原点），或它的中间（中心到中心）
            bool        byOrigin = false;
            // 只在显示的平面里，不在它标高的每个平面里
            bool        thisView = false;
            std::string error;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        d->path = path;
        d->link = link;
        const std::u8string stem = ToPath(path).stem().u8string();
        d->name = std::string(stem.begin(), stem.end());
        auto dialog = std::make_unique<Dialog>(link ? Tr("Link CAD") : Tr("Import CAD"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        const bool plan = m_planLevel.has_value();
        *build = [d, plan]
        {
            Node* body = d->dialog->GetBody();
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction = FlexDirection::Column;
            bs.gap       = 6.0f;
            bs.width     = 430.0f;
            const std::u8string file = ToPath(d->path).filename().u8string();
            body->AddChild<Label>(std::string(file.begin(), file.end()), 13.0f, Theme::TextDim);
            Node* row = Row(body);
            Sized(row->AddChild<Label>(Tr("Name"), 13.0f), 160.0f);
            auto* name = Sized(row->AddChild<TextBox>(d->name), 250.0f);
            name->SetOnChanged([d](const std::string& s) { d->name = s; });
            row = Row(body);
            Sized(row->AddChild<Label>(Tr("Colors"), 13.0f), 160.0f);
            std::vector<std::string> colors;
            for (const auto& [_, label] : Colors)
                colors.push_back(Tr(label));
            Sized(row->AddChild<ComboBox>(colors, d->colors), 250.0f)->SetOnChanged([d](int i) { d->colors = std::max(i, 0); });
            row = Row(body);
            Sized(row->AddChild<Label>(Tr("Layers/Levels"), 13.0f), 160.0f);
            Sized(row->AddChild<ComboBox>(std::vector<std::string>{ Tr("Visible"), Tr("All") }, d->allLayers ? 1 : 0), 250.0f)
                ->SetOnChanged([d](int i) { d->allLayers = i == 1; });
            row = Row(body);
            Sized(row->AddChild<Label>(Tr("Import units"), 13.0f), 160.0f);
            std::vector<std::string> units;
            for (const char* u : Units)
                units.push_back(std::string_view(u) == "auto" ? Tr("Auto-Detect") : std::string(u));
            Sized(row->AddChild<ComboBox>(units, d->units), 250.0f)->SetOnChanged([d](int i) { d->units = std::max(i, 0); });
            body->AddChild<Label>(Tr("Positioning"), 13.0f);
            // 两个勾选框当单选用：点一个时另一个跟着变
            auto* centre = body->AddChild<CheckBox>(Tr("Auto - Center to Center"), !d->byOrigin);
            centre->SetTooltip(Tr("Its middle on the project's origin"));
            auto* origin = body->AddChild<CheckBox>(Tr("Auto - Origin to Origin"), d->byOrigin);
            origin->SetTooltip(Tr("Its origin on the project's origin, where its file's coordinates put it"));
            centre->SetOnChanged([d, origin](bool on) { d->byOrigin = !on; origin->SetChecked(!on); });
            origin->SetOnChanged([d, centre](bool on) { d->byOrigin = on; centre->SetChecked(!on); });
            auto* only = body->AddChild<CheckBox>(Tr("Current view only"), d->thisView && plan);
            only->SetTooltip(Tr("In this plan only, not in every plan of its level"));
            only->SetEnabled(plan);
            only->SetOnChanged([d](bool on) { d->thisView = on; });
            if (!d->error.empty())
                body->AddChild<Label>(d->error, 12.0f, Theme::Danger)->SetWrap(true);
        };
        // 运行 import_cad：放在显示的平面的标高上（或只在这个平面里），选中它。拒绝时对话框开着，原因写在里面
        d->dialog->AddButton(Tr("OK"), [this, d, rebuild]
        {
            if (Bim::Text::Trim(d->name).empty())
                return;
            Bim::Json p{ { "path", d->path },
                         { "name", std::string(Bim::Text::Trim(d->name)) },
                         { "colors", Colors[static_cast<size_t>(d->colors)].first },
                         { "layers", d->allLayers ? "all" : "visible" },
                         { "units", Units[static_cast<size_t>(d->units)] },
                         { "position", d->byOrigin ? "origin" : "center" } };
            if (d->link)
                p["link"] = true;
            if (m_planLevel)
            {
                if (d->thisView)
                    p["view"] = Bim::ToJson(PlanKey());
                else
                    p["level"] = Bim::ToJson(*m_planLevel);
            }
            Bim::Json r;
            if (!RunQuiet("import_cad", p, d->error, &r))
            {
                rebuild();
                return;
            }
            d->dialog->Close();
            SetSelection({ r["id"].get<Bim::Id>() });
        }, DialogButtonRole::Default);
        d->dialog->AddButton(Tr("Cancel"), [d] { d->dialog->Close(); }, DialogButtonRole::Cancel);
        d->dialog->SetOnClosed([build] { *build = nullptr; });
        (*build)();
        m_ui->OpenPopup(std::move(dialog));
    }

    void MainFrame::LinkModel()
    {
        m_platform->PickFile(FileKind::Project, [this](const std::string& path)
        {
            if (path.empty())
                return;
            // 在项目文件夹里的存相对路径
            std::string kept = path;
            if (const auto& project = m_session.Path())
            {
                const std::filesystem::path dir = ToPath(*project).parent_path(), file = ToPath(path);
                std::error_code ec;
                const std::filesystem::path rel = std::filesystem::relative(file, dir, ec);
                if (!ec && !rel.empty() && !rel.is_absolute() && rel.begin()->u8string() != u8"..")
                {
                    const std::u8string u = rel.generic_u8string();
                    kept = std::string(u.begin(), u.end());
                }
            }
            RunCommand("link", Bim::Json{ { "path", kept } }, N_("Could not link the model"));
        });
    }

    void MainFrame::InsertFromFile()
    {
        m_platform->PickFile(FileKind::Project, [this](const std::string& path)
        {
            if (path.empty())
                return;
            Bim::Json r;
            if (!RunCommand("insert", Bim::Json{ { "path", path } }, N_("Could not insert from the file"), &r))
                return;
            std::vector<Bim::Id> ids;
            for (const auto& [_, here] : r["ids"].items())
                ids.push_back(here.get<Bim::Id>());
            SetSelection(std::move(ids));
        });
    }

    void MainFrame::OpenManageLinks()
    {
        struct State
        {
            std::string error;
            std::string note;
            Dialog*     dialog = nullptr;
        };
        auto d = std::make_shared<State>();
        auto dialog = std::make_unique<Dialog>(Tr("Manage Links"));
        d->dialog = dialog.get();
        auto build = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        // 再读一个（id）或全部链接，说读到了几个、哪些没读到
        auto reload = [this, d, rebuild](std::optional<Bim::Id> id)
        {
            Bim::Json r;
            if (RunQuiet("reload", id ? Bim::Json{ { "id", *id } } : Bim::Json::object(), d->error, &r))
            {
                d->error.clear();
                d->note = Format(TrN("Reloaded {0} link.", "Reloaded {0} links.", static_cast<long long>(r["reloaded"].size())),
                                 { std::to_string(r["reloaded"].size()) });
                for (const Bim::Json& f : r["failed"])
                    d->error += f["name"].get<std::string>() + ": " + f["error"].get<std::string>() + "\n";
            }
            rebuild();
        };
        *build = [this, d, rebuild, reload]
        {
            Node* body = d->dialog->GetBody();
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction = FlexDirection::Column;
            bs.gap       = 6.0f;
            bs.width     = 560.0f;
            const Bim::Model& m = m_session.GetModel();
            auto* scroll = body->AddChild<ScrollView>();
            scroll->EditLayoutStyle().height = 220.0f;
            Node* list = scroll->SetContent<Node>();
            list->EditLayoutStyle().gap = 4.0f;
            size_t n = 0;
            for (const auto& [id, e] : m.elements.Items())
            {
                const Bim::Link* l = e->As<Bim::Link>();
                if (!l)
                    continue;
                ++n;
                Node* line = Row(list);
                auto* name = line->AddChild<Label>(l->name, 13.0f);
                name->EditLayoutStyle().width = 130.0f;
                name->SetEllipsis(true);
                auto* file = line->AddChild<Label>(l->path, 12.0f, Theme::TextDim);
                file->EditLayoutStyle().width = 220.0f;
                file->SetEllipsis(true);
                const bool loaded = m.loaded.count(l->path) > 0;
                auto* state = line->AddChild<Label>(loaded ? Tr("Loaded") : Tr("Not loaded"), 12.0f, loaded ? Theme::TextDim : Theme::Danger);
                state->EditLayoutStyle().width = 70.0f;
                const Bim::Id lid = id;
                line->AddChild<Button>(Tr("Reload"), [reload, lid] { reload(lid); });
                line->AddChild<Button>(Tr("Remove"), [this, d, rebuild, lid]
                {
                    if (RunQuiet("delete", Bim::Json{ { "ids", { lid } } }, d->error))
                        d->error.clear();
                    d->note.clear();
                    rebuild();
                });
            }
            if (n == 0)
                list->AddChild<Label>(Tr("This project links no models. Link one with Link Model."), 13.0f, Theme::TextDim)->SetWrap(true);
            // 链接的 CAD 图纸：从文件再读
            bool heading = false;
            for (const auto& [cadName, cad] : m.cads)
            {
                if (!cad.Path())
                    continue;
                if (!heading)
                {
                    list->AddChild<Label>(Tr("Linked CAD drawings"), 13.0f)->EditLayoutStyle().margin.top = 8.0f;
                    heading = true;
                }
                Node* line = Row(list);
                auto* label = line->AddChild<Label>(cadName, 13.0f);
                label->EditLayoutStyle().width = 130.0f;
                label->SetEllipsis(true);
                auto* file = line->AddChild<Label>(*cad.Path(), 12.0f, Theme::TextDim);
                file->EditLayoutStyle().width = 300.0f;
                file->SetEllipsis(true);
                const std::string key = cadName;
                line->AddChild<Button>(Tr("Reload"), [this, d, rebuild, key]
                {
                    Bim::Json r;
                    if (RunQuiet("reload_cad", Bim::Json{ { "cad", key } }, d->error, &r))
                    {
                        d->error.clear();
                        d->note = r["changed"] == true ? Format(Tr("{0} was read again."), { key }) : Format(Tr("{0} hasn't changed."), { key });
                    }
                    rebuild();
                });
            }
            Node* actions = Row(body);
            actions->AddChild<Button>(Tr("Reload All"), [reload] { reload(std::nullopt); })->SetEnabled(n > 0);
            actions->AddChild<Button>(Tr("Link Model..."), [this, d]
            {
                d->dialog->Close();
                m_platform->Post([this] { LinkModel(); });
            });
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
