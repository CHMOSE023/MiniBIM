// 移植自 BIMCraft crates/core/src/insert.rs 与 link.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Insert.h"
#include "Commands/Commands.h"
#include "Core/Log.h"
#include "Model/Language.h"
#include "Model/Schedule.h"
#include <cmath>
#include <set>
#include <unordered_map>

namespace MiniBIM::Bim
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        // 视图元素的图形（按元素的覆盖指着别的元素）
        const Graphics* GraphicsOf(const Element& e)
        {
            if (const Level* l = e.As<Level>())
                return &l->graphics;
            if (const PlanView* v = e.As<PlanView>())
                return &v->graphics;
            if (const View3D* v = e.As<View3D>())
                return &v->graphics;
            if (const Section* v = e.As<Section>())
                return &v->graphics;
            if (const Elevation* v = e.As<Elevation>())
                return &v->graphics;
            return nullptr;
        }

        Graphics* GraphicsOf(Element& e) { return const_cast<Graphics*>(GraphicsOf(static_cast<const Element&>(e))); }

        // e 少不了的：它指着的，但它的视图图形覆盖的元素不算（那些留下）
        std::vector<Guid> Needs(const Element& e)
        {
            std::set<Guid> overridden;
            if (const Graphics* g = GraphicsOf(e))
                for (const auto& [k, _] : g->elements)
                    overridden.insert(k);
            std::vector<Guid> out;
            for (const Guid& g : e.Refs())
                if (!overridden.count(g))
                    out.push_back(g);
            return out;
        }

        // 库条目在这里叫什么（与那边不同时）：（种类、那边的名称）→ 这里的名称
        using Renames = std::map<std::pair<Kind, std::string>, std::string>;

        // from 的条目 kind name 照原样放进 to：是不是放了一个
        bool CopyDefinition(Model& to, const Model& from, const Kind& kind, const std::string& name)
        {
            const auto one = [&](auto& into, const auto& of)
            {
                const auto it = of.find(name);
                if (it == of.end())
                    return false;
                into[name] = it->second;
                return true;
            };
            Types& t = to.types;
            const Types& f = from.types;
            switch (kind.tag)
            {
            case Kind::Tag::Type:
            {
                const std::string& c = kind.category;
                if (c == "wall") return one(t.wall, f.wall);
                if (c == "floor") return one(t.floor, f.floor);
                if (c == "ceiling") return one(t.ceiling, f.ceiling);
                if (c == "roof") return one(t.roof, f.roof);
                if (c == "door") return one(t.door, f.door);
                if (c == "window") return one(t.window, f.window);
                if (c == "column") return one(t.column, f.column);
                if (c == "structuralcolumn") return one(t.structuralColumn, f.structuralColumn);
                if (c == "footing") return one(t.footing, f.footing);
                const auto runs = f.runs.find(c);
                return runs != f.runs.end() && one(t.runs[c], runs->second);
            }
            case Kind::Tag::Material:         return one(to.materials, from.materials);
            case Kind::Tag::Family:           return one(to.families, from.families);
            case Kind::Tag::Cad:              return one(to.cads, from.cads);
            case Kind::Tag::Parameter:        return one(to.parameters, from.parameters);
            case Kind::Tag::LinePattern:      return one(to.linePatterns, from.linePatterns);
            case Kind::Tag::LineStyle:        return one(to.lineStyles, from.lineStyles);
            case Kind::Tag::FillPattern:      return one(to.fillPatterns, from.fillPatterns);
            case Kind::Tag::FilledRegionType: return one(to.regionTypes, from.regionTypes);
            case Kind::Tag::TextType:         return one(to.textTypes, from.textTypes);
            default:                          return false;
            }
        }

        // from 的库条目里 to 没有的：类型、材质、族、项目参数、线型图案、线样式、填充图案、填充区域类型、文字类型带进来，各带 GUID。
        // 这个项目按 GUID 有的是它自己的，用这里的名称（带进来的东西里的名称跟着）；只按名称有的（GUID 不同）用这个项目的
        bool BringLibrary(Model& to, const Model& from, Inserted& out, Renames& renames, std::string& error)
        {
            std::vector<std::pair<Kind, std::string>> copied;
            for (const auto& [_, e] : from.library.Entries())
            {
                if (const auto here = to.library.ByGuid(e.guid))
                {
                    if (here->second->kind == e.kind && here->second->name != e.name)
                        renames[{ e.kind, e.name }] = here->second->name;
                    continue;
                }
                if (to.library.IdOf(e.kind, e.name) || !CopyDefinition(to, from, e.kind, e.name))
                    continue;
                if (!to.library.Insert(to.FreshLibraryId(), e, error))
                    return false;
                out.library.push_back(e.kind.Label() + ": " + e.name);
                copied.emplace_back(e.kind, e.name);
            }
            // 带进来的族带着它的类型与参数，各带 GUID
            std::set<std::string> came;
            for (const auto& [k, n] : copied)
                if (k.tag == Kind::Tag::Family)
                    came.insert(n);
            for (const auto& [_, e] : from.library.Entries())
            {
                if (e.kind.tag != Kind::Tag::FamilyType && e.kind.tag != Kind::Tag::FamilyParameter)
                    continue;
                const bool of = std::any_of(came.begin(), came.end(), [&](const std::string& f) { return e.name.rfind(f + ": ", 0) == 0; });
                if (of && !to.library.IdOf(e.kind, e.name) && !to.library.ByGuid(e.guid) && !to.library.Insert(to.FreshLibraryId(), e, error))
                    return false;
            }
            // 带进来的东西用的材质、线型图案、填充图案按这个项目的叫法
            const auto here = [&](Kind::Tag tag, std::string& name)
            {
                if (const auto it = renames.find({ Kind::Of(tag), name }); it != renames.end())
                    name = it->second;
            };
            for (const auto& [kind, name] : copied)
            {
                switch (kind.tag)
                {
                case Kind::Tag::Family:
                    if (const auto f = to.families.find(name); f != to.families.end())
                        f->second.MaterialRefsMut([&](std::string& m) { here(Kind::Tag::Material, m); });
                    break;
                case Kind::Tag::LineStyle:
                    if (const auto s = to.lineStyles.find(name); s != to.lineStyles.end() && s->second.pattern)
                        here(Kind::Tag::LinePattern, *s->second.pattern);
                    break;
                case Kind::Tag::FilledRegionType:
                    if (const auto r = to.regionTypes.find(name); r != to.regionTypes.end())
                        for (std::optional<std::string>* p : { &r->second.foreground, &r->second.background })
                            if (*p)
                                here(Kind::Tag::FillPattern, **p);
                    break;
                case Kind::Tag::Type:
                {
                    Types& t = to.types;
                    std::vector<Layer>* layers = nullptr;
                    if (kind.category == "wall" && t.wall.count(name))
                        layers = &t.wall[name].layers;
                    else if (kind.category == "floor" && t.floor.count(name))
                        layers = &t.floor[name].layers;
                    else if (kind.category == "roof" && t.roof.count(name))
                        layers = &t.roof[name].layers;
                    else if (kind.category == "ceiling" && t.ceiling.count(name))
                        layers = &t.ceiling[name].layers;
                    if (layers)
                        for (Layer& l : *layers)
                            here(Kind::Tag::Material, l.material);
                    if (const auto r = t.runs.find(kind.category); r != t.runs.end())
                        if (const auto rt = r->second.find(name); rt != r->second.end())
                            here(Kind::Tag::Material, rt->second.material);
                    break;
                }
                default:
                    break;
                }
            }
            return true;
        }

        // 不能重的名称与编号，已经用了的：标高与视图的名称、轴网的、图纸的编号
        struct Names
        {
            std::set<std::string> views, grids, sheets;

            static Names Of(const Model& m)
            {
                Names n;
                for (const auto& [id, e] : m.elements.Items())
                {
                    if (const Grid* g = e->As<Grid>())
                        n.grids.insert(g->name);
                    else if (const Sheet* sh = e->As<Sheet>())
                        n.sheets.insert(sh->number);
                    else if (const std::string* v = m.ViewName(m.GuidOf(id)))
                        n.views.insert(*v);
                }
                // 内置的明细表还没成为元素之前就是视图
                for (const Schedules::Builtin& b : Schedules::Views)
                    n.views.insert(LanguageOf::Word(m.language, b.name));
                return n;
            }

            // e 的名称或编号避开已用的（已用时在后面加 2、3……），再记下它
            void Free(Element& e)
            {
                std::string* name = nullptr;
                std::set<std::string>* taken = &views;
                if (Level* l = e.As<Level>())
                    name = &l->name;
                else if (Section* v = e.As<Section>())
                    name = &v->name;
                else if (Elevation* v = e.As<Elevation>())
                    name = &v->name;
                else if (View3D* v = e.As<View3D>())
                    name = &v->name;
                else if (Schedule* sc = e.As<Schedule>())
                    name = &sc->name;
                else if (Grid* g = e.As<Grid>())
                    name = &g->name, taken = &grids;
                else if (Sheet* sh = e.As<Sheet>())
                    name = &sh->number, taken = &sheets;
                if (!name)
                    return;
                if (taken->count(*name))
                {
                    const std::string base = *name;
                    for (int i = 2;; ++i)
                        if (!taken->count(base + " " + std::to_string(i)))
                        {
                            *name = base + " " + std::to_string(i);
                            break;
                        }
                }
                taken->insert(*name);
            }
        };

        // 各视图按元素的覆盖里这个项目没有的元素去掉：插进来的视图覆盖了、没带进来的
        void TidyGraphics(Model& m)
        {
            std::set<Guid> known;
            for (const auto& [id, _] : m.elements.Items())
                if (const auto g = m.elements.GuidOf(id))
                    known.insert(*g);
            std::vector<Id> views;
            for (const auto& [id, e] : m.elements.Items())
                if (const Graphics* g = GraphicsOf(*e))
                    if (std::any_of(g->elements.begin(), g->elements.end(), [&](const auto& kv) { return !known.count(kv.first); }))
                        views.push_back(id);
            for (const Id id : views)
                if (Graphics* g = GraphicsOf(*m.elements.GetMut(id)))
                    std::erase_if(g->elements, [&](const auto& kv) { return !known.count(kv.first); });
        }
    }

    bool Insert(Model& to, const Model& from, const std::vector<Id>& ids, bool linked, Inserted& out, std::string& error)
    {
        if (linked)
            to.nextId = std::max(to.nextId, Linked);
        std::vector<Id> asked = ids;
        if (asked.empty())
            for (const auto& [id, _] : from.elements.Items())
                asked.push_back(id);
        for (const Id id : asked)
            if (!from.elements.Contains(id))
            {
                error = "the other project has no element " + std::to_string(id);
                return false;
            }
        // 这个项目有同名标高时，那边没被要的标高换成它
        std::unordered_map<std::string, Guid> levelsHere;
        for (const auto& [id, l] : to.Levels())
            levelsHere.emplace(l->name, to.GuidOf(id));
        std::map<Guid, Guid> instead;
        // 要的，以及它们指着、这个项目没有的，如此往下
        std::set<Id> take(asked.begin(), asked.end());
        std::vector<Id> todo = asked;
        while (!todo.empty())
        {
            const Id id = todo.back();
            todo.pop_back();
            const Element* e = from.elements.Get(id);
            if (!e)
                continue;
            for (const Guid& g : Needs(*e))
            {
                const auto rid = from.elements.IdOf(g);
                if (!rid || take.count(*rid) || to.elements.Contains(g))
                    continue;
                if (const Level* l = from.elements.Get(*rid)->As<Level>())
                    if (const auto here = levelsHere.find(l->name); here != levelsHere.end())
                    {
                        instead[g] = here->second;
                        continue;
                    }
                take.insert(*rid);
                todo.push_back(*rid);
            }
        }
        // 这个项目已有 GUID 的元素是副本：自己的 GUID
        for (const Id id : take)
        {
            const auto g = from.elements.GuidOf(id);
            if (!g || !to.elements.Contains(*g))
                continue;
            Guid fresh = to.elements.Fresh();
            while (std::any_of(instead.begin(), instead.end(), [&](const auto& kv) { return kv.second == fresh; }))
                fresh = to.elements.Fresh();
            instead[*g] = fresh;
        }
        Renames renames;
        if (!take.empty() && !BringLibrary(to, from, out, renames, error))
            return false;
        Names names = Names::Of(to);
        for (const Id id : take)
        {
            const Element* src = from.elements.Get(id);
            const auto g = from.elements.GuidOf(id);
            if (!src || !g)
                continue;
            Element e = *src;
            e.RefsMut([&](Guid& r)
            {
                if (const auto n = instead.find(r); n != instead.end())
                    r = n->second;
            });
            names.Free(e);
            e.LibRefsMut([&](const Kind& kind, std::string& name)
            {
                if (const auto n = renames.find({ kind, name }); n != renames.end())
                    name = n->second;
            });
            const bool taken = to.elements.Contains(id) || to.library.Taken(id);
            const Id now = linked || taken || id < FirstId ? to.FreshId() : id;
            const auto i = instead.find(*g);
            const Guid guid = i != instead.end() ? i->second : *g;
            if (!to.elements.InsertAs(now, guid, std::move(e), error))
                return false;
            to.nextId = std::max(to.nextId, now + 1);
            if (guid != *g)
                out.copies.push_back(now);
            out.ids[id] = now;
        }
        TidyGraphics(to);
        return true;
    }

    namespace Links
    {
        namespace
        {
            // 链接的项目里画出来的：造出来的东西。它的视图、图纸、明细表与注释留在它那里，它自己的链接也是
            bool Built(const Element& e)
            {
                return !(e.As<Grid>() || e.As<Room>() || e.As<Dimension>() || e.As<Tag>() || e.As<Section>() || e.As<Elevation>() || e.As<Sheet>() ||
                         e.As<Viewport>() || e.As<Note>() || e.As<DetailLine>() || e.As<FilledRegion>() || e.As<Spot>() || e.As<Schedule>() ||
                         e.As<View3D>() || e.As<PlanView>() || e.As<Link>());
            }

            // 链接 l 造的东西放进 to：照链接说的转过、挪过，同高的站在 to 的标高上
            bool Bring(Model& to, const Link& l, const Model& from, std::string& error)
            {
                Model other = from;
                std::vector<Id> drop;
                for (const auto& [id, e] : other.elements.Items())
                    if (!e->As<Level>() && !Built(*e))
                        drop.push_back(id);
                for (const Id id : drop)
                    other.elements.Remove(id);
                std::set<Id> all;
                for (const auto& [id, _] : other.elements.Items())
                    all.insert(id);
                // 绕它的原点转，再挪到它站的地方
                const double a = l.angle * kPi / 180.0, s = std::sin(a), c = std::cos(a);
                const std::function<P2(P2)> map = [&](P2 p) { return P2(p.x * c - p.y * s + l.at.x, p.x * s + p.y * c + l.at.y); };
                for (const Id id : all)
                {
                    if (other.elements.Get(id)->As<Level>())
                        continue;
                    Element moved;
                    if (!Moved(other, id, map, all, moved, error))
                        return false;
                    other.elements.Insert(id, std::move(moved));
                }
                // 站在这个项目某个标高的高度上的就是那个标高；别的是链接自己的
                std::set<Id> matched;
                const auto here = to.Levels();
                for (const Id id : all)
                {
                    const Element* e = other.elements.Get(id);
                    if (!e->As<Level>())
                        continue;
                    Level lv = *e->As<Level>();
                    const auto same = std::find_if(here.begin(), here.end(), [&](const auto& t) { return std::abs(t.second->elevation - lv.elevation) < SameLevel; });
                    if (same != here.end())
                    {
                        lv.name = same->second->name;
                        matched.insert(id);
                    }
                    else
                        lv.name = l.name + ": " + lv.name;
                    other.elements.Insert(id, Element(std::move(lv)));
                }
                std::vector<Id> ask;
                for (const Id id : all)
                    if (!matched.count(id))
                        ask.push_back(id);
                if (ask.empty())
                    return true;
                Inserted made;
                return Insert(to, other, ask, true, made, error);
            }
        }

        std::shared_ptr<const Model> Seen(const std::shared_ptr<const Model>& m)
        {
            std::vector<std::pair<const Link*, const Model*>> links;
            for (const auto& [_, e] : m->elements.Items())
                if (const Link* l = e->As<Link>())
                    if (const auto it = m->loaded.find(l->path); it != m->loaded.end() && it->second)
                        links.emplace_back(l, it->second.get());
            if (links.empty())
                return m;
            auto out = std::make_shared<Model>(*m);
            for (const auto& [l, other] : links)
            {
                // 带不进来的链接（文件里的东西与这个项目的冲突）当作不在；reload 的结果说哪些链接读到了
                Model before = *out;
                std::string why;
                if (!Bring(*out, *l, *other, why))
                {
                    LOG_WARN("链接 %s 带不进来：%s", l->name.c_str(), why.c_str());
                    *out = std::move(before);
                }
            }
            out->Resolve();
            return out;
        }
    }
}
