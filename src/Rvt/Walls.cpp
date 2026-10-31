// 移植自 BIMCraft crates/rvt/src/walls.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
// Portions of this file are ported from rvt-rs (https://github.com/DrunkOnJava/rvt-rs), Apache-2.0,
// Copyright 2026 Griffin Long, at commit b6086d41b9758fd9b92f42f0a47b8345b9c58056: src/element_record_level_refs.rs,
// src/element_record_wall_joins.rs and src/partition_compound_structure.rs (see Rvt/Walls.h).
// Changed: rewritten for BIMCraft, which builds a parametric wall from them instead of a solid; then rewritten in C++ for
// MiniBIM.
#include "Rvt/Walls.h"

#include "Model/Text.h"
#include "Rvt/Bytes.h"
#include "Rvt/Curves.h"
#include "Rvt/Data.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <variant>

namespace MiniBIM::Rvt::Walls
{
    using Bim::P2;

    namespace
    {
        // 墙的定位线或圆弧
        using Curve = std::variant<Curves::BoundedLine, Curves::BoundedArc>;

        std::optional<Curve> CurveOf(std::span<const uint8_t> data)
        {
            if (const auto l = Curves::FirstLine(data))
                return Curve(*l);
            if (const auto a = Curves::FirstArc(data))
                return Curve(*a);
            return std::nullopt;
        }

        // 曲线在平面上的两端与转过的角度（度，逆时针为正）；不能是墙的时 why 说为什么
        bool Plan(const Curve& c, P2& start, P2& end, double& arc, std::string& why)
        {
            if (const auto* l = std::get_if<Curves::BoundedLine>(&c))
            {
                const auto s = l->Start(), e = l->End();
                if (std::abs(s[2] - e[2]) > 1e-6)
                {
                    why = "its location line is not level";
                    return false;
                }
                start = Point(s[0], s[1]);
                end   = Point(e[0], e[1]);
                arc   = 0.0;
                return true;
            }
            const auto& a = std::get<Curves::BoundedArc>(c);
            if (std::abs(a.xAxis[2]) > 1e-9 || std::abs(a.yAxis[2]) > 1e-9)
            {
                why = "its location arc is not in plan";
                return false;
            }
            const auto s = a.Point(a.startAngle), e = a.Point(a.endAngle);
            const double sweep = (a.endAngle - a.startAngle) * (180.0 / std::numbers::pi);
            const double turn  = a.CounterClockwise() ? sweep : -sweep;
            start = Point(s[0], s[1]);
            end   = Point(e[0], e[1]);
            arc   = std::round(turn * 1e6) / 1e6;
            return true;
        }

        std::optional<std::string> NameOf(const Facts& f, const Gathered& g, uint32_t tid)
        {
            const auto it = g.data.find(tid);
            if (it == g.data.end())
                return std::nullopt;
            const auto header = f.tags.DataHeader();
            const auto got = Data::Newest<std::string>(it->second, [&](std::span<const uint8_t> b) { return Types::FirstName(b, header, f.release); });
            return got ? std::optional<std::string>(got->first) : std::nullopt;
        }

        // 墙的类型为什么没有层，数据里有类型的名字时说出来：幕墙、店面之类没有
        std::string NoLayers(const Facts& f, const Gathered& g, uint32_t tid)
        {
            if (const auto name = NameOf(f, g, tid))
                return "its type '" + *name + "' has no layers the reader can read";
            return "its type has no layers the reader can read";
        }

        // 记录里没有定位线的墙的线：它自己包围盒的长边，过中间，包围盒的短边是它的厚度。平面上是正方形的包围盒没有线
        std::optional<Curve> BoxLine(const std::array<double, 6>& bbox)
        {
            const double dx = bbox[3] - bbox[0], dy = bbox[4] - bbox[1];
            if (!std::isfinite(dx) || !std::isfinite(dy) || std::min(std::abs(dx), std::abs(dy)) <= 1e-6)
                return std::nullopt;
            const double cx = (bbox[0] + bbox[3]) / 2.0, cy = (bbox[1] + bbox[4]) / 2.0;
            const double z  = (bbox[2] + bbox[5]) / 2.0;
            if (dx >= dy)
                return Curve(Curves::BoundedLine{ -dx / 2.0, dx / 2.0, { cx, cy, z }, { 1.0, 0.0, 0.0 } });
            return Curve(Curves::BoundedLine{ -dy / 2.0, dy / 2.0, { cx, cy, z }, { 0.0, 1.0, 0.0 } });
        }

