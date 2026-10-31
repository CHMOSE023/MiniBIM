// 移植自 BIMCraft crates/core/src/library.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Library.h"
#include "Model/Model.h"
#include <set>
#include <tuple>
#include <unordered_set>

namespace MiniBIM::Bim
{
    namespace
    {
        struct KindName
        {
            Kind::Tag   tag;
            const char* file;       // 文件里的写法
            const char* label;      // 报错里的叫法
        };

        constexpr KindName kKinds[] = {
            { Kind::Tag::Material,         "material",           "material" },
            { Kind::Tag::Picture,          "picture",            "picture" },
            { Kind::Tag::Cloud,            "cloud",              "scan" },
            { Kind::Tag::Cad,              "cad",                "CAD drawing" },
            { Kind::Tag::Family,           "family",             "family" },
            { Kind::Tag::Parameter,        "parameter",          "parameter" },
            { Kind::Tag::ViewFilter,       "view-filter",        "view filter" },
            { Kind::Tag::ViewTemplate,     "view-template",      "view template" },
            { Kind::Tag::Phase,            "phase",              "phase" },
            { Kind::Tag::PhaseFilter,      "phase-filter",       "phase filter" },
            { Kind::Tag::OptionSet,        "option-set",         "design option set" },
            { Kind::Tag::DesignOption,     "design-option",      "design option" },
            { Kind::Tag::FamilyType,       "family-type",        "family type" },
            { Kind::Tag::FamilyParameter,  "family-parameter",   "family parameter" },
            { Kind::Tag::LinePattern,      "line-pattern",       "line pattern" },
            { Kind::Tag::LineStyle,        "line-style",         "line style" },
            { Kind::Tag::FillPattern,      "fill-pattern",       "fill pattern" },
            { Kind::Tag::FilledRegionType, "filled-region-type", "filled region type" },
            { Kind::Tag::DetailGroup,      "detail-group",       "detail group" },
            { Kind::Tag::TextType,         "text-type",          "text type" },
        };
    }

    // ── Kind、Part ───────────────────────────────────────────────
    std::string Kind::Label() const
    {
        if (tag == Tag::Type)
            return category + " type";
        for (const KindName& k : kKinds)
            if (k.tag == tag)
                return k.label;
        return "entry";
    }

    std::string Kind::ToString() const
    {
        if (tag == Tag::Type)
            return "type:" + category;
        for (const KindName& k : kKinds)
            if (k.tag == tag)
                return k.file;
        return "";
    }

    std::optional<Kind> Kind::Parse(std::string_view s)
    {
        for (const KindName& k : kKinds)
            if (s == k.file)
                return Of(k.tag);
        if (s.substr(0, 5) == "type:")
            return Type(std::string(s.substr(5)));
        return std::nullopt;
    }

    std::string Part::Name() const
    {
        return what + " " + std::to_string(index + 1) + " of " + of;
    }

    // ── Library ──────────────────────────────────────────────────
    const Entry* Library::Get(Id id) const
    {
        const auto it = m_byId.find(id);
        return it == m_byId.end() ? nullptr : &it->second;
    }

    const Part* Library::GetPart(Id id) const
    {
        const auto it = m_parts.find(id);
        return it == m_parts.end() ? nullptr : &it->second;
    }

    std::optional<std::pair<Id, const Part*>> Library::PartByGuid(Guid g) const
    {
        for (const auto& [id, p] : m_parts)
            if (p.guid == g)
                return std::pair<Id, const Part*>{ id, &p };
        return std::nullopt;
    }

    std::optional<Id> Library::IdOf(const Kind& kind, std::string_view name) const
    {
        const auto it = m_byName.find({ kind, std::string(name) });
        if (it == m_byName.end())
            return std::nullopt;
        return it->second;
    }

    std::optional<Guid> Library::GuidOf(const Kind& kind, std::string_view name) const
    {
        const auto id = IdOf(kind, name);
        const Entry* e = id ? Get(*id) : nullptr;
        if (!e)
            return std::nullopt;
        return e->guid;
    }

