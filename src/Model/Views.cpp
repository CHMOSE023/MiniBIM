// 移植自 BIMCraft crates/core/src/model.rs 与 graphics.rs（f3bbb33）的视图部分，MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Insert.h"
#include "Model/Language.h"
#include "Model/Model.h"
#include "Model/Text.h"
#include "Model/Walls.h"

namespace MiniBIM::Bim
{
    std::optional<Guid> Model::PlanLevel(Guid key) const
    {
        const Element* e = elements.Get(key);
        if (!e)
            return std::nullopt;
        if (e->As<Level>())
            return GuidOf(key);
        if (const PlanView* v = e->As<PlanView>(); v && GetLevel(v->level))
            return GuidOf(v->level);
        return std::nullopt;
    }

    const PlanView* Model::GetPlanView(Guid k) const
    {
        const Element* e = elements.Get(k);
        return e ? e->As<PlanView>() : nullptr;
    }

    std::vector<std::pair<Id, const PlanView*>> Model::PlanViews(Guid level) const
    {
        std::vector<std::pair<Id, const PlanView*>> out;
        for (const auto& [id, e] : elements.Items())
            if (const PlanView* v = e->As<PlanView>(); v && Same(v->level, level))
                out.emplace_back(id, v);
        return out;
    }

    Guid Model::PlanSource(Guid key) const
    {
        const PlanView* v = GetPlanView(key);
        return v && v->parent ? GuidOf(*v->parent) : GuidOf(key);
    }

    std::optional<std::array<P2, 2>> Model::CropOf(Guid key) const
    {
        const Element* e = elements.Get(key);
        if (!e)
            return std::nullopt;
        if (const Level* l = e->As<Level>())
            return l->crop;
        if (const PlanView* v = e->As<PlanView>())
            return v->crop;
        return std::nullopt;
    }

    ViewRange Model::ViewRangeOf(Guid key) const
    {
        const Element* e = elements.Get(PlanSource(key));
        if (!e)
            return ViewRange{};
        const ViewRange* range = nullptr;
        const Graphics*  g     = nullptr;
        if (const Level* l = e->As<Level>())
            range = &l->viewRange, g = &l->graphics;
        else if (const PlanView* v = e->As<PlanView>())
            range = &v->viewRange, g = &v->graphics;
        else
            return ViewRange{};
        // 跟随的样板控制视图范围时取样板的
        if (g->ControlledBy(*this, Group::Range))
            if (const auto& t = viewTemplates.at(*g->templateName); t.viewRange)
                return *t.viewRange;
        return *range;
    }

    namespace
    {
        template <class E> auto* GraphicsIn(E* e)
        {
            using G = std::conditional_t<std::is_const_v<E>, const Graphics, Graphics>;
            G* g = nullptr;
            if (!e)
                return g;
            if (auto* l = e->template As<Level>())
                g = &l->graphics;
            else if (auto* v = e->template As<PlanView>())
                g = &v->graphics;
            else if (auto* v3 = e->template As<View3D>())
                g = &v3->graphics;
            else if (auto* s = e->template As<Section>())
                g = &s->graphics;
            else if (auto* ev = e->template As<Elevation>())
                g = &ev->graphics;
            return g;
        }
    }

    const Graphics* Model::ViewGraphics(Guid view) const
    {
        return GraphicsIn(elements.Get(PlanSource(view)));
    }

    Graphics* Model::ViewGraphicsMut(Guid view)
    {
        return GraphicsIn(elements.GetMut(GuidOf(view)));
    }

    double Model::ScaleOf(Guid view) const
    {
        const Graphics* g = ViewGraphics(view);
        return g ? g->Resolved(*this).ScaleN(units) : UnitsOf::DefaultScale(units);
    }

    const std::string* Model::ViewName(Guid id) const
    {
        const Element* e = elements.Get(id);
        if (!e)
            return nullptr;
        if (const Level* l = e->As<Level>())
            return &l->name;
        if (const PlanView* v = e->As<PlanView>())
            return &v->name;
        if (const View3D* v3 = e->As<View3D>())
            return &v3->name;
        if (const Schedule* sc = e->As<Schedule>())
            return &sc->name;
        if (const Section* s = GetSection(id))
            return &s->name;
        return nullptr;
    }

