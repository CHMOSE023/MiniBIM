// 移植自 BIMCraft crates/rvt/src/floors.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
#include "Rvt/Floors.h"

#include "Model/Text.h"
#include "Rvt/Bytes.h"
#include "Rvt/Data.h"
#include "Rvt/Types.h"

#include <cmath>

namespace MiniBIM::Rvt::Floors
{
    std::map<uint32_t, std::vector<const Record*>> SketchLines(const Facts& f)
    {
        std::set<uint32_t> sketched;
        for (const int64_t c : { OstFloors, OstCeilings, Roofs::OstRoofs })
            for (const Record* r : f.OfCategory(c))
                sketched.insert(r->id);
        std::map<uint32_t, std::vector<const Record*>> out;
        for (const Record* r : f.OfCategory(OstSketchLines))
            for (auto v = r->refs2.rbegin(); v != r->refs2.rend(); ++v)
                if (*v <= UINT32_MAX && sketched.contains(static_cast<uint32_t>(*v)))
                {
                    out[static_cast<uint32_t>(*v)].push_back(r);
                    break;
                }
        return out;
    }

    void Wanted(const Facts& f, std::set<uint32_t>& out)
    {
        const auto sketches = SketchLines(f);
        for (const int64_t category : { OstFloors, OstCeilings })
        {
            for (const Record* r : f.OfCategory(category))
            {
                out.insert(r->id);
                if (const auto it = sketches.find(r->id); it != sketches.end())
                    for (const Record* l : it->second)
                        out.insert(l->id);
            }
            if (const auto it = f.types.find(category); it != f.types.end())
                out.insert(it->second.begin(), it->second.end());
        }
    }

    std::optional<Regions> RegionsOf(const std::vector<const Record*>& lines, const Gathered& g, const std::array<double, 6>& bbox)
    {
        std::vector<Sketch::Edge> edges;
        edges.reserve(lines.size());
        std::vector<uint32_t> unread;
        for (const Record* l : lines)
        {
            std::optional<Sketch::Edge> e;
            if (const auto it = g.data.find(l->id); it != g.data.end())
                if (const auto got = Data::Newest<Sketch::Edge>(it->second, [&](std::span<const uint8_t> b) { return Sketch::EdgeOf(b, l->bbox); }))
                    e = got->first;
            if (e)
                edges.push_back(std::move(*e));
            else
                unread.push_back(l->id);
        }
        auto pieces = Sketch::FromEdges(edges);
        if (!pieces)
            pieces = Sketch::FromEdgesSnapped(edges);
        if (pieces)
            return Regions{ std::move(*pieces), unread, false, false };
        // 数据读不出时由线的包围盒围成边界：那时一条也不略去
        std::vector<std::array<double, 4>> boxes;
        for (const Record* l : lines)
            boxes.push_back({ l->bbox[0], l->bbox[1], l->bbox[3], l->bbox[4] });
        if (auto boxed = Sketch::FromBoxes(boxes))
            return Regions{ std::move(*boxed), {}, false, false };
        // 什么也不围的曲线不是边界：它所在的环仍闭合
        if (auto open = Sketch::FromEdgesOpen(edges))
        {
            std::vector<uint32_t> dangling = unread;
            for (const size_t i : open->second)
                if (i < lines.size())
                    dangling.push_back(lines[i]->id);
            return Regions{ std::move(open->first), dangling, false, false };
        }
        // 没闭合的草图（对齐也闭合不了的缺口）：端点用直线连上，并说明
        if (auto closed = Sketch::FromEdgesClosed(edges))
            return Regions{ std::move(*closed), unread, true, false };
        // 根本没有边界：元素自己包围盒的轮廓，这是文件对它说的、剩下能读的全部
        const double x0 = bbox[0], y0 = bbox[1], x1 = bbox[3], y1 = bbox[4];
        if (!(x1 > x0 && y1 > y0))
            return std::nullopt;
        Sketch::Piece piece;
        piece.outer = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
        return Regions{ { piece }, unread, false, true };
    }

