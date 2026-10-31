// 开始页：启动时没给项目就打开在这里——新建、打开、示例房屋，最近打开的文件排成卡片，底下是源代码的链接与版本。
// 快速访问工具栏的"开始页"（Ctrl+D）把它叫到项目上面，项目原样留着，再按一次回到项目。
// 移植自 BIMCraft crates/ui-egui/src/start.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Core/UIContext.h"
#include "I18n/Translator.h"
#include "MiniBIMVersion.h"
#include "Style/Theme.hpp"
#include "Widgets/Button.h"
#include "Widgets/ImageView.h"
#include "Widgets/Label.h"
#include "Widgets/Menu.h"
#include "Widgets/Panel.h"
#include "Widgets/ScrollView.h"
#include "Widgets/TitleBar.h"
#include <algorithm>
#include <cmath>

namespace MiniBIM
{
    namespace
    {
        // 源代码在哪里
        constexpr const char* kSource = "https://github.com/CHMOSE023/MiniBIM";
        constexpr float kTile    = 232.0f;  // 卡片的宽
        constexpr float kGap     = 18.0f;
        constexpr float kActions = 260.0f;  // 左边"开始"一栏的宽
        constexpr float kSide    = 36.0f;   // 两栏之间

        std::filesystem::path ToPath(const std::string& utf8)
        {
            return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }

        std::string ToUtf8(const std::filesystem::path& p)
        {
            const std::u8string u8 = p.u8string();
            return std::string(u8.begin(), u8.end());
        }

        MiniGUI::LayoutStyle Row(float gap = 0.0f)
        {
            MiniGUI::LayoutStyle s;
            s.direction  = MiniGUI::FlexDirection::Row;
            s.alignItems = MiniGUI::Align::Center;
            s.gap        = gap;
            return s;
        }

        MiniGUI::Label* Heading(MiniGUI::Node* parent, std::string text)
        {
            auto* l = parent->AddChild<MiniGUI::Label>(std::move(text), 16.0f, MiniGUI::Theme::Text);
            l->SetBold(true);
            MiniGUI::LayoutStyle s;
            s.margin = MiniGUI::Edges::Make(0.0f, 0.0f, 0.0f, 10.0f);
            l->SetLayoutStyle(s);
            return l;
        }

        // 按钮里的子节点不接指针：点到哪里都是按钮
        template <typename T>
        T* Passive(T* n)
        {
            n->SetHitTestVisible(false);
            return n;
        }
    }

    void MainFrame::CreateStartPage()
    {
        using namespace MiniGUI;
        // 盖在标题栏下面的整个窗口上：菜单与快速访问工具栏照常能用
        auto* page = m_appHost->AddChild<Panel>(Theme::Background);
        LayoutStyle s;
        s.position = PositionType::Absolute;
        s.left     = 0.0f;
        s.right    = 0.0f;
        s.top      = TitleBar::kHeight;
        s.bottom   = 0.0f;
        page->SetLayoutStyle(s);
        page->SetVisible(false);
        m_startPage = page;
    }

    void MainFrame::ShowStartPage(bool show, bool over)
    {
        if (!m_startPage)
            return;
        m_startShown = show;
        m_startOver  = show && over;
        if (show)
            BuildStartPage();
        m_startPage->SetVisible(show);
        m_commands.NotifyStateChanged();
        m_platform->RequestRedraw();
    }

    void MainFrame::ToggleStartPage()
    {
        if (!m_startShown)
            ShowStartPage(true, true);
        else if (m_startOver)
            ShowStartPage(false, false);
    }

    void MainFrame::PostStartPage()
    {
        // 在卡片自己的回调里不能拆掉它：下一轮消息循环里重建
        m_platform->Post([this] { if (m_startShown) BuildStartPage(); });
    }

    int MainFrame::StartColumns() const
    {
        // 右边一栏放得下几张卡片
        const float width = std::clamp(m_ui->GetDisplaySize().x - 64.0f, 320.0f, 1180.0f);
        const float recent = width >= 760.0f ? width - kActions - kSide : width;
        return std::max(1, static_cast<int>(std::floor((recent + kGap) / (kTile + kGap))));
    }

