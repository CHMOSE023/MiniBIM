// 移植自 BIMCraft crates/core/src/align.rs 与 model.rs 的 reference_line（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Align.h"
#include "Model/Geom.h"
#include "Model/Walls.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace MiniBIM::Bim::Align
{
    namespace
    {
        void Path(std::vector<Line>& out, const std::vector<P2>& pts, bool closed)
        {
            for (size_t i = 0; i + 1 < pts.size(); ++i)
                out.emplace_back(pts[i], pts[i + 1]);
            if (closed && pts.size() > 2)
                out.emplace_back(pts.back(), pts.front());
        }

        P2 Heading(double degrees)
        {
            const double r = degrees * std::numbers::pi / 180.0;
            return { std::cos(r), std::sin(r) };
        }

        // 过 at、沿 angle 与它垂直方向的两条轴线，各长 2 × half
        void Axes(std::vector<Line>& out, P2 at, double angle, double half)
        {
            for (const P2 u : { Heading(angle), Heading(angle + 90.0) })
                out.emplace_back(at - u * half, at + u * half);
        }

        // 墙的参照线（同 BIMCraft 的 reference_line）：中心（两面中间）、外面、内面、两端；弧墙只有两端
        void WallLines(std::vector<Line>& out, const Model& m, Id id, const Wall& w)
        {
            const Walls::Axis axis = Walls::Axis::Of(w);
            const auto [ext, in] = Walls::Faces(m, w);
            double a = 0.0, b = axis.Length();
            if (axis.Radius())
            {
                out.emplace_back(axis.At(a, in), axis.At(a, ext));
                out.emplace_back(axis.At(b, in), axis.At(b, ext));
                return;
            }
            // 直墙的面按它的轮廓（斜接处伸出去的）量
            const P2 t = axis.Tangent(0.0);
            for (const P2& p : Walls::Outline(m, id))
            {
                const double along = (p - w.start).Dot(t);
                a = std::min(a, along);
                b = std::max(b, along);
            }
            const double mid = (ext + in) / 2.0;
            out.emplace_back(axis.At(a, mid), axis.At(b, mid));
            out.emplace_back(axis.At(a, ext), axis.At(b, ext));
            out.emplace_back(axis.At(a, in), axis.At(b, in));
            out.emplace_back(axis.At(a, in), axis.At(a, ext));
            out.emplace_back(axis.At(b, in), axis.At(b, ext));
        }
    }

    std::vector<Line> Lines(const Model& m, Id id)
    {
        std::vector<Line> out;
        const Element* e = m.elements.Get(id);
        if (!e)
            return out;
        if (const Wall* w = e->As<Wall>())
            WallLines(out, m, id, *w);
        else if (const Opening* o = e->AsOpening())
        {
            const Wall* w = m.GetWall(o->host);
            const auto t = m.OpeningTypeOf(*e);
            if (w && t)
            {
                const Walls::Axis axis = Walls::Axis::Of(*w);
                const auto [ext, in] = Walls::Faces(m, *w);
                for (const double along : { o->offset, o->offset - t->width / 2.0, o->offset + t->width / 2.0 })
                    out.emplace_back(axis.At(along, in), axis.At(along, ext));
            }
        }
        else if (const Grid* g = e->As<Grid>())
            out.emplace_back(g->start, g->end);
        else if (const RoomSeparator* rs = e->As<RoomSeparator>())
            out.emplace_back(rs->start, rs->end);
        else if (const Floor* f = e->As<Floor>())
        {
            Path(out, f->boundary, true);
            for (const Ring& r : f->openings)
                Path(out, r, true);
        }
        else if (const Mass* ms = e->As<Mass>())
            Path(out, ms->boundary, true);
        else if (const Ceiling* c = e->As<Ceiling>())
        {
            Path(out, c->boundary, true);
            for (const Ring& r : c->openings)
                Path(out, r, true);
        }
        else if (const Roof* r = e->As<Roof>())
        {
            Path(out, m.RoofOutline(*r), true);
            for (const Ring& q : r->courtyards)
                Path(out, q, true);
            for (const Ring& q : r->openings)
                Path(out, q, true);
        }
        else if (const Column* col = e->AsColumn())
        {
            // 圆柱只有十字轴线：圆弧对不齐直线
            if (const ColumnType* t = m.ColumnTypeOf(*e))
            {
                if (!t->round)
                    Path(out, m.ColumnOutline(*e), true);
                Axes(out, col->at, col->angle, std::max(t->width, t->depth) / 2.0);
            }
        }
        else if (const Run* r = e->AsRun())
        {
            if (const auto g = m.RunGeom(*e))
            {
                if (g->Vertical() && g->round)
                    Axes(out, r->start, 0.0, g->width / 2.0);
                else
                    Path(out, g->Footprint(), true);
            }
            out.emplace_back(r->start, r->end);
        }
        else if (const Component* c = e->As<Component>())
        {
            for (const auto& piece : m.ComponentPieces(*c))
                if (!piece.round)
                    Path(out, piece.outline, true);
            Axes(out, c->at, c->angle, 100.0);
        }
        else if (const Link* l = e->As<Link>())
            Axes(out, l->at, l->angle, 100.0);
        // CAD 导入：显示的直线、多段线与填充的边
        else if (const CadImport* ci = e->As<CadImport>())
        {
            if (const auto c = m.cads.find(ci->cad); c != m.cads.end())
                for (const Dxf::Entity& x : c->second.Drawing().entities)
                {
                    if (!c->second.Shows(x, ci->hidden) ||
                        !(std::holds_alternative<Dxf::Line>(x.shape) || std::holds_alternative<Dxf::Polyline>(x.shape) || std::holds_alternative<Dxf::Fill>(x.shape)))
                        continue;
                    for (const auto& [pts, closed] : Cads::Paths(x.shape))
                    {
                        std::vector<P2> placed;
                        for (const P2& p : pts)
                            placed.push_back(ci->Place(p));
                        Path(out, placed, closed);
                    }
                }
        }
        std::erase_if(out, [](const Line& l) { return !(l.first.Dist(l.second) > Tol); });
        return out;
    }

    std::optional<std::pair<Line, double>> Nearest(const Model& m, Id id, P2 at)
    {
        std::optional<std::pair<Line, double>> best;
        for (const Line& l : Lines(m, id))
        {
            const double d = Geom::DistToSegment(at, l.first, l.second);
            if (!best || d < best->second)
                best = std::pair{ l, d };
        }
        return best;
    }

    bool Placement::Make(Line reference, Line line, Placement& out, std::string& error)
    {
        if (!(reference.first.Dist(reference.second) > Tol) || !(line.first.Dist(line.second) > Tol))
        {
            error = "align: a reference or target line has no length";
            return false;
        }
        const P2 u = (reference.second - reference.first).Unit(), v = (line.second - line.first).Unit();
        double angle = std::atan2(v.Cross(u), v.Dot(u)) * 180.0 / std::numbers::pi + 90.0;
        angle = std::fmod(angle, 180.0);
        if (angle < 0.0)
            angle += 180.0;
        out.angle = angle - 90.0;
        out.about = (line.first + line.second) * 0.5;
        const P2 n{ -u.y, u.x };
        out.by = n * (reference.first - out.about).Dot(n);
        return true;
    }

    P2 Placement::Map(P2 q) const
    {
        const double r = angle * std::numbers::pi / 180.0, s = std::sin(r), c = std::cos(r);
        const P2 d = q - about;
        return Micron(about + P2{ d.x * c - d.y * s, d.x * s + d.y * c } + by);
    }
}
