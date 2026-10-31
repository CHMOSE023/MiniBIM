// Revit 文件：在后台线程上读，提示框盖住窗口；读进来的是新的、没保存的项目，导入报告说进来了什么、略去了什么。
// 读取器（src/Rvt，Apache-2.0）主窗口不依赖：宿主接上（MainFrame::SetRevitReader），没接时说这个版本读不了。
// 移植自 BIMCraft crates/ui-egui/src/revit.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Core/Log.h"
#include "Core/Popup.h"
#include "Core/UIContext.h"
#include "I18n/Translator.h"
#include "Style/Theme.hpp"
#include "Widgets/Button.h"
#include "Widgets/Dialog.h"
#include "Widgets/Label.h"
#include "Widgets/ScrollView.h"
#include <algorithm>
#include <exception>
#include <filesystem>
#include <mutex>
#include <thread>

namespace MiniBIM
{
    // 工作线程读完后放在这里；wake 叫界面线程来取（MainFrame 销毁时清掉，之后读完的没人要）
    struct MainFrame::RevitRead
    {
        std::mutex mutex;
        bool done = false;
        bool ok   = false;
        Bim::Ifc::Imported imported;
        std::string error;
        std::function<void()> wake;
    };

    namespace
    {
        std::string FileName(const std::string& path)
        {
            const std::u8string u8 = std::filesystem::path(std::u8string(path.begin(), path.end())).filename().u8string();
            return u8.empty() ? path : std::string(u8.begin(), u8.end());
        }

        // 读取器是读不可信输入的代码：它抛出什么，窗口都得到一个回答
        void Read(const MainFrame::RevitReader& reader, const std::string& path, MainFrame::RevitRead& read)
        {
            Bim::Ifc::Imported imported;
            std::string error;
            bool ok = false;
            try
            {
                ok = reader(path, RevitReport::ProjectName(path), imported, error);
            }
            catch (const std::exception& e)
            {
                ok = false;
                error = std::string("the Revit reader stopped on this file (") + e.what() + ")";
            }
            catch (...)
            {
                ok = false;
                error = "the Revit reader stopped on this file";
            }
            std::lock_guard lock(read.mutex);
            read.ok       = ok;
            read.imported = std::move(imported);
            read.error    = ok ? std::string() : (error.empty() ? std::string("the Revit reader stopped without an answer") : std::move(error));
            read.done     = true;
            if (read.wake)
                read.wake();
        }
    }

    bool MainFrame::OpenRevit(const std::string& path)
    {
        const std::string file = FileName(path);
        if (!m_revitReader)
        {
            MiniGUI::ShowMessageBox(*m_ui, Tr("Could not open the project"),
                Format(Tr("{0}: this build of MiniBIM was made without Revit support (MINIBIM_WITH_RVT=OFF); build it with that option on to read Revit files"),
                       { file }));
            return false;
        }
        // 一次读一个
        if (m_revitJob)
            return false;

        RevitJob job;
        job.path = path;
        job.file = file;
        job.read = std::make_shared<RevitRead>();
        job.here = !m_platform->HasBackgroundThread();
        if (!job.here)
        {
            job.read->wake = [this] { m_platform->Post([this] { FinishRevit(); }); };
            try
            {
                std::thread([reader = m_revitReader, path, read = job.read] { Read(reader, path, *read); }).detach();
            }
            catch (const std::exception& e)
            {
                MiniGUI::ShowMessageBox(*m_ui, Tr("Could not open the project"), Format(Tr("Couldn't open {0}: {1}"), { file, e.what() }));
                return false;
            }
        }

        // 提示框盖住窗口：读进来的项目要换掉这一个，下面的什么都不能动（模态的弹层也挡住快捷键）；Esc 不关它
        using namespace MiniGUI;
        auto box = std::make_unique<Popup>();
        box->SetModal(true);
        box->SetCloseOnEscape(false);
        box->SetCentered();
        LayoutStyle s;
        s.padding = Edges::All(20.0f);
        s.gap     = 6.0f;
        box->SetLayoutStyle(s);
        box->AddChild<Label>(Format(Tr("Opening a Revit file: {0}"), { file }), 14.0f)->SetBold(true);
        box->AddChild<Label>(Tr("A big file can take a few minutes."), 12.0f, Theme::TextDim);
        job.box = box.get();
        m_ui->OpenPopup(std::move(box));
        m_revitJob = std::move(job);
        LOG_INFO("读 Revit 文件：%s", path.c_str());
        m_platform->RequestRedraw();
        return true;
    }

    void MainFrame::ReadRevit()
    {
        if (!m_revitJob || !m_revitJob->here)
            return;
        Read(m_revitReader, m_revitJob->path, *m_revitJob->read);
        FinishRevit();
    }

