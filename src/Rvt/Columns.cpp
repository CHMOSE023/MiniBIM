// 移植自 BIMCraft crates/rvt/src/columns.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
#include "Rvt/Columns.h"

#include "Rvt/Data.h"
#include "Rvt/Openings.h"
#include "Rvt/Placement.h"
#include "Rvt/Walls.h"

#include <cmath>
#include <numbers>

namespace MiniBIM::Rvt::Columns
{
    void Wanted(const Facts& f, Wants& out)
    {
        for (const int64_t category : { OstColumns, OstStructuralColumns })
            for (const Record* r : f.OfCategory(category))
            {
                out.data.insert(r->id);
                out.columns[r->id] = r->bbox;
            }
    }

    void Add(Importing& cx, const Facts& f, const Gathered& g)
    {
        for (const auto& [category, structural] : { std::pair{ OstColumns, false }, std::pair{ OstStructuralColumns, true } })
        {
            const auto symbolsIt = f.symbols.find(category);
            const std::set<uint32_t> symbols = symbolsIt != f.symbols.end() ? symbolsIt->second : std::set<uint32_t>{};
            for (const Record* original : f.OfCategory(category))
            {
                std::string why;
                bool fellBack = false;
                const auto made = [&]() -> std::optional<Bim::Column>
                {
                    // 包围盒从地面起而柱从更高处起：几何节点的底部更真
                    Record r = *original;
                    if (const auto b = g.columnBottoms.find(r.id); b != g.columnBottoms.end())
                        r.bbox[2] = b->second;
                    const auto [base, top] = Walls::Constraints(r, f.elevations);
                    const auto levelIt = base ? cx.levels.find(*base) : cx.levels.end();
                    if (levelIt == cx.levels.end())
                    {
                        why = "it stands on no level";
                        return std::nullopt;
                    }
                    const double elevation = f.elevations.contains(*base) ? f.elevations.at(*base) : 0.0;
                    std::optional<Bim::Guid> topLevel;
                    if (top)
                        if (const auto e = f.elevations.find(*top); e != f.elevations.end() && std::abs(e->second - r.bbox[5]) * 304.8 <= 1.0 && e->second > r.bbox[2])
                            if (const auto l = cx.levels.find(*top); l != cx.levels.end())
                                topLevel = l->second;
                    // 转过的角度不是直角：族符号的外包框，就是转过之前的截面
                    std::optional<Placement::Transform> placed;
                    if (const auto d = g.data.find(r.id); d != g.data.end())
                        if (const auto t = Data::Newest<Placement::Transform>(d->second, Placement::TransformOf))
                            placed = t->first;
                    double angle = 0.0;
                    if (placed)
                        if (const auto a = placed->PlanAngle())
                            angle = *a * (180.0 / std::numbers::pi);
                    const bool quarter = std::abs(angle / 90.0 - std::round(angle / 90.0)) < 1e-6;
                    double width = Mm(r.bbox[3] - r.bbox[0]), depth = Mm(r.bbox[4] - r.bbox[1]);
                    Bim::P2 at = Point((r.bbox[0] + r.bbox[3]) / 2.0, (r.bbox[1] + r.bbox[4]) / 2.0);
                    double turned = 0.0;
                    if (placed && !quarter)
                    {
                        turned = Micron(angle);
                        // 转过的柱的包围盒是围着它的那个，所以文件没给符号大小时以它的包围盒进来
                        const auto symbol = Openings::SymbolOf(r, symbols, g, category);
                        const auto box    = symbol ? f.symbolBoxes.find(symbol->first) : f.symbolBoxes.end();
                        if (box != f.symbolBoxes.end())
                        {
                            const auto& sym = box->second;
                            width = Mm(sym[3] - sym[0]);
                            depth = Mm(sym[4] - sym[1]);
                            at    = Point(placed->origin[0], placed->origin[1]);
                        }
                        else
                            fellBack = true;
                    }
                    const std::string name = Openings::SymbolName(r, symbols, g, category).value_or("Imported column");
                    auto& types = structural ? cx.model.types.structuralColumn : cx.model.types.column;
                    const std::string typeName = KindOf(
                        types, name, [&](const Bim::ColumnType& t) { return std::abs(t.width - width) < 0.05 && std::abs(t.depth - depth) < 0.05 && !t.round; },
                        Bim::ColumnType{ width, depth, false });
                    Bim::Column c;
                    c.typeName   = typeName;
                    c.level      = levelIt->second;
                    c.at         = at;
                    c.angle      = turned;
                    c.topLevel   = topLevel;
                    c.height     = Mm(r.bbox[5] - r.bbox[2]);
                    c.baseOffset = Micron(Mm(r.bbox[2]) - Mm(elevation));
                    return c;
                };
                const auto column = made();
                if (!column)
                {
                    cx.LeftOut("column", original->id, why);
                    continue;
                }
                Bim::Id id = 0;
                if (structural)
                {
                    Bim::StructuralColumn s;
                    static_cast<Bim::Column&>(s) = *column;
                    id = cx.model.Insert(s);
                    cx.from[id] = "Structural column " + std::to_string(original->id);
                }
                else
                {
                    id = cx.model.Insert(*column);
                    cx.from[id] = "Column " + std::to_string(original->id);
                }
                cx.Note("column", original->id, "comes in as the rectangle of its box: the shape of its section is not read");
                if (fellBack)
                    cx.Note("column", original->id, "it is turned, and its family symbol's size is not known: it comes in as its box, turned");
            }
        }
    }
}