        // 墙自己的包围盒说它的墙体多厚（毫米）：记录包围盒是墙的线与厚度的矩形，所以横过线的那一边给出厚度。
        // 圆弧的包围盒不是那个矩形，没有。没有复合结构的类型（幕墙、店面）这样从文件读厚度，不由我们给一个
        std::optional<double> BoxThickness(const Curve& c, const std::array<double, 6>& bbox)
        {
            const auto* l = std::get_if<Curves::BoundedLine>(&c);
            if (!l)
                return std::nullopt;
            const auto a = l->Start(), b = l->End();
            const double len = std::hypot(b[0] - a[0], b[1] - a[1]);
            if (len <= 1e-6)
                return std::nullopt;
            const double u = (b[0] - a[0]) / len, v = (b[1] - a[1]) / len;
            const double acrossX = std::abs(u), acrossY = std::abs(v);
            const double boxX = bbox[3] - bbox[0], boxY = bbox[4] - bbox[1];
            double t = 0.0;
            if (acrossY > 1e-3)
                t = (boxX - len * acrossX) / acrossY;
            else if (acrossX > 1e-3)
                t = (boxY - len * acrossY) / acrossX;
            else
                return std::nullopt;
            t = Mm(t);
            if (!(std::isfinite(t) && t > 0.0))
                return std::nullopt;
            return Micron(t);
        }

        // 取整到整数，越界时取最近的端点，同 Rust 的 as i64
        int64_t Saturated(double v)
        {
            if (std::isnan(v))
                return 0;
            if (v >= 9.2e18)
                return INT64_MAX;
            if (v <= -9.2e18)
                return INT64_MIN;
            return static_cast<int64_t>(v);
        }
    }

    void Wanted(const Facts& f, std::set<uint32_t>& out)
    {
        for (const Record* r : f.OfCategory(OstWalls))
            out.insert(r->id);
        if (const auto it = f.types.find(OstWalls); it != f.types.end())
            out.insert(it->second.begin(), it->second.end());
    }

    std::vector<uint32_t> NamedLevels(const std::vector<uint64_t>& refs, const std::map<uint32_t, double>& elevations)
    {
        std::vector<uint32_t> named;
        for (const uint64_t v : refs)
        {
            if (v > UINT32_MAX)
                continue;
            const auto id = static_cast<uint32_t>(v);
            if (elevations.contains(id) && std::find(named.begin(), named.end(), id) == named.end())
                named.push_back(id);
        }
        return named;
    }

    std::optional<uint32_t> LevelAtOrBelow(const std::map<uint32_t, double>& elevations, double z)
    {
        std::optional<std::pair<uint32_t, double>> best;
        bool tied = false;
        for (const auto& [id, at] : elevations)
        {
            if (!(at <= z + 1e-3))
                continue;
            if (best && at < best->second)
                continue;
            if (best && at == best->second)
            {
                tied = true;
                continue;
            }
            best = { id, at };
            tied = false;
        }
        if (best)
            return tied ? std::nullopt : std::optional<uint32_t>(best->first);
        // 第一个最低的
        std::optional<std::pair<uint32_t, double>> low;
        for (const auto& [id, at] : elevations)
            if (!low || Bytes::TotalLess(at, low->second))
                low = { id, at };
        return low ? std::optional<uint32_t>(low->first) : std::nullopt;
    }

