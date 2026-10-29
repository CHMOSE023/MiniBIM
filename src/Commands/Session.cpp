// 移植自 BIMCraft crates/core/src/engine.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Commands/Session.h"
#include "Model/Text.h"
#include <algorithm>
#include <exception>

namespace MiniBIM::Bim
{
    namespace
    {
        // params 里每个是库条目 GUID 的字符串换成条目的名称；没有这样的字符串时为空
        std::optional<Json> Named(const Model& m, const Json& params)
        {
            bool changed = false;
            std::function<void(Json&)> walk = [&](Json& v)
            {
                if (v.is_string())
                {
                    const auto g = Guid::Parse(v.get<std::string>());
                    if (g && !g->Pending())
                        if (const auto e = m.library.ByGuid(*g))
                        {
                            v = e->second->name;
                            changed = true;
                        }
                }
                else if (v.is_array() || v.is_object())
                {
                    for (auto& x : v)
                        walk(x);
                }
            };
            Json v = params;
            walk(v);
            if (!changed)
                return std::nullopt;
            return v;
        }

        std::vector<std::pair<std::string, size_t>> Tally(const std::vector<const Element*>& elements)
        {
            std::vector<std::pair<std::string, size_t>> counts;
            for (const Element* e : elements)
            {
                const std::string c = e->Category();
                auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& x) { return x.first == c; });
                if (it == counts.end())
                    counts.emplace_back(c, 1);
                else
                    ++it->second;
            }
            return counts;
        }
    }

    Step Step::Between(std::string label, const Model& before, const Model& after)
    {
        const Id firstNew = std::max(before.elements.LastId() ? *before.elements.LastId() + 1 : 0, before.nextId);
        std::vector<const Element*> made;
        for (auto it = after.elements.Items().lower_bound(firstNew); it != after.elements.Items().end(); ++it)
            made.push_back(it->second.get());
        const size_t kept = after.elements.Size() - made.size();
        std::vector<const Element*> removed;
        if (before.elements.Size() > kept)
            for (const auto& [id, e] : before.elements.Items())
                if (!after.elements.Contains(id))
                    removed.push_back(e.get());
        return Step{ std::move(label), Tally(made), Tally(removed) };
    }

    const Command* FindCommand(std::string_view name)
    {
        const std::string n = Text::AsciiLower(name);
        for (const Command& c : Commands())
        {
            if (n == c.id)
                return &c;
            for (const char* a : c.aliases)
                if (n == Text::AsciiLower(a))
                    return &c;
        }
        return nullptr;
    }

    Json Catalog()
    {
        Json out = Json::array();
        for (const Command& c : Commands())
        {
            Json aliases = Json::array();
            for (const char* a : c.aliases)
                aliases.push_back(a);
            out.push_back(Json{ { "id", c.id }, { "label", c.label }, { "aliases", aliases }, { "params", c.params } });
        }
        return out;
    }

    Session::Session() : m_model(std::make_shared<Model>(Model::New("Project")))
    {
    }

    Model& Session::ModelMut()
    {
        // 与撤销步骤（或调用方拿着的快照）共用时先复制；新对象不是 const 的，所以下面的 const_cast 合法
        if (m_model.use_count() > 1)
            m_model = std::make_shared<Model>(*m_model);
        return const_cast<Model&>(*m_model);
    }

    void Session::Reset(Model model, std::optional<std::string> path)
    {
        m_model = std::make_shared<Model>(std::move(model));
        m_path  = std::move(path);
        ++m_resets;
        m_undo.clear();
        m_redo.clear();
    }

    std::vector<std::string> Session::History() const
    {
        std::vector<std::string> out;
        for (const auto& [step, _] : m_undo)
            out.push_back(step.label);
        return out;
    }

    std::vector<const Step*> Session::Steps(bool redo) const
    {
        const auto& from = redo ? m_redo : m_undo;
        std::vector<const Step*> out;
        for (auto it = from.rbegin(); it != from.rend(); ++it)
            out.push_back(&it->first);
        return out;
    }

    bool Session::Execute(std::string_view name, const Json& params, Json& result, std::string& error)
    {
        const Command* cmd = FindCommand(name);
        if (!cmd)
        {
            error = "unknown command `" + std::string(name) + "`";
            return false;
        }
        // 命令收名称的地方，库条目的 GUID 也算那个名称
        const std::optional<Json> named = Named(*m_model, params);
        const Json& p = named ? *named : params;
        const std::shared_ptr<const Model> before = m_model;
        bool ok = false;
        try
        {
            ok = cmd->run(*this, p, result, error);
        }
        catch (const std::exception& e)
        {
            error = "internal error in `" + std::string(cmd->id) + "`: " + e.what();
            ok = false;
        }
        catch (...)
        {
            error = "internal error in `" + std::string(cmd->id) + "`";
            ok = false;
        }
        if (ok && m_model != before)
        {
            // 命令按 ID 给的引用换成元素的 GUID；放进库的东西给上 ID 和 GUID
            Model& m = ModelMut();
            m.Resolve();
            m.SyncLibrary();
            ok = m.Check(error);
        }
        if (!ok)
        {
            // 被拒绝的命令什么都不改
            m_model = before;
            return false;
        }
        if (cmd->undoable && m_model != before)
        {
            m_undo.emplace_back(Step::Between(cmd->label, *before, *m_model), before);
            if (m_undo.size() > UndoLimit)
                m_undo.erase(m_undo.begin());
            m_redo.clear();
        }
        return true;
    }

    bool Session::Script(std::string_view text, std::vector<Json>& results, std::string& error)
    {
        size_t n = 0;
        while (!text.empty())
        {
            const size_t eol = text.find('\n');
            const std::string_view raw = text.substr(0, eol);
            text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
            ++n;
            const std::string_view line = Text::Trim(raw);
            if (line.empty() || line.front() == '#')
                continue;
            const size_t space = line.find_first_of(" \t");
            const std::string id(line.substr(0, space));
            const std::string_view rest = space == std::string_view::npos ? std::string_view() : Text::Trim(line.substr(space));
            Json params = Json::object();
            if (!rest.empty())
            {
                params = Json::parse(rest.begin(), rest.end(), nullptr, false);
                if (params.is_discarded())
                {
                    error = "line " + std::to_string(n) + ": bad JSON after `" + id + "`";
                    return false;
                }
            }
            Json result;
            std::string why;
            if (!Execute(id, params, result, why))
            {
                error = "line " + std::to_string(n) + " (" + id + "): " + why;
                return false;
            }
            results.push_back(std::move(result));
        }
        return true;
    }

    bool Session::ScriptAtomic(std::string_view text, std::vector<Json>& results, std::string& error)
    {
        const std::string script(text);
        return Transaction("Script", [&](Session& s, std::string& why) { return s.Script(script, results, why); }, error);
    }

    bool Session::Transaction(std::string label, const std::function<bool(Session&, std::string&)>& f, std::string& error)
    {
        const std::shared_ptr<const Model> before = m_model;
        const std::optional<std::string> path = m_path;
        auto undo = std::move(m_undo);
        auto redo = std::move(m_redo);
        m_undo.clear();
        m_redo.clear();
        const size_t resets = m_resets;
        bool ok = false;
        try
        {
            ok = f(*this, error);
        }
        catch (const std::exception& e)
        {
            error = "internal error in `" + label + "`: " + e.what();
        }
        catch (...)
        {
            error = "internal error in `" + label + "`";
        }
        if (!ok)
        {
            m_model = before;
            m_path  = path;
            m_undo  = std::move(undo);
            m_redo  = std::move(redo);
            return false;
        }
        if (m_resets != resets)
            return true;
        // 只有可撤销的命令做出一步；什么都没改（或只改了视图）的这里也不算一步
        if (m_undo.empty())
            m_redo = std::move(redo);
        else
        {
            undo.emplace_back(Step::Between(std::move(label), *before, *m_model), before);
            if (undo.size() > UndoLimit)
                undo.erase(undo.begin());
            m_redo.clear();
        }
        m_undo = std::move(undo);
        return true;
    }

    bool Session::StepBack(bool redo, size_t count, Json& result, std::string& error)
    {
        auto& from = redo ? m_redo : m_undo;
        auto& to   = redo ? m_undo : m_redo;
        const char* key  = redo ? "redone" : "undone";
        const char* what = redo ? "redo" : "undo";
        if (from.empty())
        {
            error = std::string("nothing to ") + what;
            return false;
        }
        if (count == 0 || count > from.size())
        {
            error = "`count` must be from 1 to " + std::to_string(from.size()) + ", the steps there are to " + what;
            return false;
        }
        std::string label;
        for (size_t i = 0; i < count; ++i)
        {
            auto [step, model] = std::move(from.back());
            from.pop_back();
            label = step.label;
            to.emplace_back(std::move(step), std::exchange(m_model, std::move(model)));
        }
        result = Json{ { key, label }, { "count", count } };
        return true;
    }
}
