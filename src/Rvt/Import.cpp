// 移植自 BIMCraft crates/rvt/src/import.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
#include "Rvt/Import.h"

#include "Model/Text.h"
#include "Model/Walls.h"
#include "Rvt/Atom.h"
#include "Rvt/Columns.h"
#include "Rvt/Container.h"
#include "Rvt/Floors.h"
#include "Rvt/Importing.h"
#include "Rvt/Openings.h"
#include "Rvt/Roofs.h"
#include "Rvt/Rooms.h"
#include "Rvt/Walls.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>

namespace MiniBIM::Rvt
{
    using Bim::Id;
    using Bim::Model;

    double Mm(double feet) { return Micron(feet * 304.8); }

    double Micron(double v) { return std::round(v * 1000.0) / 1000.0; }

    Bim::P2 Point(double x, double y) { return Bim::P2(Mm(x), Mm(y)); }

    std::string Clip(std::string_view text)
    {
        text = Bim::Text::Trim(text);
        size_t n = 0, i = 0;
        for (; i < text.size(); ++i)
            if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
            {
                if (n == Bim::MaxText)
                    break;
                ++n;
            }
        return std::string(text.substr(0, i));
    }

    std::string Fresh(std::set<std::string>& taken, std::string_view base)
    {
        const std::string clipped = Clip(base);
        std::string name = clipped;
        for (size_t n = 2; taken.contains(name); ++n)
            name = clipped + " (" + std::to_string(n) + ")";
        taken.insert(name);
        return name;
    }