    std::pair<std::optional<uint32_t>, std::optional<uint32_t>> Constraints(const Record& r, const std::map<uint32_t, double>& elevations)
    {
        const auto named  = NamedLevels(r.refs, elevations);
        const double bottom = r.bbox[2];
        if (named.size() == 2)
        {
            const uint32_t a = named[0], b = named[1];
            const double ea = elevations.at(a), eb = elevations.at(b);
            if (ea == eb)
                return { LevelAtOrBelow(elevations, bottom), std::nullopt };
            const auto [low, high, highAt] = ea < eb ? std::tuple{ a, b, eb } : std::tuple{ b, a, ea };
            if (highAt <= bottom + 1e-3)
                return { high, low };
            return { low, high };
        }
        if (named.size() == 1)
            return { named[0], std::nullopt };
        return { LevelAtOrBelow(elevations, bottom), std::nullopt };
    }

    Bim::Function FunctionOf(uint32_t revit)
    {
        switch (revit)
        {
        case 1:
        case 200: return Bim::Function::Structure;
        case 3: return Bim::Function::Thermal;
        case 4:
        case 5: return Bim::Function::Finish;
        case 100: return Bim::Function::Membrane;
        default: return Bim::Function::Substrate;
        }
    }

    std::optional<LayeredType> LayeredTypeOf(const Facts& f, const Gathered& g, uint32_t id, int64_t category)
    {
        const auto it = g.data.find(id);
        if (it == g.data.end())
            return std::nullopt;
        const auto layout = Types::LayoutOf(f.release);
        if (!layout || !f.tags.verticalRegions)
            return std::nullopt;
        const uint16_t t = *f.tags.verticalRegions;
        const std::array<uint8_t, 2> tag{ static_cast<uint8_t>(t), static_cast<uint8_t>(t >> 8) };
        const auto layers = Data::Newest<std::vector<Types::CompoundLayer>>(
            it->second, [&](std::span<const uint8_t> b) { return Types::FindLayers(b, tag, *layout, f.materials, f.ids); });
        if (!layers)
            return std::nullopt;
        return Layered(NameOf(f, g, id), layers->first, g, category);
    }

    LayeredType Layered(std::optional<std::string> name, const std::vector<Types::CompoundLayer>& layers, const Gathered& g, int64_t category)
    {
        LayeredType out;
        out.name = std::move(name);
        double thickness = 0.0;
        for (const auto& l : layers)
        {
            // 结构压型板可以从它上面的层取厚度，自己不写；不是膜又没有厚度的层模型存不下，对类型也什么都不加
            if (!(l.widthFeet > 0.0 || l.function == 100))
                continue;
            Bim::Layer layer;
            layer.function = FunctionOf(l.function);
            // 没有自己材质的层取它类别的，对象样式给了时
            std::optional<uint32_t> material = l.material;
            if (!material)
                if (const auto c = g.category.find(category); c != g.category.end())
                    material = c->second;
            if (material)
                if (const auto m = g.materials.find(*material); m != g.materials.end())
                    layer.material = m->second;
            layer.thickness = Mm(l.widthFeet);
            thickness += layer.thickness;
            out.layers.push_back(std::move(layer));
        }
        out.thickness = Micron(thickness);
        return out;
    }

