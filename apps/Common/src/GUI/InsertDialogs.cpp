// 移植自 BIMCraft crates/ui-egui/src/ 的 links.rs 与 browser.rs 的链接部分（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "I18n/Translator.h"
#include "Style/Theme.hpp"
#include "Widgets/Button.h"
#include "Widgets/Dialog.h"
#include "Widgets/Label.h"
#include "Widgets/ScrollView.h"
#include <filesystem>

// 插入选项卡：
//   - 链接模型：选一个项目或 IFC 文件链接进来；在项目文件夹里的存相对路径，两者一起挪
//   - 管理链接：每个链接的名称、文件与读到没有，再读或去掉；全部再读
//   - 从文件插入：选一个项目，它的元素全部放进这个项目，之后选中放进来的
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
