// 移植自 BIMCraft crates/core/src/massing.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Massing.h"
#include "Model/Geom.h"
#include "Model/ModelJson.h"
#include <algorithm>
#include <cmath>

namespace MiniBIM::Bim::Massing
{
    namespace
    {
        // 记号在 key（mass 或 level）下的 GUID：写成 GUID 的，或命令给的 ID
        std::optional<Guid> Mark(const Json& o, const char* key)
        {
            if (!o.is_object() || !o.contains(key))
                return std::nullopt;
            Guid g;
            std::string error;
            if (!FromJson(o[key], g, error))
                return std::nullopt;
            return g;
        }

        constexpr const char* Keys[] = { FloorMark, Face };

        // 墙记着的面：边的编号，不小于 0 的整数（JSON 里可能是有符号的）
        std::optional<size_t> Edge(const Json& f)
        {
            if (f.is_number_unsigned())
                return f.get<size_t>();
            if (f.is_number_integer() && f.get<int64_t>() >= 0)
                return static_cast<size_t>(f.get<int64_t>());
            return std::nullopt;
        }
    }

    std::optional<std::pair<Id, size_t>> FaceAt(const Model& m, P2 p, double reach)
    {
        std::optional<std::tuple<double, Id, size_t>> best;
        for (const auto& [id, e] : m.elements.Items())
        {
            const Mass* ms = e->As<Mass>();
            if (!ms)
                continue;
            const Ring ring = ms->Footprint();
            for (size_t i = 0; i < ring.size(); ++i)
            {
                const double d = Geom::DistToSegment(p, ring[i], ring[(i + 1) % ring.size()]);
                if (d <= reach && (!best || d < std::get<0>(*best)))
                    best = { d, id, i };
            }
        }
        if (!best)
            return std::nullopt;
        return std::pair{ std::get<1>(*best), std::get<2>(*best) };
    }

    bool Reserved(std::string_view name)
    {
        return name == FloorMark || name == Face;
    }

    void RefsMut(std::map<std::string, Json>& params, const std::function<void(Guid&)>& f)
    {
        for (const char* key : Keys)
        {
            const auto it = params.find(key);
            if (it == params.end() || !it->second.is_object())
                continue;
            for (const char* k : { "mass", "level" })
                if (auto g = Mark(it->second, k))
                {
                    f(*g);
                    it->second[k] = ToJson(*g);
                }
        }
    }

    std::vector<Guid> Refs(const std::map<std::string, Json>& params)
    {
        std::vector<Guid> out;
        for (const char* key : Keys)
            if (const auto it = params.find(key); it != params.end())
                for (const char* k : { "mass", "level" })
                    if (const auto g = Mark(it->second, k))
                        out.push_back(*g);
        return out;
    }

    bool CheckValue(const Model& m, const std::string& who, const std::string& key, const Json& v, const Element& e, std::string& error)
    {
        const bool floor       = key == FloorMark;
        const std::string shape = floor ? "{mass, level}" : "{mass, face}";
        const auto wrong = [&]
        {
            error = who + ": `" + key + "` is " + shape + ", not " + v.dump();
            return false;
        };
        if (!v.is_object() || v.size() != 2)
            return wrong();
        const auto mass = Mark(v, "mass");
        if (!mass)
            return wrong();
        const Element* me = m.elements.Get(*mass);
        if (!me || !me->As<Mass>())
        {
            error = who + ": `" + key + "` names " + m.Shown(*mass) + ", which isn't a mass";
            return false;
        }
        if (floor)
        {
            if (!e.As<Floor>())
            {
                error = who + ": only a floor holds `" + key + "`";
                return false;
            }
            const auto level = Mark(v, "level");
            if (!level)
                return wrong();
            if (!m.GetLevel(*level))
            {
                error = who + ": `" + key + "` names " + m.Shown(*level) + ", which isn't a level";
                return false;
            }
            return true;
        }
        const Json* f = v.contains("face") ? &v["face"] : nullptr;
        if ((e.As<Wall>() && f && Edge(*f)) || ((e.As<Floor>() || e.As<Roof>()) && f && *f == "top"))
            return true;
        error = who + ": `" + key + "` is " + shape + ": a wall's face is an edge number, a floor's or a roof's is \"top\", not " + v.dump();
        return false;
    }

