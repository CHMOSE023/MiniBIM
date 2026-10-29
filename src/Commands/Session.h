// 移植自 BIMCraft crates/core/src/engine.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Json.h"
#include "Model/Model.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 每个前端驱动的会话：一个模型、它的撤销历史和命令表。用户看得见的每个操作都是一条命令（Commands.cpp 的命令表），
// 界面、脚本（以后还有命令行、AI 代理）都调同样的命令。
//
// 撤销照 BIMCraft：每条改了模型的命令之后记下改之前的整个模型。模型里的元素是共享的不可变对象，
// 复制模型只复制指针，改动时才复制那个元素（Elements::GetMut），所以快照很便宜
namespace MiniBIM::Bim
{
    class Session;

    // 命令：参数是 JSON，成功时结果放进 result，失败时返回 false、原因（英文，给人和 AI 代理读）放进 error
    using RunFn = bool (*)(Session& s, const Json& params, Json& result, std::string& error);

    struct Command
    {
        const char*              id;        // 小写，照 Revit 的工具名
        const char*              label;     // 名称（英文原文，用 N_ 标记，界面显示前 Tr）
        std::vector<const char*> aliases;   // 输入的快捷名，Revit 风格（WA、DR、WN……）
        const char*              params;    // 参数说明，给 AI 代理读
        RunFn                    run;
        bool                     undoable;  // 它做的改动是不是一个撤销步骤
    };

    inline constexpr size_t UndoLimit = 200;

    // 撤销（或重做）的一步：是哪条命令，做出与去掉的元素按类别计数，各类别按其第一个元素的 ID 排序，
    // 列表可以这样称呼它：创建墙、删除门
    struct Step
    {
        std::string                                  label;
        std::vector<std::pair<std::string, size_t>>  made;
        std::vector<std::pair<std::string, size_t>>  removed;

        // 不逐个比对元素：新的 ID 都比 before 的大（ID 只增不减）；只有数目说明去掉了东西时才在 before 里找是哪些
        static Step Between(std::string label, const Model& before, const Model& after);
        bool operator==(const Step&) const = default;
    };

    // 全部命令
    const std::vector<Command>& Commands();
    // 按 ID 或快捷名找命令，不分大小写
    const Command* FindCommand(std::string_view name);
    // 命令表，给 AI 代理读：[{id, label, aliases, params}, …]
    Json Catalog();

    class Session
    {
    public:
        Session();

        const Model& GetModel() const { return *m_model; }
        // 当前模型的快照（撤销历史里的也是这样共享的）
        std::shared_ptr<const Model> Snapshot() const { return m_model; }
        // 要改的模型：撤销步骤还共用着它时先复制一份
        Model& ModelMut();

        // 打开或最后保存到的路径（UTF-8）
        const std::optional<std::string>& Path() const { return m_path; }
        void SetPath(std::optional<std::string> path) { m_path = std::move(path); }

        // 换掉整个模型（新建、打开），忘掉历史
        void Reset(Model model, std::optional<std::string> path);
        // 项目被换掉过几次：事务据此知道历史跟着换了
        size_t Resets() const { return m_resets; }

        // 撤销步骤的名称，从早到晚
        std::vector<std::string> History() const;
        // 撤销（redo 时为重做）的各步，下一个要撤销的在前
        std::vector<const Step*> Steps(bool redo) const;
        bool CanUndo() const { return !m_undo.empty(); }
        bool CanRedo() const { return !m_redo.empty(); }

        // 按 ID 或快捷名执行命令。命令失败、抛出异常，或让模型通不过 Model::Check 时什么都不改
        bool Execute(std::string_view name, const Json& params, Json& result, std::string& error);
        // 执行脚本：一行一条命令，`id` 或 `id {json 参数}`，# 开头为注释。在第一行失败处停下并说明是哪一行，之前的行保留
        bool Script(std::string_view text, std::vector<Json>& results, std::string& error);
        // 全做或全不做的脚本：一行失败时前面的也撤回；做成时是一个撤销步骤 Script
        bool ScriptAtomic(std::string_view text, std::vector<Json>& results, std::string& error);
        // 把 f 作为一个名为 label 的撤销步骤执行，全做或全不做：失败时模型、路径、撤销与重做历史都回到之前。
        // 里面的撤销最多退到它开始处。其间命令写出的文件（save）保留；换掉了项目的（new、open）丢下它开始时的历史
        bool Transaction(std::string label, const std::function<bool(Session&, std::string&)>& f, std::string& error);

        // 撤销（redo 时为重做）count 步；结果说明最后一步是什么
        bool StepBack(bool redo, size_t count, Json& result, std::string& error);

    private:
        std::shared_ptr<const Model>                                        m_model;
        std::optional<std::string>                                          m_path;
        std::vector<std::pair<Step, std::shared_ptr<const Model>>>          m_undo;
        std::vector<std::pair<Step, std::shared_ptr<const Model>>>          m_redo;
        size_t                                                              m_resets = 0;
    };
}
