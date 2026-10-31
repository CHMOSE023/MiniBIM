// 移植自 BIMCraft crates/core/src/cad.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Cad.h"
#include "Model/Model.h"
#include "Model/Text.h"
#include "Model/Zlib.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

namespace MiniBIM::Bim
{
    namespace
    {
        // 图元少于这么多时不算哪个离得远：稀疏的图纸全是主体
        constexpr size_t BulkFrom = 20;
        // 图元在图纸中段跨度的几倍之外算离其余的远
        constexpr double FarTimes = 20.0;

        bool EqualsIgnoreCase(std::string_view a, std::string_view b) { return a.size() == b.size() && Text::AsciiLower(a) == Text::AsciiLower(b); }

        // 图纸主体外面的框，以及几个图元离它远：在各轴上图元中点的第 2 到第 98 个百分位的跨度的 FarTimes 倍之外
        std::pair<std::optional<std::array<P2, 2>>, size_t> BulkOf(const Dxf::Drawing& d)
        {
            std::vector<std::array<P2, 2>> boxes;
            boxes.reserve(d.entities.size());
            for (const Dxf::Entity& e : d.entities)
                boxes.push_back(Dxf::Bounds(e.shape));
            if (boxes.size() < BulkFrom)
                return { Dxf::Union(boxes), 0 };
            std::vector<double> xs, ys;
            for (const auto& b : boxes)
            {
                xs.push_back((b[0].x + b[1].x) * 0.5);
                ys.push_back((b[0].y + b[1].y) * 0.5);
            }
            std::sort(xs.begin(), xs.end());
            std::sort(ys.begin(), ys.end());
            const auto at = [](const std::vector<double>& v, size_t q) { return v[(v.size() - 1) * q / 100]; };
            P2 lo(at(xs, 2), at(ys, 2)), hi(at(xs, 98), at(ys, 98));
            const double reach = std::max(std::max(hi.x - lo.x, hi.y - lo.y) * FarTimes, 1.0);
            lo = lo - P2(reach, reach);
            hi = hi + P2(reach, reach);
            const auto near = [&](const std::array<P2, 2>& b) { return b[0].x <= hi.x && b[1].x >= lo.x && b[0].y <= hi.y && b[1].y >= lo.y; };
            std::vector<std::array<P2, 2>> kept;
            size_t far = 0;
            for (const auto& b : boxes)
            {
                if (near(b))
                    kept.push_back(b);
                else
                    ++far;
            }
            return { Dxf::Union(kept), far };
        }

        // 形状离 p 多远，图纸单位：离它的线，点或文字的点；在填充里面为 0
        double Distance(const Dxf::Shape& s, P2 p)
        {
            if (const Dxf::Point* q = std::get_if<Dxf::Point>(&s))
                return q->p.Dist(p);
            if (const Dxf::Text* t = std::get_if<Dxf::Text>(&s))
                return t->at.Dist(p);
            if (const Dxf::Fill* f = std::get_if<Dxf::Fill>(&s))
            {
                size_t inside = 0;
                for (const auto& r : f->rings)
                    inside += Geom::Contains(r, p) ? 1 : 0;
                if (inside % 2 == 1)
                    return 0.0;
            }
            double best = std::numeric_limits<double>::infinity();
            for (const auto& [pts, closed] : Cads::Paths(s))
            {
                for (size_t i = 0; i + 1 < pts.size(); ++i)
                    best = std::min(best, Geom::DistToSegment(p, pts[i], pts[i + 1]));
                if (closed && pts.size() > 1)
                    best = std::min(best, Geom::DistToSegment(p, pts.back(), pts.front()));
            }
            return best;
        }
    }

    std::optional<CadColors> CadColorsFrom(std::string_view name)
    {
        const std::string n = Text::AsciiLower(Text::Trim(name));
        if (n == "preserve")
            return CadColors::Preserve;
        if (n == "invert")
            return CadColors::Invert;
        if (n == "black" || n == "black and white" || n == "black_and_white")
            return CadColors::Black;
        return std::nullopt;
    }

    const char* Name(CadColors c)
    {
        switch (c)
        {
        case CadColors::Invert: return "invert";
        case CadColors::Black: return "black";
        default: return "preserve";
        }
    }

    struct Cad::Pack
    {
        std::once_flag once;
        std::string    text;
    };

    bool Cad::Make(std::string_view source, std::optional<std::string> path, std::string bytes, Cad& out, std::string& error)
    {
        auto dxf = std::make_shared<Dxf::Drawing>();
        if (!Dxf::Read(bytes, *dxf, error))
            return false;
        if (dxf->entities.empty())
        {
            error = "it draws nothing in model space";
            return false;
        }
        Cad c;
        c.m_source = Text::TakeChars(source, MaxText);
        if (path)
            c.m_path = Text::TakeChars(*path, 4096);
        std::tie(c.m_bulk, c.m_far) = BulkOf(*dxf);
        c.m_bytes  = std::make_shared<const std::string>(std::move(bytes));
        c.m_dxf    = std::move(dxf);
        c.m_packed = std::make_shared<Pack>();
        out = std::move(c);
        return true;
    }

    bool Cad::FromFile(std::string_view source, std::optional<std::string> path, const std::string& packed, size_t most, Cad& out, std::string& error)
    {
        std::string bytes;
        if (!Zlib::Unpack("a CAD drawing", packed, std::min(most, Dxf::MaxBytes), bytes, error))
            return false;
        std::string why;
        if (!Make(source, std::move(path), std::move(bytes), out, why))
        {
            error = "CAD drawing `" + std::string(source) + "`: " + why;
            return false;
        }
        // 读进来的就是它存的样子：照原样写回
        std::call_once(out.m_packed->once, [&] { out.m_packed->text = packed; });
        return true;
    }