    std::optional<std::pair<Id, const Entry*>> Library::ByGuid(Guid g) const
    {
        const auto it = m_byGuid.find(g);
        if (it == m_byGuid.end())
            return std::nullopt;
        const Entry* e = Get(it->second);
        if (!e)
            return std::nullopt;
        return std::pair<Id, const Entry*>{ it->second, e };
    }

    bool Library::Insert(Id id, Entry e, std::string& error)
    {
        if (m_byId.count(id))
        {
            error = "two library entries have the id " + std::to_string(id);
            return false;
        }
        if (const auto other = m_byGuid.find(e.guid); other != m_byGuid.end())
        {
            error = "library entries " + std::to_string(other->second) + " and " + std::to_string(id) + " have the same GUID " + e.guid.ToString();
            return false;
        }
        if (m_byName.count({ e.kind, e.name }))
        {
            error = "two " + e.kind.Label() + "s are named `" + e.name + "` in the library";
            return false;
        }
        m_byName[{ e.kind, e.name }] = id;
        m_byGuid[e.guid] = id;
        m_byId.emplace(id, std::move(e));
        m_lowest = LowestId().value_or(id);
        return true;
    }

    std::optional<Id> Library::LowestId() const
    {
        std::optional<Id> low;
        auto take = [&](Id id) { low = low ? std::min(*low, id) : id; };
        if (!m_byId.empty())
            take(m_byId.begin()->first);
        if (!m_parts.empty())
            take(m_parts.begin()->first);
        if (m_lowest != 0)
            take(m_lowest);
        return low;
    }

    void Library::Remove(Id id)
    {
        const auto it = m_byId.find(id);
        if (it == m_byId.end())
            return;
        m_byName.erase({ it->second.kind, it->second.name });
        m_byGuid.erase(it->second.guid);
        m_byId.erase(it);
    }

    void Library::Renamed(const Kind& kind, const std::string& oldName, const std::string& newName)
    {
        if (oldName == newName)
            return;
        const auto id = IdOf(kind, oldName);
        if (!id)
            return;
        m_byName.erase({ kind, oldName });
        m_byId[*id].name = newName;
        m_byName[{ kind, newName }] = *id;
    }

    void Library::FamilyRenamed(const std::string& oldName, const std::string& newName)
    {
        // 名为 "旧名: …" 的族类型、族参数改为 "新名: …"
        const std::string prefix = oldName + ": ";
        for (const Kind kind : { Kind::Of(Kind::Tag::FamilyType), Kind::Of(Kind::Tag::FamilyParameter) })
        {
            std::vector<std::string> names;
            for (const auto& [key, _] : m_byName)
                if (key.first == kind && key.second.rfind(prefix, 0) == 0)
                    names.push_back(key.second);
            for (const std::string& n : names)
                Renamed(kind, n, newName + ": " + n.substr(prefix.size()));
        }
        Renamed(Kind::Of(Kind::Tag::Family), oldName, newName);
    }

