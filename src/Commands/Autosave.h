// 移植自 BIMCraft crates/core/src/autosave.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Model.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

// 自动保存：在后台写不可变模型的快照（恢复副本）。一个工作线程按顺序做写、删与查找，
// 所以晚到的一次自动保存不会把"保存"或"丢弃"刚删掉的文件又写回来。不碰界面，也不锁模型（模型快照本身不可变）。
// 路径一律 UTF-8
namespace MiniBIM::Bim::Autosave
{
    inline constexpr const char* Suffix = ".autosave.minibim";

    // 项目 project 的恢复副本：在扩展名后面再接一段（不是换掉扩展名），不同名的项目不会撞在一起
    std::string BackupPath(const std::string& project);

    struct Backup
    {
        std::string                path;
        std::optional<std::string> project;     // 没保存过的项目为空：恢复后保存时要问存到哪里
        bool operator==(const Backup&) const = default;
    };

    // 工作线程做完一件事的通知
    struct Saved      { std::string path; std::shared_ptr<const Model> snapshot; bool ok; std::string error; };
    struct Removed    { std::string path; bool ok; std::string error; };
    struct Discovered { uint64_t generation; bool ok; std::vector<Backup> found; std::string error; };
    using Notice = std::variant<Saved, Removed, Discovered>;

    class Worker
    {
    public:
        // temp：没保存过的项目的恢复副本放在这里（要写时才建）。wake 在工作线程上调用：有通知了，界面线程该来 Poll
        Worker(std::string temp, std::function<void()> wake);
        // 不丢下已经接下的事：做完队列里的再退出
        ~Worker();
        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;

        // 写一份快照。上一份还没写完时不接（返回 false）：一直在打字也不会堆起一串过时的模型
        bool Save(std::string path, std::shared_ptr<const Model> model);
        void Remove(std::string path);
        // 找恢复副本：给了项目时只看它旁边的那一份，否则看 temp 里没保存过的项目留下的（最多 128 个，按路径排）
        void Discover(std::optional<std::string> project, uint64_t generation);

        std::optional<Notice> Poll();
        bool Busy() const { return m_busy; }

    private:
        struct SaveJob     { std::string path; std::shared_ptr<const Model> model; };
        struct RemoveJob   { std::string path; };
        struct DiscoverJob { std::optional<std::string> project; uint64_t generation; };
        using Job = std::variant<SaveJob, RemoveJob, DiscoverJob>;

        void Run();
        void Send(Job job);

        std::string             m_temp;
        std::function<void()>   m_wake;
        std::mutex              m_mutex;
        std::condition_variable m_cv;
        std::deque<Job>         m_jobs;
        std::deque<Notice>      m_notes;
        bool                    m_stop = false;
        bool                    m_busy = false;      // 只在调用方线程上读写
        std::thread             m_thread;
    };
}
