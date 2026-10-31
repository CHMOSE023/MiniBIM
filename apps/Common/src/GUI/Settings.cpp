// 文件 > 设置：窗口自己的设置，属于用户、不属于项目，存在用户设置目录的 settings.json（MainFrame::SaveSettings）。
// 现在有：界面的主题、语言、两块面板，自动保存的间隔。BIMCraft 设置里的性能、图形、更新等页随那些功能再加
// 移植自 BIMCraft crates/ui-egui/src/settings.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Core/UIContext.h"
#include "I18n/Translator.h"
#include "Style/Theme.hpp"
#include "Style/ThemeColors.h"
#include "Widgets/Button.h"
#include "Widgets/ComboBox.h"
#include "Widgets/Controls.h"
#include "Widgets/Dialog.h"
#include "Widgets/Label.h"
#include "Widgets/NumberBox.h"

namespace MiniBIM
{
    namespace
    {
        using namespace MiniGUI;

        // 设置窗口的页
        enum class Page { Interface, Files };
        constexpr std::pair<Page, const char*> Pages[] = { { Page::Interface, N_("User Interface") }, { Page::Files, N_("Files") } };

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

        // 页里一组的标题
        void Heading(Node* parent, const char* text)
        {
            auto* l = parent->AddChild<Label>(Tr(text), 14.0f, Theme::Text);
            l->SetBold(true);
            LayoutStyle s;
            s.margin = Edges::Make(0.0f, 6.0f, 0.0f, 0.0f);
            l->SetLayoutStyle(s);
        }
    }

    void MainFrame::ApplyTheme()
    {
        m_ui->SetTheme(m_themeSetting == "light" ? ThemeColors::Light() : ThemeColors::Dark());
    }

    void MainFrame::OpenSettings()
    {
        auto page   = std::make_shared<Page>(Page::Interface);
        auto dialog = std::make_unique<Dialog>(Tr("Settings"));
        Dialog* dlg = dialog.get();
        auto build  = std::make_shared<std::function<void()>>();
        auto rebuild = [this, build] { m_platform->Post([build] { if (*build) (*build)(); }); };
        *build = [this, dlg, page, rebuild]
        {
            dlg->SetTitle(Tr("Settings"));     // 换了语言时跟着换
            Node* body = dlg->GetBody();
            body->RemoveAllChildren();
            LayoutStyle& bs = body->EditLayoutStyle();
            bs.direction  = FlexDirection::Row;
            bs.alignItems = Align::Start;
            bs.gap        = 16.0f;
            bs.width      = 600.0f;
            bs.height     = 300.0f;

            // 左边：各页
            auto* list = body->AddChild<Node>();
            LayoutStyle ls;
            ls.width = 150.0f;
            ls.gap   = 2.0f;
            list->SetLayoutStyle(ls);
            for (const auto& [p, name] : Pages)
            {
                auto* b = list->AddChild<Button>(Tr(name), [page, p, rebuild] { *page = p; rebuild(); });
                b->SetStyle(ButtonStyle::Flat());
                b->SetChecked(*page == p);
                b->EditLayoutStyle().justify = Justify::Start;
            }

            auto* content = body->AddChild<Node>();
            LayoutStyle cs;
            cs.grow = 1.0f;
            cs.gap  = 8.0f;
            content->SetLayoutStyle(cs);
            if (*page == Page::Interface)
            {
                Heading(content, N_("Theme"));
                const std::vector<std::string> themes = { Tr("Dark"), Tr("Light") };
                auto* theme = content->AddChild<ComboBox>(themes, m_themeSetting == "light" ? 1 : 0);
                theme->EditLayoutStyle().width = 220.0f;
                theme->SetOnChanged([this](int i)
                {
                    m_themeSetting = i == 1 ? "light" : "dark";
                    ApplyTheme();
                    SaveSettings();
                });
                content->AddChild<Label>(Tr("The views keep their own colours."), 12.0f, Theme::TextDim);

                Heading(content, N_("Language"));
                // 各语言的名称用它自己的语言写（同语言菜单）
                std::vector<std::string> names = { Tr("System Default") };
                std::vector<std::string> codes = { std::string() };
                int at = 0;
                for (const I18n::LanguageInfo& lang : I18n::Translator::Instance().GetLanguages())
                {
                    if (!m_languageSetting.empty() && I18n::NormalizeLanguage(m_languageSetting) == lang.code)
                        at = static_cast<int>(names.size());
                    names.push_back(lang.name);
                    codes.push_back(lang.code);
                }
                auto* language = content->AddChild<ComboBox>(names, at);
                language->EditLayoutStyle().width = 220.0f;
                language->SetOnChanged([this, codes, rebuild](int i)
                {
                    if (i < 0)
                        return;
                    const std::string code = codes[static_cast<size_t>(i)];
                    // 换语言会重建整个界面：等下拉列表关掉之后
                    m_platform->Post([this, code, rebuild] { SetLanguage(code, true); rebuild(); });
                });

                Heading(content, N_("Palettes"));
                for (const auto& [panel, name] : { std::pair{ "properties", N_("Properties") }, std::pair{ "browser", N_("Project Browser") } })
                {
                    auto* box = content->AddChild<CheckBox>(Tr(name), PanelShown(panel));
                    box->SetOnChanged([this, panel](bool on) { if (on != PanelShown(panel)) TogglePanel(panel); });
                }
            }
            else
            {
                Heading(content, N_("Autosave"));
                auto* on = content->AddChild<CheckBox>(Tr("Autosave in the background"), m_autosave.minutes > 0);
                on->SetOnChanged([this, rebuild](bool enabled)
                {
                    m_autosave.minutes = enabled ? 2 : 0;
                    SaveSettings();
                    ScheduleAutosave();
                    rebuild();
                });
                if (m_autosave.minutes > 0)
                {
                    Node* row = Row(content);
                    row->AddChild<Label>(Tr("Save a recovery copy every"), 13.0f);
                    auto* minutes = row->AddChild<NumberBox>(m_autosave.minutes, 1.0, 120.0);
                    minutes->SetDecimals(0);
                    minutes->EditLayoutStyle().width = 70.0f;
                    minutes->SetOnChanged([this](double v)
                    {
                        m_autosave.minutes = std::clamp(static_cast<int>(v), 1, 120);
                        SaveSettings();
                        ScheduleAutosave();
                    });
                    row->AddChild<Label>(Tr("min"), 13.0f);
                }
                auto* note = content->AddChild<Label>(
                    Tr("Recovery copies are kept beside saved projects; untitled projects use the temporary folder."), 12.0f, Theme::TextDim);
                note->SetWrap(true);
            }
        };
        (*build)();
        dlg->AddButton(Tr("Close"), [dlg] { dlg->Close(); }, DialogButtonRole::Cancel);
        m_ui->OpenPopup(std::move(dialog));
    }
}