    // ── 模型的库 ─────────────────────────────────────────────────
    std::vector<std::pair<Kind, std::string>> Model::LibraryNames() const
    {
        std::vector<std::pair<Kind, std::string>> out;
        for (const auto& [n, _] : types.wall)
            out.emplace_back(Kind::Type("wall"), n);
        for (const auto& [n, _] : types.floor)
            out.emplace_back(Kind::Type("floor"), n);
        for (const auto& [n, _] : types.door)
            out.emplace_back(Kind::Type("door"), n);
        for (const auto& [n, _] : types.window)
            out.emplace_back(Kind::Type("window"), n);
        for (const auto& [n, _] : types.column)
            out.emplace_back(Kind::Type("column"), n);
        for (const auto& [n, _] : types.roof)
            out.emplace_back(Kind::Type("roof"), n);
        for (const auto& [n, _] : types.ceiling)
            out.emplace_back(Kind::Type("ceiling"), n);
        for (const auto& [n, _] : types.structuralColumn)
            out.emplace_back(Kind::Type("structuralcolumn"), n);
        for (const auto& [n, _] : types.footing)
            out.emplace_back(Kind::Type("footing"), n);
        for (const auto& [cat, map] : types.runs)
            for (const auto& [n, _] : map)
                out.emplace_back(Kind::Type(cat), n);
        for (const auto& [n, _] : materials)
            out.emplace_back(Kind::Of(Kind::Tag::Material), n);
        for (const auto& [n, _] : families)
            out.emplace_back(Kind::Of(Kind::Tag::Family), n);
        for (const auto& [f, fam] : families)
            for (const auto& [t, _] : fam.types)
                out.emplace_back(Kind::Of(Kind::Tag::FamilyType), f + ": " + t);
        for (const auto& [f, fam] : families)
            for (const Fam::Parameter& p : fam.parameters)
                out.emplace_back(Kind::Of(Kind::Tag::FamilyParameter), f + ": " + p.name);
        for (const auto& [n, _] : linePatterns)
            out.emplace_back(Kind::Of(Kind::Tag::LinePattern), n);
        for (const auto& [n, _] : lineStyles)
            out.emplace_back(Kind::Of(Kind::Tag::LineStyle), n);
        for (const auto& [n, _] : fillPatterns)
            out.emplace_back(Kind::Of(Kind::Tag::FillPattern), n);
        for (const auto& [n, _] : regionTypes)
            out.emplace_back(Kind::Of(Kind::Tag::FilledRegionType), n);
        for (const auto& [n, _] : textTypes)
            out.emplace_back(Kind::Of(Kind::Tag::TextType), n);
        return out;
    }

    bool Model::LibraryInStep() const
    {
        const auto now = LibraryNames();
        if (now.size() != library.Size())
            return false;
        for (const auto& [k, n] : now)
            if (!library.IdOf(k, n))
                return false;
        return PartsInStep();
    }

    std::vector<std::pair<Id, Part>> Model::PartsNow() const
    {
        std::vector<std::pair<Id, Part>> out;
        auto add = [&](const char* what, const char* of, const std::string& owner, const std::vector<Layer>& layers)
        {
            for (size_t i = 0; i < layers.size(); ++i)
                out.emplace_back(layers[i].id, Part{ what, std::string(of) + " " + owner, owner, i, layers[i].guid });
        };
        for (const auto& [n, x] : types.wall)
            add("layer", "wall type", n, x.layers);
        for (const auto& [n, x] : types.floor)
            add("layer", "floor type", n, x.layers);
        for (const auto& [n, x] : types.roof)
            add("layer", "roof type", n, x.layers);
        for (const auto& [n, x] : types.ceiling)
            add("layer", "ceiling type", n, x.layers);
        for (const auto& [n, f] : families)
        {
            for (size_t i = 0; i < f.forms.size(); ++i)
                out.emplace_back(f.forms[i].id, Part{ "form", "family " + n, n, i, f.forms[i].guid });
            for (size_t i = 0; i < f.symbols.size(); ++i)
                out.emplace_back(f.symbols[i].id, Part{ "symbol", "family " + n, n, i, f.symbols[i].guid });
        }
        // 元素的草图线：元素按它的 GUID 叫（不会变）
        for (const auto& [id, e] : elements.Items())
            if (const auto* lines = e->SketchOf(); lines && !lines->empty())
            {
                const std::string owner = GuidOf(id).ToString();
                for (size_t i = 0; i < lines->size(); ++i)
                    out.emplace_back((*lines)[i].id, Part{ "sketch line", std::string(e->Category()) + " " + owner, owner, i, (*lines)[i].guid });
            }
        return out;
    }

