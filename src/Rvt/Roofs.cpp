// 移植自 BIMCraft crates/rvt/src/roofs.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
#include "Rvt/Roofs.h"

#include "Model/Text.h"
#include "Rvt/Curves.h"
#include "Rvt/Data.h"
#include "Rvt/Floors.h"
#include "Rvt/Types.h"
#include "Rvt/Walls.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MiniBIM::Rvt::Roofs
{
    using Vec3 = std::array<double, 3>;

    namespace
    {
        // 英尺里的一毫米：读出的东西的角与记录包围盒相差不能超过的
        constexpr double FeetMm = 1.0 / 304.8;

        // 屋顶轮廓最多的线数：它的链靠两两比较找出，恶意文件的几千条不去试
        constexpr size_t MaxLines = 256;

        // 线段首尾相接成一条开着的链：它按顺序的各点，从一端起。连不成一条时（有缺口、分叉、成环）为空
        std::optional<std::vector<Vec3>> Chain(const std::vector<std::pair<Vec3, Vec3>>& segments)
        {
            const auto same = [](const Vec3& a, const Vec3& b)
            {
                for (size_t i = 0; i < 3; ++i)
                    if (!(std::abs(a[i] - b[i]) < 1e-6))
                        return false;
                return true;
            };
            if (segments.empty())
                return std::nullopt;
            std::vector<std::pair<Vec3, Vec3>> rest(segments.begin() + 1, segments.end());
            std::vector<Vec3> points{ segments[0].first, segments[0].second };
            while (!rest.empty())
            {
                const Vec3 tail = points.back(), head = points.front();
                const auto at = std::find_if(rest.begin(), rest.end(), [&](const auto& s)
                                             { return same(s.first, tail) || same(s.second, tail) || same(s.first, head) || same(s.second, head); });
                if (at == rest.end())
                    return std::nullopt;
                const auto [a, b] = *at;
                // 同 Rust 的 swap_remove：最后一个挪到它的位置
                *at = rest.back();
                rest.pop_back();
                if (same(a, tail))
                    points.push_back(b);
                else if (same(b, tail))
                    points.push_back(a);
                else if (same(a, head))
                    points.insert(points.begin(), b);
                else
                    points.insert(points.begin(), a);
            }
            if (same(points.front(), points.back()))
                return std::nullopt;
            return points;
        }

        // （沿线、向上）英尺的顶面轮廓 top 垂直于每个坡面厚 thickness、两端竖直切开时的底面：每一段下移 thickness 除以坡度的
        // 余弦，相邻的在那些线相交处相接。这样不能让各点一路往前时为空
        std::optional<std::vector<std::pair<double, double>>> Underside(const std::vector<std::pair<double, double>>& top, double thickness)
        {
            std::vector<double> slopes, drops;
            for (size_t i = 0; i + 1 < top.size(); ++i)
                slopes.push_back((top[i + 1].second - top[i].second) / (top[i + 1].first - top[i].first));
            for (const double m : slopes)
                drops.push_back(thickness * std::hypot(m, 1.0));
            std::vector<std::pair<double, double>> out{ { top[0].first, top[0].second - drops[0] } };
            for (size_t k = 1; k + 1 < top.size(); ++k)
            {
                const double m0 = slopes[k - 1], m1 = slopes[k], d0 = drops[k - 1], d1 = drops[k];
                const double shift = std::abs(m0 - m1) < 1e-12 ? 0.0 : (d0 - d1) / (m0 - m1);
                out.emplace_back(top[k].first + shift, top[k].second + m0 * shift - d0);
            }
            out.emplace_back(top.back().first, top.back().second - drops.back());
            for (size_t i = 0; i + 1 < out.size(); ++i)
                if (!(out[i + 1].first - out[i].first >= FeetMm && std::isfinite(out[i].second) && std::isfinite(out[i + 1].second)))
                    return std::nullopt;
            return out;
        }

        std::set<uint32_t> RoofTypes(const Facts& f)
        {
            const auto it = f.types.find(OstRoofs);
            return it != f.types.end() ? it->second : std::set<uint32_t>{};
        }

        // 面没读的屋顶 r，以它包围盒的轮廓：在它底面立在的标高上，与类型一样厚
        bool Boxed(Importing& cx, const Facts& f, const Gathered& g, const Record& r, const Floors::TypeNamed& typeNamed, std::string& why)
        {
            const uint32_t tid = Types::UniqueType(r.refs, RoofTypes(f)).value_or(0);
            const double tall = Mm(r.Extent()[2]);
            auto typ = Walls::LayeredTypeOf(f, g, tid, OstRoofs);
            if (typ && !(typ->thickness > 0.0))
                typ.reset();
            if (!typ)
            {
                Bim::Layer layer;
                layer.thickness = tall;
                typ = Walls::LayeredType{ std::nullopt, { layer }, tall };
            }
            const auto level = Floors::LevelOf(r, f.elevations, r.bbox[2]);
            const auto model = level ? cx.levels.find(*level) : cx.levels.end();
            if (model == cx.levels.end())
            {
                why = "it stands on no level";
                return false;
            }
            const double offset = Micron(Mm(r.bbox[2]) - Mm(f.elevations.contains(*level) ? f.elevations.at(*level) : 0.0));
            const std::string typeName = typeNamed(cx, tid, *typ);
            const double x0 = r.bbox[0], y0 = r.bbox[1], x1 = r.bbox[3], y1 = r.bbox[4];
            if (!(x1 - x0 > 0.0 && y1 - y0 > 0.0))
            {
                why = "its box has no footprint";
                return false;
            }
            Bim::Roof roof;
            roof.typeName = typeName;
            roof.level    = model->second;
            roof.boundary = Floors::RingOf({ { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } });
            roof.offset   = offset;
            const Bim::Id id = cx.model.Insert(roof);
            cx.from[id] = "Roof " + std::to_string(r.id);
            return true;
        }

        // 拉伸屋顶 r：它草图线的链是顶面的轮廓，横过包围盒的宽度扫过去，垂直于每个坡面与类型一样厚。没进来时 why 说为什么
        bool Extruded(Importing& cx, const Facts& f, const Gathered& g, const Record& r, const std::map<uint32_t, std::vector<const Record*>>& sketches,
                      const Floors::TypeNamed& typeNamed, std::string& why)
        {
            const auto lines = sketches.find(r.id);
            if (lines == sketches.end())
            {
                why = "it has no sketch lines";
                return false;
            }
            if (lines->second.size() > MaxLines)
            {
                why = "its profile has " + std::to_string(lines->second.size()) + " lines";
                return false;
            }
            std::vector<std::pair<Vec3, Vec3>> ends;
            for (const Record* l : lines->second)
            {
                std::optional<Curves::BoundedLine> line;
                if (const auto it = g.data.find(l->id); it != g.data.end())
                    if (const auto got = Data::Newest<Curves::BoundedLine>(it->second, Curves::FirstLine))
                        line = got->first;
                if (!line)
                {
                    why = "its profile has a line that is not a straight one the reader can read (an arc, or no curve in its data)";
                    return false;
                }
                ends.emplace_back(line->Start(), line->End());
            }
            auto chain = Chain(ends);
            if (!chain)
            {
                why = "its profile's lines do not make one open chain";
                return false;
            }
            // 不论线按什么顺序、什么方向列出，它都朝一个方向走：向东，垂直于东时向北
            const double dx = chain->back()[0] - chain->front()[0], dy = chain->back()[1] - chain->front()[1];
            if (dx < -1e-6 || (std::abs(dx) <= 1e-6 && dy < 0.0))
                std::reverse(chain->begin(), chain->end());
            const Vec3 first = chain->front(), last = chain->back();
            const double run = std::hypot(last[0] - first[0], last[1] - first[1]);
            if (run < FeetMm)
            {
                why = "its profile does not run along a plan line";
                return false;
            }
            const std::array<double, 2> u{ (last[0] - first[0]) / run, (last[1] - first[1]) / run };
            if (std::min(std::abs(u[0]), std::abs(u[1])) > 1e-9)
            {
                why = "its profile's plane is not square to an axis, so its box does not say how far it is swept";
                return false;
            }
            const std::array<double, 2> n{ -u[1], u[0] };
            const auto along  = [&](const Vec3& p) { return (p[0] - first[0]) * u[0] + (p[1] - first[1]) * u[1]; };
            const auto across = [&](const Vec3& p) { return (p[0] - first[0]) * n[0] + (p[1] - first[1]) * n[1]; };
            std::vector<std::pair<double, double>> top;
            bool straight = true;
            for (const Vec3& p : *chain)
            {
                top.emplace_back(along(p), p[2]);
                straight = straight && std::abs(across(p)) < 1e-6;
            }
            bool onward = true;
            for (size_t i = 0; i + 1 < top.size(); ++i)
                onward = onward && !(top[i + 1].first - top[i].first < FeetMm);
            if (!straight || !onward)
            {
                why = "its profile is not one upright plane running one way along its line";
                return false;
            }
            const auto tid = Types::UniqueType(r.refs, RoofTypes(f));
            if (!tid)
            {
                why = "its record does not name one roof type";
                return false;
            }
            auto typ = Walls::LayeredTypeOf(f, g, *tid, OstRoofs);
            if (!typ || !(typ->thickness > 0.0))
            {
                why = "its type has no layers the reader can read";
                return false;
            }
            const auto under = Underside(top, typ->thickness / 304.8);
            if (!under)
            {
                why = "its type is thicker than its profile's stretches allow";
                return false;
            }
            double z0 = std::numeric_limits<double>::infinity(), z1 = -std::numeric_limits<double>::infinity();
            for (const auto& p : *under)
                z0 = std::fmin(z0, p.second);
            for (const auto& p : top)
                z1 = std::fmax(z1, p.second);
            // 轮廓与类型构成的包围盒就是记录存的那个：沿着轮廓、横过它、向上
            double aLo = std::numeric_limits<double>::infinity(), aHi = -aLo, bLo = aLo, bHi = -aLo;
            for (int k = 0; k < 8; ++k)
            {
                const Vec3 c{ r.bbox[3 * (k & 1)], r.bbox[1 + 3 * ((k >> 1) & 1)], r.bbox[2 + 3 * ((k >> 2) & 1)] };
                const double a = along(c), b = across(c);
                aLo = std::fmin(aLo, a);
                aHi = std::fmax(aHi, a);
                bLo = std::fmin(bLo, b);
                bHi = std::fmax(bHi, b);
            }
            const double length = top.back().first;
            const bool fits = std::abs(aLo) <= FeetMm && std::abs(aHi - length) <= FeetMm && std::abs(r.bbox[2] - z0) <= FeetMm && std::abs(r.bbox[5] - z1) <= FeetMm;
            if (!fits || bHi - bLo < FeetMm)
            {
                why = "its box is not the one its profile and its type's thickness square to each slope make";
                return false;
            }
            const auto level = Floors::LevelOf(r, f.elevations, z0);
            const auto model = level ? cx.levels.find(*level) : cx.levels.end();
            if (model == cx.levels.end())
            {
                why = "it stands on no level";
                return false;
            }
            const double offset = Micron(Mm(z0) - Mm(f.elevations.contains(*level) ? f.elevations.at(*level) : 0.0));
            Bim::Roof roof;
            roof.typeName = typeNamed(cx, *tid, *typ);
            roof.level    = model->second;
            roof.offset   = offset;
            Bim::Extrusion x;
            x.line = { Bim::P2(Mm(first[0]), Mm(first[1])), Bim::P2(Mm(last[0]), Mm(last[1])) };
            for (const auto& p : *under)
                x.profile.emplace_back(Mm(p.first), Mm(p.second - z0));
            x.from        = Mm(bLo);
            x.to          = Mm(bHi);
            roof.extrusion = x;
            const Bim::Id id = cx.model.Insert(roof);
            cx.from[id]     = "Roof " + std::to_string(r.id);
            cx.roofs[r.id]  = id;
            return true;
        }
    }

    void Wanted(const Facts& f, std::set<uint32_t>& out)
    {
        const auto sketches = Floors::SketchLines(f);
        for (const Record* r : f.OfCategory(OstRoofs))
        {
            out.insert(r->id);
            if (const auto it = sketches.find(r->id); it != sketches.end())
                for (const Record* l : it->second)
                    out.insert(l->id);
        }
        if (const auto it = f.types.find(OstRoofs); it != f.types.end())
            out.insert(it->second.begin(), it->second.end());
    }

    void Add(Importing& cx, const Facts& f, const Gathered& g)
    {
        const auto sketches = Floors::SketchLines(f);
        std::map<uint32_t, std::string> made;
        const Floors::TypeNamed name = [&](Importing& c, uint32_t tid, const Walls::LayeredType& typ)
        {
            auto it = made.find(tid);
            if (it == made.end())
            {
                const std::string base = typ.name.value_or("Imported - " + Bim::Text::Shortest(typ.thickness) + "mm");
                it = made.emplace(tid, KindOf(c.model.types.roof, base,
                                              [&](const Bim::RoofType& k) { return std::abs(k.thickness - typ.thickness) < 0.05 && k.layers == typ.layers; },
                                              Bim::RoofType{ typ.thickness, typ.layers }))
                         .first;
            }
            return it->second;
        };
        for (const Record* r : f.OfCategory(OstRoofs))
        {
            std::string why;
            if (f.tags.profileRoof != r->classTag)
            {
                if (f.tags.roof == r->classTag)
                {
                    if (!Extruded(cx, f, g, *r, sketches, name, why))
                        cx.LeftOut("roof", r->id, why);
                }
                // 按面的屋顶（比如在体量上）没有平面线的草图可闭合，也没有轮廓可扫。它以自己包围盒的轮廓进来，并说明它的面没读
                else if (Boxed(cx, f, g, *r, name, why))
                    cx.Note("roof", r->id, "it is a roof by face (over a mass, say), whose face is not read: it comes in as the footprint of its own box");
                else
                    cx.LeftOut("roof", r->id, why);
                continue;
            }
            Floors::Sketched s;
            if (!Floors::SketchedOf(cx, f, g, *r, Floors::Roof, sketches, name, s, why))
            {
                // BIMCraft 早先的说法换成现在的
                const std::string from = "sloped or stepped roof, which is not described";
                if (const size_t at = why.find(from); at != std::string::npos)
                    why.replace(at, from.size(), "sloped or stepped roof, whose slopes are not read");
                cx.LeftOut("roof", r->id, why);
                continue;
            }
            if (s.pieces.size() > 1)
                cx.Note("roof", r->id, "its sketch is several separate loops: one roof for each");
            for (const auto& p : s.pieces)
            {
                Bim::Roof roof;
                roof.typeName = s.typeName;
                roof.level    = s.level;
                roof.boundary = Floors::RingOf(p.outer);
                for (const auto& h : p.holes)
                    roof.courtyards.push_back(Floors::RingOf(h));
                roof.offset = s.offset;
                const Bim::Id id = cx.model.Insert(roof);
                cx.from[id] = "Roof " + std::to_string(r->id);
                // 墙的顶部只跟着与记录说的一样的屋顶：坡屋顶放平的替身，或分成几块的屋顶，会把山墙在檐口切掉
                if (s.flat && s.pieces.size() == 1)
                    cx.roofs[r->id] = id;
            }
        }
    }
}
