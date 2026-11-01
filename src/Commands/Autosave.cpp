// 移植自 BIMCraft crates/core/src/autosave.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Commands/Autosave.h"
#include "Commands/Session.h"
#include <algorithm>
#include <filesystem>

namespace MiniBIM::Bim::Autosave
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

        bool EndsWith(const std::string& s, std::string_view tail)
        {
            return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
        }

        Discovered Find(const std::string& temp, const std::optional<std::string>& project, uint64_t generation)
        {
            Discovered d{ generation, true, {}, {} };
            std::error_code ec;
            if (project)
            {
                const std::string path = BackupPath(*project);
                if (std::filesystem::is_regular_file(ToPath(path), ec))
                    d.found.push_back({ path, project });
                return d;
            }
            if (!std::filesystem::exists(ToPath(temp), ec))
                return d;
            std::filesystem::directory_iterator it(ToPath(temp), ec), end;
            if (ec)
            {
                d.ok    = false;
                d.error = temp + ": " + ec.message();
                return d;
            }
            for (; it != end && d.found.size() < 128; it.increment(ec))
            {
                if (ec)
                    break;
                const std::string name = ToUtf8(it->path().filename());
                if (EndsWith(name, Suffix) && it->is_regular_file(ec))
                    d.found.push_back({ ToUtf8(it->path()), std::nullopt });
            }
            std::sort(d.found.begin(), d.found.end(), [](const Backup& a, const Backup& b) { return a.path < b.path; });
            return d;
        }
    }

    std::string BackupPath(const std::string& project)
    {
        return project + Suffix;
    }

    Worker::Worker(std::string temp, std::function<void()> wake)
        : m_temp(std::move(temp)), m_wake(std::move(wake))
    {
        m_thread = std::thread([this] { Run(); });
    }

    Worker::~Worker()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

    bool Worker::Save(std::string path, std::shared_ptr<const Model> model)
    {
        if (m_busy)
            return false;
        m_busy = true;
        Send(SaveJob{ std::move(path), std::move(model) });
        return true;
    }

    void Worker::Remove(std::string path)
    {
        Send(RemoveJob{ std::move(path) });
    }

    void Worker::Discover(std::optional<std::string> project, uint64_t generation)
    {
        Send(DiscoverJob{ std::move(project), generation });
    }

    std::optional<Notice> Worker::Poll()
    {
        std::lock_guard lock(m_mutex);
        if (m_notes.empty())
            return std::nullopt;
        Notice n = std::move(m_notes.front());
        m_notes.pop_front();
        if (std::holds_alternative<Saved>(n))
            m_busy = false;
        return n;
    }

    void Worker::Send(Job job)
    {
        {
            std::lock_guard lock(m_mutex);
            m_jobs.push_back(std::move(job));
        }
        m_cv.notify_one();
    }

    void Worker::Run()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
                // 要停时也先做完已经接下的事（排在后面的删除不能丢）
                if (m_jobs.empty())
                    return;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            Notice note;
            if (auto* save = std::get_if<SaveJob>(&job))
            {
                Saved s{ save->path, save->model, true, {} };
                std::error_code ec;
                if (ToPath(save->path).parent_path() == ToPath(m_temp))
                    std::filesystem::create_directories(ToPath(m_temp), ec);
                Session session;
                session.Reset(Model(*save->model), std::nullopt);
                Json result;
                s.ok = session.Execute("autosave", Json{ { "path", save->path } }, result, s.error);
                note = std::move(s);
            }
            else if (auto* remove = std::get_if<RemoveJob>(&job))
            {
                Removed r{ remove->path, true, {} };
                std::error_code ec;
                // 不在的副本不用删（它的文件夹可能还没有）
                if (std::filesystem::exists(ToPath(remove->path), ec) && !std::filesystem::remove(ToPath(remove->path), ec) && ec)
                {
                    r.ok    = false;
                    r.error = remove->path + ": " + ec.message();
                }
                note = std::move(r);
            }
            else if (auto* discover = std::get_if<DiscoverJob>(&job))
                note = Find(m_temp, discover->project, discover->generation);
            {
                std::lock_guard lock(m_mutex);
                m_notes.push_back(std::move(note));
            }
            if (m_wake)
                m_wake();
        }
    }
}