    bool Model::PartsInStep() const
    {
        const auto now = PartsNow();
        if (now.size() != library.m_parts.size())
            return false;
        std::set<Id> ids;
        std::unordered_set<Guid, GuidHash> guids;
        for (const auto& [id, p] : now)
        {
            const Part* held = library.GetPart(id);
            if (id == 0 || p.guid.IsNil() || p.guid.Pending() || !ids.insert(id).second || !guids.insert(p.guid).second || !held ||
                !(*held == p) || library.Get(id) || library.ByGuid(p.guid) || elements.Contains(id) || elements.Contains(p.guid))
                return false;
        }
        return true;
    }

    void Model::SyncParts(const GuidMaker& make)
    {
        const auto now = PartsNow();
        std::set<Id> ids;
        for (const auto& [id, _] : elements.Items())
            ids.insert(id);
        for (const auto& [id, _] : library.Entries())
            ids.insert(id);
        std::unordered_set<Guid, GuidHash> guids;
        for (const auto& [id, _] : elements.Items())
            guids.insert(GuidOf(id));
        for (const auto& [_, e] : library.Entries())
            guids.insert(e.guid);
        auto place = [](const Part& p) { return std::tuple(p.what, p.of, p.index); };
        // 谁保留什么：先是以前在同一位置拿着它的；再是剩下的第一个
        std::vector<std::pair<bool, bool>> keep(now.size(), { false, false });
        for (size_t k = 0; k < now.size(); ++k)
        {
            const auto& [id, p] = now[k];
            const Part* before = library.GetPart(id);
            const bool samePlace = before && place(*before) == place(p);
            if (samePlace && !ids.count(id))
            {
                ids.insert(id);
                keep[k].first = true;
            }
            if (samePlace && before->guid == p.guid && !guids.count(p.guid))
            {
                guids.insert(p.guid);
                keep[k].second = true;
            }
        }
        for (size_t k = 0; k < now.size(); ++k)
        {
            const auto& [id, p] = now[k];
            if (!keep[k].first && id != 0 && !ids.count(id))
            {
                ids.insert(id);
                keep[k].first = true;
            }
            if (!keep[k].second && !p.guid.IsNil() && !p.guid.Pending() && !guids.count(p.guid))
            {
                guids.insert(p.guid);
                keep[k].second = true;
            }
        }
        // 新的：ID 从库的最低处往下，GUID 依次生成
        const Id floor = std::max(elements.LastId() ? *elements.LastId() + 1 : FirstId, nextId);
        // 在每个条目、每个部件用过的 ID 之下，所以去掉的部件留下的 ID 不再用
        Id below = library.LowestId().value_or(LastId + 1);
        std::vector<std::pair<Id, Guid>> given;
        for (size_t k = 0; k < now.size(); ++k)
        {
            const auto& [oldId, p] = now[k];
            Id id = oldId;
            if (!keep[k].first)
            {
                Id next = below > 0 ? below - 1 : 0;
                while (next >= floor && ids.count(next))
                    --next;
                if (next < floor)
                    next = std::max(ids.empty() ? FirstId : *ids.rbegin() + 1, floor);
                else
                    below = next;
                ids.insert(next);
                id = next;
            }
            Guid guid = p.guid;
            if (!keep[k].second)
            {
                guid = make(*this, nullptr, p.Name());
                while (guid.IsNil() || guid.Pending() || guids.count(guid))
                    guid = Guid::New();
                guids.insert(guid);
            }
            given.emplace_back(id, guid);
        }
        // 按 PartsNow 的顺序放回
        size_t k = 0;
        auto give = [&](auto& l)
        {
            if (k < given.size())
            {
                l.id   = given[k].first;
                l.guid = given[k].second;
                ++k;
            }
        };
        for (auto& [_, x] : types.wall)
            for (Layer& l : x.layers)
                give(l);
        for (auto& [_, x] : types.floor)
            for (Layer& l : x.layers)
                give(l);
        for (auto& [_, x] : types.roof)
            for (Layer& l : x.layers)
                give(l);
        for (auto& [_, x] : types.ceiling)
            for (Layer& l : x.layers)
                give(l);
        for (auto& [_, f] : families)
        {
            for (Fam::Form& x : f.forms)
                give(x);
            for (Fam::Symbol& x : f.symbols)
                give(x);
        }
        // 元素是共享的：草图线要换 ID、GUID 时才复制那个元素
        std::vector<Id> sketched;
        for (const auto& [id, e] : elements.Items())
            if (const auto* lines = e->SketchOf(); lines && !lines->empty())
                sketched.push_back(id);
        for (const Id id : sketched)
        {
            const auto* lines = elements.Get(id)->SketchOf();
            bool same = true;
            for (size_t i = 0; i < lines->size() && same; ++i)
                same = k + i < given.size() && (*lines)[i].id == given[k + i].first && (*lines)[i].guid == given[k + i].second;
            if (same)
            {
                k += lines->size();
                continue;
            }
            for (SketchLine& l : *elements.GetMut(id)->SketchOf())
                give(l);
        }
        library.m_parts.clear();
        for (auto& [id, p] : PartsNow())
            library.m_parts.emplace(id, std::move(p));
        library.m_lowest = library.LowestId().value_or(0);
    }