    std::optional<std::pair<Guid, std::optional<size_t>>> FaceOf(const Element& e)
    {
        const auto* params = e.Params();
        if (!params)
            return std::nullopt;
        const auto it = params->find(Face);
        if (it == params->end() || !it->second.is_object() || !it->second.contains("face"))
            return std::nullopt;
        const auto mass = Mark(it->second, "mass");
        if (!mass)
            return std::nullopt;
        const Json& f = it->second["face"];
        return std::pair{ *mass, Edge(f) };
    }

    std::vector<std::pair<Id, Guid>> FloorsOf(const Model& m, Guid mass)
    {
        std::vector<std::pair<Id, Guid>> out;
        for (const auto& [id, e] : m.elements.Items())
        {
            const Floor* f = e->As<Floor>();
            if (!f)
                continue;
            const auto it = f->params.find(FloorMark);
            if (it == f->params.end())
                continue;
            const auto ms = Mark(it->second, "mass"), level = Mark(it->second, "level");
            if (ms && level && m.Same(*ms, mass) && m.Same(*level, f->level))
                out.emplace_back(id, f->level);
        }
        return out;
    }

    void Detach(std::map<std::string, Json>& params)
    {
        params.erase(FloorMark);
        params.erase(Face);
    }

    void Forget(Model& m, const std::set<Guid>& gone)
    {
        std::vector<Id> ids;
        for (const auto& [id, e] : m.elements.Items())
            if (e->Params() && (e->Params()->count(FloorMark) || e->Params()->count(Face)))
                ids.push_back(id);
        for (const Id id : ids)
        {
            auto& params = *m.elements.GetMut(id)->ParamsMut();
            for (const char* key : Keys)
                if (const auto it = params.find(key); it != params.end())
                    if (const auto mass = Mark(it->second, "mass");
                        mass && std::any_of(gone.begin(), gone.end(), [&](const Guid& g) { return m.Same(g, *mass); }))
                        params.erase(it);
        }
    }

    bool Follow(Element& e, const Mass& ms, Id mass, std::optional<size_t> face, std::string& why)
    {
        const Ring ring = ms.Footprint();
        const std::string of = "mass " + std::to_string(mass);
        Wall* w = e.As<Wall>();
        if (w && face)
        {
            const size_t n = ring.size(), f = *face;
            if (f >= n)
            {
                why = "was made on face " + std::to_string(f) + " of " + of + ", which has " + std::to_string(n) + " faces now (0 to " +
                      std::to_string(n == 0 ? 0 : n - 1) + ")";
                return false;
            }
            if (!Plumb(ring, ms.TopRing(), f))
            {
                why = "was made on face " + std::to_string(f) + " of " + of + ", which leans now: a wall stands plumb";
                return false;
            }
            // 逆时针时体量在边的左边；墙的外侧在它的左边，所以反过来走
            w->start      = ring[(f + 1) % n];
            w->end        = ring[f];
            w->level      = ms.level;
            w->baseOffset = ms.baseOffset;
            w->topLevel   = ms.topLevel;
            if (!ms.topLevel)
                w->height = ms.height;
            return true;
        }
        const std::pair<Guid, double> top = ms.topLevel ? std::pair{ *ms.topLevel, 0.0 } : std::pair{ ms.level, ms.baseOffset + ms.height };
        if (Floor* fl = e.As<Floor>(); fl && !face)
        {
            fl->boundary = ms.TopRing();
            fl->level    = top.first;
            fl->offset   = top.second;
            return true;
        }
        if (Roof* r = e.As<Roof>(); r && !face)
        {
            r->boundary = ms.TopRing();
            r->level    = top.first;
            r->offset   = top.second;
            return true;
        }
        why = std::string("is a ") + e.Category() + ", which holds no such face of " + of;
        return false;
    }

    void Mirrored(Model& m, Id mass)
    {
        const Element* me = m.elements.Get(mass);
        const Mass* ms = me ? me->As<Mass>() : nullptr;
        if (!ms)
            return;
        const Guid guid = m.GuidOf(mass);
        const size_t n = ms->boundary.size();
        std::vector<Id> ids;
        for (const auto& [id, e] : m.elements.Items())
            if (e->Params() && e->Params()->count(Face))
                ids.push_back(id);
        for (const Id id : ids)
        {
            Json& o = m.elements.GetMut(id)->ParamsMut()->at(Face);
            const auto of = Mark(o, "mass");
            if (!of || !m.Same(*of, guid) || !o.contains("face") || !Edge(o["face"]))
                continue;
            const size_t f = *Edge(o["face"]);
            if (f < n)
                o["face"] = (2 * n - 2 - f) % n;
        }
    }

