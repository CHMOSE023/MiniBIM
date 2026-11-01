// 自动保存与恢复：桌面上的策略，写文件交给核心的 Bim::Autosave::Worker（后台线程）。
// 移植自 BIMCraft crates/ui-egui/src/autosave.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/MainFrame.h"
#include "Core/Log.h"
#include "Core/UIContext.h"
#include "I18n/Translator.h"
#include "Widgets/Dialog.h"
#include <algorithm>
#include <filesystem>

namespace MiniBIM
{
    namespace
    {
        std::filesystem::path ToPath(const std::string& utf8)
        {
            return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }

        std::string ToUtf8(const std::filesystem::path& p)
        {
            const std::u8string u8 = p.u8string();
            return std::string(u8.begin(), u8.end());
        }
    }

    void MainFrame::EnableAutosave()
    {
        // 写恢复副本的是工作线程：没有线程的平台（网页版）不自动保存
        if (!m_platform->HasBackgroundThread())
            return;
        std::error_code ec;
        const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
        if (ec)
        {
            LOG_WARN("自动保存：找不到临时文件夹，%s", ec.message().c_str());
            return;
        }
        AutosaveState& a = m_autosave;
        a.root   = ToUtf8(temp / "MiniBIM");
        a.worker = std::make_unique<Bim::Autosave::Worker>(a.root, [this] { m_platform->Post([this] { PollAutosave(); }); });
        AutosaveDocument();
        // 没保存过的项目留下的副本（上次没正常退出）
        ++a.discovering;
        a.worker->Discover(std::nullopt, a.generation);
        ScheduleAutosave();
    }

    void MainFrame::AutosaveDocument()
    {
        AutosaveState& a = m_autosave;
        if (!a.worker)
            return;
        RetireAutosave();
        ++a.generation;
        a.discovering = 0;
        a.snapshot.reset();
        a.project = m_session.Path();
        // 保存过的项目的副本再打开它时会再找到；没保存过的留着问
        a.candidates.erase(std::remove_if(a.candidates.begin(), a.candidates.end(),
                                          [](const Bim::Autosave::Backup& b) { return b.project.has_value(); }),
                           a.candidates.end());
        a.temporary = Bim::Autosave::BackupPath(ToUtf8(ToPath(a.root) / ("untitled-" + Bim::Guid::New().ToString())));
        SelectAutosaveDestination();
        if (a.project)
        {
            ++a.discovering;
            a.worker->Discover(a.project, a.generation);
        }
    }

    void MainFrame::SelectAutosaveDestination()
    {
        AutosaveState& a = m_autosave;
        auto isCandidate = [&](const std::string& p)
        { return std::any_of(a.candidates.begin(), a.candidates.end(), [&](const Bim::Autosave::Backup& b) { return b.path == p; }); };
        // 项目旁边那份还在问要不要恢复时不覆盖它，先写到临时文件夹
        std::string next = a.temporary;
        if (a.project && !isCandidate(Bim::Autosave::BackupPath(*a.project)))
            next = Bim::Autosave::BackupPath(*a.project);
        if (a.active && *a.active != next && !isCandidate(*a.active) &&
            std::find(a.retired.begin(), a.retired.end(), *a.active) == a.retired.end())
            a.retired.push_back(*a.active);
        a.active = next;
    }

    void MainFrame::RetireAutosave()
    {
        AutosaveState& a = m_autosave;
        if (!a.worker)
            return;
        auto isCandidate = [&](const std::string& p)
        { return std::any_of(a.candidates.begin(), a.candidates.end(), [&](const Bim::Autosave::Backup& b) { return b.path == p; }); };
        for (const std::string& p : a.retired)
            if (!isCandidate(p))
                a.worker->Remove(p);
        a.retired.clear();
        if (a.active && !isCandidate(*a.active))
            a.worker->Remove(*a.active);
        a.active.reset();
    }

    void MainFrame::AutosaveSaved()
    {
        AutosaveState& a = m_autosave;
        if (!a.worker)
            return;
        RetireAutosave();
        if (a.project != m_session.Path())
        {
            // 另存到了一个有自己副本的项目：先找出来，没看过的副本绝不删
            a.project = m_session.Path();
            if (a.project)
            {
                ++a.discovering;
                a.worker->Discover(a.project, a.generation);
            }
        }
        SelectAutosaveDestination();
        a.snapshot.reset();
        ScheduleAutosave();
    }

    void MainFrame::ScheduleAutosave()
    {
        AutosaveState& a = m_autosave;
        const uint64_t token = ++a.timer;
        if (!a.worker || a.minutes <= 0)
            return;
        m_platform->PostDelayed(static_cast<unsigned>(a.minutes) * 60000u, [this, token]
        {
            if (token == m_autosave.timer)
                AutosaveTick();
        });
    }