    const Section* Model::GetSection(Guid key) const
    {
        const Element* e = elements.Get(key);
        if (!e)
            return nullptr;
        if (const Section* s = e->As<Section>())
            return s;
        return e->As<Bim::Elevation>();
    }

    const Sheet* Model::GetSheet(Guid key) const
    {
        const Element* e = elements.Get(key);
        return e ? e->As<Sheet>() : nullptr;
    }

    std::optional<Id> Model::ViewportOf(Guid view) const
    {
        for (const auto& [id, e] : elements.Items())
            if (const Viewport* v = e->As<Viewport>(); v && Same(v->view, view))
                return id;
        return std::nullopt;
    }

    std::optional<P2> PaperOf(std::string_view size)
    {
        const std::string s = Text::AsciiLower(Text::Trim(std::string(size)));
        if (s == "a0") return P2{ 1189.0, 841.0 };
        if (s == "a1") return P2{ 841.0, 594.0 };
        if (s == "a2") return P2{ 594.0, 420.0 };
        if (s == "a3") return P2{ 420.0, 297.0 };
        if (s == "a4") return P2{ 297.0, 210.0 };
        return std::nullopt;
    }

    std::optional<std::array<P2, 2>> Model::BuildingBox() const
    {
        std::optional<std::array<P2, 2>> box;
        const auto add = [&](P2 p)
        {
            if (!box)
                box = std::array<P2, 2>{ p, p };
            else
                box = std::array<P2, 2>{ P2{ std::min((*box)[0].x, p.x), std::min((*box)[0].y, p.y) },
                                         P2{ std::max((*box)[1].x, p.x), std::max((*box)[1].y, p.y) } };
        };
        for (const auto& [id, e] : elements.Items())
        {
            if (e->As<Wall>())
                for (const P2& p : Walls::Outline(*this, id))
                    add(p);
            else if (const Floor* f = e->As<Floor>())
                for (const P2& p : f->boundary)
                    add(p);
        }
        return box;
    }

    std::optional<Facing> Model::FacingFrom(P2 at) const
    {
        const auto box = BuildingBox();
        if (!box)
            return std::nullopt;
        const P2 d = at - ((*box)[0] + (*box)[1]) * 0.5;
        if (std::abs(d.x) > std::abs(d.y))
            return d.x > 0.0 ? Facing::East : Facing::West;
        return d.y > 0.0 ? Facing::North : Facing::South;
    }

    std::optional<std::pair<P2, P2>> Model::ElevationLine(Facing facing, std::optional<P2> through) const
    {
        const auto box = BuildingBox();
        if (!box)
            return std::nullopt;
        const auto [lo, hi] = *box;
        const P2 middle = (lo + hi) * 0.5, out = FacingOf::Out(facing), u = out.Perp();
        const double reach = through ? (*through - middle).Dot(out) : std::abs((hi - lo).Dot(out)) / 2.0 + 3000.0;
        const double half  = std::abs((hi - lo).Dot(u)) / 2.0 + 1000.0;
        const P2 base = middle + out * reach;
        return std::pair{ base - u * half, base + u * half };
    }

    namespace FacingOf
    {
        const char* Name(Facing f)
        {
            switch (f)
            {
            case Facing::North: return "North";
            case Facing::South: return "South";
            case Facing::East:  return "East";
            case Facing::West:  return "West";
            }
            return "North";
        }

        std::optional<Facing> Parse(std::string_view s)
        {
            const std::string n = Text::AsciiLower(Text::Trim(std::string(s)));
            for (const Facing f : All)
                for (const Language l : LanguageOf::All)
                    if (Text::AsciiLower(LanguageOf::Word(l, Name(f))) == n)
                        return f;
            return std::nullopt;
        }

        P2 Out(Facing f)
        {
            switch (f)
            {
            case Facing::North: return { 0.0, 1.0 };
            case Facing::South: return { 0.0, -1.0 };
            case Facing::East:  return { 1.0, 0.0 };
            case Facing::West:  return { -1.0, 0.0 };
            }
            return { 0.0, 1.0 };
        }
    }