    std::optional<std::pair<std::string, std::variant<uint32_t, std::string>>> Origin(std::string_view from)
    {
        const auto lower = [](std::string_view s)
        {
            std::string out(s);
            for (char& c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        };
        if (const size_t space = from.rfind(' '); space != std::string_view::npos)
        {
            const std::string_view n = from.substr(space + 1);
            uint32_t id = 0;
            const auto [end, ec] = std::from_chars(n.data(), n.data() + n.size(), id);
            if (!n.empty() && ec == std::errc() && end == n.data() + n.size())
                return std::pair{ lower(from.substr(0, space)), std::variant<uint32_t, std::string>(id) };
        }
        if (const size_t space = from.find(' '); space != std::string_view::npos)
            return std::pair{ lower(from.substr(0, space)), std::variant<uint32_t, std::string>(std::string(from.substr(space + 1))) };
        return std::nullopt;
    }

    void Importing::LeftOut(std::string_view kind, uint32_t id, std::string_view reason) { m_groups[{ true, std::string(kind), std::string(reason) }].push_back(id); }

    void Importing::Note(std::string_view kind, uint32_t id, std::string_view text) { m_groups[{ false, std::string(kind), std::string(text) }].push_back(id); }

    void Importing::Dropped(Id id, std::string_view why)
    {
        const auto it = from.find(id);
        const std::string was = it != from.end() ? it->second : std::string();
        const auto origin = Origin(was);
        if (origin && std::holds_alternative<uint32_t>(origin->second))
            LeftOut(origin->first, std::get<uint32_t>(origin->second), why);
        else if (origin)
            skipped.push_back("1 " + origin->first + " left out, " + std::string(why) + " (" + std::get<std::string>(origin->second) + ")");
        else
            skipped.push_back("1 element left out, " + std::string(why));
    }

    void Importing::Tell()
    {
        auto groups = std::move(m_groups);
        m_groups.clear();
        for (auto& [key, ids] : groups)
        {
            const auto& [left, kind, reason] = key;
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            std::string list;
            for (size_t i = 0; i < ids.size() && i < 8; ++i)
                list += (i ? ", " : "") + std::to_string(ids[i]);
            const char* more   = ids.size() > 8 ? ", …" : "";
            const size_t n     = ids.size();
            const char* plural = n == 1 ? "" : "s";
            skipped.push_back(std::to_string(n) + " " + kind + plural + (left ? " left out, " : ": ") + reason + " (ids " + list + more + ")");
        }
    }

    std::vector<const Record*> Facts::OfCategory(int64_t category) const
    {
        std::vector<const Record*> out;
        for (const auto& [_, r] : current)
            if (r.category == category)
                out.push_back(&r);
        return out;
    }

    namespace
    {
        // 没有体积的放置的实例是二维符号（平面上画的地漏）：没有实体可导入
        bool HasVolume(const Record& r)
        {
            for (const double e : r.Extent())
                if (!(std::isfinite(e) && e > 1e-6))
                    return false;
            return true;
        }

        // 第一遍留下记录的类别
        constexpr std::array<int64_t, 10> Categories{
            Walls::OstWalls,         Floors::OstFloors,   Floors::OstCeilings,           Floors::OstSketchLines, Openings::OstDoors,
            Openings::OstWindows,    Columns::OstColumns, Columns::OstStructuralColumns, Rooms::OstRooms,        Roofs::OstRoofs,
        };

        // 第一遍找到的，按各类元素的读法整理好；以及清单
        Facts FactsOf(Surveyed& s)
        {
            Facts f;
            f.release = s.release;
            f.tags    = s.tags;
            f.ids     = std::move(s.ids);
            std::map<uint32_t, std::pair<std::pair<uint32_t, size_t>, std::array<double, 6>>> symbolBoxes;
            for (const Record& r : s.records)
            {
                if (r.placed)
                    continue;
                f.symbols[r.category].insert(r.id);
                const auto held = symbolBoxes.find(r.id);
                if (held == symbolBoxes.end() || held->second.first < std::pair{ r.partition, r.offset })
                    symbolBoxes[r.id] = { { r.partition, r.offset }, r.bbox };
            }
            for (const auto& [id, b] : symbolBoxes)
                f.symbolBoxes[id] = b.second;
            for (Record& r : s.records)
            {
                if (!(r.placed && (HasVolume(r) || r.category == Floors::OstSketchLines)))
                    continue;
                // 最高的那份是元素的整个形体；一样高时取最新的
                const auto key = [](const Record& x) { return std::tuple{ std::bit_cast<uint64_t>(x.Extent()[2]), x.partition, x.offset }; };
                const uint32_t id   = r.id;
                const auto     held = f.current.find(id);
                if (held == f.current.end() || key(held->second) < key(r))
                    f.current.insert_or_assign(id, std::move(r));
            }
            for (const TypeRecord& t : s.types)
            {
                if (t.category == OstMaterials)
                    f.materials.insert(t.id);
                else if (t.kind == Records::KindType)
                    f.types[t.category].insert(t.id);
            }
            for (const Level& l : s.inventory.levels)
                f.elevations[l.id] = l.elevationFeet;
            return f;
        }

        // 项目的标高，低的在前。名字与别的重复的加 (2)；一个也没有时给项目一个在零的
        void LevelsOf(Importing& cx, const Inventory& inv)
        {
            std::set<std::string> taken;
            for (const Level& l : inv.levels)
            {
                Bim::Level level;
                level.name      = Fresh(taken, l.name);
                level.elevation = Mm(l.elevationFeet);
                const Id id = cx.model.Insert(level);
                cx.from[id]       = "Level '" + l.name + "'";
                cx.levels[l.id]   = cx.model.GuidOf(id);
            }
            if (inv.levels.empty())
            {
                Bim::Level level;
                level.name = Fresh(taken, "Level 1");
                const Id id = cx.model.Insert(level);
                cx.from[id] = "Level 'Level 1'";
                cx.skipped.push_back("the file has no levels the reader can find: the project has one, Level 1, at zero");
            }
            for (const uint32_t id : inv.ownedLevels)
                cx.LeftOut("level", id, "it sits inside other elements, so it is not a project level");
            if (inv.unreadLevels > 0)
            {
                const size_t n = inv.unreadLevels;
                cx.skipped.push_back(std::to_string(n) + " level" + (n == 1 ? "" : "s") +
                                     " left out, no name or elevation the reader can find (its block is missing or damaged)");
            }
        }

        // 顶部跟着屋顶的墙：墙的记录把它贴着的屋顶与它约束到的标高一起说，所以它的顶部被屋顶的底面切开，而不是升到它自己
        // 包围盒的顶部（屋脊下的墙就是那样）
        void Attached(Importing& cx, const Facts& f)
        {
            if (cx.roofs.empty())
                return;
            for (const Record* r : f.OfCategory(Walls::OstWalls))
            {
                const auto wallIt = cx.walls.find(r->id);
                if (wallIt == cx.walls.end())
                    continue;
                const Id id = wallIt->second;
                std::optional<Id> roof;
                for (const uint64_t v : r->refs)
                    if (v <= UINT32_MAX)
                        if (const auto it = cx.roofs.find(static_cast<uint32_t>(v)); it != cx.roofs.end())
                        {
                            roof = it->second;
                            break;
                        }
                if (!roof)
                    continue;
                const Bim::Guid guid = cx.model.GuidOf(*roof);
                // 屋顶得给它留下点什么：墙中间上方的底面在它底部以下的屋顶把它切没了，那样的墙保持自己的包围盒
                const Bim::Wall* w = cx.model.GetWall(id);
                if (!w)
                    continue;
                const auto axis = Bim::Walls::Axis::Of(*w);
                const double room = Bim::Walls::Underside(cx.model, *roof, axis.At(axis.Length() / 2.0, 0.0)).value_or(cx.model.OwnTop(*w));
                if (room <= cx.model.OwnBase(*w) + 1.0)
                {
                    cx.Note("wall", r->id, "the roof it names leaves it no height: it comes in as its own box");
                    continue;
                }
                if (Bim::Element* e = cx.model.elements.GetMut(id))
                    if (Bim::Wall* ww = e->As<Bim::Wall>())
                    {
                        ww->attachTop = guid;
                        cx.Note("wall", r->id, "its top follows the roof it names: it comes in cut by the roof's underside");
                    }
            }
        }

        // 违反规则的略去，连同规则（墙带着它的门窗），然后再检查一遍模型
        bool Finish(Importing& cx, std::string schema, Bim::Ifc::Imported& out, std::string& error)
        {
            cx.model.AdoptMaterials();
            // 导入放进库的（类型、材质）与它们的部件得到 ID 与 GUID，同 Model::Check 要的
            cx.model.SyncLibrary();
            Model& m = cx.model;
            std::vector<std::pair<Id, std::string>> dropped;
            bool checked = false;
            const size_t rounds = m.elements.Size() + 1;
            for (size_t i = 0; i < rounds; ++i)
            {
                std::string why;
                if (m.Check(why))
                {
                    checked = true;
                    break;
                }
                const auto id = Bim::Culprit(why);
                if (!id || !cx.from.contains(*id))
                {
                    error = why;
                    return false;
                }
                const size_t colon = why.find(": ");
                dropped.emplace_back(*id, colon == std::string::npos ? why : why.substr(colon + 2));
                const Bim::Guid gone = m.GuidOf(*id);
                m.elements.Remove(*id);
                std::vector<Id> hosted;
                for (const auto& [k, x] : m.elements.Items())
                    if (const Bim::Opening* o = x->AsOpening(); o && o->host == gone)
                        hosted.push_back(k);
                for (const Id h : hosted)
                {
                    dropped.emplace_back(h, "its wall was left out");
                    m.elements.Remove(h);
                }
            }
            if (!checked && !m.Check(error))
                return false;
            for (const auto& [id, why] : dropped)
                cx.Dropped(id, why);
            cx.Tell();
            out.model   = std::move(cx.model);
            out.schema  = std::move(schema);
            out.skipped = std::move(cx.skipped);
            out.moved   = std::nullopt;
            return true;
        }
    }

    bool Build(Revit& r, std::string_view name, Bim::Ifc::Imported& out, std::map<Id, std::string>& from, std::string& error)
    {
        Error e;
        Surveyed survey;
        if (!Survey(r, [](int64_t c) { return std::find(Categories.begin(), Categories.end(), c) != Categories.end(); }, survey, e))
        {
            error = e.message;
            return false;
        }
        std::optional<std::string> title;
        {
            std::optional<Atom> atom;
            Error ignored;
            if (DocumentAtom(r, atom, ignored) && atom && atom->title && !Bim::Text::Trim(*atom->title).empty())
                title = atom->title;
        }
        Importing cx;
        cx.model = Model::New(Clip(title ? *title : std::string(name)));
        cx.model.elements.Clear();
        cx.model.nextId = Bim::FirstId;
        const uint32_t release = survey.release;
        const Facts f = FactsOf(survey);
        LevelsOf(cx, survey.inventory);
        Wants wants;
        wants.materials = f.materials;
        Walls::Wanted(f, wants.data);
        Floors::Wanted(f, wants.data);
        Roofs::Wanted(f, wants.data);
        Openings::Wanted(f, wants.data);
        Columns::Wanted(f, wants);
        Rooms::Wanted(f, wants);
        Gathered g;
        if (!Gather(r, release, f.tags, f.ids, wants, g, e))
        {
            error = e.message;
            return false;
        }
        Walls::Add(cx, f, g);
        Floors::Add(cx, f, g);
        Roofs::Add(cx, f, g);
        Attached(cx, f);
        Openings::Add(cx, f, g);
        Columns::Add(cx, f, g);
        Rooms::Add(cx, f, g);
        from = cx.from;
        return Finish(cx, "Revit " + std::to_string(release), out, error);
    }

    bool Import(std::span<const uint8_t> bytes, std::string_view name, Bim::Ifc::Imported& out, std::string& error)
    {
        Revit r;
        Error e;
        if (!Revit::FromBytes(std::vector<uint8_t>(bytes.begin(), bytes.end()), Limits{}, r, e))
        {
            error = e.message;
            return false;
        }
        std::map<Id, std::string> from;
        return Build(r, name, out, from, error);
    }

    bool ImportPath(const std::string& path, std::string_view name, Bim::Ifc::Imported& out, std::string& error)
    {
        std::map<Id, std::string> from;
        return ImportPathWithOrigins(path, name, out, from, error);
    }

    bool ImportPathWithOrigins(const std::string& path, std::string_view name, Bim::Ifc::Imported& out, std::map<Id, std::string>& from, std::string& error)
    {
        Revit r;
        Error e;
        if (!Revit::Open(path, Limits{}, r, e))
        {
            error = e.message;
            return false;
        }
        return Build(r, name, out, from, error);
    }
}
