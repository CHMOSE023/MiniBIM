// 移植自 BIMCraft crates/rvt/src/openings.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
#include "Rvt/Openings.h"

#include "Rvt/Floors.h"
#include "Rvt/Roofs.h"
#include "Rvt/Walls.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace MiniBIM::Rvt::Openings
{
    using Bim::P2;

    namespace
    {
        // 记录说的可能是它宿主的墙，近的在前：它自己 id 以下的，近的在前，然后是以上的
        std::vector<uint32_t> Candidates(const Record& r, const std::set<uint32_t>& walls)
        {
            std::vector<uint32_t> below, above;
            for (const uint64_t v : r.refs)
            {
                if (v > UINT32_MAX)
                    continue;
                const auto id = static_cast<uint32_t>(v);
                if (id == r.id || !walls.contains(id))
                    continue;
                (id < r.id ? below : above).push_back(id);
            }
            std::reverse(below.begin(), below.end());
            below.insert(below.end(), above.begin(), above.end());
            return below;
        }

        double Signum(double v) { return std::isnan(v) ? v : std::copysign(1.0, v); }

        // 斜墙能离 45° 多近（Turned）：在那里两个方程一样，宽度淹没在误差里
        constexpr double MinTurn = 0.1;

        // 斜墙的 Along。包围盒平行于坐标轴，所以它是转到墙的方向的一个矩形的范围：墙的方向是 (cos, sin) 时，它在 x 上的范围是
        // 宽 · |cos| + 深 · |sin|，在 y 上是 宽 · |sin| + 深 · |cos|，宽度与横过墙的深度的两个方程。45° 附近（|cos² − sin²| 小于
        // MinTurn）它们是一个方程，不是转过的矩形的包围盒（宽或深为负）也没有。包围盒的中心是洞的中心
        std::optional<std::pair<double, double>> Turned(P2 start, P2 end, const std::array<double, 6>& bbox)
        {
            const double dx = end.x - start.x, dy = end.y - start.y;
            const double length = std::hypot(dx, dy);
            if (length <= 0.01)
                return std::nullopt;
            const double cos = std::abs(dx / length), sin = std::abs(dy / length);
            const double det = cos * cos - sin * sin;
            if (std::abs(det) < MinTurn)
                return std::nullopt;
            const double ex = Mm(bbox[3]) - Mm(bbox[0]), ey = Mm(bbox[4]) - Mm(bbox[1]);
            const double width = (cos * ex - sin * ey) / det, depth = (cos * ey - sin * ex) / det;
            const double x = (Mm(bbox[0]) + Mm(bbox[3])) / 2.0, y = (Mm(bbox[1]) + Mm(bbox[4])) / 2.0;
            const double offset = ((x - start.x) * dx + (y - start.y) * dy) / length;
            if (!(width > 0.0 && depth > 0.0 && std::isfinite(offset)))
                return std::nullopt;
            return std::pair{ Micron(width), Micron(offset) };
        }

        // 包围盒在墙上的位置：它的宽度与中心离墙起点的距离，都是毫米。沿坐标轴的墙由包围盒在那根轴上的范围给出；斜墙由 Turned。
        // 包围盒给不出时为空
        std::optional<std::pair<double, double>> Along(P2 start, P2 end, const std::array<double, 6>& bbox)
        {
            const double dx = end.x - start.x, dy = end.y - start.y;
            double lo = 0.0, hi = 0.0, from = 0.0, to = 0.0;
            if (std::abs(dy) <= 0.01 && std::abs(dx) > 0.01)
            {
                lo = bbox[0], hi = bbox[3], from = start.x, to = end.x;
            }
            else if (std::abs(dx) <= 0.01 && std::abs(dy) > 0.01)
            {
                lo = bbox[1], hi = bbox[4], from = start.y, to = end.y;
            }
            else
                return Turned(start, end, bbox);
            const double middle = (Mm(lo) + Mm(hi)) / 2.0;
            return std::pair{ Micron(Mm(hi) - Mm(lo)), Micron((middle - from) * Signum(to - from)) };
        }

        // 从记录读出、还没与邻居的洞对好的门窗
        struct Pending
        {
            uint32_t id   = 0;
            bool     door = false;
            Bim::Id  host = 0;
            std::string name;
            // 沿墙的洞：从、到，离墙起点的毫米
            std::pair<double, double> span;
            double height = 0.0;
            double sill   = 0.0;
            // 洞被裁到墙给得出的范围：它的包围盒超出了
            bool clamped = false;
        };

        // 相邻两个洞重叠最多多少（较窄那个的份额）还对得好：Revit 允许窗重叠，MiniBIM 不允许
        constexpr double MaxFittedOverlap = 0.1;

        struct Candidate
        {
            uint32_t host   = 0;
            Bim::Id  hostId = 0;
            double   width  = 0.0;
            double   offset = 0.0;
            double   height = 0.0;
            double   sill   = 0.0;
        };
    }

    void Wanted(const Facts&, std::set<uint32_t>&) {}

    std::optional<std::pair<uint32_t, std::string>> SymbolOf(const Record& r, const std::set<uint32_t>& symbols, const Gathered& g, int64_t category)
    {
        std::vector<std::pair<uint32_t, std::string>> named;
        for (const uint64_t v : r.refs)
        {
            if (v > UINT32_MAX)
                continue;
            const auto id = static_cast<uint32_t>(v);
            if (id == r.id || !symbols.contains(id))
                continue;
            const auto it = g.names.find(id);
            if (it == g.names.end() || (it->second.second && *it->second.second != category))
                continue;
            named.emplace_back(id, it->second.first);
        }
        std::sort(named.begin(), named.end());
        named.erase(std::unique(named.begin(), named.end()), named.end());
        std::set<std::string> names;
        for (const auto& [_, n] : named)
            names.insert(n);
        if (!named.empty() && names.size() == 1)
            return named.front();
        return std::nullopt;
    }

    std::optional<std::string> SymbolName(const Record& r, const std::set<uint32_t>& symbols, const Gathered& g, int64_t category)
    {
        if (const auto s = SymbolOf(r, symbols, g, category))
            return s->second;
        return std::nullopt;
    }

    void Add(Importing& cx, const Facts& f, const Gathered& g)
    {
        std::set<uint32_t> walls;
        for (const Record* r : f.OfCategory(Walls::OstWalls))
            walls.insert(r->id);
        std::vector<Pending> pending;
        for (const auto& [category, label] : { std::pair{ OstDoors, "door" }, std::pair{ OstWindows, "window" } })
        {
            const auto symbolsIt = f.symbols.find(category);
            const std::set<uint32_t> symbols = symbolsIt != f.symbols.end() ? symbolsIt->second : std::set<uint32_t>{};
            for (const Record* r : f.OfCategory(category))
            {
                std::string why;
                const auto read = [&]() -> std::optional<Pending>
                {
                    const auto named = Candidates(*r, walls);
                    if (named.empty())
                    {
                        // 立在屋顶或楼板里的窗是天窗：MiniBIM 的窗立在墙里，没有给别的平面的洞口，所以略去
                        for (const int64_t c : { Roofs::OstRoofs, Floors::OstFloors, Floors::OstCeilings })
                            for (const Record* o : f.OfCategory(c))
                                if (std::find(r->refs.begin(), r->refs.end(), uint64_t(o->id)) != r->refs.end())
                                {
                                    why = "it is set in a roof or a floor (a skylight), and BIMCraft's windows stand in walls";
                                    return std::nullopt;
                                }
                        why = "its record names no wall";
                        return std::nullopt;
                    }
                    // 宿主是列表说的最近的、门窗放得下的墙（RE-23、RE-85）；一面也放不下时取说的最近的，洞裁到那面墙给得出的。
                    // 墙的包围盒把门窗截短是文件自己的不一致：Revit 把门框画得比墙的包围盒出去一点
                    std::optional<Candidate> first, fits;
                    std::optional<std::string> unreadable;
                    for (const uint32_t host : named)
                    {
                        if (cx.curtain.contains(host))
                            continue;
                        const auto hostIt = cx.walls.find(host);
                        if (hostIt == cx.walls.end())
                            continue;
                        const Bim::Wall* wall = cx.model.GetWall(hostIt->second);
                        if (!wall)
                            continue;
                        if (wall->arc != 0.0)
                        {
                            if (!unreadable)
                                unreadable = "its host wall " + std::to_string(host) + " is curved, and a curved wall's box gives no width";
                            continue;
                        }
                        const auto at = Along(wall->start, wall->end, r->bbox);
                        if (!at)
                        {
                            if (!unreadable)
                                unreadable = "its host wall " + std::to_string(host) + " runs at an angle, and the box of a door or window in it gives no width";
                            continue;
                        }
                        const auto [width, offset] = *at;
                        const double height = Micron(Mm(r->bbox[5]) - Mm(r->bbox[2]));
                        double sill = Micron(Mm(r->bbox[2]) - cx.model.OwnBase(*wall));
                        if (sill >= -2.0 && sill < 2.0)
                            sill = 0.0;
                        const double longest = wall->start.Dist(wall->end), tall = cx.model.TopZ(*wall) - cx.model.OwnBase(*wall);
                        const bool fitsHere =
                            offset - width / 2.0 >= -1.0 && offset + width / 2.0 <= longest + 1.0 && sill >= 0.0 && sill + height <= tall + 1.0;
                        const Candidate candidate{ host, hostIt->second, width, offset, height, sill };
                        if (!first)
                            first = candidate;
                        if (fitsHere)
                        {
                            fits = candidate;
                            break;
                        }
                    }
                    const auto chosen = fits ? fits : first;
                    if (!chosen)
                    {
                        if (unreadable)
                            why = *unreadable;
                        else if (std::any_of(named.begin(), named.end(), [&](uint32_t w) { return !cx.curtain.contains(w); }))
                            why = "its host wall " + std::to_string(named[0]) + " was left out";
                        else
                            why = "its host is a curtain wall, which BIMCraft has no element for";
                        return std::nullopt;
                    }
                    const Bim::Wall* wall = cx.model.GetWall(chosen->hostId);
                    if (!wall)
                    {
                        why = "its host wall is gone";
                        return std::nullopt;
                    }
                    const double longest = wall->start.Dist(wall->end), tall = cx.model.TopZ(*wall) - cx.model.OwnBase(*wall);
                    const double sill = std::fmax(chosen->sill, 0.0);
                    double offset = chosen->offset;
                    const bool overruns = (offset - chosen->width / 2.0) < -1.0 || (offset + chosen->width / 2.0) > longest + 1.0 || sill + chosen->height > tall + 1.0;
                    const double width = std::fmin(chosen->width, longest);
                    if (offset - width / 2.0 < 0.0)
                        offset = width / 2.0;
                    if (offset + width / 2.0 > longest)
                        offset = longest - width / 2.0;
                    const double height = std::fmin(chosen->height, std::fmax(tall - sill, 0.0));
                    if (width <= 0.0 || height <= 0.0)
                    {
                        why = "its host wall " + std::to_string(chosen->host) + " leaves no room for it";
                        return std::nullopt;
                    }
                    Pending p;
                    p.id      = r->id;
                    p.door    = std::string_view(label) == "door";
                    p.host    = chosen->hostId;
                    p.name    = SymbolName(*r, symbols, g, category).value_or(std::string("Imported ") + label);
                    p.span    = { offset - width / 2.0, offset + width / 2.0 };
                    p.height  = height;
                    p.sill    = sill;
                    p.clamped = overruns;
                    return p;
                };
                if (auto p = read())
                    pending.push_back(std::move(*p));
                else
                    cx.LeftOut(label, r->id, why);
            }
        }
        // 墙里重叠一点的邻居对好：各让出一半的重叠
        std::vector<size_t> order(pending.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b)
        {
            if (pending[a].host != pending[b].host)
                return pending[a].host < pending[b].host;
            return pending[a].span.first < pending[b].span.first;
        });
        for (size_t k = 0; k + 1 < order.size(); ++k)
        {
            Pending& a = pending[order[k]];
            Pending& b = pending[order[k + 1]];
            if (a.host != b.host)
                continue;
            const double overlap  = a.span.second - b.span.first;
            const double narrower = std::fmin(a.span.second - a.span.first, b.span.second - b.span.first);
            if (overlap > 0.0 && overlap <= MaxFittedOverlap * narrower)
            {
                const double middle = Micron((a.span.second + b.span.first) / 2.0);
                a.span.second = middle;
                b.span.first  = middle;
                for (const uint32_t id : { a.id, b.id })
                    cx.Note("opening", id, "it overlaps a neighbour in its wall by up to a tenth of its width, which BIMCraft does not allow: narrowed to fit");
            }
        }
        for (const Pending& p : pending)
        {
            const double width = Micron(p.span.second - p.span.first), offset = Micron((p.span.first + p.span.second) / 2.0);
            auto& types = p.door ? cx.model.types.door : cx.model.types.window;
            Bim::OpeningType made;
            made.width  = width;
            made.height = p.height;
            made.sill   = p.sill;
            const std::string typeName = KindOf(
                types, p.name, [&](const Bim::OpeningType& t) { return std::abs(t.width - width) < 0.05 && std::abs(t.height - p.height) < 0.05; }, made);
            Bim::Opening o;
            o.typeName = typeName;
            o.host     = cx.model.GuidOf(p.host);
            o.offset   = offset;
            o.sill     = p.sill;
            if (p.clamped)
                cx.Note("opening", p.id, "its box overruns its wall, and it comes in as far as the wall goes");
            Bim::Id id = 0;
            if (p.door)
            {
                Bim::Door d;
                static_cast<Bim::Opening&>(d) = o;
                id = cx.model.Insert(d);
                cx.from[id] = "Door " + std::to_string(p.id);
            }
            else
            {
                Bim::Window w;
                static_cast<Bim::Opening&>(w) = o;
                id = cx.model.Insert(w);
                cx.from[id] = "Window " + std::to_string(p.id);
            }
        }
    }
}