    void Model::SyncLibrary()
    {
        SyncLibraryWith([](const Model& m, const Kind*, const std::string&) { return m.elements.Fresh(); });
    }

    void Model::SyncLibraryWith(const GuidMaker& guid)
    {
        if (LibraryInStep())
            return;
        const auto now = LibraryNames();
        const std::set<std::pair<Kind, std::string>> keep(now.begin(), now.end());
        std::vector<Id> gone;
        for (const auto& [id, e] : library.Entries())
            if (!keep.count({ e.kind, e.name }))
                gone.push_back(id);
        for (const Id id : gone)
            library.Remove(id);
        // 部件自己存着 ID，旧文件里它们就在条目下面：新条目也从它们下面数，部件不会被换 ID
        std::optional<Id> lowest;
        for (const auto& [id, _] : PartsNow())
            if (id != 0)
                lowest = lowest ? std::min(*lowest, id) : id;
        if (lowest)
            library.Hold(*lowest);
        for (const auto& [kind, entryName] : now)
        {
            if (library.IdOf(kind, entryName))
                continue;
            const Id id = FreshLibraryId();
            Guid g = guid(*this, &kind, entryName);
            while (library.ByGuid(g) || elements.Contains(g) || g.Pending() || g.IsNil())
                g = elements.Fresh();
            std::string ignored;
            library.Insert(id, Entry{ kind, entryName, g }, ignored);
        }
        if (!PartsInStep())
            SyncParts(guid);
    }

    void Model::ForgetParts()
    {
        const auto forget = [](auto& x)
        {
            x.id   = 0;
            x.guid = Guid();
        };
        for (auto& [_, t] : types.wall)
            for (Layer& l : t.layers)
                forget(l);
        for (auto& [_, t] : types.floor)
            for (Layer& l : t.layers)
                forget(l);
        for (auto& [_, t] : types.roof)
            for (Layer& l : t.layers)
                forget(l);
        for (auto& [_, t] : types.ceiling)
            for (Layer& l : t.layers)
                forget(l);
        for (auto& [_, f] : families)
        {
            for (Fam::Form& x : f.forms)
                forget(x);
            for (Fam::Symbol& x : f.symbols)
                forget(x);
        }
        std::vector<Id> sketched;
        for (const auto& [id, e] : elements.Items())
            if (const auto* lines = e->SketchOf(); lines && !lines->empty())
                sketched.push_back(id);
        for (const Id id : sketched)
            for (SketchLine& l : *elements.GetMut(id)->SketchOf())
                forget(l);
        library.m_parts.clear();
    }