    void MainFrame::AutosaveTick()
    {
        AutosaveState& a = m_autosave;
        // 只写已经做成的模型：画了一半的草图由它的工具留着
        if (a.worker && Dirty() && a.discovering == 0 && !a.worker->Busy() && a.active)
        {
            const std::shared_ptr<const Bim::Model> now = m_session.Snapshot();
            if (a.snapshot.lock() != now && a.worker->Save(*a.active, now))
                a.snapshot = now;
        }
        ScheduleAutosave();
    }

    void MainFrame::PollAutosave()
    {
        AutosaveState& a = m_autosave;
        if (!a.worker)
            return;
        bool ask = false;
        while (std::optional<Bim::Autosave::Notice> note = a.worker->Poll())
        {
            if (auto* saved = std::get_if<Bim::Autosave::Saved>(&*note))
            {
                if (a.active && *a.active == saved->path && a.snapshot.lock() == saved->snapshot)
                {
                    if (saved->ok)
                    {
                        // 新的一份写好了：换地方之前的那份可以删了
                        for (const std::string& p : a.retired)
                            a.worker->Remove(p);
                        a.retired.clear();
                    }
                    else
                    {
                        a.snapshot.reset();
                        LOG_WARN("自动保存：%s", saved->error.c_str());
                    }
                }
            }
            else if (auto* removed = std::get_if<Bim::Autosave::Removed>(&*note))
            {
                if (a.deleting && *a.deleting == removed->path)
                {
                    a.deleting.reset();
                    if (removed->ok)
                    {
                        std::erase_if(a.candidates, [&](const Bim::Autosave::Backup& b) { return b.path == removed->path; });
                        SelectAutosaveDestination();
                        a.snapshot.reset();
                        ask = true;
                    }
                    else
                        MiniGUI::ShowMessageBox(*m_ui, Tr("Could not discard the recovery copy"), removed->error);
                }
                else if (!removed->ok)
                    LOG_WARN("自动保存：%s", removed->error.c_str());
            }
            else if (auto* found = std::get_if<Bim::Autosave::Discovered>(&*note))
            {
                if (found->generation != a.generation)
                    continue;
                a.discovering = std::max(0, a.discovering - 1);
                if (!found->ok)
                {
                    LOG_WARN("自动保存：%s", found->error.c_str());
                    continue;
                }
                for (const Bim::Autosave::Backup& b : found->found)
                    if (std::find(a.candidates.begin(), a.candidates.end(), b) == a.candidates.end() && b.path != a.active)
                    {
                        a.candidates.push_back(b);
                        ask = true;
                    }
                SelectAutosaveDestination();
            }
        }
        if (ask)
            ShowRecovery();
    }

    void MainFrame::ShowRecovery()
    {
        AutosaveState& a = m_autosave;
        if (a.asking || a.deleting)
            return;
        const auto it = std::find_if(a.candidates.begin(), a.candidates.end(),
                                     [&](const Bim::Autosave::Backup& b) { return !a.postponed.contains(b.path); });
        if (it == a.candidates.end())
            return;
        const Bim::Autosave::Backup backup = *it;
        const std::string name = backup.project ? ToUtf8(ToPath(*backup.project).filename()) : Tr("Untitled project");
        const std::string message = name + "\n\n" +
            Tr("Recover opens the backup as unsaved work. Discard deletes it. Postpone keeps it for later without replacing it.") +
            "\n\n" + backup.path;
        a.asking = true;
        MiniGUI::ShowMessageBox(*m_ui, Tr("Recovery copy found"), message, { Tr("Recover"), Tr("Discard"), Tr("Postpone") },
            [this, backup](int button)
            {
                m_autosave.asking = false;
                if (button == 0)
                    m_platform->Post([this, backup] { Guard([this, backup] { RecoverBackup(backup); }); });
                else if (button == 1 && m_autosave.worker)
                {
                    m_autosave.deleting = backup.path;
                    m_autosave.worker->Remove(backup.path);
                }
                else
                {
                    // 以后再说：文件留着（这次不再问，也不覆盖它），下次启动或打开它的项目时再问
                    m_autosave.postponed.insert(backup.path);
                    m_platform->Post([this] { ShowRecovery(); });
                }
            });
    }

    void MainFrame::RecoverBackup(const Bim::Autosave::Backup& backup)
    {
        Bim::Json params{ { "path", backup.path } };
        if (backup.project)
            params["project"] = *backup.project;
        if (!RunCommand("recover", params, N_("Could not recover the project")))
        {
            m_autosave.postponed.insert(backup.path);
            return;
        }
        AutosaveState& a = m_autosave;
        // 恢复出来的是没保存的工作：接着往这份副本里写；换文档时的查找（会又找到它）作废
        m_savedModel.reset();
        std::erase(a.candidates, backup);
        ++a.generation;
        a.discovering = 0;
        a.active  = backup.path;
        a.project = backup.project;
        if (!backup.project)
            a.temporary = backup.path;
        a.snapshot = m_session.Snapshot();
        ScheduleAutosave();
        UpdateTitle();
        m_commands.NotifyStateChanged();
        ShowRecovery();
    }
}
