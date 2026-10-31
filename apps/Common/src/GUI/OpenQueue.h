#pragma once
// 移植自 BIMCraft crates/ui-egui/src/lib.rs 的 opens（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include <optional>
#include <string>
#include <vector>

namespace MiniBIM
{
    // 系统要打开的文件（拖进窗口的）：读 Revit 文件或有对话框时等着，之后只开最后一个，其余的作废。
    // 不碰界面控件，AppTests 测；何时算"忙"由 MainFrame 定
    class OpenQueue
    {
    public:
        void Add(const std::vector<std::string>& paths)
        {
            for (const std::string& p : paths)
                if (!p.empty())
                    m_paths.push_back(p);
        }

        bool Waiting() const { return !m_paths.empty(); }

        // 不忙时取出最后一个并清空队列
        std::optional<std::string> Take(bool busy)
        {
            if (busy || m_paths.empty())
                return std::nullopt;
            std::string last = std::move(m_paths.back());
            m_paths.clear();
            return last;
        }

    private:
        std::vector<std::string> m_paths;
    };
}