    Id Model::FreshLibraryId()
    {
        const Id below = library.LowestId() ? *library.LowestId() - 1 : LastId;
        const Id floor = std::max(elements.LastId() ? *elements.LastId() + 1 : FirstId, nextId);
        Id id = below;
        while (id >= floor && (elements.Contains(id) || library.Taken(id)))
            --id;
        if (id < floor)
            return FreshId();
        return id;
    }

    void Model::LibRefsMut(const std::function<void(const Kind&, std::string&)>& f)
    {
        std::vector<Id> ids;
        for (const auto& [id, _] : elements.Items())
            ids.push_back(id);
        for (const Id id : ids)
        {
            // 先看有没有要改的，免得把与撤销快照共用的元素都复制一遍
            Element copy = *elements.Get(id);
            bool changed = false;
            copy.LibRefsMut([&](const Kind& k, std::string& text)
            {
                const std::string before = text;
                f(k, text);
                changed |= text != before;
            });
            if (changed)
                *elements.GetMut(id) = std::move(copy);
        }
        // 线样式的图案、填充区域类型的前景与背景图案
        for (auto& [_, s] : lineStyles)
            if (s.pattern)
                f(Kind::Of(Kind::Tag::LinePattern), *s.pattern);
        for (auto& [_, t] : regionTypes)
            for (std::optional<std::string>* p : { &t.foreground, &t.background })
                if (*p)
                    f(Kind::Of(Kind::Tag::FillPattern), **p);
        const Kind material = Kind::Of(Kind::Tag::Material);
        for (auto& [_, x] : types.wall)
            for (Layer& l : x.layers)
                if (!l.material.empty())
                    f(material, l.material);
        for (auto& [_, x] : types.floor)
            for (Layer& l : x.layers)
                if (!l.material.empty())
                    f(material, l.material);
        for (auto& [_, x] : types.roof)
            for (Layer& l : x.layers)
                if (!l.material.empty())
                    f(material, l.material);
        for (auto& [_, x] : types.ceiling)
            for (Layer& l : x.layers)
                if (!l.material.empty())
                    f(material, l.material);
        for (auto& [cat, map] : types.runs)
            for (auto& [n, r] : map)
                if (!r.material.empty())
                    f(material, r.material);
        for (auto& [_, fam] : families)
            fam.MaterialRefsMut([&](std::string& m) { f(material, m); });
    }

    std::optional<std::string> Model::LibraryName(const Kind& kind, std::string_view text) const
    {
        if (library.IdOf(kind, text))
            return std::string(text);
        const auto g = Guid::Parse(text);
        if (!g)
            return std::nullopt;
        const auto e = library.ByGuid(*g);
        if (!e || !(e->second->kind == kind))
            return std::nullopt;
        return e->second->name;
    }

    void Model::NamesFromFile()
    {
        const Library lib = library;
        LibRefsMut([&](const Kind& kind, std::string& text)
        {
            const auto g = Guid::Parse(text);
            if (!g || g->Pending())
                return;
            const auto e = lib.ByGuid(*g);
            if (e && e->second->kind == kind)
                text = e->second->name;
        });
        const auto name = [&](const Kind& kind, const std::string& text) -> std::optional<std::string>
        {
            const auto g = Guid::Parse(text);
            if (!g || g->Pending())
                return std::nullopt;
            const auto e = lib.ByGuid(*g);
            if (!e || !(e->second->kind == kind))
                return std::nullopt;
            return e->second->name;
        };
        RekeyFamilyParams(
            [&](const std::string& family, const std::string& k) -> std::optional<std::string>
            {
                const auto n = name(Kind::Of(Kind::Tag::FamilyParameter), k);
                const std::string prefix = family + ": ";
                if (!n || n->rfind(prefix, 0) != 0)
                    return std::nullopt;
                return n->substr(prefix.size());
            },
            [&](const std::string& m) { return name(Kind::Of(Kind::Tag::Material), m); });
        SyncLibrary();
    }
}
