// 移植自 BIMCraft crates/core/src/model.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Model.h"
#include "Model/Skeleton.h"
#include "Model/Text.h"
#include "Model/Walls.h"
#include "I18n/Translator.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

#ifdef _WIN32
#include <process.h>
#define MINIBIM_GETPID _getpid
#else
#include <unistd.h>
#define MINIBIM_GETPID getpid
#endif

namespace MiniBIM::Bim
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        // ring 逆时针
        Ring Ccw(const Ring& ring)
        {
            return Geom::Area(ring) < 0.0 ? Ring(ring.rbegin(), ring.rend()) : ring;
        }

        // loops（外轮廓，然后是其中的洞）以内、holes 以外的部分：外轮廓原样，被洞切开时为凸块
        std::vector<Ring> SolidOf(const std::vector<Ring>& loops, const std::vector<Ring>& holes)
        {
            if (loops.empty())
                return {};
            std::vector<Ring> parts{ loops.front() };
            for (size_t i = 1; i < loops.size(); ++i)
                parts = Walls::Minus(parts, loops[i]);
            for (const Ring& hole : holes)
                parts = Walls::Minus(parts, hole);
            return parts;
        }

        std::string Num(double x) { return Text::Shortest(x); }

        std::string Join(const std::vector<std::string>& names, const char* sep = ", ")
        {
            std::string out;
            for (size_t i = 0; i < names.size(); ++i)
                out += (i ? sep : "") + names[i];
            return out;
        }

        template <class T>
        std::vector<std::string> Keys(const std::map<std::string, T>& m)
        {
            std::vector<std::string> out;
            for (const auto& [k, _] : m)
                out.push_back(k);
            return out;
        }
    }

    // ── 枚举的名称 ───────────────────────────────────────────────
    namespace LocationOf
    {
        const char* Name(Location l)
        {
            switch (l)
            {
            case Location::Centre:         return "centre";
            case Location::CoreCentre:     return "core-centre";
            case Location::ExteriorFinish: return "exterior-finish";
            case Location::InteriorFinish: return "interior-finish";
            case Location::ExteriorCore:   return "exterior-core";
            case Location::InteriorCore:   return "interior-core";
            }
            return "centre";
        }

        const char* Label(Location l)
        {
            switch (l)
            {
            case Location::Centre:         return N_("Wall Centreline");
            case Location::CoreCentre:     return N_("Core Centreline");
            case Location::ExteriorFinish: return N_("Finish Face: Exterior");
            case Location::InteriorFinish: return N_("Finish Face: Interior");
            case Location::ExteriorCore:   return N_("Core Face: Exterior");
            case Location::InteriorCore:   return N_("Core Face: Interior");
            }
            return "";
        }

        std::optional<Location> FromName(std::string_view name)
        {
            const std::string n = Text::ReplaceAll(Text::AsciiLower(Text::Trim(name)), "center", "centre");
            for (Location l : All)
                if (n == Name(l))
                    return l;
            return std::nullopt;
        }
    }

    namespace FunctionOf
    {
        const char* Name(Function f)
        {
            switch (f)
            {
            case Function::Structure: return "structure";
            case Function::Substrate: return "substrate";
            case Function::Thermal:   return "thermal";
            case Function::Membrane:  return "membrane";
            case Function::Finish:    return "finish";
            }
            return "structure";
        }

        std::optional<Function> FromName(std::string_view name)
        {
            for (Function f : { Function::Structure, Function::Substrate, Function::Thermal, Function::Membrane, Function::Finish })
                if (name == Name(f))
                    return f;
            return std::nullopt;
        }
    }

    namespace PatternOf
    {
        const char* Name(Pattern p)
        {
            switch (p)
            {
            case Pattern::None:       return "none";
            case Pattern::Solid:      return "solid";
            case Pattern::Diagonal:   return "diagonal";
            case Pattern::Crosshatch: return "crosshatch";
            case Pattern::Horizontal: return "horizontal";
            case Pattern::Vertical:   return "vertical";
            case Pattern::Grid:       return "grid";
            case Pattern::Brick:      return "brick";
            case Pattern::Concrete:   return "concrete";
            case Pattern::Insulation: return "insulation";
            }
            return "none";
        }

        std::optional<Pattern> FromName(std::string_view name)
        {
            for (Pattern p : All)
                if (name == Name(p))
                    return p;
            return std::nullopt;
        }
    }

    namespace OperationOf
    {
        const char* Name(Operation o)
        {
            switch (o)
            {
            case Operation::Swing:         return "swing";
            case Operation::DoubleSwing:   return "double-swing";
            case Operation::Sliding:       return "sliding";
            case Operation::DoubleSliding: return "double-sliding";
            case Operation::Folding:       return "folding";
            case Operation::Fixed:         return "fixed";
            }
            return "swing";
        }

        const char* Label(Operation o)
        {
            switch (o)
            {
            case Operation::Swing:         return "Swing";
            case Operation::DoubleSwing:   return "Double Swing";
            case Operation::Sliding:       return "Sliding";
            case Operation::DoubleSliding: return "Double Sliding";
            case Operation::Folding:       return "Folding";
            case Operation::Fixed:         return "Fixed";
            }
            return "";
        }

        std::optional<Operation> Parse(std::string_view name)
        {
            const std::string n = Text::AsciiLower(Text::Trim(name));
            for (Operation o : All)
                if (n == Name(o))
                    return o;
            return std::nullopt;
        }
    }

    std::string NormalizeCategory(std::string_view s)
    {
        // structural column 与 structural_column 都是 structuralcolumn
        std::string out;
        for (const char c : Text::AsciiLower(s))
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                out += c;
        // mass 以复数的字母结尾
        if (out == "mass" || out == "masses")
            return "mass";
        while (!out.empty() && out.back() == 's')
            out.pop_back();
        return out;
    }

    // ── Element ──────────────────────────────────────────────────
    const Opening* Element::AsOpening() const
    {
        if (const Door* d = As<Door>())
            return d;
        if (const Window* w = As<Window>())
            return w;
        return nullptr;
    }

    Opening* Element::AsOpening()
    {
        if (Door* d = As<Door>())
            return d;
        if (Window* w = As<Window>())
            return w;
        return nullptr;
    }

    const char* Element::Category() const
    {
        struct Visitor
        {
            const char* operator()(const Level&) const  { return "level"; }
            const char* operator()(const Grid&) const   { return "grid"; }
            const char* operator()(const RoomSeparator&) const { return "roomseparator"; }
            const char* operator()(const Wall&) const   { return "wall"; }
            const char* operator()(const Floor&) const  { return "floor"; }
            const char* operator()(const Ceiling&) const { return "ceiling"; }
            const char* operator()(const Door&) const   { return "door"; }
            const char* operator()(const Window&) const { return "window"; }
            const char* operator()(const Room&) const   { return "room"; }
            const char* operator()(const Roof&) const   { return "roof"; }
        };
        return std::visit(Visitor{}, v);
    }

    void Element::RefsMut(const std::function<void(Guid&)>& f)
    {
        struct Visitor
        {
            const std::function<void(Guid&)>& f;
            void operator()(Level&) const {}
            void operator()(Grid&) const {}
            void operator()(RoomSeparator& x) const { f(x.level); }
            void operator()(Wall& w) const
            {
                f(w.level);
                if (w.topLevel) f(*w.topLevel);
                if (w.attachTop) f(*w.attachTop);
                if (w.attachBase) f(*w.attachBase);
            }
            void operator()(Floor& x) const { f(x.level); }
            void operator()(Ceiling& x) const { f(x.level); }
            void operator()(Door& o) const { f(o.host); }
            void operator()(Window& o) const { f(o.host); }
            void operator()(Room& x) const { f(x.level); }
            void operator()(Roof& r) const
            {
                f(r.level);
                if (r.join) f(*r.join);
            }
        };
        std::visit(Visitor{ f }, v);
    }

    std::vector<Guid> Element::Refs() const
    {
        std::vector<Guid> out;
        Element copy = *this;
        copy.RefsMut([&](Guid& g) { out.push_back(g); });
        return out;
    }

    void Element::LibRefsMut(const std::function<void(const Kind&, std::string&)>& f)
    {
        const std::string category = Category();
        if (Wall* w = As<Wall>())
            f(Kind::Type(category), w->typeName);
        else if (Floor* x = As<Floor>())
            f(Kind::Type(category), x->typeName);
        else if (Ceiling* c = As<Ceiling>())
            f(Kind::Type(category), c->typeName);
        else if (Roof* r = As<Roof>())
            f(Kind::Type(category), r->typeName);
        else if (Opening* o = AsOpening())
        {
            // 族的门窗：类型是族的（族还没移植，名称原样保留）
            if (!o->family)
                f(Kind::Type(category), o->typeName);
        }
    }

    // ── 新模型 ───────────────────────────────────────────────────
    Model Model::New(std::string_view name, Language language)
    {
        Model m;
        m.format   = FileFormat;
        m.name     = std::string(name);
        m.uid      = FreshUid();
        m.language = language;
        // 起步的材质库：各自的颜色、剖切面图案、表面图案（BIMCraft 的贴图以后移植）
        struct Start
        {
            const char*            name;
            std::array<uint8_t, 3> color;
            Pattern                cut;
            Pattern                surface;
        };
        const Start materials[] = {
            { "Brick",          { 178, 92, 70 },   Pattern::Brick,      Pattern::Brick },
            { "Insulation",     { 232, 214, 140 }, Pattern::Insulation, Pattern::None },
            { "Concrete block", { 176, 176, 170 }, Pattern::Crosshatch, Pattern::None },
            { "Plaster",        { 238, 234, 224 }, Pattern::None,       Pattern::None },
            { "Concrete",       { 188, 188, 182 }, Pattern::Concrete,   Pattern::None },
            { "Screed",         { 206, 200, 188 }, Pattern::Concrete,   Pattern::None },
            { "Timber",         { 196, 154, 102 }, Pattern::Diagonal,   Pattern::Horizontal },
            { "Gypsum board",   { 242, 242, 238 }, Pattern::None,       Pattern::None },
            { "Ceramic tile",   { 222, 220, 210 }, Pattern::Solid,      Pattern::Grid },
            { "Roof tiles",     { 170, 86, 64 },   Pattern::Solid,      Pattern::Horizontal },
            { "Membrane",       { 70, 70, 74 },    Pattern::Solid,      Pattern::None },
            { "Fabric",         { 176, 160, 136 }, Pattern::None,       Pattern::None },
            { "Porcelain",      { 244, 244, 240 }, Pattern::None,       Pattern::None },
            { "Glass",          { 166, 200, 228 }, Pattern::None,       Pattern::None },
            { "Metal",          { 150, 152, 158 }, Pattern::Solid,      Pattern::None },
            { "PVC",            { 226, 226, 214 }, Pattern::Solid,      Pattern::None },
            { "Copper",         { 184, 115, 51 },  Pattern::Solid,      Pattern::None },
        };
        for (const Start& s : materials)
            m.materials[LanguageOf::Word(language, s.name)] = Material{ s.color, s.cut, s.surface };
        m.types.wall["Generic - 200mm"]  = WallType{ 200.0, {} };
        m.types.wall["Interior - 100mm"] = WallType{ 100.0, {} };
        // 外墙从外往里的各层：砖、保温、砌块核心层、抹灰
        auto layer = [&](Function f, const char* material, double thickness)
        {
            Layer l;
            l.function  = f;
            l.material  = LanguageOf::Word(language, material);
            l.thickness = thickness;
            return l;
        };
        m.types.wall["Exterior - 300mm"] = WallType{ 300.0, { layer(Function::Finish, "Brick", 100.0), layer(Function::Thermal, "Insulation", 50.0),
                                                              layer(Function::Structure, "Concrete block", 140.0), layer(Function::Finish, "Plaster", 10.0) } };
        m.types.floor["Generic - 200mm"] = FloorType{ 200.0, {} };
        for (const auto& [n, w] : { std::pair{ "Single 900 x 2100", 900.0 }, std::pair{ "Single 800 x 2100", 800.0 } })
            m.types.door[n] = OpeningType{ w, 2100.0, 0.0, std::nullopt };
        m.types.window["Fixed 1200 x 1500"] = OpeningType{ 1200.0, 1500.0, 900.0, std::nullopt };
        m.types.window["Fixed 600 x 900"]   = OpeningType{ 600.0, 900.0, 1100.0, std::nullopt };
        m.types.roof["Generic - 300mm"]    = RoofType{ 300.0, {} };
        m.types.ceiling["Generic - 20mm"]  = CeilingType{ 20.0, 0.0, {} };
        m.types.ceiling["Tile 600 x 600"]  = CeilingType{ 20.0, 600.0, {} };
        const std::string level = LanguageOf::Word(language, "Level");
        m.Insert(Level{ level + " 1", 0.0, {}, {}, {}, false });
        m.Insert(Level{ level + " 2", 3000.0, {}, {}, {}, false });
        m.SyncLibrary();
        return m;
    }

    // ── 元素 ─────────────────────────────────────────────────────
    Id Model::Insert(Element e)
    {
        const Id id = FreshId();
        elements.Insert(id, std::move(e));
        return id;
    }

    Id Model::FreshId()
    {
        const Id last = elements.LastId() ? *elements.LastId() + 1 : FirstId;
        Id id = std::max({ nextId, FirstId, last });
        // 库从上往下数：越过它占着的 ID
        while (library.Taken(id))
            ++id;
        nextId = id + 1;
        return id;
    }

    void Model::Resolve()
    {
        std::vector<Id> ids;
        for (const auto& [id, e] : elements.Items())
            for (const Guid g : e->Refs())
                if (g.Pending())
                {
                    ids.push_back(id);
                    break;
                }
        for (const Id id : ids)
        {
            Element e = *elements.Get(id);
            e.RefsMut([&](Guid& g)
            {
                if (const auto pending = g.Pending())
                    if (const auto guid = elements.GuidOf(*pending))
                        g = *guid;
            });
            elements.Insert(id, std::move(e));
        }
    }

    void Model::Migrate()
    {
        if (format < 1)
        {
            Resolve();
            if (!elements.Empty() && elements.Items().begin()->first < FirstId)
            {
                const Id shift = FirstId - 1;
                std::vector<Id> low;
                for (const auto& [id, _] : elements.Items())
                    if (id < FirstId)
                        low.push_back(id);
                if (auto es = elements.Renumbered([shift](Id id) { return id < FirstId ? id + shift : id; }))
                {
                    elements = std::move(*es);
                    nextId = nextId > UINT64_MAX - shift ? UINT64_MAX : nextId + shift;
                }
                else
                {
                    // 要占的 ID 有的已被占用：这些给新的
                    for (const Id old : low)
                    {
                        const Element* e = elements.Get(old);
                        const auto g = elements.GuidOf(old);
                        if (!e || !g)
                            continue;
                        const Element copy = *e;
                        elements.Remove(old);
                        const Id id = FreshId();
                        std::string ignored;
                        elements.InsertAs(id, *g, copy, ignored);
                    }
                }
            }
        }
        nextId = std::max(nextId, FirstId);
        format = FileFormat;
    }

    std::string Model::Shown(Guid k) const
    {
        const auto id = elements.IdOf(k);
        return id ? std::to_string(*id) : k.ToString();
    }

    std::string Model::Shown(Id k) const
    {
        return std::to_string(k);
    }

    bool Model::Same(Guid a, Guid b) const
    {
        const auto ia = elements.IdOf(a);
        return ia && elements.IdOf(b) == ia;
    }

    Guid Model::GuidOf(Id k) const
    {
        return elements.GuidOf(k).value_or(Guid{});
    }

    Guid Model::GuidOf(Guid k) const
    {
        return elements.GuidOf(k).value_or(Guid{});
    }

    const Level* Model::GetLevel(Guid k) const
    {
        const Element* e = elements.Get(k);
        return e ? e->As<Level>() : nullptr;
    }

    const Level* Model::GetLevel(Id k) const
    {
        const Element* e = elements.Get(k);
        return e ? e->As<Level>() : nullptr;
    }

    const Wall* Model::GetWall(Guid k) const
    {
        const Element* e = elements.Get(k);
        return e ? e->As<Wall>() : nullptr;
    }

    const Wall* Model::GetWall(Id k) const
    {
        const Element* e = elements.Get(k);
        return e ? e->As<Wall>() : nullptr;
    }

    std::vector<std::pair<Id, const Level*>> Model::Levels() const
    {
        std::vector<std::pair<Id, const Level*>> v;
        for (const auto& [id, e] : elements.Items())
            if (const Level* l = e->As<Level>())
                v.emplace_back(id, l);
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second->elevation < b.second->elevation; });
        return v;
    }

    double Model::Elevation(Guid level) const
    {
        const Level* l = GetLevel(level);
        return l ? l->elevation : 0.0;
    }

    std::optional<Guid> Model::LevelAbove(Guid level) const
    {
        const double z = Elevation(level);
        for (const auto& [id, l] : Levels())
            if (l->elevation > z + Tol)
                return GuidOf(id);
        return std::nullopt;
    }

    // ── 墙 ───────────────────────────────────────────────────────
    double Model::Thickness(const Wall& w) const
    {
        const auto it = types.wall.find(w.typeName);
        return it == types.wall.end() ? 0.0 : it->second.thickness;
    }

    double Model::OwnBase(const Wall& w) const
    {
        return Elevation(w.level) + w.baseOffset;
    }

    double Model::OwnTop(const Wall& w) const
    {
        return w.topLevel ? Elevation(*w.topLevel) : OwnBase(w) + w.height;
    }

    double Model::BaseZ(const Wall& w) const
    {
        return Walls::Heights(*this, w).first;
    }

    double Model::TopZ(const Wall& w) const
    {
        return Walls::Heights(*this, w).second;
    }

    bool Model::CutBy(const Wall& w, Guid level) const
    {
        return Spans(w, GetPlanRange(level).cut);
    }

    bool Model::Bounding(const Wall& w, Guid level) const
    {
        return Spans(w, Elevation(level) + CutHeight);
    }

    bool Model::Spans(const Wall& w, double z) const
    {
        return BaseZ(w) <= z && z < TopZ(w);
    }

    PlanRange Model::GetPlanRange(Guid level) const
    {
        // 顶没有设时为上一个标高（上面没有时不限），且不低于剖切面
        const double z = Elevation(level);
        const Level* l = GetLevel(level);
        const ViewRange r = l ? l->viewRange : ViewRange{};
        double top = INFINITY;
        if (r.top)
            top = z + *r.top;
        else if (const auto above = LevelAbove(level))
            top = Elevation(*above);
        return PlanRange{ std::max(top, z + r.cut), z + r.cut, z + r.bottom, z + r.depth };
    }

    // ── 类型 ─────────────────────────────────────────────────────
    std::vector<std::string> Model::TypeNames(std::string_view category) const
    {
        const std::string c = NormalizeCategory(category);
        if (c == "wall")   return Keys(types.wall);
        if (c == "floor")  return Keys(types.floor);
        if (c == "door")   return Keys(types.door);
        if (c == "window") return Keys(types.window);
        if (c == "roof")   return Keys(types.roof);
        if (c == "ceiling") return Keys(types.ceiling);
        return {};
    }

    std::optional<std::string> Model::DefaultType(std::string_view category) const
    {
        const std::string c = NormalizeCategory(category);
        const char* preferred = "Fixed 1200 x 1500";
        if (c == "wall" || c == "floor")
            preferred = "Generic - 200mm";
        else if (c == "door")
            preferred = "Single 900 x 2100";
        else if (c == "roof")
            preferred = "Generic - 300mm";
        else if (c == "ceiling")
            preferred = "Generic - 20mm";
        const std::vector<std::string> names = TypeNames(category);
        for (const std::string& n : names)
            if (n == preferred)
                return n;
        if (names.empty())
            return std::nullopt;
        return names.front();
    }

    bool Model::OpeningDims(const Element& e, OpeningType& out, std::string& error) const
    {
        const Opening* o = e.AsOpening();
        if (!o)
        {
            error = std::string(e.Category()) + " isn't a door or a window";
            return false;
        }
        if (o->family)
        {
            // 族还没移植：项目里没有任何族
            error = "family `" + *o->family + "` doesn't exist (there are: )";
            return false;
        }
        const auto& map = e.IsDoor() ? types.door : types.window;
        const auto it = map.find(o->typeName);
        if (it == map.end())
        {
            error = "type `" + o->typeName + "` doesn't exist (there are: " + Join(Keys(map)) + ")";
            return false;
        }
        out = it->second;
        return true;
    }

    std::optional<OpeningType> Model::OpeningTypeOf(const Element& e) const
    {
        OpeningType t;
        std::string error;
        if (!OpeningDims(e, t, error))
            return std::nullopt;
        return t;
    }

    std::vector<Model::Layered> Model::LayeredTypes() const
    {
        std::vector<Layered> out;
        for (const auto& [n, x] : types.wall)
            out.push_back({ "wall", &n, x.thickness, &x.layers });
        for (const auto& [n, x] : types.floor)
            out.push_back({ "floor", &n, x.thickness, &x.layers });
        for (const auto& [n, x] : types.roof)
            out.push_back({ "roof", &n, x.thickness, &x.layers });
        for (const auto& [n, x] : types.ceiling)
            out.push_back({ "ceiling", &n, x.thickness, &x.layers });
        return out;
    }

    std::optional<Guid> Model::LevelOf(const Element& e) const
    {
        if (const Wall* w = e.As<Wall>())
            return w->level;
        if (const Floor* f = e.As<Floor>())
            return f->level;
        if (const Ceiling* c = e.As<Ceiling>())
            return c->level;
        if (const Room* r = e.As<Room>())
            return r->level;
        if (const RoomSeparator* rs = e.As<RoomSeparator>())
            return rs->level;
        if (const Roof* r = e.As<Roof>())
            return r->level;
        if (const Opening* o = e.AsOpening())
        {
            const Wall* w = GetWall(o->host);
            if (!w)
                return std::nullopt;
            return w->level;
        }
        return std::nullopt;
    }

    // ── 楼板 ─────────────────────────────────────────────────────
    Plane Model::FloorTop(const Floor& f) const
    {
        const double z = Elevation(f.level) + f.offset;
        if (f.arrow && f.slope != 0.0 && (*f.arrow)[0].Dist((*f.arrow)[1]) > 0.0)
        {
            const P2 tail = (*f.arrow)[0], head = (*f.arrow)[1];
            const P2 g = (head - tail).Unit() * std::tan(f.slope * kPi / 180.0);
            return Plane{ z - g.Dot(tail), g };
        }
        return Plane::Flat(z);
    }

    double Model::FloorDepth(const Floor& f) const
    {
        const auto it = types.floor.find(f.typeName);
        const double t = it == types.floor.end() ? 0.0 : it->second.thickness;
        return t / std::cos(f.slope * kPi / 180.0);
    }

    std::vector<std::pair<std::vector<P3>, double>> Model::FloorSlabs(const Floor& f) const
    {
        const Plane top = FloorTop(f);
        const double depth = FloorDepth(f);
        const Plane under{ top.z - depth, top.g };
        // 外轮廓减去洞口：没有洞口时原样，被洞口切开时为凸块
        std::vector<std::pair<std::vector<P3>, double>> out;
        for (const Ring& ring : SolidOf({ Ccw(f.boundary) }, f.openings))
            out.emplace_back(under.Lift(ring), depth);
        return out;
    }

    std::vector<std::pair<std::vector<P3>, double>> Model::CeilingSlabs(const Ceiling& c) const
    {
        const Plane under = Plane::Flat(Elevation(c.level) + c.offset);
        const auto it = types.ceiling.find(c.typeName);
        const double t = it == types.ceiling.end() ? 0.0 : it->second.thickness;
        std::vector<std::pair<std::vector<P3>, double>> out;
        for (const Ring& ring : SolidOf({ Ccw(c.boundary) }, c.openings))
            out.emplace_back(under.Lift(ring), t);
        return out;
    }

    // ── 屋顶 ─────────────────────────────────────────────────────
    std::vector<Segment> Roof::Edges() const
    {
        std::vector<Segment> out = Geom::RingEdges(boundary);
        for (const Ring& c : courtyards)
            for (const Segment& e : Geom::RingEdges(c))
                out.push_back(e);
        return out;
    }

    Ring Model::RoofOutline(const Roof& r) const
    {
        if (!r.extrusion)
            return Ccw(r.boundary);
        const Extrusion& x = *r.extrusion;
        const P2 o = x.line[0], u = (x.line[1] - x.line[0]).Unit();
        const double a0 = x.profile.empty() ? 0.0 : x.profile.front().x;
        const double a1 = x.profile.empty() ? 0.0 : x.profile.back().x;
        Ring ring;
        for (const auto& [a, c] : { std::pair{ a0, x.from }, std::pair{ a1, x.from }, std::pair{ a1, x.to }, std::pair{ a0, x.to } })
            ring.push_back(o + u * a + u.Perp() * c);
        return Ccw(ring);
    }

    double Model::RoofRise(const Roof& r, const Plane& plane) const
    {
        const auto it = types.roof.find(r.typeName);
        const double t = it == types.roof.end() ? 0.0 : it->second.thickness;
        return t * std::hypot(std::sqrt(plane.g.Dot(plane.g)), 1.0);
    }

    bool Model::RoofFaces(const Roof& r, std::vector<Slope>& out, std::string& error) const
    {
        const double base = Elevation(r.level) + r.offset;
        std::vector<Slope> faces;
        if (r.extrusion)
        {
            const Extrusion& x = *r.extrusion;
            const P2 o = x.line[0], u = (x.line[1] - x.line[0]).Unit();
            for (size_t i = 0; i + 1 < x.profile.size(); ++i)
            {
                const P2 a = x.profile[i], b = x.profile[i + 1];
                if (!(b.x > a.x))
                    continue;
                const double k = (b.y - a.y) / (b.x - a.x);
                const Plane plane{ base + a.y - k * (a.x + o.Dot(u)), u * k };
                Ring ring;
                for (const auto& [along, across] : { std::pair{ a.x, x.from }, std::pair{ b.x, x.from }, std::pair{ b.x, x.to }, std::pair{ a.x, x.to } })
                    ring.push_back(o + u * along + u.Perp() * across);
                faces.push_back({ { ring }, plane });
            }
        }
        else if (r.slope == 0.0)
        {
            std::vector<Ring> loops{ Ccw(r.boundary) };
            for (const Ring& c : r.courtyards)
            {
                Ring turned = Ccw(c);
                std::reverse(turned.begin(), turned.end());
                loops.push_back(std::move(turned));
            }
            faces.push_back({ std::move(loops), Plane::Flat(base) });
        }
        else
        {
            // 外轮廓逆时针、天井顺时针，屋顶在每条边的左边；竖直的边在倒过来的环上反着数
            std::vector<bool> up;
            if (!RoofUprights(r, up, error))
                return false;
            std::vector<Ring> rings;
            std::vector<double> speeds;
            std::vector<const Ring*> given{ &r.boundary };
            for (const Ring& c : r.courtyards)
                given.push_back(&c);
            for (size_t k = 0; k < given.size(); ++k)
            {
                const Ring& g = *given[k];
                const size_t n = g.size(), first = speeds.size();
                const bool turn = (Geom::Area(g) < 0.0) == (k == 0);
                rings.push_back(turn ? Ring(g.rbegin(), g.rend()) : g);
                for (size_t i = 0; i < n; ++i)
                {
                    const size_t at = first + (turn ? (2 * n - 2 - i) % n : i);
                    speeds.push_back(at < up.size() && up[at] ? 0.0 : 1.0);
                }
            }
            if (speeds.size() < 3 || speeds.size() > MaxRoofPoints)
            {
                error = "a pitched roof's footprint has 3 to " + std::to_string(MaxRoofPoints) + " points, courtyards and all, not " +
                        std::to_string(speeds.size());
                return false;
            }
            const double tan = std::tan(r.slope * kPi / 180.0);
            const double eave = base - r.overhang * tan;
            std::vector<std::vector<Skeleton::Face>> skeleton;
            if (!Skeleton::Faces(rings, speeds, skeleton, error))
                return false;
            size_t e = 0;
            for (const Ring& ring : rings)
                for (const auto& [a, b] : Geom::RingEdges(ring))
                {
                    if (e >= skeleton.size())
                        break;
                    const std::vector<Skeleton::Face>& face = skeleton[e++];
                    const P2 inward = (b - a).Unit().Perp();
                    const Plane plane{ eave - tan * a.Dot(inward), inward * tan };
                    std::vector<Ring> loops;
                    for (const Skeleton::Face& l : face)
                    {
                        Ring pts;
                        for (const auto& [q, when] : l)
                            pts.push_back(q);
                        loops.push_back(std::move(pts));
                    }
                    if (!loops.empty() && loops.front().size() >= 3)
                        faces.push_back({ std::move(loops), plane });
                }
        }
        if (!r.join)
        {
            out = std::move(faces);
            return true;
        }
        const Element* he = elements.Get(*r.join);
        const Roof* host = he ? he->As<Roof>() : nullptr;
        if (!host || host->join)
        {
            error = "it joins element " + r.join->ToString() + ", which isn't a roof that joins none";
            return false;
        }
        // 在插进的屋顶上方，只留它的顶面高过那个屋顶顶面的部分
        std::vector<std::pair<Ring, Plane>> hostPieces;
        if (!RoofPieces(*host, hostPieces, error))
            return false;
        std::vector<std::pair<Ring, Plane>> tops;
        for (const auto& [ring, p] : hostPieces)
        {
            const Plane top{ p.z + RoofRise(*host, p), p.g };
            for (Ring& c : Walls::ConvexPieces(ring))
                tops.emplace_back(std::move(c), top);
        }
        out.clear();
        for (const auto& [loops, plane] : faces)
        {
            const Plane mine{ plane.z + RoofRise(r, plane), plane.g };
            std::vector<Ring> kept;
            std::vector<Ring> rest = SolidOf(loops, {});
            for (const auto& [piece, top] : tops)
            {
                const Plane hostTop = top;
                std::vector<Ring> outside;
                for (Ring& part : rest)
                {
                    auto [inside, outer] = Walls::Split(std::move(part), piece);
                    if (inside)
                    {
                        Ring clipped = Geom::Clip(*inside, [&](P2 q) { return mine.At(q) - hostTop.At(q); });
                        if (clipped.size() >= 3 && Geom::Area(clipped) > 1.0)
                            kept.push_back(std::move(clipped));
                    }
                    for (Ring& o : outer)
                        outside.push_back(std::move(o));
                }
                rest = std::move(outside);
            }
            for (Ring& k : rest)
                kept.push_back(std::move(k));
            for (Ring& k : kept)
                out.push_back({ { std::move(k) }, plane });
        }
        return true;
    }

    bool Model::RoofUprights(const Roof& r, std::vector<bool>& out, std::string& error) const
    {
        const size_t n = r.Edges().size();
        std::vector<bool> up(n, false);
        for (const size_t g : r.gables)
        {
            if (g >= n)
            {
                error = "gable edge " + std::to_string(g) + " isn't one of its " + std::to_string(n) + " edges (0 to " +
                        std::to_string(n == 0 ? 0 : n - 1) + ")";
                return false;
            }
            up[g] = true;
        }
        if (r.gable)
        {
            const auto square = [](P2 a, P2 b, P2 c) { return std::abs((b - a).Unit().Dot((c - b).Unit())) < 1e-6; };
            const Ring corners = Geom::Corners(Ccw(r.boundary));
            if (!(corners.size() == 4 && square(corners[0], corners[1], corners[2]) && square(corners[1], corners[2], corners[3]) &&
                  square(corners[2], corners[3], corners[0])))
            {
                error = "`gable` leaves a rectangle's ends upright; over any other footprint, name the edges in `gables`";
                return false;
            }
            const P2 a = corners[0], b = corners[1], c = corners[2], d = corners[3];
            const std::array<Segment, 2> ends = a.Dist(b) >= b.Dist(c) ? std::array<Segment, 2>{ Segment{ b, c }, Segment{ d, a } }
                                                                       : std::array<Segment, 2>{ Segment{ a, b }, Segment{ c, d } };
            const std::vector<Segment> edges = Geom::RingEdges(r.boundary);
            for (size_t i = 0; i < edges.size() && i < up.size(); ++i)
            {
                const auto [p, q] = edges[i];
                for (const auto& [s, t] : ends)
                    if (Geom::DistToSegment(p, s, t) < Tol && Geom::DistToSegment(q, s, t) < Tol)
                        up[i] = true;
            }
        }
        out = std::move(up);
        return true;
    }

    std::vector<std::pair<std::vector<P3>, double>> Model::RoofSlabs(const Roof& r) const
    {
        std::vector<std::pair<Ring, Plane>> pieces;
        std::string error;
        if (!RoofPieces(r, pieces, error))
            pieces.clear();
        std::vector<std::pair<std::vector<P3>, double>> out;
        for (const auto& [ring, p] : pieces)
            out.emplace_back(p.Lift(ring), RoofRise(r, p));
        return out;
    }

    bool Model::RoofPieces(const Roof& r, std::vector<std::pair<Ring, Plane>>& out, std::string& error) const
    {
        std::vector<Slope> faces;
        if (!RoofFaces(r, faces, error))
            return false;
        out.clear();
        for (const auto& [loops, plane] : faces)
            for (Ring& p : SolidOf(loops, r.openings))
                out.emplace_back(std::move(p), plane);
        return true;
    }

    // ── 房间找墙用的线 ───────────────────────────────────────────
    std::vector<Segment> Model::WallLines(Guid level) const
    {
        std::vector<Segment> out;
        for (const auto& [id, e] : elements.Items())
        {
            const Wall* w = e->As<Wall>();
            if (!w || !Bounding(*w, level))
                continue;
            const std::vector<P2> line = Walls::CentreLine(*this, id);
            for (size_t i = 0; i + 1 < line.size(); ++i)
                out.emplace_back(line[i], line[i + 1]);
        }
        return out;
    }

    std::vector<Segment> Model::BoundingLines(Guid level) const
    {
        std::vector<Segment> lines = WallLines(level);
        for (const Segment& s : Separators(level))
            lines.push_back(s);
        return lines;
    }

    std::vector<Segment> Model::Separators(Guid level) const
    {
        std::vector<Segment> out;
        for (const auto& [id, e] : elements.Items())
            if (const RoomSeparator* rs = e->As<RoomSeparator>(); rs && Same(rs->level, level))
                out.emplace_back(rs->start, rs->end);
        return out;
    }

    std::vector<Segment> Model::RoomLines(Guid level, RoomArea at) const
    {
        if (at == RoomArea::Centre)
            return BoundingLines(level);
        std::vector<Segment> lines = FaceLines(level);
        for (const Segment& s : Separators(level))
            lines.push_back(s);
        return lines;
    }

    std::optional<Geom::Region> Model::RoomRegion(const Room& r) const
    {
        return RoomRegionAt(r, roomArea);
    }

    std::optional<Geom::Region> Model::RoomRegionAt(const Room& r, RoomArea at) const
    {
        return Geom::RegionAt(RoomLines(r.level, at), r.point, Tol);
    }

    std::vector<Segment> Model::FaceLines(Guid level) const
    {
        std::vector<Segment> out;
        for (const auto& [id, e] : elements.Items())
        {
            const Wall* w = e->As<Wall>();
            if (!w || !Bounding(*w, level))
                continue;
            for (const Segment& s : Geom::RingEdges(Walls::Outline(*this, id)))
                out.push_back(s);
        }
        return out;
    }

    // ── 检查 ─────────────────────────────────────────────────────
    namespace
    {
        bool Coord(const std::string& what, double v, std::string& error)
        {
            if (std::isfinite(v) && std::abs(v) <= MaxCoord)
                return true;
            error = what + ": " + Num(v) + " is not a usable number";
            return false;
        }

        bool Size(const std::string& what, double v, std::string& error)
        {
            if (!Coord(what, v, error))
                return false;
            if (v > 0.0)
                return true;
            error = what + " must be above zero, got " + Num(v);
            return false;
        }

        bool Point(const std::string& who, P2 p, std::string& error)
        {
            return Coord(who + " x", p.x, error) && Coord(who + " y", p.y, error);
        }

        bool Line(const std::string& who, P2 a, P2 b, std::string& error)
        {
            if (!Point(who, a, error) || !Point(who, b, error))
                return false;
            if (a.Dist(b) > Tol)
                return true;
            error = who + ": too short (" + Num(a.Dist(b)) + " mm)";
            return false;
        }

        bool TextOk(const std::string& who, const char* what, const std::string& s, std::string& error)
        {
            if (Text::CharCount(s) <= MaxText)
                return true;
            error = who + ": " + what + " is longer than " + std::to_string(MaxText) + " characters";
            return false;
        }

        template <class T>
        bool Known(const std::string& who, const std::string& name, const std::map<std::string, T>& types, std::string& error)
        {
            if (types.count(name))
                return true;
            error = who + ": type `" + name + "` doesn't exist (there are: " + Join(Keys(types)) + ")";
            return false;
        }

        bool RangeOk(const std::string& who, const ViewRange& r, std::string& error)
        {
            if (!Coord(who + " view range cut", r.cut, error) || !Coord(who + " view range bottom", r.bottom, error) ||
                !Coord(who + " view range depth", r.depth, error) || (r.top && !Coord(who + " view range top", *r.top, error)))
                return false;
            if (!(r.depth <= r.bottom && r.bottom <= r.cut && (!r.top || r.cut <= *r.top)))
            {
                error = who + ": its plan's view range runs top ≥ cut plane ≥ bottom ≥ view depth, not top " +
                        (r.top ? Num(*r.top) : std::string("at the level above")) + ", cut " + Num(r.cut) + ", bottom " + Num(r.bottom) +
                        ", depth " + Num(r.depth);
                return false;
            }
            return true;
        }

        bool Crop(const std::string& who, P2 a, P2 b, std::string& error)
        {
            if (!Point(who + " crop", a, error) || !Point(who + " crop", b, error))
                return false;
            if (a.x < b.x - Tol && a.y < b.y - Tol)
                return true;
            error = who + ": its crop region runs from its south-west corner to its north-east one";
            return false;
        }

        // 轮廓：3 到 MaxPoints 个点，围出一些面积
        bool Boundary(const std::string& who, const Ring& ring, std::string& error)
        {
            if (ring.size() < 3 || ring.size() > MaxPoints)
            {
                error = who + ": a boundary needs 3 to " + std::to_string(MaxPoints) + " points";
                return false;
            }
            for (const P2& p : ring)
                if (!Point(who, p, error))
                    return false;
            if (std::abs(Geom::Area(ring)) <= Tol * Tol)
            {
                error = who + ": its boundary encloses no area";
                return false;
            }
            return true;
        }

        // 轮廓里的洞（楼板、天花板的洞口）：各在 outline 里面，与它、与别的洞都不相交
        bool Holes(const std::string& who, const char* what, const Ring& outline, const std::vector<Ring>& rings, std::string& error)
        {
            // p 在 ring 里面（或外面），离它的边都超过 Tol
            auto clear = [](const Ring& ring, P2 p, bool inside)
            {
                if (Geom::Contains(ring, p) != inside)
                    return false;
                for (const auto& [a, b] : Geom::RingEdges(ring))
                    if (Geom::DistToSegment(p, a, b) <= Tol)
                        return false;
                return true;
            };
            auto cross = [](const Ring& r, const Ring& s)
            {
                for (const auto& [a, b] : Geom::RingEdges(r))
                    for (const auto& [c, d] : Geom::RingEdges(s))
                        if (Geom::Crossing(a, b, c, d))
                            return true;
                return false;
            };
            for (size_t i = 0; i < rings.size(); ++i)
            {
                const Ring& c = rings[i];
                if (!std::all_of(c.begin(), c.end(), [&](const P2& p) { return clear(outline, p, true); }) || cross(c, outline))
                {
                    error = who + ": " + what + " " + std::to_string(i) + " isn't inside its outline";
                    return false;
                }
                for (size_t j = i + 1; j < rings.size(); ++j)
                {
                    const Ring& o = rings[j];
                    if (!std::all_of(c.begin(), c.end(), [&](const P2& p) { return clear(o, p, false); }) ||
                        !std::all_of(o.begin(), o.end(), [&](const P2& p) { return clear(c, p, false); }) || cross(c, o))
                    {
                        error = who + ": " + what + "s " + std::to_string(i) + " and " + std::to_string(j) + " overlap";
                        return false;
                    }
                }
            }
            return true;
        }

        bool Openings(const std::string& who, const Ring& outline, const std::vector<Ring>& rings, std::string& error)
        {
            size_t total = 0;
            for (const Ring& r : rings)
                total += r.size();
            if (total > MaxPoints)
            {
                error = who + ": its openings have more than " + std::to_string(MaxPoints) + " points between them";
                return false;
            }
            for (size_t i = 0; i < rings.size(); ++i)
                if (!Boundary(who + " opening " + std::to_string(i), rings[i], error))
                    return false;
            return Holes(who, "opening", outline, rings, error);
        }

        // 门窗在墙上的位置：沿墙从哪到哪、窗台与洞顶在墙底以上的高度、是哪个门窗
        struct OpeningSpan
        {
            double start;
            double end;
            double sill;
            double head;
            Id     id;
        };
    }

    bool Model::Check(std::string& error) const
    {
        for (const char* cat : { "door", "window" })
        {
            const auto& map = std::string(cat) == "door" ? types.door : types.window;
            for (const auto& [n, t] : map)
            {
                const std::string who = std::string(cat) + " type `" + n + "`";
                if (!Size(who + " width", t.width, error) || !Size(who + " height", t.height, error) || !Coord(who + " sill", t.sill, error))
                    return false;
            }
        }
        for (const auto& [n, x] : materials)
        {
            (void)x;
            if (!TextOk("material `" + n + "`", "name", n, error))
                return false;
            if (Text::Trim(n).empty())
            {
                error = "a material needs a name";
                return false;
            }
        }
        for (const Layered& t : LayeredTypes())
        {
            const std::string who = std::string(t.category) + " type `" + *t.name + "`";
            if (!Size(who + " thickness", t.thickness, error))
                return false;
            if (t.layers->size() > MaxLayers)
            {
                error = who + ": more than " + std::to_string(MaxLayers) + " layers";
                return false;
            }
            double sum = 0.0;
            for (size_t i = 0; i < t.layers->size(); ++i)
            {
                const Layer& l = (*t.layers)[i];
                const std::string what = who + " layer " + std::to_string(i + 1);
                if (!TextOk(what, "material", l.material, error))
                    return false;
                if (!l.material.empty() && !materials.count(l.material))
                {
                    error = what + ": material `" + l.material + "` isn't in the library; add it with `material`";
                    return false;
                }
                if (!(l.function == Function::Membrane && l.thickness == 0.0) && !Size(what + " thickness", l.thickness, error))
                    return false;
                sum += l.thickness;
            }
            if (!t.layers->empty() && std::abs(sum - t.thickness) > 1e-6)
            {
                error = who + ": its layers add up to " + Num(sum) + " mm, not its thickness, " + Num(t.thickness) + " mm";
                return false;
            }
        }
        for (const auto& [n, t] : types.ceiling)
        {
            // 更细的格子每隔几个像素就画一条线，大天花板上有几百万条
            if (!(t.grid == 0.0 || (t.grid >= 100.0 && t.grid <= MaxCoord)))
            {
                error = "ceiling type `" + n + "`: grid " + Num(t.grid) + " mm; its tiles are 100 mm or wider, or 0 for none";
                return false;
            }
        }
        std::set<std::string> levelNames, gridNames;
        // 每面墙上的门窗，按沿墙的起点排序
        std::map<Guid, std::vector<OpeningSpan>> spans;
        for (const auto& [id, ptr] : elements.Items())
        {
            const Element& e = *ptr;
            const std::string who = std::string(e.Category()) + " " + std::to_string(id);
            auto levelOk = [&](Guid l)
            {
                if (GetLevel(l))
                    return true;
                error = who + ": level " + Shown(l) + " doesn't exist";
                return false;
            };
            if (const Level* l = e.As<Level>())
            {
                if (!TextOk(who, "name", l->name, error) || !Coord(who + " elevation", l->elevation, error))
                    return false;
                if (!levelNames.insert(l->name).second)
                {
                    error = who + ": another level is already named `" + l->name + "`";
                    return false;
                }
                if (!RangeOk(who, l->viewRange, error))
                    return false;
                if (l->crop && !Crop(who, (*l->crop)[0], (*l->crop)[1], error))
                    return false;
                if (l->extent)
                {
                    const P2 a = (*l->extent)[0], b = (*l->extent)[1];
                    if (!Point(who + " extent", a, error) || !Point(who + " extent", b, error))
                        return false;
                    if (!(a.x < b.x - Tol && a.y < b.y - Tol))
                    {
                        error = who + ": its extent runs from its south-west corner to its north-east one";
                        return false;
                    }
                }
            }
            else if (const Grid* g = e.As<Grid>())
            {
                if (!TextOk(who, "name", g->name, error) || !Line(who, g->start, g->end, error))
                    return false;
                if (!gridNames.insert(g->name).second)
                {
                    error = who + ": another grid is already named `" + g->name + "`";
                    return false;
                }
            }
            else if (const RoomSeparator* rs = e.As<RoomSeparator>())
            {
                if (!levelOk(rs->level) || !Line(who, rs->start, rs->end, error))
                    return false;
            }
            else if (const Ceiling* c = e.As<Ceiling>())
            {
                if (!Known(who, c->typeName, types.ceiling, error) || !levelOk(c->level) || !Boundary(who, c->boundary, error) ||
                    !Openings(who, c->boundary, c->openings, error) || !Coord(who + " offset", c->offset, error))
                    return false;
            }
            else if (const Room* rm = e.As<Room>())
            {
                if (!levelOk(rm->level) || !Point(who, rm->point, error) || !TextOk(who, "name", rm->name, error) ||
                    !TextOk(who, "number", rm->number, error))
                    return false;
            }
            else if (const Roof* r = e.As<Roof>())
            {
                if (!Known(who, r->typeName, types.roof, error) || !levelOk(r->level) || !Coord(who + " overhang", r->overhang, error) ||
                    !Coord(who + " offset", r->offset, error))
                    return false;
                if (r->extrusion)
                {
                    const Extrusion& x = *r->extrusion;
                    if (!Line(who, x.line[0], x.line[1], error))
                        return false;
                    if (x.profile.size() < 2 || x.profile.size() > MaxPoints)
                    {
                        error = who + ": an extrusion's profile needs 2 to " + std::to_string(MaxPoints) + " points";
                        return false;
                    }
                    for (const P2& p : x.profile)
                        if (!Point(who, p, error))
                            return false;
                    for (size_t i = 0; i + 1 < x.profile.size(); ++i)
                        if (!(x.profile[i + 1].x - x.profile[i].x > Tol))
                        {
                            error = who + ": its profile runs along its line one way, each point past the one before";
                            return false;
                        }
                    if (!Coord(who + " from", x.from, error) || !Coord(who + " to", x.to, error))
                        return false;
                    if (x.to - x.from <= Tol)
                    {
                        error = who + ": it's swept across its line from `from` to `to`, further to the left";
                        return false;
                    }
                }
                else
                {
                    if (!Boundary(who, r->boundary, error))
                        return false;
                    if (!(r->slope >= 0.0 && r->slope <= MaxSlope))
                    {
                        error = who + ": slope " + Num(r->slope) + "° isn't from 0° (flat) to " + Num(MaxSlope) + "°";
                        return false;
                    }
                }
                if (r->openings.size() > MaxRoofPoints)
                {
                    error = who + ": " + std::to_string(r->openings.size()) + " openings, more than " + std::to_string(MaxRoofPoints);
                    return false;
                }
                for (const Ring& o : r->openings)
                    if (!Boundary(who + " opening", o, error))
                        return false;
                if (r->extrusion && !r->courtyards.empty())
                {
                    error = who + ": courtyards are holes in a roof's footprint; cut an extrusion roof with `openings`";
                    return false;
                }
                size_t courtyardPoints = 0;
                for (const Ring& c : r->courtyards)
                    courtyardPoints += c.size();
                if (courtyardPoints > MaxRoofPoints)
                {
                    error = who + ": its courtyards have more than " + std::to_string(MaxRoofPoints) + " points between them";
                    return false;
                }
                for (size_t i = 0; i < r->courtyards.size(); ++i)
                    if (!Boundary(who + " courtyard " + std::to_string(i), r->courtyards[i], error))
                        return false;
                if (!Holes(who, "courtyard", r->boundary, r->courtyards, error))
                    return false;
                std::vector<std::pair<Ring, Plane>> pieces;
                std::string why;
                if (!RoofPieces(*r, pieces, why))
                {
                    error = who + ": " + why;
                    return false;
                }
            }
            else if (const Wall* w = e.As<Wall>())
            {
                if (!Known(who, w->typeName, types.wall, error) || !levelOk(w->level) || !Line(who, w->start, w->end, error) ||
                    !Coord(who + " base_offset", w->baseOffset, error))
                    return false;
                // 圆弧在范围内：整个圆都在模型的范围以内
                const Walls::Axis axis = Walls::Axis::Of(*w);
                const auto c = axis.Centre();
                const auto r = axis.Radius();
                const bool reach = !c || !r || std::max(std::abs(c->x), std::abs(c->y)) + *r <= MaxCoord;
                if (!(std::isfinite(w->arc) && std::abs(w->arc) < 360.0 && reach))
                {
                    error = who + ": arc " + Num(w->arc) + "° isn't between -360° and 360°, reaching no farther than 1000 km";
                    return false;
                }
                for (const auto& [what, at] : { std::pair{ "attach_top", w->attachTop }, std::pair{ "attach_base", w->attachBase } })
                {
                    if (!at)
                        continue;
                    const Element* to = elements.Get(*at);
                    if (!to || !(to->As<Floor>() || to->As<Roof>()))
                    {
                        error = who + ": " + what + " " + Shown(*at) + " isn't a roof or a floor";
                        return false;
                    }
                }
                if (w->topLevel)
                {
                    if (!levelOk(*w->topLevel))
                        return false;
                }
                else if (!Size(who + " height", w->height, error))
                    return false;
                if (TopZ(*w) <= BaseZ(*w) + Tol)
                {
                    error = who + ": its top must be above its base";
                    return false;
                }
            }
            else if (const Floor* f = e.As<Floor>())
            {
                if (!Point(who + " pattern origin", f->patternOrigin, error) || !Coord(who + " pattern angle", f->patternAngle, error) ||
                    !Known(who, f->typeName, types.floor, error) || !levelOk(f->level) || !Boundary(who, f->boundary, error) ||
                    !Openings(who, f->boundary, f->openings, error) || !Coord(who + " offset", f->offset, error))
                    return false;
                if (!(std::isfinite(f->slope) && std::abs(f->slope) <= MaxSlope))
                {
                    error = who + ": slope " + Num(f->slope) + "° isn't between -" + Num(MaxSlope) + "° and " + Num(MaxSlope) + "°";
                    return false;
                }
                if (f->arrow)
                {
                    if (!Line(who + " arrow", (*f->arrow)[0], (*f->arrow)[1], error))
                        return false;
                }
                else if (f->slope != 0.0)
                {
                    error = who + ": a sloped floor needs a slope arrow, from where its top is at its offset toward where it rises";
                    return false;
                }
            }
            else if (const Opening* o = e.AsOpening())
            {
                const Wall* host = GetWall(o->host);
                if (!host)
                {
                    error = who + ": host wall " + Shown(o->host) + " doesn't exist";
                    return false;
                }
                OpeningType t;
                std::string why;
                if (!OpeningDims(e, t, why))
                {
                    error = who + ": " + why;
                    return false;
                }
                if (!Coord(who + " offset", o->offset, error) || !Coord(who + " sill", o->sill, error) || !TextOk(who, "mark", o->mark, error))
                    return false;
                const double a = o->offset - t.width / 2.0, b = o->offset + t.width / 2.0;
                const double length = Walls::Axis::Of(*host).Length();
                if (a < -Tol || b > length + Tol)
                {
                    error = who + ": " + Num(t.width) + " mm wide at offset " + Num(o->offset) + " doesn't fit wall " + Shown(o->host) + ", " +
                            Num(length) + " mm long";
                    return false;
                }
                // 到墙在它上方的顶部为止（贴着的屋顶可能抬高它）
                double top = INFINITY;
                for (const double s : { a, o->offset, b })
                    top = std::min(top, Walls::TopAt(*this, *host, s));
                if (o->sill < 0.0 || OwnBase(*host) + o->sill + t.height > top + Tol)
                {
                    error = who + ": sill " + Num(o->sill) + " + height " + Num(t.height) + " doesn't fit wall " + Shown(o->host) + "'s height";
                    return false;
                }
                auto& list = spans[o->host];
                const OpeningSpan span{ a, b, o->sill, o->sill + t.height, id };
                const auto at = std::partition_point(list.begin(), list.end(), [&](const OpeningSpan& held) { return held.start <= a; });
                list.insert(at, span);
            }
        }
        for (const auto& [host, v] : spans)
        {
            // 门窗只有在墙上的位置和高度都重叠时才算重叠：上下相叠的不算（一层一扇的店面墙在平面上重叠）
            for (size_t i = 0; i < v.size(); ++i)
            {
                for (size_t j = i + 1; j < v.size(); ++j)
                {
                    const OpeningSpan& a = v[i];
                    const OpeningSpan& b = v[j];
                    if (b.start > a.end + Tol)
                        break;
                    if (a.end > b.start + Tol && a.head > b.sill + Tol && b.head > a.sill + Tol)
                    {
                        auto cat = [&](Id id) { const Element* x = elements.Get(id); return std::string(x ? x->Category() : "element"); };
                        error = cat(a.id) + " " + std::to_string(a.id) + " and " + cat(b.id) + " " + std::to_string(b.id) + " overlap in wall " +
                                host.ToString();
                        return false;
                    }
                }
            }
        }
        // 最后查引用，上面的规则对引用的说法先出来
        return CheckIds(error);
    }

    bool Model::CheckIds(std::string& error) const
    {
        if (!LibraryInStep())
        {
            error = "the library's ids and GUIDs are not those of what it has";
            return false;
        }
        for (const auto& [id, e] : library.Entries())
        {
            if (id < FirstId)
            {
                error = e.kind.Label() + " `" + e.name + "`: ids start at " + std::to_string(FirstId);
                return false;
            }
            if (elements.Contains(id))
            {
                error = e.kind.Label() + " `" + e.name + "` and element " + std::to_string(id) + " have the same id";
                return false;
            }
            if (elements.Contains(e.guid))
            {
                error = e.kind.Label() + " `" + e.name + "` has the GUID of element " + Shown(e.guid);
                return false;
            }
            const auto named = Guid::Parse(e.name);
            if (named && !named->Pending())
            {
                error = e.kind.Label() + " `" + e.name + "`: a name isn't a GUID; name it otherwise";
                return false;
            }
        }
        for (const auto& [id, e] : elements.Items())
        {
            if (id < FirstId)
            {
                error = "element " + std::to_string(id) + ": ids start at " + std::to_string(FirstId);
                return false;
            }
            for (const Guid g : e->Refs())
            {
                if (const auto n = g.Pending())
                {
                    if (elements.Contains(*n))
                        error = std::string(e->Category()) + " " + std::to_string(id) + " points at element " + std::to_string(*n) + " by its id, not its GUID";
                    else
                        error = std::string(e->Category()) + " " + std::to_string(id) + " points at element " + std::to_string(*n) + ", which the project hasn't";
                    return false;
                }
                if (!elements.Contains(g))
                {
                    error = std::string(e->Category()) + " " + std::to_string(id) + " points at " + g.ToString() + ", which no element of the project is";
                    return false;
                }
            }
        }
        return true;
    }

    void Model::AdoptMaterials()
    {
        for (const Layered& t : LayeredTypes())
            for (const Layer& l : *t.layers)
                if (!l.material.empty() && !materials.count(l.material))
                    materials[l.material] = Material{};
    }

    uint64_t FreshUid()
    {
        const uint64_t nanos = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        const uint64_t pid = static_cast<uint64_t>(MINIBIM_GETPID());
        return Mix(nanos ^ ((pid << 32) | (pid >> 32)));
    }

    P2 Micron(P2 p)
    {
        // + 0.0：文件里不写 -0
        return P2(std::round(p.x * 1e3) / 1e3 + 0.0, std::round(p.y * 1e3) / 1e3 + 0.0);
    }
}
