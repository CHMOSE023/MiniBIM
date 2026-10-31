// 移植自 BIMCraft crates/rvt/src/rooms.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
#include "Rvt/Rooms.h"

#include "Model/Geom.h"
#include "Model/Text.h"
#include "Rvt/Bytes.h"
#include "Rvt/Walls.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MiniBIM::Rvt::Rooms
{
    using Bim::P2;

    namespace
    {
        // 区域的包围盒能与房间的差多少，毫米
        constexpr double Slack = 600.0;
        // 房间包围盒每边试的点数
        constexpr size_t Grid = 9;

        // 房间 r 平面包围盒里、它标高的墙（all，它们围成的面）围在与包围盒一样大的区域里的一点，离中心最近的
        std::optional<P2> PointIn(const std::vector<Bim::Ring>& all, const Record& r)
        {
            const P2 lo = Point(r.bbox[0], r.bbox[1]), hi = Point(r.bbox[3], r.bbox[4]);
            const P2 middle((lo.x + hi.x) / 2.0, (lo.y + hi.y) / 2.0);
            std::vector<P2> candidates;
            for (size_t k = 0; k < Grid * Grid; ++k)
            {
                const double i = static_cast<double>(k % Grid) + 0.5, j = static_cast<double>(k / Grid) + 0.5;
                candidates.emplace_back(lo.x + (hi.x - lo.x) * i / double(Grid), lo.y + (hi.y - lo.y) * j / double(Grid));
            }
            std::stable_sort(candidates.begin(), candidates.end(), [&](const P2& a, const P2& b) { return Bytes::TotalLess(a.Dist(middle), b.Dist(middle)); });
            candidates.insert(candidates.begin(), middle);
            for (const P2& p : candidates)
            {
                const auto region = Bim::Geom::RegionIn(all, p, Bim::Tol);
                if (!region)
                    continue;
                P2 a(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
                P2 b(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
                for (const P2& q : region->outer)
                {
                    a = P2(std::fmin(a.x, q.x), std::fmin(a.y, q.y));
                    b = P2(std::fmax(b.x, q.x), std::fmax(b.y, q.y));
                }
                const auto near = [](double x, double y) { return std::abs(x - y) <= Slack; };
                if (near(a.x, lo.x) && near(a.y, lo.y) && near(b.x, hi.x) && near(b.y, hi.y) && std::abs(Bim::Geom::Area(region->outer)) > 0.0)
                    return p;
            }
            return std::nullopt;
        }
    }

    void Wanted(const Facts& f, Wants& out)
    {
        for (const Record* r : f.OfCategory(OstRooms))
            out.rooms.insert(r->id);
    }

    void Add(Importing& cx, const Facts& f, const Gathered& g)
    {
        std::set<uint32_t> seen;
        // 每个标高的墙围成的面，为它所有的房间只建一次：房间要试一格点，房间进来时标高的墙不变
        std::map<Bim::Guid, std::vector<Bim::Ring>> faces;
        for (const Record* r : f.OfCategory(OstRooms))
        {
            if (!seen.insert(r->id).second)
                continue;
            std::string why;
            const auto made = [&]() -> std::optional<Bim::Room>
            {
                const auto [base, _] = Walls::Constraints(*r, f.elevations);
                const auto levelIt = base ? cx.levels.find(*base) : cx.levels.end();
                if (levelIt == cx.levels.end())
                {
                    why = "it stands on no level";
                    return std::nullopt;
                }
                const Bim::Guid level = levelIt->second;
                auto it = faces.find(level);
                if (it == faces.end())
                    it = faces.emplace(level, Bim::Geom::Faces(cx.model.BoundingLines(level), Bim::Tol)).first;
                const auto point = PointIn(it->second, *r);
                if (!point)
                {
                    why = "the walls imported do not enclose its box (curtain walls and room separation lines are not imported)";
                    return std::nullopt;
                }
                std::string number, name;
                if (const auto entry = g.rooms.find(r->id); entry != g.rooms.end())
                    std::tie(number, name) = entry->second;
                Bim::Room room;
                room.level  = level;
                room.point  = *point;
                room.name   = Bim::Text::Trim(name).empty() ? std::string("Room") : Clip(name);
                room.number = Clip(number);
                return room;
            };
            if (const auto room = made())
            {
                const Bim::Id id = cx.model.Insert(*room);
                cx.from[id] = "Room " + std::to_string(r->id);
            }
            else
                cx.LeftOut("room", r->id, why);
        }
    }
}