    Cad Cad::Linked(std::optional<std::string> path) const
    {
        Cad c = *this;
        c.m_path   = std::move(path);
        c.m_packed = std::make_shared<Pack>();
        return c;
    }

    const std::string& Cad::Packed() const
    {
        static const std::string none;
        if (!m_packed || !m_bytes)
            return none;
        std::call_once(m_packed->once, [&] { m_packed->text = Zlib::Pack(*m_bytes); });
        return m_packed->text;
    }

    std::optional<Dxf::Rgb> Cad::ColorOf(const Dxf::Entity& e, CadColors colors) const
    {
        Dxf::Color own = e.color;
        if (own.kind == Dxf::Color::Kind::ByLayer)
        {
            const Dxf::Layer* l = m_dxf->LayerOf(e.layer);
            own = l ? l->color : Dxf::Color::Indexed(7);
        }
        const auto rgb = own.ToRgb();
        if (!rgb)
            return std::nullopt;
        switch (colors)
        {
        case CadColors::Preserve: return rgb;
        case CadColors::Invert: return Dxf::Rgb{ static_cast<uint8_t>(255 - rgb->r), static_cast<uint8_t>(255 - rgb->g), static_cast<uint8_t>(255 - rgb->b) };
        default: return std::nullopt;
        }
    }

    std::optional<double> Cad::WeightOf(const Dxf::Entity& e) const
    {
        std::optional<double> w = e.weight;
        if (!w)
            if (const Dxf::Layer* l = m_dxf->LayerOf(e.layer))
                w = l->weight;
        if (w && *w > 0.0)
            return w;
        return std::nullopt;
    }

    std::optional<std::array<double, 2>> Cad::Dash(const Dxf::Entity& e) const
    {
        std::string_view name;
        if (e.linetype)
            name = *e.linetype;
        else if (const Dxf::Layer* l = m_dxf->LayerOf(e.layer))
            name = l->linetype;
        else
            return std::nullopt;
        const auto lt = std::find_if(m_dxf->linetypes.begin(), m_dxf->linetypes.end(), [&](const Dxf::Linetype& l) { return EqualsIgnoreCase(l.name, name); });
        if (lt == m_dxf->linetypes.end())
            return std::nullopt;
        const auto dash = std::find_if(lt->dashes.begin(), lt->dashes.end(), [](double d) { return d > 0.0; });
        const auto gap  = std::find_if(lt->dashes.begin(), lt->dashes.end(), [](double d) { return d < 0.0; });
        if (gap == lt->dashes.end())
            return std::nullopt;
        const double g = std::abs(*gap), k = m_dxf->ltscale;
        // 点画成短划
        return std::array<double, 2>{ (dash == lt->dashes.end() ? g / 4.0 : *dash) * k, g * k };
    }

    bool Cad::operator==(const Cad& o) const
    {
        return m_source == o.m_source && m_path == o.m_path && (m_bytes == o.m_bytes || (m_bytes && o.m_bytes && *m_bytes == *o.m_bytes));
    }

    namespace Cads
    {
        std::vector<std::pair<std::vector<P2>, bool>> Paths(const Dxf::Shape& s)
        {
            if (const Dxf::Line* l = std::get_if<Dxf::Line>(&s))
                return { { { l->a, l->b }, false } };
            if (const Dxf::Polyline* pl = std::get_if<Dxf::Polyline>(&s))
                return { { pl->pts, pl->closed } };
            if (const Dxf::Circle* c = std::get_if<Dxf::Circle>(&s))
            {
                std::vector<P2> pts = Dxf::ArcPoints(c->c, c->r, 0.0, 360.0);
                pts.pop_back();
                return { { std::move(pts), true } };
            }
            if (const Dxf::Arc* a = std::get_if<Dxf::Arc>(&s))
                return { { Dxf::ArcPoints(a->c, a->r, a->start, a->end - a->start), false } };
            if (const Dxf::Fill* f = std::get_if<Dxf::Fill>(&s))
            {
                std::vector<std::pair<std::vector<P2>, bool>> out;
                for (const auto& r : f->rings)
                    out.emplace_back(r, true);
                return out;
            }
            return {};
        }

        const char* Kind(const Dxf::Shape& s)
        {
            switch (s.index())
            {
            case 0: return "line";
            case 1: return "polyline";
            case 2: return "circle";
            case 3: return "arc";
            case 4: return "point";
            case 5: return "text";
            default: return "hatch";
            }
        }

        std::optional<std::pair<const Dxf::Entity*, double>> Nearest(const Cad& cad, const CadImport& ci, P2 at, double reachMm)
        {
            const P2 q = ci.Unplace(at);
            const double reach = reachMm / ci.scale;
            std::optional<std::pair<const Dxf::Entity*, double>> best;
            for (const Dxf::Entity& e : cad.Drawing().entities)
            {
                if (!cad.Shows(e, ci.hidden))
                    continue;
                const auto [lo, hi] = Dxf::Bounds(e.shape);
                if (!(q.x >= lo.x - reach && q.x <= hi.x + reach && q.y >= lo.y - reach && q.y <= hi.y + reach))
                    continue;
                const double d = Distance(e.shape, q);
                if (d <= reach && (!best || d < best->second))
                    best = std::pair<const Dxf::Entity*, double>(&e, d);
            }
            if (best)
                best->second *= ci.scale;
            return best;
        }
    }
}