    void Add(Importing& cx, const Facts& f, const Gathered& g)
    {
        const auto typesIt = f.types.find(OstWalls);
        const std::set<uint32_t> types = typesIt != f.types.end() ? typesIt->second : std::set<uint32_t>{};
        std::map<uint32_t, std::optional<LayeredType>> read;
        const auto readType = [&](uint32_t tid) -> const std::optional<LayeredType>&
        {
            auto it = read.find(tid);
            if (it == read.end())
                it = read.emplace(tid, LayeredTypeOf(f, g, tid, OstWalls)).first;
            return it->second;
        };
        const auto copiesOf = [&](uint32_t id) -> const std::vector<Data::Copy>&
        {
            static const std::vector<Data::Copy> none;
            const auto it = g.data.find(id);
            return it != g.data.end() ? it->second : none;
        };
        std::map<std::pair<uint32_t, int64_t>, std::string> made;
        // 根本没有复合结构的类型（幕墙、店面），以及它们自己的墙的包围盒一致的厚度：墙不一致的类型不当作一个平面读，
        // 那个厚度会是我们猜的
        std::map<uint32_t, double> panel;
        {
            std::set<uint32_t> bare;
            for (const uint32_t tid : types)
                if (!readType(tid))
                    bare.insert(tid);
            std::map<uint32_t, std::vector<double>> seen;
            if (!bare.empty())
                for (const Record* r : f.OfCategory(OstWalls))
                {
                    const auto tid = Types::UniqueType(r->refs, types);
                    if (!tid || !bare.contains(*tid))
                        continue;
                    const auto t = Data::Newest<double>(copiesOf(r->id), [&](std::span<const uint8_t> b) -> std::optional<double>
                    {
                        const auto c = CurveOf(b);
                        return c ? BoxThickness(*c, r->bbox) : std::nullopt;
                    });
                    if (t)
                        seen[*tid].push_back(t->first);
                }
            for (const auto& [tid, ts] : seen)
            {
                // 类型的大多数墙一致的厚度（一毫米以内）：类型只有一个厚度，包围盒说法不同的墙（相接处、转角）不让类型成为两种平面
                std::map<int64_t, std::vector<double>> buckets;
                for (const double t : ts)
                    buckets[Saturated(std::round(t * 1000.0)) / 1000].push_back(t);
                // 一样多时取后一个，同 Rust 的 max_by_key
                const std::vector<double>* agreed = nullptr;
                for (const auto& [_, v] : buckets)
                    if (!agreed || v.size() >= agreed->size())
                        agreed = &v;
                if (agreed && agreed->size() * 10 >= ts.size() * 7)
                {
                    double sum = 0.0;
                    for (const double t : *agreed)
                        sum += t;
                    const double t = sum / static_cast<double>(agreed->size());
                    if (t >= 25.0)
                        panel[tid] = Micron(t);
                }
            }
        }
        for (const Record* r : f.OfCategory(OstWalls))
        {
            std::string why;
            std::string note;
            const auto wallOf = [&]() -> std::optional<Bim::Wall>
            {
                const auto& copies = copiesOf(r->id);
                // 记录里没有定位线的墙（这个类别的族实例、花架的构件）沿它自己包围盒的长边进来，同柱以它包围盒的矩形进来，并说明
                bool fromBox = false;
                std::optional<Curve> curve;
                if (const auto got = Data::Newest<Curve>(copies, CurveOf))
                    curve = got->first;
                else
                {
                    curve = BoxLine(r->bbox);
                    if (!curve)
                    {
                        why = "no location line the reader can read, and no box to read one from";
                        return std::nullopt;
                    }
                    fromBox = true;
                }
                std::optional<double> ownThickness = BoxThickness(*curve, r->bbox);
                if (ownThickness)
                    ownThickness = Micron(*ownThickness);
                if (ownThickness && !(*ownThickness >= 25.0))
                    ownThickness.reset();
                P2 start, end;
                double arc = 0.0;
                if (!Plan(*curve, start, end, arc, why))
                    return std::nullopt;
                std::optional<bool> flip;
                if (const auto o = Data::Newest<Types::WallOrientation>(copies, [&](std::span<const uint8_t> b) { return Types::Orientation(b, f.release); }))
                    flip = o->first.flip;
                // 不指墙类别的任何类型的墙（族实例、这个类别的导入对象）以自己的一个类型进来，与它的包围盒说的一样厚；下面的说明这么说
                const uint32_t tid = Types::UniqueType(r->refs, types).value_or(0);
                const auto& fileType = readType(tid);
                // 幕墙或店面没有复合结构：它以自己包围盒给出的平面进来，下面的说明说哪里没读。墙一致的类型取那个厚度；
                // 墙不一致的类型（屋顶上斜放的嵌板）每面墙以它包围盒给的进来
                const auto agreedIt = panel.find(tid);
                const std::optional<double> agreed = agreedIt != panel.end() ? std::optional<double>(agreedIt->second) : std::nullopt;
                std::optional<LayeredType> local;
                if (!fileType)
                {
                    if (const auto t = agreed ? agreed : ownThickness)
                    {
                        // 文件给类型的名字，或像楼板那样导入给叫不出名字的类型起的
                        std::string name = NameOf(f, g, tid).value_or("Imported");
                        if (!agreed)
                            name += " - " + Bim::Text::Shortest(*t) + "mm";
                        Bim::Layer layer;
                        layer.thickness = *t;
                        local = LayeredType{ name, { layer }, *t };
                    }
                }
                const LayeredType* typ = local ? &*local : fileType ? &*fileType : nullptr;
                if (typ && !(typ->thickness > 0.0))
                    typ = nullptr;
                if (!typ)
                {
                    why = NoLayers(f, g, tid);
                    return std::nullopt;
                }
                const auto [base, top] = Constraints(*r, f.elevations);
                const auto levelIt = base ? cx.levels.find(*base) : cx.levels.end();
                if (levelIt == cx.levels.end())
                {
                    why = "it stands on no level";
                    return std::nullopt;
                }
                const double elevation = f.elevations.contains(*base) ? f.elevations.at(*base) : 0.0;
                std::optional<Bim::Guid> topLevel;
                if (top)
                    if (const auto e = f.elevations.find(*top); e != f.elevations.end() && std::abs(e->second - r->bbox[5]) * 304.8 <= 1.0 && e->second > r->bbox[2])
                        if (const auto l = cx.levels.find(*top); l != cx.levels.end())
                            topLevel = l->second;
                const std::pair<uint32_t, int64_t> key{ tid, Saturated(std::round(typ->thickness * 1000.0)) };
                std::string typeName;
                if (const auto m = made.find(key); m != made.end())
                    typeName = m->second;
                else
                {
                    const std::string name = typ->name.value_or("Imported - " + Bim::Text::Shortest(typ->thickness) + "mm");
                    typeName = KindOf(
                        cx.model.types.wall, name, [&](const Bim::WallType& k) { return std::abs(k.thickness - typ->thickness) < 0.05 && k.layers == typ->layers; },
                        Bim::WallType{ typ->thickness, typ->layers });
                    made.emplace(key, typeName);
                }
                // MiniBIM 的墙外侧在左边；flip 设了的 Revit 墙外侧在右边
                if (flip == true)
                {
                    std::swap(start, end);
                    arc = -arc;
                }
                Bim::Wall w;
                w.typeName   = typeName;
                w.level      = levelIt->second;
                w.start      = start;
                w.end        = end;
                w.topLevel   = topLevel;
                w.height     = Mm(r->bbox[5] - r->bbox[2]);
                w.baseOffset = Micron(Mm(r->bbox[2]) - Mm(elevation));
                w.arc        = arc;
                w.location   = Bim::Location::Centre;
                note = tid == 0 ? "no_type" : fromBox ? "from_box" : !flip ? "side" : "";
                if (agreed || local)
                    note = "panels";
                return w;
            };
            if (const auto wall = wallOf())
            {
                const Bim::Id id = cx.model.Insert(*wall);
                cx.from[id]       = "Wall " + std::to_string(r->id);
                cx.walls[r->id]   = id;
                if (note == "side")
                    cx.Note("wall", r->id, "the data does not say which side is the exterior: kept as drawn");
                else if (note == "no_type")
                    cx.Note("wall", r->id, "its record names no type of the wall category: it comes in with a type of its own, as thick as its own box says");
                else if (note == "from_box")
                    cx.Note("wall", r->id,
                            "its record holds no location line (a family instance of the wall category): it comes in along the long side of its own box, as a column comes in as the rectangle of its box");
                else if (note == "panels")
                    cx.Note("wall", r->id,
                            "its type has no layers (a curtain wall or a storefront): it comes in as the plane its own box gives, and the panels and mullions are not read");
            }
            else
            {
                // 类型没有复合层的墙是幕墙（或类似的）：MiniBIM 没有这种元素，立在它里面的门立在没有墙里
                if (why.find("no layers") != std::string::npos)
                    cx.curtain.insert(r->id);
                cx.LeftOut("wall", r->id, why);
            }
        }
    }
}
