// 移植自 BIMCraft crates/core/src/model.rs（f3bbb33）的注释部分（参照线、尺寸、标记、高程点、检查），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Model.h"
#include "Model/Sketch.h"
#include "Model/Text.h"
#include "Model/Walls.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace MiniBIM::Bim
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        std::string Num(double x) { return Text::Shortest(x); }

        bool PointOk(const std::string& who, P2 p, std::string& error)
        {
            if (p.IsFinite() && std::abs(p.x) <= MaxCoord && std::abs(p.y) <= MaxCoord)
                return true;
            error = who + ": [" + Num(p.x) + ", " + Num(p.y) + "] isn't a point within " + Num(MaxCoord) + " mm";
            return false;
        }

        bool CoordOk(const std::string& who, double x, std::string& error)
        {
            if (std::isfinite(x) && std::abs(x) <= MaxCoord)
                return true;
            error = who + " must be a number within " + Num(MaxCoord) + ", got " + Num(x);
            return false;
        }

        template <class T>
        std::string Names(const std::map<std::string, T>& m)
        {
            std::string out;
            for (const auto& [k, _] : m)
                out += (out.empty() ? "" : ", ") + k;
            return out;
        }

        // 逆时针扫过的角，[0, 2π)
        double Euclid(double a)
        {
            const double t = std::fmod(a, 2.0 * kPi);
            return t < 0.0 ? t + 2.0 * kPi : t;
        }
    }

    namespace FaceOf
    {
        const char* Name(Face f)
        {
            switch (f)
            {
            case Face::Centre: return "centre";
            case Face::Left:   return "left";
            case Face::Right:  return "right";
            case Face::Start:  return "start";
            case Face::End:    return "end";
            }
            return "centre";
        }

        const char* Said(Face f)
        {
            switch (f)
            {
            case Face::Centre: return "centre line";
            case Face::Left:   return "left face";
            case Face::Right:  return "right face";
            case Face::Start:  return "start";
            case Face::End:    return "end";
            }
            return "centre line";
        }

        Face Mirrored(Face f) { return f == Face::Start ? Face::End : f == Face::End ? Face::Start : f; }

        Face Flipped(Face f) { return f == Face::Left ? Face::Right : f == Face::Right ? Face::Left : Mirrored(f); }

        std::optional<Face> FromName(std::string_view s)
        {
            const std::string n = Text::AsciiLower(Text::Trim(s));
            if (n == "centre" || n == "center")
                return Face::Centre;
            if (n == "left")
                return Face::Left;
            if (n == "right")
                return Face::Right;
            if (n == "start")
                return Face::Start;
            if (n == "end")
                return Face::End;
            return std::nullopt;
        }
    }

    const char* Tag::Usual(std::string_view category)
    {
        if (category == "door" || category == "window")
            return "Mark";
        if (category == "room")
            return "Name";
        return "Type";
    }

    bool Element::IsAnnotation() const
    {
        return As<Dimension>() || As<Note>() || As<Spot>() || As<Tag>() || As<DetailLine>() || As<FilledRegion>();
    }

    // ── 注释在哪里 ────────────────────────────────────────────────
    std::optional<Guid> Model::AnnotationPlan(const Element& e) const
    {
        if (const Dimension* d = e.As<Dimension>())
            return d->view.value_or(d->level);
        if (const Tag* t = e.As<Tag>())
            return t->view ? t->view : LevelOf(e);
        if (const Note* n = e.As<Note>())
            return n->view;
        if (const Spot* s = e.As<Spot>())
            return s->view;
        if (const DetailLine* l = e.As<DetailLine>())
            return l->view;
        if (const FilledRegion* r = e.As<FilledRegion>())
            return r->view;
        return std::nullopt;
    }

    bool Model::AnnotationIn(const Element& e, Guid plan) const
    {
        const auto at = AnnotationPlan(e);
        if (!at)
            return false;
        if (Same(*at, plan))
            return true;
        const PlanView* v = GetPlanView(plan);
        return v && v->parent && Same(*v->parent, *at);
    }

    std::optional<P2> Model::Anchor(Guid id) const
    {
        const Element* e = elements.Get(id);
        if (!e)
            return std::nullopt;
        if (const Opening* o = e->AsOpening())
        {
            const Wall* w = GetWall(o->host);
            return w ? std::optional<P2>(Walls::Axis::Of(*w).At(o->offset, 0.0)) : std::nullopt;
        }
        if (const Wall* w = e->As<Wall>())
        {
            const Walls::Axis axis = Walls::Axis::Of(*w);
            return axis.At(axis.Length() / 2.0, 0.0);
        }
        if (const Column* c = e->AsColumn())
            return c->at;
        if (const Component* c = e->As<Component>())
            return c->at;
        if (const Room* r = e->As<Room>())
            return r->point;
        if (const Run* r = e->AsRun())
            return (r->start + r->end) * 0.5;
        const Ring* ring = nullptr;
        if (const Floor* f = e->As<Floor>())
            ring = &f->boundary;
        else if (const Ceiling* c = e->As<Ceiling>())
            ring = &c->boundary;
        else if (const Roof* r = e->As<Roof>())
            ring = &r->boundary;
        if (!ring || ring->empty())
            return std::nullopt;
        P2 sum;
        for (const P2& q : *ring)
            sum = sum + q;
        return sum * (1.0 / static_cast<double>(ring->size()));
    }

    P2 Model::TagOffset(Guid host) const
    {
        const Element* e = elements.Get(host);
        const Opening* o = e ? e->AsOpening() : nullptr;
        const Wall* w = o ? GetWall(o->host) : nullptr;
        if (!w)
            return Anchor(host) ? P2(400.0, 400.0) : P2();
        const auto [ext, in] = Walls::Faces(*this, *w);
        const bool outside = o->flipFacing == e->IsDoor();
        const P2 t = Walls::Axis::Of(*w).Tangent(o->offset).Perp() * (outside ? ext + 400.0 : in - 400.0);
        // + 0.0：文件里不写 -0
        return P2(t.x + 0.0, t.y + 0.0);
    }

    std::vector<std::string> Model::TagFields(std::string_view category) const
    {
        std::vector<std::string> out;
        if (category == "room")
            return { "Name", "Number", "Area", "Level" };
        out.push_back("Type");
        if (category == "door" || category == "window")
            for (const char* f : { "Mark", "Family", "Width", "Height", "Sill Height" })
                out.push_back(f);
        if (category == "component")
            out.push_back("Family");
        if (category == "wall")
            out.push_back("Length");
        out.push_back("Level");
        return out;
    }

    std::optional<std::string> Model::FieldText(Guid host, const std::string& field) const
    {
        const Element* e = elements.Get(host);
        if (!e)
            return std::nullopt;
        const auto fields = TagFields(e->Category());
        if (std::find(fields.begin(), fields.end(), field) == fields.end())
            return std::nullopt;
        if (field == "Level")
        {
            const auto l = LevelOf(*e);
            const Level* lv = l ? GetLevel(*l) : nullptr;
            return lv ? lv->name : std::string();
        }
        if (field == "Type")
        {
            if (const Opening* o = e->AsOpening())
                return o->typeName;
            if (const Component* c = e->As<Component>())
                return c->typeName;
            if (const Wall* w = e->As<Wall>())
                return w->typeName;
            if (const Floor* f = e->As<Floor>())
                return f->typeName;
            if (const Ceiling* c = e->As<Ceiling>())
                return c->typeName;
            if (const Roof* r = e->As<Roof>())
                return r->typeName;
            if (const Column* c = e->AsColumn())
                return c->typeName;
            if (const Run* r = e->AsRun())
                return r->typeName;
            return std::string();
        }
        if (field == "Family")
        {
            const auto f = e->FamilyOf();
            return f ? f->first : std::string();
        }
        if (const Opening* o = e->AsOpening())
        {
            if (field == "Mark")
                return o->mark;
            const auto t = OpeningTypeOf(*e);
            if (!t)
                return std::string();
            if (field == "Width")
                return UnitsOf::Length(units, t->width);
            if (field == "Height")
                return UnitsOf::Length(units, t->height);
            if (field == "Sill Height")
                return UnitsOf::Length(units, o->sill);
        }
        if (const Room* r = e->As<Room>())
        {
            if (field == "Name")
                return r->name;
            if (field == "Number")
                return r->number;
            if (field == "Area")
            {
                const auto region = RoomRegion(*r);
                if (!region)
                    return std::string();
                const auto [value, unit] = UnitsOf::Area(units, region->Area());
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.2f %s", value, unit);
                return std::string(buf);
            }
        }
        if (const Wall* w = e->As<Wall>(); w && field == "Length")
            return UnitsOf::Length(units, Walls::Axis::Of(*w).Length());
        return std::string();
    }

    std::string Model::TagText(const Tag& t) const
    {
        const Element* e = elements.Get(t.host);
        if (!e)
            return "?";
        const std::string field = t.label ? *t.label : Tag::Usual(e->Category());
        const auto text = FieldText(t.host, field);
        return text && !Text::Trim(*text).empty() ? *text : "?";
    }

    // ── 参照线 ────────────────────────────────────────────────────
    bool Model::ReferenceLine(const Reference& r, Segment& out, std::string& error) const
    {
        const Element* e = elements.Get(r.id);
        const std::string shown = Shown(r.id);
        if (!e)
        {
            error = "element " + shown + " doesn't exist";
            return false;
        }
        if (const Grid* g = e->As<Grid>())
        {
            if (r.face != Face::Centre)
            {
                error = "grid " + shown + " has no faces; measure to its line";
                return false;
            }
            out = { g->start, g->end };
            return true;
        }
        if (const Wall* w = e->As<Wall>())
        {
            const Walls::Axis axis = Walls::Axis::Of(*w);
            const auto [ext, in] = Walls::Faces(*this, *w);
            double a = 0.0, b = axis.Length();
            if (axis.Radius())
            {
                if (r.face != Face::Start && r.face != Face::End)
                {
                    error = "wall " + shown + "'s faces are arcs; measure to its start or its end, or its radius";
                    return false;
                }
            }
            else if (const auto id = elements.IdOf(r.id))
            {
                // 直墙的线伸到它的轮廓那么远：越过与别的墙共有的端头
                for (const P2& p : Walls::Outline(*this, *id))
                {
                    const double along = (p - w->start).Dot(axis.Tangent(0.0));
                    a = std::min(a, along);
                    b = std::max(b, along);
                }
            }
            switch (r.face)
            {
            case Face::Centre: out = { axis.At(a, (ext + in) / 2.0), axis.At(b, (ext + in) / 2.0) }; break;
            case Face::Left:   out = { axis.At(a, ext), axis.At(b, ext) }; break;
            case Face::Right:  out = { axis.At(a, in), axis.At(b, in) }; break;
            case Face::Start:  out = { axis.At(a, in), axis.At(a, ext) }; break;
            case Face::End:    out = { axis.At(b, in), axis.At(b, ext) }; break;
            }
            return true;
        }
        if (const Opening* o = e->AsOpening())
        {
            const std::string what = std::string(e->Category()) + " " + shown;
            const Wall* w = GetWall(o->host);
            const auto t = OpeningTypeOf(*e);
            if (!w || !t)
            {
                error = what + " has no wall or no type to measure";
                return false;
            }
            double along = o->offset;
            if (r.face == Face::Start)
                along -= t->width / 2.0;
            else if (r.face == Face::End)
                along += t->width / 2.0;
            else if (r.face != Face::Centre)
            {
                error = what + " has a centre and two jambs, start and end; measure to one of those";
                return false;
            }
            const Walls::Axis axis = Walls::Axis::Of(*w);
            const auto [ext, in] = Walls::Faces(*this, *w);
            out = { axis.At(along, in), axis.At(along, ext) };
            return true;
        }
        if (const DetailLine* l = e->As<DetailLine>())
        {
            if (std::string_view(l->curve.Name()) != "line")
            {
                error = "detail line " + shown + " is a " + l->curve.Name() +
                        "; dimensions measure to straight ones (and radial ones to circles and arcs)";
                return false;
            }
            const P2 a = l->curve.Start(), b = l->curve.End();
            const P2 n = (b - a).Unit().Perp() * 100.0;
            switch (r.face)
            {
            case Face::Centre: out = { a, b }; return true;
            case Face::Start:  out = { a - n, a + n }; return true;
            case Face::End:    out = { b - n, b + n }; return true;
            default:
                error = "detail line " + shown + " has a line and two ends; measure to one of those";
                return false;
            }
        }
        error = std::string(e->Category()) + " " + shown + " isn't a wall, a door, a window, a grid or a detail line";
        return false;
    }

    std::vector<std::pair<Reference, Segment>> Model::ReferenceLines(Guid id) const
    {
        const Element* e = elements.Get(id);
        std::vector<Face> faces;
        if (!e)
            return {};
        if (e->As<Wall>())
            faces = { Face::Centre, Face::Left, Face::Right, Face::Start, Face::End };
        else if (e->AsOpening() || e->As<DetailLine>())
            faces = { Face::Centre, Face::Start, Face::End };
        else if (e->As<Grid>())
            faces = { Face::Centre };
        std::vector<std::pair<Reference, Segment>> out;
        const Guid g = GuidOf(id);
        for (const Face f : faces)
        {
            const Reference r{ g, f };
            Segment s;
            std::string why;
            if (ReferenceLine(r, s, why))
                out.emplace_back(r, s);
        }
        return out;
    }

    std::optional<std::pair<Reference, double>> Model::ReferenceAt(Guid id, P2 p, const std::function<bool(const Segment&)>& keep) const
    {
        std::optional<std::pair<Reference, double>> best;
        for (const auto& [r, s] : ReferenceLines(id))
        {
            if (!keep(s))
                continue;
            const double d = Geom::DistToSegment(p, s.first, s.second);
            if (!best || d < best->second)
                best = std::pair{ r, d };
        }
        return best;
    }

    bool Model::ReferenceArc(const Reference& r, P2& centre, double& radius, std::string& error) const
    {
        const char* what = "a radial dimension measures round columns and curved walls";
        const Element* e = elements.Get(r.id);
        const std::string shown = Shown(r.id);
        if (!e)
        {
            error = "element " + shown + " doesn't exist";
            return false;
        }
        if (const Column* c = e->AsColumn())
        {
            const ColumnType* t = ColumnTypeOf(*e);
            if (!t || !t->round)
            {
                error = std::string(e->Category()) + " " + shown + " isn't round; " + what;
                return false;
            }
            centre = c->at;
            radius = t->width / 2.0;
            return true;
        }
        if (const Wall* w = e->As<Wall>())
        {
            const Walls::Axis axis = Walls::Axis::Of(*w);
            const auto [ext, in] = Walls::Faces(*this, *w);
            const double off = r.face == Face::Left ? ext : r.face == Face::Right ? in : (ext + in) / 2.0;
            const auto c = axis.Centre();
            if (!c)
            {
                error = "wall " + shown + " is straight; " + what;
                return false;
            }
            centre = *c;
            radius = axis.At(0.0, off).Dist(*c);
            return true;
        }
        if (const DetailLine* l = e->As<DetailLine>())
        {
            const auto round = l->curve.Round();
            if (!round)
            {
                error = "detail line " + shown + " is a " + l->curve.Name() + "; " + what + ", circles and arcs";
                return false;
            }
            centre = round->first;
            radius = round->second;
            return true;
        }
        error = std::string(e->Category()) + " " + shown + " isn't round; " + what;
        return false;
    }

    std::optional<std::tuple<P2, double, double>> Model::AngleSector(const Dimension& d) const
    {
        if (d.refs.size() != 2)
            return std::nullopt;
        Segment l1, l2;
        std::string why;
        if (!ReferenceLine(d.refs[0], l1, why) || !ReferenceLine(d.refs[1], l2, why))
            return std::nullopt;
        const auto o = Geom::LineIntersection(l1.first, l1.second, l2.first, l2.second);
        if (!o)
            return std::nullopt;
        const P2 at = d.at - *o;
        const double theta = std::atan2(at.y, at.x);
        const auto rays = [](P2 p, P2 q)
        {
            const P2 u = q - p;
            return std::array<double, 2>{ std::atan2(u.y, u.x), std::atan2(-u.y, -u.x) };
        };
        for (const double s : rays(l1.first, l1.second))
            for (const double t : rays(l2.first, l2.second))
                for (const auto& [from, to] : { std::pair{ s, t }, std::pair{ t, s } })
                {
                    const double sweep = Euclid(to - from);
                    if (sweep < kPi && Euclid(theta - from) <= sweep)
                        return std::tuple{ *o, from, sweep };
                }
        return std::nullopt;
    }

    std::pair<P2, std::vector<std::pair<double, Segment>>> Model::Crossings(const Dimension& d) const
    {
        std::vector<Segment> lines;
        for (const Reference& r : d.refs)
        {
            Segment s;
            std::string why;
            if (ReferenceLine(r, s, why))
                lines.push_back(s);
        }
        if (lines.empty())
            return { P2(), {} };
        P2 n = (lines[0].second - lines[0].first).Unit().Perp();
        if (n.x < -1e-9 || (std::abs(n.x) <= 1e-9 && n.y < 0.0))
            n = n * -1.0;
        std::vector<std::pair<double, Segment>> v;
        for (const Segment& s : lines)
        {
            const P2 c = Geom::LineIntersection(d.at, d.at + n, s.first, s.second).value_or(s.first);
            v.emplace_back((c - d.at).Dot(n), s);
        }
        std::stable_sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        return { n, v };
    }

    std::vector<double> Model::DimensionValues(const Dimension& d) const
    {
        std::vector<double> out;
        switch (d.kind)
        {
        case DimKind::Aligned:
        {
            const auto cross = Crossings(d).second;
            for (size_t i = 0; i + 1 < cross.size(); ++i)
                out.push_back(cross[i + 1].first - cross[i].first);
            break;
        }
        case DimKind::Angular:
            if (const auto s = AngleSector(d))
                out.push_back(std::get<2>(*s) * 180.0 / kPi);
            break;
        case DimKind::Radial:
            if (!d.refs.empty())
            {
                P2 c;
                double r = 0.0;
                std::string why;
                if (ReferenceArc(d.refs[0], c, r, why))
                    out.push_back(r);
            }
            break;
        }
        return out;
    }

    // ── 高程点 ────────────────────────────────────────────────────
    std::optional<double> Model::TopAt(Id id, P2 p) const
    {
        const Element* e = elements.Get(id);
        if (!e)
            return std::nullopt;
        if (const Floor* f = e->As<Floor>())
        {
            if (!Geom::Contains(f->boundary, p))
                return std::nullopt;
            for (const Ring& o : f->openings)
                if (Geom::Contains(o, p))
                    return std::nullopt;
            return FloorTop(*f).At(p);
        }
        if (const Roof* r = e->As<Roof>())
        {
            std::vector<Slope> faces;
            std::string why;
            if (!RoofFaces(*r, faces, why))
                return std::nullopt;
            std::optional<double> best;
            for (const auto& [loops, plane] : faces)
            {
                if (loops.empty() || !Geom::Contains(loops[0], p))
                    continue;
                bool hole = false;
                for (size_t i = 1; i < loops.size(); ++i)
                    hole = hole || Geom::Contains(loops[i], p);
                if (hole)
                    continue;
                const double z = plane.At(p) + RoofRise(*r, plane);
                best = best ? std::max(*best, z) : z;
            }
            return best;
        }
        if (const Wall* w = e->As<Wall>())
        {
            if (!Geom::Contains(Walls::Outline(*this, id), p))
                return std::nullopt;
            return Walls::TopAt(*this, *w, Walls::Axis::Of(*w).Project(p).first);
        }
        if (const Column* c = e->AsColumn())
        {
            if (!Geom::Contains(ColumnOutline(*e), p))
                return std::nullopt;
            return ColumnSpan(*c).second;
        }
        if (const auto g = RunGeom(*e))
        {
            if (!Geom::Contains(g->Footprint(), p))
                return std::nullopt;
            return g->ZSpan().second;
        }
        if (const Component* c = e->As<Component>())
        {
            std::optional<double> best;
            for (const Fam::Piece& piece : ComponentPieces(*c))
                if (Geom::Contains(piece.outline, p))
                    best = best ? std::max(*best, piece.top) : piece.top;
            return best;
        }
        return std::nullopt;
    }

    std::optional<Id> Model::SpotHost(Guid level, P2 p) const
    {
        std::optional<std::pair<Id, double>> best;
        for (const auto& [id, e] : elements.Items())
        {
            if (e->As<Ceiling>() || e->IsAnnotation())
                continue;
            const auto l = LevelOf(*e);
            if (!l || !Same(*l, level))
                continue;
            if (const auto z = TopAt(id, p); z && (!best || *z > best->second))
                best = std::pair{ id, *z };
        }
        return best ? std::optional<Id>(best->first) : std::nullopt;
    }

    std::optional<double> Model::SpotValue(const Spot& sp) const
    {
        if (!sp.host || !GetLevel(sp.view))
            return std::nullopt;
        const auto id = elements.IdOf(*sp.host);
        return id ? TopAt(*id, sp.at) : std::nullopt;
    }

    // ── 检查 ──────────────────────────────────────────────────────
    bool Model::CheckStyles(std::string& error) const
    {
        for (const auto& [n, p] : linePatterns)
        {
            if (n == Lines::Solid)
            {
                error = "line pattern `" + n + "`: that name is a line with no pattern; call it something else";
                return false;
            }
            if (Text::Trim(n).empty() || Text::CharCount(n) > MaxText)
            {
                error = "a line pattern needs a name of up to " + std::to_string(MaxText) + " characters";
                return false;
            }
            if (!p.Check(n, error))
                return false;
        }
        for (const auto& [n, s] : lineStyles)
        {
            if (Text::Trim(n).empty() || Text::CharCount(n) > MaxText)
            {
                error = "a line style needs a name of up to " + std::to_string(MaxText) + " characters";
                return false;
            }
            if (!Lines::CheckStyle(s, n, linePatterns, error))
                return false;
        }
        for (const auto& [n, p] : fillPatterns)
        {
            if (Text::Trim(n).empty() || Text::CharCount(n) > MaxText)
            {
                error = "a fill pattern needs a name of up to " + std::to_string(MaxText) + " characters";
                return false;
            }
            if (!p.Check(n, error))
                return false;
        }
        for (const auto& [n, t] : regionTypes)
        {
            if (Text::Trim(n).empty() || Text::CharCount(n) > MaxText)
            {
                error = "a filled region type needs a name of up to " + std::to_string(MaxText) + " characters";
                return false;
            }
            if (!Fills::CheckRegionType(t, n, fillPatterns, error))
                return false;
        }
        for (const auto& [n, t] : textTypes)
        {
            if (Text::Trim(n).empty() || Text::CharCount(n) > MaxText)
            {
                error = "a text type needs a name of up to " + std::to_string(MaxText) + " characters";
                return false;
            }
            if (!t.Check(n, error))
                return false;
        }
        return true;
    }

    bool Model::CheckAnnotation(const std::string& who, const Element& e, std::string& error) const
    {
        // 注释所在的视图：标高的平面，或平面视图
        const auto viewOk = [&](Guid v)
        {
            if (PlanLevel(v))
                return true;
            error = who + ": view " + Shown(v) + " isn't a plan";
            return false;
        };
        if (const Dimension* d = e.As<Dimension>())
        {
            if (!GetLevel(d->level))
            {
                error = who + ": level " + Shown(d->level) + " doesn't exist";
                return false;
            }
            if (const PlanView* v = d->view ? GetPlanView(*d->view) : nullptr; d->view && !(v && Same(v->level, d->level)))
            {
                error = who + ": view " + Shown(*d->view) + " isn't a plan view of its level";
                return false;
            }
            if (!PointOk(who, d->at, error))
                return false;
            const size_t n = d->refs.size();
            if (d->kind == DimKind::Aligned && (n < 2 || n > MaxPoints))
            {
                error = who + ": a dimension measures between 2 to " + std::to_string(MaxPoints) + " references";
                return false;
            }
            if (d->kind == DimKind::Angular && n != 2)
            {
                error = who + ": an angular dimension measures between two lines";
                return false;
            }
            if (d->kind == DimKind::Radial && n != 1)
            {
                error = who + ": a radial dimension measures one round element";
                return false;
            }
            std::set<std::pair<Guid, Face>> seen;
            std::optional<std::pair<Reference, Segment>> first;
            for (const Reference& r : d->refs)
            {
                std::string why;
                if (d->kind == DimKind::Radial)
                {
                    P2 c;
                    double rad = 0.0;
                    if (!ReferenceArc(r, c, rad, why))
                    {
                        error = who + ": " + why;
                        return false;
                    }
                    continue;
                }
                Segment line;
                if (!ReferenceLine(r, line, why))
                {
                    error = who + ": " + why;
                    return false;
                }
                const Element* re = elements.Get(r.id);
                const std::string what = std::string(re->Category()) + " " + Shown(r.id);
                if (!seen.insert({ r.id, r.face }).second)
                {
                    error = who + ": it measures to " + what + "'s " + FaceOf::Said(r.face) + " twice";
                    return false;
                }
                if (!first)
                {
                    first = std::pair{ r, line };
                    continue;
                }
                const bool parallel = Geom::Parallel(first->second.first, first->second.second, line.first, line.second);
                const Element* fe = elements.Get(first->first.id);
                const std::string other = std::string(fe->Category()) + " " + Shown(first->first.id);
                if (d->kind == DimKind::Aligned && !parallel)
                {
                    error = who + ": " + what + " isn't parallel to " + other + "; a dimension measures between parallel walls and grids";
                    return false;
                }
                if (d->kind == DimKind::Angular && parallel)
                {
                    error = who + ": " + what + " is parallel to " + other + "; an angle is between lines that meet";
                    return false;
                }
            }
            return true;
        }
        if (const Tag* t = e.As<Tag>())
        {
            const Element* host = elements.Get(t->host);
            if (!host)
            {
                error = who + ": element " + Shown(t->host) + " doesn't exist";
                return false;
            }
            if (t->view)
            {
                const PlanView* v   = GetPlanView(*t->view);
                const auto      own = LevelOf(*host);
                if (!v || (own && !Same(v->level, *own)))
                {
                    error = who + ": view " + Shown(*t->view) + " isn't a plan view of its host's level";
                    return false;
                }
            }
            // 墙不在了的门窗由墙去报；别的东西要在平面上有个点
            if (!host->AsOpening() && !Anchor(t->host))
            {
                error = who + ": tags name what has a place in a plan (doors, windows, walls, columns, floors, rooms and the like), not " +
                        host->Category() + " " + Shown(t->host);
                return false;
            }
            if (t->label)
            {
                const std::string cat = host->Category();
                const auto fields = TagFields(cat);
                if (std::find(fields.begin(), fields.end(), *t->label) == fields.end())
                {
                    std::string names;
                    for (const std::string& f : fields)
                        names += (names.empty() ? "" : ", ") + f;
                    error = who + ": " + cat + " " + Shown(t->host) + " has no field `" + *t->label + "`; its fields are: " + names;
                    return false;
                }
            }
            return PointOk(who + " offset", t->offset, error);
        }
        if (const Spot* sp = e.As<Spot>())
        {
            if (!viewOk(sp->view) || !PointOk(who, sp->at, error))
                return false;
            if (!sp->host)
            {
                error = who + ": a spot elevation in a plan reads an element's top; give `host`";
                return false;
            }
            if (!elements.Get(*sp->host))
            {
                error = who + ": element " + Shown(*sp->host) + " doesn't exist";
                return false;
            }
            return true;
        }
        if (const Note* n = e.As<Note>())
        {
            if (!viewOk(n->view) || !PointOk(who, n->at, error))
                return false;
            const size_t len = Text::CharCount(n->text);
            if (len > TextTypes::MaxNote)
            {
                error = who + ": its text is " + std::to_string(len) + " characters long; a note holds up to " + std::to_string(TextTypes::MaxNote);
                return false;
            }
            if (Text::Trim(n->text).empty())
            {
                error = who + ": a text note needs text";
                return false;
            }
            if (!textTypes.count(n->typeName))
            {
                error = who + ": there's no text type `" + n->typeName + "` (there are: " + Names(textTypes) + ")";
                return false;
            }
            if (!TextTypes::CheckRuns(who, n->runs, len, error) || !CoordOk(who + " angle", n->angle, error))
                return false;
            return !n->leader || PointOk(who + " leader", *n->leader, error);
        }
        if (const DetailLine* l = e.As<DetailLine>())
        {
            if (!viewOk(l->view) || !l->curve.Check(who, error))
                return false;
            if (l->insulation && !(std::isfinite(*l->insulation) && *l->insulation > Tol && *l->insulation <= 10000.0))
            {
                error = who + ": its insulation is " + Num(*l->insulation) + " mm wide; it's more than 1 mm and at most 10000";
                return false;
            }
            if (l->cloud && !(std::isfinite(*l->cloud) && *l->cloud > Tol && *l->cloud <= 10000.0))
            {
                error = who + ": its cloud's arcs are " + Num(*l->cloud) + " mm long; they're more than 1 mm and at most 10000";
                return false;
            }
            if (l->cloud && l->insulation)
            {
                error = who + ": it is insulation or a revision cloud, not both";
                return false;
            }
            if (l->revision && Text::CharCount(*l->revision) > MaxText)
            {
                error = who + ": its revision is longer than " + std::to_string(MaxText) + " letters";
                return false;
            }
            if (!lineStyles.count(l->style))
            {
                error = who + ": there's no line style `" + l->style + "` (there are: " + Names(lineStyles) + ")";
                return false;
            }
            return true;
        }
        if (const FilledRegion* r = e.As<FilledRegion>())
        {
            if (!viewOk(r->view) || !PointOk(who + " pattern origin", r->patternOrigin, error) ||
                !CoordOk(who + " pattern angle", r->patternAngle, error))
                return false;
            if (r->sketch.empty())
            {
                error = who + ": a region's sketch has no lines; it needs a loop of them";
                return false;
            }
            Sketch::Profile profile;
            std::vector<Sketch::Problem> problems;
            if (!Sketch::Solve(r->sketch, profile, problems))
            {
                error = who + ": its sketch doesn't close into loops: " + Sketch::Say(problems, Sketch::Namer(r->sketch));
                return false;
            }
            if (!regionTypes.count(r->typeName))
            {
                error = who + ": there's no filled region type `" + r->typeName + "` (there are: " + Names(regionTypes) + ")";
                return false;
            }
            for (const std::optional<std::string>* s : { &r->lineStyle })
                if (*s && !lineStyles.count(**s))
                {
                    error = who + ": there's no line style `" + **s + "` (there are: " + Names(lineStyles) + ")";
                    return false;
                }
            for (const SketchLine& l : r->sketch)
                if (l.style && !lineStyles.count(*l.style))
                {
                    error = who + ": there's no line style `" + *l.style + "` (there are: " + Names(lineStyles) + ")";
                    return false;
                }
            return true;
        }
        return true;
    }
}
