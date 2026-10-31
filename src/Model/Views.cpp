// 移植自 BIMCraft crates/core/src/model.rs 与 graphics.rs（f3bbb33）的视图部分，MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Model.h"

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
        if (const Level* l = e->As<Level>())
            return l->viewRange;
        if (const PlanView* v = e->As<PlanView>())
            return v->viewRange;
        return ViewRange{};
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
        return g ? g->ScaleN(units) : UnitsOf::DefaultScale(units);
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
        return nullptr;
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
        Look look;
        const Graphics* g = ViewGraphics(view);
        if (!g)
            return look;
        look.style = g->style;
        if (g->categories.empty() && g->elements.empty())
            return look;
        for (const auto& [id, e] : elements.Items())
        {
            const char* cat = e->Category();
            if (!GraphicsOf::Drawn(cat))
                continue;
            // 类别的，再是元素自己的
            const auto c = g->categories.find(cat);
            Override over = c == g->categories.end() ? Override{} : c->second;
            if (const auto own = g->elements.find(GuidOf(id)); own != g->elements.end())
                over = own->second.Over(over);
            if (over.hidden)
                look.hidden.insert(id);
            else if (!over.Empty())
                look.paint.emplace(id, std::move(over));
        }
        return look;
    }

    Look Model::LookOf3D() const
    {
        const auto id = Default3D();
        return id ? LookOf(GuidOf(*id)) : Look{};
    }
}