    Ring RingAt(const Ring& bottom, const Ring& top, double t)
    {
        Ring out;
        for (size_t i = 0; i < std::min(bottom.size(), top.size()); ++i)
            out.push_back(bottom[i] + (top[i] - bottom[i]) * t);
        return out;
    }

    bool Plumb(const Ring& bottom, const Ring& top, size_t f)
    {
        const size_t n = bottom.size();
        if (n == 0)
            return false;
        for (const size_t i : { f, (f + 1) % n })
            if (i >= bottom.size() || i >= top.size() || bottom[i].Dist(top[i]) > 1e-6)
                return false;
        return true;
    }

    // 扭的侧面沿高度切成几条。侧面的两条边平行时是平的（斜的、收分的体量），两个三角形就是它；扭的是曲面，每条两个三角形略在它里面。
    // 简化：误差随条数成反比（转 45° 时体积差不到 2%）；放样要更细时调大
    constexpr size_t Strips = 16;

    std::vector<Tri> Tris(const Ring& bottom, const Ring& top, double z0, double z1)
    {
        const auto pt = [](const Ring& ring, size_t i, double z) -> P3
        {
            const P2 p = ring.empty() ? P2{} : ring[i % ring.size()];
            return { p.x, p.y, z };
        };
        std::vector<Tri> out;
        for (const auto& [a, b, c] : Geom::Triangulate(top))
            out.push_back({ pt(top, a, z1), pt(top, b, z1), pt(top, c, z1) });
        for (const auto& [a, b, c] : Geom::Triangulate(bottom))
            out.push_back({ pt(bottom, a, z0), pt(bottom, c, z0), pt(bottom, b, z0) });
        const size_t n      = std::min(bottom.size(), top.size());
        const size_t strips = std::clamp<size_t>(20000 / std::max<size_t>(n, 1), 1, Strips);
        for (size_t i = 0; i < n; ++i)
        {
            const P2 b0 = bottom[i], b1 = bottom[(i + 1) % n], t0 = top[i], t1 = top[(i + 1) % n];
            const bool flat = std::abs((b1 - b0).Cross(t1 - t0)) <= 1e-9 * (b1 - b0).Length() * (t1 - t0).Length() + 1e-9;
            const size_t k  = flat ? 1 : strips;
            // 侧面的角点在 f 高处
            const auto at = [&](double f)
            { return std::tuple{ b0 + (t0 - b0) * f, b1 + (t1 - b1) * f, z0 + (z1 - z0) * f }; };
            for (size_t j = 0; j < k; ++j)
            {
                const auto [l0, l1, zl] = at(static_cast<double>(j) / static_cast<double>(k));
                const auto [h0, h1, zh] = at(static_cast<double>(j + 1) / static_cast<double>(k));
                out.push_back({ P3{ l0.x, l0.y, zl }, P3{ l1.x, l1.y, zl }, P3{ h1.x, h1.y, zh } });
                out.push_back({ P3{ l0.x, l0.y, zl }, P3{ h1.x, h1.y, zh }, P3{ h0.x, h0.y, zh } });
            }
        }
        return out;
    }

    std::optional<std::string> LoftProblem(const Ring& bottom, const Ring& top)
    {
        if (top.size() != bottom.size())
            return "its top outline has " + std::to_string(top.size()) + " points and its footprint " + std::to_string(bottom.size()) +
                   "; they pair up one for one";
        const double base = Geom::Area(bottom);
        if (Geom::Area(top) * base <= 0.0)
            return std::string("its top outline runs the other way round from its footprint");
        // 面积是高度的二次式，十六步看得出它缩没
        for (int k = 1; k < 16; ++k)
            if (Geom::Area(RingAt(bottom, top, k / 16.0)) * base <= Tol * Tol)
                return "lofted between its outlines it collapses " + std::to_string(k) + "/16 of the way up";
        return std::nullopt;
    }
}
