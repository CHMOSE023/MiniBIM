// 移植自 BIMCraft crates/ui-egui/src/shortcuts.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Json.h"
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MiniBIM
{
    // Revit 风格的两个字母的快捷键（WA 墙、DR 门、ZF 缩放到全部……）：在视图上直接打字母，凑成一个就执行它的界面命令。
    // 默认的一张表由主窗口给（MainFrame::RegisterCommands），自己改的存在 settings.json 的 "shortcuts"。
    // 与 Ctrl+字母那种快捷键（MiniGUI::ShortcutTable，界面描述文件的 "shortcut"）是两回事
    class Shortcuts
    {
    public:
        struct Entry
        {
            std::string keys;
            std::string command;    // 界面命令的 ID
            bool        custom = false;
            bool operator==(const Entry&) const = default;
        };

        // 快捷键文件只有几行；超过这个的不是
        static constexpr size_t MaxFileBytes = 64 << 10;

        // 快捷键：两个字母或数字（打字的缓冲就这么长）
        static bool IsKey(std::string_view keys);
        // 去掉两头空白、变大写
        static std::string Normalize(std::string_view keys);

        // 默认的表，按顺序；同一个键第一个算
        void SetDefaults(std::vector<std::pair<std::string, std::string>> defaults);
        // keys 换成 command（和默认一样时就是回到默认）
        void Set(std::string_view keys, const std::string& command);
        // 不要这个键了：默认有的记成关掉，自己加的去掉
        void Remove(std::string_view keys);
        // 去掉所有自己改的，回到默认
        void Reset() { m_custom.clear(); }

        // keys 现在执行哪个命令，没有时为空
        std::string Find(std::string_view keys) const;
        // keys 已经给了 command 以外的命令时，那个命令
        std::string Conflict(std::string_view keys, std::string_view command) const;
        // 现在有的全部快捷键：没改过的默认在前（按表的顺序），然后自己加的（按键）
        std::vector<Entry> All() const;

        // 打字：字母和数字进两个字的缓冲（大写，多了丢最早的）；凑成一个快捷键时返回它的命令并清空缓冲
        std::string Type(char c);
        void ClearTyped() { m_typed.clear(); }
        const std::string& Typed() const { return m_typed; }

        // 自己改的：{"WA": "arch.wall", "ZF": ""}（空 = 关掉默认的）。读时略去键不对的
        Bim::Json ToJson() const;
        void FromJson(const Bim::Json& j);

        // 快捷键文件里写的（键，说明）：键到说明的 JSON 对象、Revit 导出的 <Shortcut CommandName=".." Shortcuts="WA#W1"/> 行，
        // 或 "WA - Wall"、"WA = Wall"、"WA: Wall" 这样的行
        static std::vector<std::pair<std::string, std::string>> ParseFile(std::string_view content);
        // 把文件里的加进自己改的：resolve 把说明换成命令 ID（不认识时为空）。键不对、不认识的命令、键已经给了别的命令的都略过。
        // 返回加了几个、略过几个
        std::pair<size_t, size_t> Add(const std::vector<std::pair<std::string, std::string>>& parsed,
                                      const std::function<std::string(const std::string&)>& resolve);

    private:
        std::string Default(std::string_view keys) const;

        std::vector<std::pair<std::string, std::string>> m_defaults;
        std::map<std::string, std::string, std::less<>> m_custom;
        std::string m_typed;
    };
}