    void MainFrame::FinishRevit()
    {
        if (!m_revitJob)
            return;
        const std::shared_ptr<RevitRead> read = m_revitJob->read;
        {
            std::lock_guard lock(read->mutex);
            if (!read->done)
                return;
        }
        const RevitJob job = std::move(*m_revitJob);
        m_revitJob.reset();
        if (job.box)
            job.box->Close();
        if (!read->ok)
        {
            LOG_WARN("读不了 Revit 文件 %s：%s", job.path.c_str(), read->error.c_str());
            MiniGUI::ShowMessageBox(*m_ui, Tr("Could not open the project"), Format(Tr("Couldn't open {0}: {1}"), { job.file, read->error }));
        }
        else
            InstallRevit(job.file, std::move(read->imported));
        m_platform->RequestRedraw();
    }

    void MainFrame::AbandonRevit()
    {
        if (!m_revitJob)
            return;
        // 退出不等它读完：读完的结果没人要
        std::lock_guard lock(m_revitJob->read->mutex);
        m_revitJob->read->wake = nullptr;
    }

    void MainFrame::InstallRevit(const std::string& file, Bim::Ifc::Imported imported)
    {
        RevitReport::Report report = RevitReport::Of(file, imported);
        // 新的、没保存的项目（保存时问存成哪个 .minibim，不写回 .rvt），不进最近打开；有它自己的恢复副本
        m_session.Reset(std::move(imported.model), std::nullopt);
        OnModelChanged();
        m_revitResets = m_session.Resets();
        m_revitModel  = m_session.Snapshot();
        const bool said = !report.notes.empty();
        m_revitReport = std::move(report);
        UpdateStatus();
        m_commands.NotifyStateChanged();
        // 导入有话说时报告自己弹出来；干干净净的导入不弹，报告在"文件"菜单里
        if (said)
            ShowRevitReport();
    }

    bool MainFrame::HasRevitReport() const
    {
        return m_revitReport && m_revitResets == m_session.Resets();
    }

    void MainFrame::ShowRevitReport()
    {
        if (!HasRevitReport())
            return;
        using namespace MiniGUI;
        const RevitReport::Report& r = *m_revitReport;
        auto dialog = std::make_unique<Dialog>(Tr("Revit Import Report"));
        Dialog* dlg = dialog.get();
        Node* body = dlg->GetBody();
        LayoutStyle& bs = body->EditLayoutStyle();
        bs.width = 560.0f;
        bs.gap   = 6.0f;
        body->AddChild<Label>(r.file + " · " + r.schema, 12.0f, Theme::TextDim);

        // 窗口给得下时一长串也不用滚动：至多窗口高的三分之二
        auto* scroll = body->AddChild<ScrollView>();
        scroll->EditLayoutStyle().maxHeight = std::max(200.0f, m_ui->GetDisplaySize().y * 0.65f);
        Node* list = scroll->SetContent<Node>();
        LayoutStyle ls;
        ls.gap = 3.0f;
        list->SetLayoutStyle(ls);
        auto heading = [list](const std::string& text, bool space)
        {
            auto* l = list->AddChild<Label>(text, 13.0f);
            l->SetBold(true);
            if (space)
                l->EditLayoutStyle().margin.top = 8.0f;
        };
        // 一条说明，元素 id 在它下面，用淡色
        auto note = [list](std::string_view text)
        {
            const auto [what, ids] = RevitReport::SplitIds(text);
            auto* l = list->AddChild<Label>("• " + std::string(what), 13.0f);
            l->SetWrap(true);
            if (ids)
            {
                auto* i = list->AddChild<Label>(std::string(*ids), 11.0f, Theme::TextDim);
                i->SetWrap(true);
                i->EditLayoutStyle().margin.left = 12.0f;
            }
        };
        heading(Format(Tr("Came in: {0}"), { std::to_string(r.CameIn()) }), false);
        for (const auto& [category, n] : r.counts)
            list->AddChild<Label>(RevitReport::CountLine(category, n), 13.0f);
        heading(Format(Tr("Left out: {0}"), { std::to_string(r.LeftOutCount()) }), true);
        const std::vector<std::string_view> left = r.LeftOut(), noted = r.Noted();
        if (left.empty())
            list->AddChild<Label>(Tr("Nothing was left out."), 13.0f);
        for (const std::string_view n : left)
            note(n);
        if (!noted.empty())
        {
            heading(Tr("Noted"), true);
            for (const std::string_view n : noted)
                note(n);
        }

        // 复制成纯文本（贴进问题报告或文件）：窗口里的字只能看不能选
        Button* copy = dlg->AddButton(Tr("Copy Report"), {});
        copy->SetOnClick([this, copy, text = RevitReport::Text(r)]
        {
            m_ui->GetClipboard().SetText(text);
            copy->SetText(Tr("Copied"));
        });
        dlg->AddButton(Tr("Close"), [dlg] { dlg->Close(); }, DialogButtonRole::Cancel);
        m_ui->OpenPopup(std::move(dialog));
    }
}