    Bim::Ring RingOf(const std::vector<Sketch::Pt>& points)
    {
        Bim::Ring out;
        out.reserve(points.size());
        for (const auto& q : points)
        {
            const Bim::P2 p = Point(q.x, q.y);
            if (out.empty() || out.back() != p)
                out.push_back(p);
        }
        if (out.size() > 1 && out.front() == out.back())
            out.pop_back();
        return out;
    }

    std::optional<uint32_t> LevelOf(const Record& r, const std::map<uint32_t, double>& elevations, double z)
    {
        const auto named = Walls::NamedLevels(r.refs, elevations);
        if (named.size() == 1)
            return named[0];
        if (named.empty())
            return Walls::LevelAtOrBelow(elevations, z);
        std::map<uint32_t, double> namedMap;
        for (const uint32_t id : named)
            namedMap[id] = elevations.at(id);
        if (const auto at = Walls::LevelAtOrBelow(namedMap, z))
            return at;
        // 说的里面最低的（一样低时取列表里先说的）
        std::optional<uint32_t> low;
        for (const uint32_t id : named)
            if (!low || Bytes::TotalLess(elevations.at(id), elevations.at(*low)))
                low = id;
        return low;
    }

    bool SketchedOf(Importing& cx, const Facts& f, const Gathered& g, const Record& r, Which which, const std::map<uint32_t, std::vector<const Record*>>& sketches,
                    const TypeNamed& typeNamed, Sketched& out, std::string& why)
    {
        const auto typesIt = f.types.find(which.category);
        const std::set<uint32_t> types = typesIt != f.types.end() ? typesIt->second : std::set<uint32_t>{};
        const char* what = which.category == OstFloors ? "floor" : which.category == OstCeilings ? "ceiling" : "roof";
        const auto lines = sketches.find(r.id);
        if (lines == sketches.end())
        {
            why = "it has no sketch lines";
            return false;
        }
        auto regions = RegionsOf(lines->second, g, r.bbox);
        if (!regions)
        {
            why = "its sketch lines do not close into a boundary";
            return false;
        }
        if (!regions->dangling.empty())
        {
            std::string ids;
            for (const uint32_t id : regions->dangling)
                ids += (ids.empty() ? "" : ", ") + std::to_string(id);
            const char* plural = regions->dangling.size() == 1 ? "line" : "lines";
            cx.Note(what, r.id, std::to_string(regions->dangling.size()) + " sketch " + plural + " close nothing and are left out (ids " + ids + ")");
        }
        if (regions->boxed)
            cx.Note(what, r.id, "its sketch lines close no boundary the reader can follow: it comes in as the footprint of its own box");
        if (regions->closed)
            cx.Note(what, r.id, "its sketch lines do not close: the ends that do not meet are joined by a straight line");
        const auto tid = Types::UniqueType(r.refs, types);
        if (!tid)
        {
            why = std::string("its record does not name one ") + what + " type";
            return false;
        }
        const double tall = Mm(r.Extent()[2]);
        auto typ = Walls::LayeredTypeOf(f, g, *tid, which.category);
        if (typ && !(typ->thickness > 0.0))
            typ.reset();
        if (!typ)
        {
            // 没有复合结构的类型不说它多厚；它自己的包围盒说，平的那个就是它的厚度
            cx.Note(what, r.id, "its type has no layers the reader can read: it comes in as thick as its own box");
            Bim::Layer layer;
            layer.thickness = tall;
            typ = Walls::LayeredType{ std::nullopt, { layer }, tall };
        }
        const bool flat = std::abs(tall - typ->thickness) <= 1.0;
        if (!flat)
            // 带坡度或有高差的：它放平进来，与类型一样厚，在包围盒底部，并说明
            cx.Note(what, r.id,
                    "its box is " + Bim::Text::Shortest(tall) + " mm tall and its type " + Bim::Text::Shortest(typ->thickness) +
                        " mm thick: it is sloped or stepped, and its slopes are not read: it comes in level");
        const double z     = which.atTop ? r.bbox[5] : r.bbox[2];
        const auto   level = LevelOf(r, f.elevations, z);
        const auto   model = level ? cx.levels.find(*level) : cx.levels.end();
        if (model == cx.levels.end())
        {
            why = "it stands on no level";
            return false;
        }
        const double elevation = f.elevations.contains(*level) ? f.elevations.at(*level) : 0.0;
        out.pieces   = std::move(regions->pieces);
        out.level    = model->second;
        out.offset   = Micron(Mm(z) - Mm(elevation));
        out.typeName = typeNamed(cx, *tid, *typ);
        out.flat     = flat;
        return true;
    }

