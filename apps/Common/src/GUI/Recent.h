// 移植自 BIMCraft crates/ui-egui/src/recent.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace MiniBIM
{
    // "文件 > 最近打开"列的文件：最后打开或保存的项目与 IFC 文件，新的在前。存在用户设置目录里自己的文件 recent.json
    // （与 settings.json 同目录），不在项目里：项目会被拷走、给别人打开。路径一律 UTF-8
    class RecentFiles
    {
    public:
        static constexpr size_t MaxFiles = 10;
        // 文件只有几百字节；超过这个的不是我们的，不读
        static constexpr size_t MaxFileBytes = 64 << 10;

        // path 处文件里的列表。没有、坏了、不是想要的内容都略去：设置文件从不挡住程序
        static RecentFiles Load(const std::string& path);
        // 整个写进 path（先写旁边的临时文件再换过去），需要时建目录
        bool Save(const std::string& path, std::string& error) const;

        // 新的在前
        const std::vector<std::string>& Paths() const { return m_paths; }
        // path 成为最新的一项，只出现一次：原来的位置去掉，超过 MaxFiles 时最旧的去掉。相对路径先变成绝对路径
        void Push(const std::string& path);
        void Remove(const std::string& path);
        void Clear() { m_paths.clear(); }

        // 菜单上每个文件显示什么：文件名；两个文件同名时后面加它所在的文件夹
        std::vector<std::string> Labels() const;

        // 文件改过多久了（秒），用话说（开始页的卡片）：刚刚、5 分钟前、昨天、3 天前……
        static std::string Age(long long seconds);

        bool operator==(const RecentFiles&) const = default;

    private:
        std::vector<std::string> m_paths;
    };
}
