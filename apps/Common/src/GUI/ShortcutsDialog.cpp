// 两个字母的快捷键：默认的表、在视图上打的字母、键盘快捷键对话框（KS）。键表本身（不碰界面控件）在 Shortcuts.cpp
// 移植自 BIMCraft crates/ui-egui/src/shortcuts.rs 与 lib.rs 的 SHORTCUTS（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Commands/Session.h"
#include "Core/Log.h"
#include "Core/UIContext.h"
#include "I18n/Translator.h"
#include "Style/Theme.hpp"
#include "Widgets/Button.h"
#include "Widgets/ComboBox.h"
#include "Widgets/Dialog.h"
#include "Widgets/Label.h"
#include "Widgets/ScrollView.h"
#include "Widgets/TextBox.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace MiniBIM
{
    namespace
    {
        using namespace MiniGUI;

        // 桌面自己的（BIMCraft 的 SHORTCUTS 里 MiniBIM 有的），先于模型命令的快捷名
        constexpr std::pair<const char*, const char*> Desktop[] = {
            { "MD", "modify.select" },     { "MM", "modify.mirrorPick" }, { "DM", "modify.mirrorDraw" }, { "AL", "modify.align" },
            { "OF", "modify.offset" },     { "3D", "view.home" },         { "ZF", "view.zoomFit" },      { "ZE", "view.zoomFit" },
            { "ZA", "view.zoomFit" },      { "PP", "view.properties" },   { "VP", "view.properties" },   { "NP", "view.browser" },
            { "KS", "file.shortcuts" },
        };

        // 模型命令（Bim::Command，它的 aliases 是快捷名）是哪个界面命令
        constexpr std::pair<const char*, const char*> Tools[] = {
            { "level", "arch.level" },   { "grid", "arch.grid" },          { "wall", "arch.wall" },     { "door", "arch.door" },
            { "window", "arch.window" }, { "room", "arch.room" },          { "component", "arch.component" },
            { "move", "modify.move" },   { "copy", "modify.copy" },        { "align", "modify.align" }, { "rotate", "modify.rotate" },
            { "corner", "modify.trim" }, { "split", "modify.split" },      { "delete", "edit.delete" },
        };

        const char* ToolOf(std::string_view bimCommand)
        {
            for (const auto& [bim, ui] : Tools)
                if (bimCommand == bim)
                    return ui;
            return nullptr;
        }

        std::string Lower(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            return out;
        }

        // 菜单上的名称去掉末尾的 "..."（"Keyboard Shortcuts..." 在文件里写作 Keyboard Shortcuts）
        std::string Bare(std::string s)
        {
            while (!s.empty() && (s.back() == '.' || s.back() == ' '))
                s.pop_back();
            if (s.size() >= 3 && s.compare(s.size() - 3, 3, "\xE2\x80\xA6") == 0)
                s.resize(s.size() - 3);
            return s;
        }

        std::filesystem::path ToPath(const std::string& utf8)
        {
            return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }

        // 对话框的状态：重建时留着
        struct ShortcutsState
        {
            std::string filter;
            std::string message;
            bool        error  = false;
            bool        closed = false;     // 对话框关了（选文件可能是异步的，选好时对话框已经不在）
        };
    }

    void MainFrame::RegisterShortcuts()
    {
        std::vector<std::pair<std::string, std::string>> defaults;
        for (const auto& [keys, command] : Desktop)
            if (m_commands.Find(command))
                defaults.emplace_back(keys, command);
        for (const Bim::Command& c : Bim::Commands())
            if (const char* ui = ToolOf(c.id); ui && m_commands.Find(ui))
                for (const char* alias : c.aliases)
                    defaults.emplace_back(alias, ui);       // 桌面的表里已有的键前面那个算
        m_shortcuts.SetDefaults(std::move(defaults));
    }

    bool MainFrame::OnShortcutText(std::string_view text)
    {
        // 开始页上没有视图
        if (m_startShown)
            return false;
        bool used = false;
        for (const char c : text)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)))
                continue;
            used = true;
            if (const std::string command = m_shortcuts.Type(c); !command.empty())
                m_platform->Post([this, command]
                {
                    if (!m_commands.Execute(command))
                        LOG_INFO("快捷键 %s：现在不能用", command.c_str());
                });
        }
        return used;
    }

    std::string MainFrame::ShortcutCommand(const std::string& description) const
    {
        const std::string want = Lower(Bare(description));
        if (want.empty())
            return {};
        for (const std::string& id : m_commands.GetIds())
        {
            const MiniGUI::Command* c = m_commands.Find(id);
            if (!c || id.starts_with("window."))
                continue;
            if (Lower(id) == want || Lower(Bare(c->label)) == want || Lower(Bare(Tr(c->label))) == want)
                return id;
        }
        // 模型命令的名称（Split、Delete……）
        for (const Bim::Command& c : Bim::Commands())
            if (const char* ui = ToolOf(c.id); ui && (Lower(c.label) == want || Lower(Tr(c.label)) == want))
                return ui;
        return {};
    }

    void MainFrame::OpenShortcuts()
    {
        auto state  = std::make_shared<ShortcutsState>();
        auto dialog = std::make_unique<Dialog>(Tr("Keyboard Shortcuts"));
        Dialog* dlg = dialog.get();

        // 能给快捷键的命令：除了窗口的三个按钮
        std::vector<std::string> ids;
        std::vector<std::string> names;
        for (const std::string& id : m_commands.GetIds())
            if (!id.starts_with("window."))
            {
                ids.push_back(id);
                names.push_back(Bare(m_commands.GetLabel(id)));
            }
        auto nameOf = [this](const std::string& id) { return Bare(m_commands.GetLabel(id)); };

        Node* body = dlg->GetBody();
        LayoutStyle& bs = body->EditLayoutStyle();     // 留着对话框给内容区的边距
        bs.width = 520.0f;
        bs.gap   = 8.0f;
        auto* note = body->AddChild<Label>(Tr("Type two letters over the view to run a command (WA wall, DR door, ZF zoom to fit)."), 12.0f, Theme::TextDim);
        note->SetWrap(true);

        auto* search = body->AddChild<TextBox>();
        search->SetPlaceholder(Tr("Search"));

        auto* scroll = body->AddChild<ScrollView>();
        scroll->EditLayoutStyle().height = 300.0f;
        Node* list = scroll->SetContent<Node>();

        // 新加或改一个：键、命令、指定
        auto* addRow = body->AddChild<Node>();
        LayoutStyle rs;
        rs.direction  = FlexDirection::Row;
        rs.alignItems = Align::Center;
        rs.gap        = 8.0f;
        addRow->SetLayoutStyle(rs);
        auto* keys = addRow->AddChild<TextBox>();
        keys->SetPlaceholder(Tr("Keys"));
        keys->EditLayoutStyle().width = 70.0f;
        auto* command = addRow->AddChild<ComboBox>(names, 0);
        command->EditLayoutStyle().grow = 1.0f;
        auto* assign = addRow->AddChild<Button>(Tr("Assign"), std::function<void()>());

        auto* message = body->AddChild<Label>("", 12.0f, Theme::TextDim);
        message->SetWrap(true);

        auto fill = std::make_shared<std::function<void()>>();
        *fill = [this, state, list, message, nameOf, fill]
        {
            list->RemoveAllChildren();
            LayoutStyle ls;
            ls.gap = 2.0f;
            list->SetLayoutStyle(ls);
            const std::string filter = Lower(state->filter);
            for (const Shortcuts::Entry& e : m_shortcuts.All())
            {
                const std::string name = nameOf(e.command);
                if (!filter.empty() && Lower(e.keys).find(filter) == std::string::npos && Lower(name).find(filter) == std::string::npos)
                    continue;
                auto* row = list->AddChild<Node>();
                LayoutStyle r;
                r.direction  = FlexDirection::Row;
                r.alignItems = Align::Center;
                r.gap        = 8.0f;
                r.padding    = Edges::Symmetric(4.0f, 0.0f);
                row->SetLayoutStyle(r);
                auto* k = row->AddChild<Label>(e.keys, 13.0f);
                k->SetBold(true);
                k->EditLayoutStyle().width = 40.0f;
                auto* n = row->AddChild<Label>(name, 13.0f);
                n->EditLayoutStyle().grow = 1.0f;
                n->SetEllipsis(true);
                if (e.custom)
                    row->AddChild<Label>(Tr("Custom"), 12.0f, Theme::TextDim);
                auto* remove = row->AddChild<Button>(Tr("Remove"), [this, state, keys = e.keys, fill]
                {
                    m_shortcuts.Remove(keys);
                    SaveSettings();
                    state->message.clear();
                    m_platform->Post([fill] { (*fill)(); });
                });
                remove->SetStyle(ButtonStyle::Flat());
            }
            message->SetText(state->message);
            message->SetColor(state->error ? Theme::Danger : Theme::TextDim);
        };
        (*fill)();

        search->SetOnChanged([state, fill](const std::string& text)
        {
            state->filter = text;
            (*fill)();
        });
        assign->SetOnClick([this, state, keys, command, ids, nameOf, fill]
        {
            const std::string k = Shortcuts::Normalize(keys->GetText());
            const int at = command->GetSelectedIndex();
            state->error = true;
            if (!Shortcuts::IsKey(k))
                state->message = Tr("A shortcut is two letters or digits.");
            else if (at < 0 || at >= static_cast<int>(ids.size()))
                state->message = Tr("Choose a command.");
            else if (const std::string other = m_shortcuts.Conflict(k, ids[static_cast<size_t>(at)]); !other.empty())
                state->message = Format(Tr("{0} is already used by {1}: remove it there first."), { k, nameOf(other) });
            else
            {
                m_shortcuts.Set(k, ids[static_cast<size_t>(at)]);
                SaveSettings();
                state->error   = false;
                state->message = Format(Tr("{0} runs {1}."), { k, nameOf(ids[static_cast<size_t>(at)]) });
                keys->SetText("");
            }
            m_platform->Post([fill] { (*fill)(); });
        });

        dlg->AddButton(Tr("Load from File..."), [this, state, fill]
        {
            m_platform->PickFile(FileKind::Shortcuts, [this, state, fill](const std::string& path)
            {
                if (path.empty() || state->closed)
                    return;
                std::error_code ec;
                const auto size = std::filesystem::file_size(ToPath(path), ec);
                state->error = true;
                if (ec)
                    state->message = Format(Tr("Could not read the file: {0}"), { ec.message() });
                else if (size > Shortcuts::MaxFileBytes)
                    state->message = Tr("That is too big to be a shortcuts file.");
                else
                {
                    std::ifstream in(ToPath(path), std::ios::binary);
                    std::ostringstream ss;
                    ss << in.rdbuf();
                    const auto [added, skipped] = m_shortcuts.Add(Shortcuts::ParseFile(ss.str()),
                                                                  [this](const std::string& d) { return ShortcutCommand(d); });
                    SaveSettings();
                    state->error   = false;
                    state->message = Format(TrN("Loaded {0} shortcut from the file.", "Loaded {0} shortcuts from the file.", static_cast<long long>(added)),
                                            { std::to_string(added) });
                    if (skipped > 0)
                        state->message += " " + Format(TrN("Skipped {0} (not two letters, a command MiniBIM doesn't have, or keys already in use).",
                                                           "Skipped {0} (not two letters, a command MiniBIM doesn't have, or keys already in use).",
                                                           static_cast<long long>(skipped)), { std::to_string(skipped) });
                }
                m_platform->Post([fill] { (*fill)(); });
            });
        });
        dlg->AddButton(Tr("Restore Defaults"), [this, state, fill]
        {
            m_shortcuts.Reset();
            SaveSettings();
            state->error   = false;
            state->message = Tr("Back to the default shortcuts.");
            m_platform->Post([fill] { (*fill)(); });
        });
        dlg->AddButton(Tr("Close"), [dlg] { dlg->Close(); }, DialogButtonRole::Cancel);
        dlg->SetInitialFocus(search);
        dlg->SetOnClosed([state] { state->closed = true; });
        m_ui->OpenPopup(std::move(dialog));
    }
}