    void Add(Importing& cx, const Facts& f, const Gathered& g)
    {
        const auto sketches = SketchLines(f);
        {
            std::map<uint32_t, std::string> made;
            const TypeNamed name = [&](Importing& c, uint32_t tid, const Walls::LayeredType& typ)
            {
                auto it = made.find(tid);
                if (it == made.end())
                {
                    const std::string base = typ.name.value_or("Imported - " + Bim::Text::Shortest(typ.thickness) + "mm");
                    it = made.emplace(tid, KindOf(c.model.types.floor, base,
                                                  [&](const Bim::FloorType& k) { return std::abs(k.thickness - typ.thickness) < 0.05 && k.layers == typ.layers; },
                                                  Bim::FloorType{ typ.thickness, typ.layers }))
                             .first;
                }
                return it->second;
            };
            for (const Record* r : f.OfCategory(OstFloors))
            {
                Sketched s;
                std::string why;
                if (!SketchedOf(cx, f, g, *r, Floor, sketches, name, s, why))
                {
                    cx.LeftOut("floor", r->id, why);
                    continue;
                }
                if (s.pieces.size() > 1)
                    cx.Note("floor", r->id, "its sketch is several separate loops: one floor for each");
                for (const auto& p : s.pieces)
                {
                    Bim::Floor floor;
                    floor.typeName = s.typeName;
                    floor.level    = s.level;
                    floor.boundary = RingOf(p.outer);
                    for (const auto& h : p.holes)
                        floor.openings.push_back(RingOf(h));
                    floor.offset = s.offset;
                    const Bim::Id id = cx.model.Insert(floor);
                    cx.from[id] = "Floor " + std::to_string(r->id);
                }
            }
        }
        std::map<uint32_t, std::string> made;
        const TypeNamed name = [&](Importing& c, uint32_t tid, const Walls::LayeredType& typ)
        {
            auto it = made.find(tid);
            if (it == made.end())
            {
                const std::string base = typ.name.value_or("Imported - " + Bim::Text::Shortest(typ.thickness) + "mm");
                it = made.emplace(tid, KindOf(c.model.types.ceiling, base,
                                              [&](const Bim::CeilingType& k)
                                              { return std::abs(k.thickness - typ.thickness) < 0.05 && k.layers == typ.layers && k.grid == 0.0; },
                                              Bim::CeilingType{ typ.thickness, 0.0, typ.layers }))
                         .first;
            }
            return it->second;
        };
        for (const Record* r : f.OfCategory(OstCeilings))
        {
            Sketched s;
            std::string why;
            if (!SketchedOf(cx, f, g, *r, Ceiling, sketches, name, s, why))
            {
                cx.LeftOut("ceiling", r->id, why);
                continue;
            }
            if (s.pieces.size() > 1)
                cx.Note("ceiling", r->id, "its sketch is several separate loops: one ceiling for each");
            for (const auto& p : s.pieces)
            {
                Bim::Ceiling ceiling;
                ceiling.typeName = s.typeName;
                ceiling.level    = s.level;
                ceiling.boundary = RingOf(p.outer);
                for (const auto& h : p.holes)
                    ceiling.openings.push_back(RingOf(h));
                ceiling.offset = s.offset;
                const Bim::Id id = cx.model.Insert(ceiling);
                cx.from[id] = "Ceiling " + std::to_string(r->id);
            }
        }
    }
}