    void MainFrame::BuildStartPage()
    {
        using namespace MiniGUI;
        m_startPage->RemoveAllChildren();
        m_startColumns = StartColumns();
        const float display = m_ui->GetDisplaySize().x;
        const float width   = std::clamp(display - 64.0f, 320.0f, 1180.0f);
        const bool  wide    = width >= 760.0f;
        const std::string icons = m_platform->GetResourceDir() + "/icons/";

        auto* scroll = m_startPage->AddChild<ScrollView>();
        LayoutStyle grow;
        grow.grow = 1.0f;
        scroll->SetLayoutStyle(grow);
        auto* body = scroll->SetContent<Node>();
        LayoutStyle bs;
        bs.alignItems = Align::Center;
        bs.padding    = Edges::Make(16.0f, 36.0f, 16.0f, 36.0f);
        body->SetLayoutStyle(bs);
        auto* column = body->AddChild<Node>();
        LayoutStyle cs;
        cs.width = width;
        cs.gap   = 30.0f;
        column->SetLayoutStyle(cs);

        // 程序图标、名称、是什么
        auto* hero = column->AddChild<Node>();
        hero->SetLayoutStyle(Row(14.0f));
        auto* logo = hero->AddChild<ImageView>();
        logo->SetImagePath(icons + "app.png", { 64.0f, 64.0f });
        auto* words = hero->AddChild<Node>();
        LayoutStyle ws;
        ws.gap = 4.0f;
        words->SetLayoutStyle(ws);
        auto* name = words->AddChild<Label>("MiniBIM", 34.0f, Theme::Text);
        name->SetBold(true);
        words->AddChild<Label>(Tr("Building information modelling, open and free."), 15.0f, Theme::TextDim);

        auto* parts = column->AddChild<Node>();
        LayoutStyle ps;
        ps.direction  = wide ? FlexDirection::Row : FlexDirection::Column;
        ps.alignItems = Align::Start;
        ps.gap        = wide ? kSide : 28.0f;
        parts->SetLayoutStyle(ps);

        // ── 开始：新建、打开、示例房屋，叫到项目上面时还有回到项目 ──
        auto* actions = parts->AddChild<Node>();
        LayoutStyle as;
        as.width = wide ? kActions : width;
        as.gap   = 10.0f;
        actions->SetLayoutStyle(as);
        Heading(actions, TrC("start page", "Start"));
        auto action = [&](const char* icon, const std::string& title, const char* what, const char* keys, std::function<void()> act)
        {
            auto* b = actions->AddChild<Button>([this, act = std::move(act)] { m_platform->Post(act); });
            LayoutStyle s = Row(12.0f);
            s.justify = Justify::Start;
            s.padding = Edges::Make(12.0f, 10.0f, 12.0f, 10.0f);
            b->SetLayoutStyle(s);
            Passive(b->AddChild<ImageView>())->SetImagePath(icons + icon, { 28.0f, 28.0f });
            // 名称与快捷键一行，说明在下面换行：宽度定死（按钮宽减去内边距、图标与间隔），量高度时才按这个宽换行
            auto* text = Passive(b->AddChild<Node>());
            LayoutStyle ts;
            ts.width = (wide ? kActions : width) - 24.0f - 28.0f - 12.0f;
            ts.gap   = 2.0f;
            text->SetLayoutStyle(ts);
            auto* head = Passive(text->AddChild<Node>());
            head->SetLayoutStyle(Row(8.0f));
            auto* name = Passive(head->AddChild<Label>(title, 14.0f, Theme::Text));
            name->SetBold(true);
            name->SetEllipsis(true);
            LayoutStyle ns;
            ns.grow = 1.0f;
            name->SetLayoutStyle(ns);
            if (keys && *keys)
                Passive(head->AddChild<Label>(keys, 12.0f, Theme::TextDim));
            auto* note = Passive(text->AddChild<Label>(Tr(what), 12.0f, Theme::TextDim));
            note->SetWrap(true);
        };
        action("New.png", Tr("New Project"), N_("An empty project to model in."), "Ctrl+N", [this] { Guard([this] { NewProject(); }); });
        action("Open.png", Tr("Open..."), m_revitReader ? N_("A MiniBIM or BIMCraft project, an IFC or a Revit file.") : N_("A MiniBIM or BIMCraft project, or an IFC file."),
               "Ctrl+O", [this] { Guard([this] { OpenProject(); }); });
        action("SampleHouse.png", Tr("Sample House"), N_("A small house to look around and try the tools on."), "",
               [this] { Guard([this] { OpenSample(); }); });
        if (m_startOver)
        {
            std::string project = m_session.GetModel().name;
            if (const auto& path = m_session.Path())
                project = ToUtf8(ToPath(*path).stem());
            action("Back.png", Format(Tr("Back to {0}"), { project }), N_("The project stays as you left it."), "Ctrl+D",
                   [this] { ShowStartPage(false, false); });
        }

        // ── 最近打开：卡片 ──
        auto* recent = parts->AddChild<Node>();
        LayoutStyle rs;
        rs.width = wide ? width - kActions - kSide : width;
        rs.gap   = kGap;
        recent->SetLayoutStyle(rs);
        Heading(recent, Tr("Recent"));
        const std::vector<std::string> labels = m_recent.Labels();
        if (labels.empty())
        {
            auto* empty = recent->AddChild<Panel>(Colors::Transparent);
            empty->SetBorder(Theme::Border);
            empty->SetRounding(Theme::RadiusMd);
            LayoutStyle es;
            es.height     = 150.0f;
            es.justify    = Justify::Center;
            es.alignItems = Align::Center;
            es.gap        = 6.0f;
            empty->SetLayoutStyle(es);
            empty->AddChild<Label>(Tr("No recent projects yet"), 15.0f, Theme::Text);
            empty->AddChild<Label>(Tr("The projects you open or save show here."), 13.0f, Theme::TextDim);
        }
        Node* row = nullptr;
        for (size_t i = 0; i < labels.size(); ++i)
        {
            if (i % static_cast<size_t>(m_startColumns) == 0)
            {
                row = recent->AddChild<Node>();
                LayoutStyle s = Row(kGap);
                s.alignItems = Align::Start;
                row->SetLayoutStyle(s);
            }
            const std::string path = m_recent.Paths()[i];
            const std::filesystem::path p = ToPath(path);
            std::error_code ec;
            const bool missing = !std::filesystem::exists(p, ec);
            auto open = [this, path] { Guard([this, path] { OpenPath(path); }); };

            auto* tile = row->AddChild<Button>([this, open] { m_platform->Post(open); });
            ButtonStyle style;
            style.normal   = Theme::Panel;
            style.hover    = Theme::Panel;
            style.pressed  = Theme::Control;
            style.border   = Theme::Border;
            style.rounding = Theme::RadiusLg;
            tile->SetStyle(style);
            LayoutStyle s;
            s.width      = kTile;
            s.direction  = FlexDirection::Column;
            s.alignItems = Align::Stretch;
            s.padding    = Edges::Make(0.0f, 0.0f, 0.0f, 12.0f);
            s.gap        = 4.0f;
            tile->SetLayoutStyle(s);
            tile->SetTooltip(path);

            // 图：还没有项目的缩略图（以后再画），先是文件类型
            auto* pic = Passive(tile->AddChild<Panel>(Theme::PanelAlt));
            pic->SetRounding(Theme::RadiusLg);
            LayoutStyle pics;
            pics.height     = 96.0f;
            pics.justify    = Justify::Center;
            pics.alignItems = Align::Center;
            pics.gap        = 6.0f;
            pics.margin     = Edges::Make(1.0f, 1.0f, 1.0f, 6.0f);
            pic->SetLayoutStyle(pics);
            std::string ext = ToUtf8(p.extension());
            if (!ext.empty() && ext.front() == '.')
                ext.erase(0, 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            auto* glyph = Passive(pic->AddChild<ImageView>());
            glyph->SetImagePath(icons + (ext == "IFC" ? "ExportIfc.png" : "FloorPlan.png"), { 36.0f, 36.0f });
            glyph->SetTint(ColorRef(ThemeColor::Text, missing ? 90 : 200));
            Passive(pic->AddChild<Label>(missing ? Tr("Not found") : ext, 12.0f, Theme::TextDim));

            const std::string stem = ToUtf8(p.stem());
            auto* title = Passive(tile->AddChild<Label>(stem.empty() ? labels[i] : stem, 14.5f, missing ? Theme::TextDim : Theme::Text));
            title->SetEllipsis(true);
            title->SetLayoutStyle([] { LayoutStyle t; t.margin = Edges::Make(14.0f, 0.0f, 12.0f, 0.0f); return t; }());
            // 所在的文件夹 · 多久以前改的
            std::string detail = ToUtf8(p.parent_path().filename());
            if (!missing)
            {
                const auto when = std::filesystem::last_write_time(p, ec);
                if (!ec)
                {
                    const auto age = std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - when).count();
                    const std::string ago = RecentFiles::Age(std::max<long long>(age, 0));
                    detail = detail.empty() ? ago : detail + " · " + ago;
                }
            }
            auto* sub = Passive(tile->AddChild<Label>(detail, 12.0f, Theme::TextDim));
            sub->SetEllipsis(true);
            sub->SetLayoutStyle([] { LayoutStyle t; t.margin = Edges::Make(14.0f, 0.0f, 12.0f, 0.0f); return t; }());

            tile->SetContextMenuHandler([this, open, path](Vec2 at)
            {
                std::vector<MenuItem> items;
                items.emplace_back(Tr("Open"), [this, open] { m_platform->Post(open); });
                items.emplace_back(Tr("Remove from List"), [this, path]
                {
                    m_recent.Remove(path);
                    KeepRecent();
                });
                ShowContextMenu(*m_ui, std::move(items), at);
                return true;
            });
        }

        // ── 底栏：源代码、版本 ──
        auto* foot = m_startPage->AddChild<Panel>(Theme::PanelAlt);
        LayoutStyle fs = Row(10.0f);
        fs.height  = 34.0f;
        fs.padding = Edges::Make(20.0f, 0.0f, 20.0f, 0.0f);
        foot->SetLayoutStyle(fs);
        auto* link = foot->AddChild<Button>(Tr("Source code on GitHub"), [this] { m_platform->OpenUrl(kSource); });
        link->SetStyle(ButtonStyle::Flat());
        link->SetTooltip(kSource);
        if (Label* l = link->GetLabel())
            l->SetColor(Theme::Accent);
        foot->AddChild<Label>("·", 13.0f, Theme::TextDim);
        foot->AddChild<Label>(Format(Tr("Version {0}"), { MINIBIM_VERSION_STRING }), 13.0f, Theme::TextDim);
    }
}