    std::string Model::FreeViewName(const std::string& base, bool numbered) const
    {
        std::set<std::string> taken;
        for (const auto& [id, e] : elements.Items())
            if (const std::string* n = ViewName(GuidOf(id)))
                taken.insert(*n);
        for (int n = 1;; ++n)
        {
            const std::string name = n == 1 && !numbered ? base : base + " " + std::to_string(n);
            if (!taken.count(name))
                return name;
        }
    }

    std::optional<Id> Model::Default3D() const
    {
        for (const auto& [id, e] : elements.Items())
            if (const View3D* v = e->As<View3D>(); v && v->name == View3D::DefaultName)
                return id;
        return std::nullopt;
    }

    Look Model::LookOf(Guid view) const
    {
        const Graphics* g = ViewGraphics(view);
        return g ? LookWith(*g) : Look{};
    }

    Look Model::LookWith(const Graphics& own) const
    {
        const Graphics g = own.Resolved(*this);
        Look look;
        look.style = g.style;
        look.layers = own.layers;
        // 视图画的模型里有链接造的东西（ID 最大的是链接的）时，它们都要半色调
        const bool linked = !elements.Items().empty() && Links::IsLinked(elements.Items().rbegin()->first);
        if (g.categories.empty() && g.filters.empty() && g.elements.empty() && !linked)
            return look;
        // 套用的每个过滤器，以及它挑出的元素
        std::vector<std::pair<const Applied*, std::set<Id>>> applied;
        for (const Applied& a : g.filters)
            if (const auto f = viewFilters.find(a.filter); f != viewFilters.end())
                applied.emplace_back(&a, Schedules::Picks(*this, f->second.categories, f->second.rules));
        for (const auto& [id, e] : elements.Items())
        {
            const char* cat = e->Category();
            if (!GraphicsOf::Drawn(cat))
                continue;
            // 类别的，再是各过滤器的（从最后一个到第一个，第一个的最后盖上），再是元素自己的
            const auto c = g.categories.find(cat);
            Override over = c == g.categories.end() ? Override{} : c->second;
            for (auto a = applied.rbegin(); a != applied.rend(); ++a)
                if (a->second.count(id))
                    over = a->first->over.Over(over);
            if (const auto mine = g.elements.find(GuidOf(id)); mine != g.elements.end())
                over = mine->second.Over(over);
            // 链接造的东西是参照：半色调，视图说的在上面
            if (Links::IsLinked(id))
                over.halftone = true;
            if (over.hidden)
                look.hidden.insert(id);
            else if (!over.Empty())
                look.paint.emplace(id, std::move(over));
        }
        return look;
    }

    bool Model::CheckFollowed(const Element& old, const Element& now, std::string& error) const
    {
        const Graphics* a = GraphicsIn(&old);
        const Graphics* b = GraphicsIn(&now);
        if (!a || !b || !b->templateName)
            return true;
        const auto t = viewTemplates.find(*b->templateName);
        if (t == viewTemplates.end())
            return true;
        const auto follows = [&](Group group, bool same, const char* label)
        {
            if (!t->second.controls.count(group) || same)
                return true;
            error = std::string("its ") + label + " follow view template `" + t->first + "`; change the template, or unfollow it (`template: null`)";
            return false;
        };
        const ViewRange* ra = nullptr;
        const ViewRange* rb = nullptr;
        if (const Level* x = old.As<Level>(); x && now.As<Level>())
            ra = &x->viewRange, rb = &now.As<Level>()->viewRange;
        if (const PlanView* x = old.As<PlanView>(); x && now.As<PlanView>())
            ra = &x->viewRange, rb = &now.As<PlanView>()->viewRange;
        return follows(Group::Categories, a->categories == b->categories, "categories") && follows(Group::Filters, a->filters == b->filters, "filters") &&
               follows(Group::Phase, a->phase == b->phase && a->phaseFilter == b->phaseFilter, "phase and phase filter") &&
               follows(Group::Options, a->options == b->options, "design options") && follows(Group::Scale, a->scale == b->scale, "scale") &&
               follows(Group::Detail, a->detail == b->detail, "detail level") && follows(Group::Style, a->style == b->style, "visual style") &&
               (!ra || follows(Group::Range, *ra == *rb, "view range"));
    }

    Look Model::LookOf3D() const
    {
        const auto id = Default3D();
        return id ? LookOf(GuidOf(*id)) : Look{};
    }
}
