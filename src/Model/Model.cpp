// 移植自 BIMCraft crates/core/src/model.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Model.h"
#include "Model/Massing.h"
#include "Model/Sketch.h"
#include "Model/Skeleton.h"
#include "Model/Text.h"
#include "Model/Walls.h"
#include "I18n/Translator.h"
#include <algorithm>
#include <charconv>
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

        // 构件（或带族的门窗）的族与它的类型交给 f：类型写作 "族: 类型"，在文件里是它的 GUID。族是 GUID 时（读文件）先给族，
        // 再给类型；否则最后给族（类型按族的名称查）
        void FamilyAndType(std::string& family, std::string& typeName, const std::function<void(const Kind&, std::string&)>& f)
        {
            const auto isGuid = [](const std::string& t)
            {
                const auto g = Guid::Parse(t);
                return g && !g->Pending();
            };
            if (isGuid(family))
                f(Kind::Of(Kind::Tag::Family), family);
            std::string full = isGuid(typeName) ? typeName : family + ": " + typeName;
            f(Kind::Of(Kind::Tag::FamilyType), full);
            if (!isGuid(family))
                f(Kind::Of(Kind::Tag::Family), family);
            // "族: 类型" 换回类型，用族现在的名称；GUID 仍是 GUID
            const std::string prefix = family + ": ";
            if (full.rfind(prefix, 0) == 0)
                typeName = full.substr(prefix.size());
            else if (isGuid(full))
                typeName = full;
            else if (const size_t at = full.find(": "); at != std::string::npos)
                typeName = full.substr(at + 2);
            else
                typeName = full;
        }

        // 两个族除了形体与符号的 ID、GUID 以外都一样：族的检查结果相同
        bool SameShape(const Family& a, const Family& b)
        {
            if (!(a.category == b.category && a.parameters == b.parameters && a.types == b.types && a.operation == b.operation &&
                  a.forms.size() == b.forms.size() && a.symbols.size() == b.symbols.size()))
                return false;
            for (size_t i = 0; i < a.forms.size(); ++i)
            {
                const Fam::Form& x = a.forms[i];
                const Fam::Form& y = b.forms[i];
                const bool sameMesh = x.mesh == y.mesh || (!x.HasMesh() && !y.HasMesh()) || (x.HasMesh() && y.HasMesh() && *x.mesh == *y.mesh);
                if (!(x.plane == y.plane && x.profile == y.profile && x.circle == y.circle && x.from == y.from && x.to == y.to && x.axis == y.axis &&
                      x.isVoid == y.isVoid && x.material == y.material && x.visible == y.visible && sameMesh))
                    return false;
            }
            for (size_t i = 0; i < a.symbols.size(); ++i)
            {
                const Fam::Symbol& x = a.symbols[i];
                const Fam::Symbol& y = b.symbols[i];
                if (!(x.line == y.line && x.arc == y.arc && x.visible == y.visible))
                    return false;
            }
            return true;
        }

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

        const char* IfcDoor(std::optional<Operation> op, bool right)
        {
            switch (op.value_or(Operation::Swing))
            {
            case Operation::Swing:         return right ? ".SINGLE_SWING_RIGHT." : ".SINGLE_SWING_LEFT.";
            case Operation::DoubleSwing:   return ".DOUBLE_DOOR_SINGLE_SWING.";
            case Operation::Sliding:       return right ? ".SLIDING_TO_RIGHT." : ".SLIDING_TO_LEFT.";
            case Operation::DoubleSliding: return ".DOUBLE_DOOR_SLIDING.";
            case Operation::Folding:       return right ? ".FOLDING_TO_RIGHT." : ".FOLDING_TO_LEFT.";
            case Operation::Fixed:         return ".NOTDEFINED.";
            }
            return ".NOTDEFINED.";
        }

        std::optional<std::pair<Operation, bool>> FromIfcDoor(std::string_view e)
        {
            while (!e.empty() && e.front() == '.')
                e.remove_prefix(1);
            while (!e.empty() && e.back() == '.')
                e.remove_suffix(1);
            const bool right = Text::EndsWith(e, "RIGHT");
            const auto is = [&](std::initializer_list<std::string_view> names)
            {
                for (const std::string_view n : names)
                    if (e == n)
                        return true;
                return false;
            };
            Operation op;
            if (is({ "SINGLE_SWING_LEFT", "SINGLE_SWING_RIGHT", "SWING_FIXED_LEFT", "SWING_FIXED_RIGHT", "DOUBLE_SWING_LEFT",
                     "DOUBLE_SWING_RIGHT" }))
                op = Operation::Swing;
            else if (is({ "DOUBLE_DOOR_SINGLE_SWING", "DOUBLE_DOOR_DOUBLE_SWING", "DOUBLE_DOOR_SINGLE_SWING_OPPOSITE_LEFT",
                          "DOUBLE_DOOR_SINGLE_SWING_OPPOSITE_RIGHT" }))
                op = Operation::DoubleSwing;
            else if (is({ "SLIDING_TO_LEFT", "SLIDING_TO_RIGHT" }))
                op = Operation::Sliding;
            else if (is({ "DOUBLE_DOOR_SLIDING" }))
                op = Operation::DoubleSliding;
            else if (is({ "FOLDING_TO_LEFT", "FOLDING_TO_RIGHT", "DOUBLE_DOOR_FOLDING" }))
                op = Operation::Folding;
            else
                return std::nullopt;
            return std::pair{ op, right };
        }

        const char* IfcWindowPartitioning(std::optional<Operation> op)
        {
            if (op == Operation::DoubleSwing || op == Operation::Sliding || op == Operation::DoubleSliding)
                return ".DOUBLE_PANEL_VERTICAL.";
            return ".SINGLE_PANEL.";
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

    const Column* Element::AsColumn() const
    {
        if (const Column* c = As<Column>())
            return c;
        if (const StructuralColumn* c = As<StructuralColumn>())
            return c;
        if (const Footing* c = As<Footing>())
            return c;
        return nullptr;
    }

    Column* Element::AsColumn()
    {
        return const_cast<Column*>(static_cast<const Element*>(this)->AsColumn());
    }

    const Run* Element::AsRun() const
    {
        return std::visit([](const auto& x) -> const Run*
        {
            if constexpr (std::is_base_of_v<Run, std::decay_t<decltype(x)>>)
                return &x;
            else
                return nullptr;
        }, v);
    }

    Run* Element::AsRun()
    {
        return const_cast<Run*>(static_cast<const Element*>(this)->AsRun());
    }

    std::optional<Element> Element::OfRun(std::string_view category, Run r)
    {
        if (category == "beam") return Element(Beam{ std::move(r) });
        if (category == "stripfooting") return Element(StripFooting{ std::move(r) });
        if (category == "pipe") return Element(Pipe{ std::move(r) });
        if (category == "conduit") return Element(Conduit{ std::move(r) });
        if (category == "cabletray") return Element(CableTray{ std::move(r) });
        if (category == "wire") return Element(Wire{ std::move(r) });
        if (category == "downconductor") return Element(DownConductor{ std::move(r) });
        if (category == "groundconductor") return Element(GroundConductor{ std::move(r) });
        if (category == "cable") return Element(Cable{ std::move(r) });
        return std::nullopt;
    }

    bool IsRunCategory(std::string_view category)
    {
        return std::find(RunCategories.begin(), RunCategories.end(), category) != RunCategories.end();
    }

    namespace JustifyOf
    {
        const char* Name(Justify j)
        {
            switch (j)
            {
            case Justify::Top:    return "top";
            case Justify::Bottom: return "bottom";
            default:              return "centre";
            }
        }

        std::optional<Justify> FromName(std::string_view name)
        {
            const std::string n = Text::AsciiLower(name);
            if (n == "top") return Justify::Top;
            if (n == "centre" || n == "center") return Justify::Centre;
            if (n == "bottom") return Justify::Bottom;
            return std::nullopt;
        }
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
            const char* operator()(const Component&) const { return "component"; }
            const char* operator()(const Column&) const { return "column"; }
            const char* operator()(const StructuralColumn&) const { return "structuralcolumn"; }
            const char* operator()(const Footing&) const { return "footing"; }
            const char* operator()(const Beam&) const { return "beam"; }
            const char* operator()(const StripFooting&) const { return "stripfooting"; }
            const char* operator()(const Pipe&) const { return "pipe"; }
            const char* operator()(const Conduit&) const { return "conduit"; }
            const char* operator()(const CableTray&) const { return "cabletray"; }
            const char* operator()(const Wire&) const { return "wire"; }
            const char* operator()(const DownConductor&) const { return "downconductor"; }
            const char* operator()(const GroundConductor&) const { return "groundconductor"; }
            const char* operator()(const Cable&) const { return "cable"; }
            const char* operator()(const Mass&) const { return "mass"; }
            const char* operator()(const Dimension&) const { return "dimension"; }
            const char* operator()(const Note&) const { return "text"; }
            const char* operator()(const Spot&) const { return "spot"; }
            const char* operator()(const Tag&) const { return "tag"; }
            const char* operator()(const DetailLine&) const { return "line"; }
            const char* operator()(const FilledRegion&) const { return "region"; }
            const char* operator()(const PlanView&) const { return "planview"; }
            const char* operator()(const View3D&) const { return "view3d"; }
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
            void operator()(View3D&) const {}
            void operator()(PlanView& v) const
            {
                f(v.level);
                if (v.parent) f(*v.parent);
            }
            void operator()(RoomSeparator& x) const { f(x.level); }
            void operator()(Wall& w) const
            {
                f(w.level);
                if (w.topLevel) f(*w.topLevel);
                if (w.attachTop) f(*w.attachTop);
                if (w.attachBase) f(*w.attachBase);
                Massing::RefsMut(w.params, f);
            }
            void operator()(Floor& x) const
            {
                f(x.level);
                Massing::RefsMut(x.params, f);
            }
            void operator()(Ceiling& x) const { f(x.level); }
            void operator()(Door& o) const { f(o.host); }
            void operator()(Window& o) const { f(o.host); }
            void operator()(Room& x) const { f(x.level); }
            void operator()(Roof& r) const
            {
                f(r.level);
                if (r.join) f(*r.join);
                Massing::RefsMut(r.params, f);
            }
            void operator()(Mass& ms) const
            {
                f(ms.level);
                if (ms.topLevel) f(*ms.topLevel);
            }
            void operator()(Dimension& d) const
            {
                f(d.level);
                if (d.view) f(*d.view);
                for (Reference& r : d.refs)
                    f(r.id);
            }
            void operator()(Note& n) const { f(n.view); }
            void operator()(Spot& s) const
            {
                f(s.view);
                if (s.host) f(*s.host);
            }
            void operator()(Tag& t) const
            {
                f(t.host);
                if (t.view) f(*t.view);
            }
            void operator()(DetailLine& l) const { f(l.view); }
            void operator()(FilledRegion& r) const { f(r.view); }
            void operator()(Component& c) const { f(c.level); }
            void operator()(Column& c) const { Stood(c); }
            void operator()(StructuralColumn& c) const { Stood(c); }
            void operator()(Footing& c) const { Stood(c); }
            void operator()(Beam& r) const { f(r.level); }
            void operator()(StripFooting& r) const { f(r.level); }
            void operator()(Pipe& r) const { f(r.level); }
            void operator()(Conduit& r) const { f(r.level); }
            void operator()(CableTray& r) const { f(r.level); }
            void operator()(Wire& r) const { f(r.level); }
            void operator()(DownConductor& r) const { f(r.level); }
            void operator()(GroundConductor& r) const { f(r.level); }
            void operator()(Cable& r) const { f(r.level); }
            void Stood(Column& c) const
            {
                f(c.level);
                if (c.topLevel) f(*c.topLevel);
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
            if (o->family)
                FamilyAndType(*o->family, o->typeName, f);
            else
                f(Kind::Type(category), o->typeName);
        }
        else if (Component* c = As<Component>())
            FamilyAndType(c->family, c->typeName, f);
        else if (Column* col = AsColumn())
            f(Kind::Type(category), col->typeName);
        else if (Run* r = AsRun())
            f(Kind::Type(category), r->typeName);
        else if (Note* n = As<Note>())
            f(Kind::Of(Kind::Tag::TextType), n->typeName);
        else if (DetailLine* l = As<DetailLine>())
            f(Kind::Of(Kind::Tag::LineStyle), l->style);
        else if (FilledRegion* r = As<FilledRegion>())
        {
            f(Kind::Of(Kind::Tag::FilledRegionType), r->typeName);
            if (r->lineStyle)
                f(Kind::Of(Kind::Tag::LineStyle), *r->lineStyle);
            for (SketchLine& l : r->sketch)
                if (l.style)
                    f(Kind::Of(Kind::Tag::LineStyle), *l.style);
        }
    }

    const std::vector<SketchLine>* Element::SketchOf() const
    {
        if (const Floor* f = As<Floor>())
            return &f->sketch;
        if (const Ceiling* c = As<Ceiling>())
            return &c->sketch;
        if (const Roof* r = As<Roof>())
            return &r->sketch;
        if (const FilledRegion* r = As<FilledRegion>())
            return &r->sketch;
        return nullptr;
    }

    std::vector<SketchLine>* Element::SketchOf()
    {
        return const_cast<std::vector<SketchLine>*>(static_cast<const Element*>(this)->SketchOf());
    }

    std::optional<std::pair<std::string, std::string>> Element::FamilyOf() const
    {
        if (const Component* c = As<Component>())
            return std::pair{ c->family, c->typeName };
        if (const Opening* o = AsOpening(); o && o->family)
            return std::pair{ *o->family, o->typeName };
        return std::nullopt;
    }

    std::string* Element::FamilyMut()
    {
        if (Component* c = As<Component>())
            return &c->family;
        if (Opening* o = AsOpening(); o && o->family)
            return &*o->family;
        return nullptr;
    }

    std::optional<Fam::Category> Element::HostedCategory() const
    {
        if (IsDoor())
            return Fam::Category::Door;
        if (IsWindow())
            return Fam::Category::Window;
        return std::nullopt;
    }

    const std::map<std::string, Json>* Element::Params() const
    {
        if (const Component* c = As<Component>())
            return &c->params;
        if (const Opening* o = AsOpening())
            return &o->params;
        if (const Wall* w = As<Wall>())
            return &w->params;
        if (const Floor* f = As<Floor>())
            return &f->params;
        if (const Roof* r = As<Roof>())
            return &r->params;
        if (const Mass* ms = As<Mass>())
            return &ms->params;
        return nullptr;
    }

    std::map<std::string, Json>* Element::ParamsMut()
    {
        return const_cast<std::map<std::string, Json>*>(static_cast<const Element*>(this)->Params());
    }

    // ── 体量 ─────────────────────────────────────────────────────
    bool Mass::Lofted() const
    {
        return top && *top != boundary;
    }

    Ring Mass::Footprint() const
    {
        Ring ring = boundary;
        if (Geom::Area(ring) < 0.0)
            std::reverse(ring.begin(), ring.end());
        return ring;
    }

    Ring Mass::TopRing() const
    {
        Ring ring = top && top->size() == boundary.size() ? *top : boundary;
        if (Geom::Area(boundary) < 0.0)
            std::reverse(ring.begin(), ring.end());
        return ring;
    }

    std::pair<double, double> Model::MassSpan(const Mass& ms) const
    {
        return Span(ms.level, ms.baseOffset, ms.topLevel, ms.height);
    }

    bool Model::MassCutBy(const Mass& ms, double z) const
    {
        const auto [bottom, top] = MassSpan(ms);
        return bottom <= z && z < top;
    }

    Ring Model::MassOutline(const Mass& ms, double z) const
    {
        if (!ms.Lofted())
            return ms.Footprint();
        const auto [bottom, top] = MassSpan(ms);
        const double t = top > bottom ? std::clamp((z - bottom) / (top - bottom), 0.0, 1.0) : 0.0;
        return Massing::RingAt(ms.Footprint(), ms.TopRing(), t);
    }

    double Model::MassVolume(const Mass& ms, double from, double to) const
    {
        const auto area = [&](double z) { return std::abs(Geom::Area(MassOutline(ms, z))); };
        return (to - from) / 6.0 * (area(from) + 4.0 * area((from + to) / 2.0) + area(to));
    }

    std::vector<Model::MassSlice> Model::MassSlices(const Mass& ms) const
    {
        const auto [bottom, top] = MassSpan(ms);
        std::vector<std::pair<Id, double>> crossed;
        for (const auto& [id, l] : Levels())
            if (l->elevation >= bottom - Tol && l->elevation < top - Tol)
                crossed.emplace_back(id, l->elevation);
        std::vector<MassSlice> out;
        for (size_t i = 0; i < crossed.size(); ++i)
            out.push_back({ crossed[i].first, crossed[i].second, (i + 1 < crossed.size() ? crossed[i + 1].second : top) - crossed[i].second });
        return out;
    }

    // ── 新模型 ───────────────────────────────────────────────────
    Model Model::New(std::string_view name, Language language)
    {
        Model m;
        m.format   = FileFormat;
        m.name     = std::string(name);
        m.uid      = FreshUid();
        m.language = language;
        m.linePatterns = Lines::Patterns(language);
        m.lineStyles   = Lines::Styles(language);
        m.fillPatterns = Fills::Patterns(language);
        m.regionTypes  = Fills::RegionTypes(language);
        m.textTypes    = TextTypes::Defaults(language);
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
            // 玻璃让身后的房子透出来
            m.materials[LanguageOf::Word(language, s.name)] =
                Material{ s.color, s.cut, s.surface, std::string_view(s.name) == "Glass" ? GlassTransparency : uint8_t(0) };
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
        m.types.column["Rectangular 400 x 400"] = ColumnType{ 400.0, 400.0, false };
        m.types.column["Round 300"]             = ColumnType{ 300.0, 300.0, true };
        // 结构的：混凝土柱、基础、梁与条形基础
        for (const auto& [n, w, d, round] : { std::tuple{ "Concrete 300 x 300", 300.0, 300.0, false }, std::tuple{ "Concrete 200 x 500", 200.0, 500.0, false },
                                              std::tuple{ "Concrete Round 400", 400.0, 400.0, true } })
            m.types.structuralColumn[n] = ColumnType{ w, d, round };
        for (const auto& [n, w] : { std::pair{ "Footing 1200 x 1200", 1200.0 }, std::pair{ "Footing 1500 x 1500", 1500.0 } })
            m.types.footing[n] = ColumnType{ w, w, false };
        struct StartRun
        {
            const char* category;
            const char* name;
            double      width, depth;
            bool        round;
            const char* material;
        };
        const StartRun runs[] = {
            { "beam", "Beam 200 x 500", 200.0, 500.0, false, "Concrete" },
            { "beam", "Beam 150 x 400", 150.0, 400.0, false, "Concrete" },
            { "beam", "Steel Tube 150", 150.0, 150.0, true, "Metal" },
            { "stripfooting", "Strip 600 x 300", 600.0, 300.0, false, "Concrete" },
            // 给排水：冷水与排水用 PVC（外径，毫米），热水用铜
            { "pipe", "PVC 25", 25.0, 25.0, true, "PVC" },
            { "pipe", "PVC 50", 50.0, 50.0, true, "PVC" },
            { "pipe", "PVC 110", 110.0, 110.0, true, "PVC" },
            { "pipe", "Copper 22", 22.0, 22.0, true, "Copper" },
            // 电气：PVC 与钢的线管、带孔的桥架与导线，按外部尺寸
            { "conduit", "PVC 20", 20.0, 20.0, true, "PVC" },
            { "conduit", "PVC 25", 25.0, 25.0, true, "PVC" },
            { "conduit", "PVC 32", 32.0, 32.0, true, "PVC" },
            { "conduit", "Steel 25", 25.0, 25.0, true, "Metal" },
            { "cabletray", "Perforated 100 x 50", 100.0, 50.0, false, "Metal" },
            { "cabletray", "Perforated 200 x 50", 200.0, 50.0, false, "Metal" },
            { "cabletray", "Perforated 300 x 100", 300.0, 100.0, false, "Metal" },
            { "wire", "Wire 2.5", 4.0, 4.0, true, "Copper" },
            { "wire", "Wire 4", 5.0, 5.0, true, "Copper" },
            { "wire", "Wire 6", 6.5, 6.5, true, "Copper" },
            // 防雷（NBR 5419）：钢、铜或扁钢的引下线，裸铜的接地线
            { "downconductor", "Steel 8", 8.0, 8.0, true, "Metal" },
            { "downconductor", "Copper 8", 8.0, 8.0, true, "Copper" },
            { "downconductor", "Flat 25 x 3", 25.0, 3.0, false, "Metal" },
            { "groundconductor", "Copper 9", 9.0, 9.0, true, "Copper" },
            { "groundconductor", "Steel 8", 8.0, 8.0, true, "Metal" },
            // 数据与语音布线（NBR 14565）：按类别的双绞线与光缆
            { "cable", "Cat6", 6.0, 6.0, true, "PVC" },
            { "cable", "Cat6A", 7.5, 7.5, true, "PVC" },
            { "cable", "Optical 3", 3.0, 3.0, true, "PVC" },
        };
        for (const StartRun& r : runs)
            m.types.runs[r.category][r.name] = RunType{ r.width, r.depth, r.round, LanguageOf::Word(language, r.material) };
        m.types.roof["Generic - 300mm"]    = RoofType{ 300.0, {} };
        m.types.ceiling["Generic - 20mm"]  = CeilingType{ 20.0, 0.0, {} };
        m.types.ceiling["Tile 600 x 600"]  = CeilingType{ 20.0, 600.0, {} };
        m.families = FamilyLibrary(language);
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

    PlanRange Model::GetPlanRange(Guid plan) const
    {
        // 顶没有设时为上一个标高（上面没有时不限），且不低于剖切面
        const Guid level = PlanLevel(plan).value_or(plan);
        const double z = Elevation(level);
        const ViewRange r = ViewRangeOf(plan);
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
        if (c == "column") return Keys(types.column);
        if (c == "roof")   return Keys(types.roof);
        if (c == "ceiling") return Keys(types.ceiling);
        if (c == "structuralcolumn") return Keys(types.structuralColumn);
        if (c == "footing") return Keys(types.footing);
        if (IsRunCategory(c))
        {
            const auto it = types.runs.find(c);
            return it == types.runs.end() ? std::vector<std::string>{} : Keys(it->second);
        }
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
        else if (c == "column")
            preferred = "Rectangular 400 x 400";
        else if (c == "structuralcolumn")
            preferred = "Concrete 300 x 300";
        else
        {
            static const std::pair<const char*, const char*> byRun[] = {
                { "footing", "Footing 1200 x 1200" }, { "beam", "Beam 200 x 500" }, { "stripfooting", "Strip 600 x 300" },
                { "pipe", "PVC 50" }, { "conduit", "PVC 25" }, { "cabletray", "Perforated 100 x 50" }, { "wire", "Wire 2.5" },
                { "downconductor", "Steel 8" }, { "groundconductor", "Copper 9" }, { "cable", "Cat6" },
            };
            for (const auto& [cat, name] : byRun)
                if (c == cat)
                    preferred = name;
        }
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
            const std::string& name = *o->family;
            const auto it = families.find(name);
            if (it == families.end())
            {
                std::vector<std::string> names;
                for (const auto& [n, f] : families)
                    if (f.category == e.HostedCategory())
                        names.push_back(n);
                error = "family `" + name + "` doesn't exist (there are: " + Join(names) + ")";
                return false;
            }
            const Family& f = it->second;
            if (f.category != e.HostedCategory())
            {
                error = "family `" + name + "` isn't a " + e.Category() + " family";
                return false;
            }
            const Wall* w = GetWall(o->host);
            Fam::Values values;
            if (!f.ValuesIn(o->typeName, o->params, w ? Thickness(*w) : Fam::DefaultWall, values, error))
                return false;
            const auto number = [&](const char* key) -> std::optional<double>
            {
                const auto v = values.find(key);
                if (v == values.end())
                    return std::nullopt;
                if (const double* d = std::get_if<double>(&v->second))
                    return *d;
                return std::nullopt;
            };
            const auto width = number(Fam::Width), height = number(Fam::Height);
            if (!width || !height)
            {
                error = "family `" + name + "` has no length `" + (width ? Fam::Height : Fam::Width) + "`";
                return false;
            }
            out = OpeningType{ *width, *height, number(Fam::Sill).value_or(e.IsDoor() ? 0.0 : 900.0), f.operation };
            return true;
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

    std::optional<std::pair<double, double>> Model::OpeningSpan(const Opening& o) const
    {
        const Wall* host = GetWall(o.host);
        if (!host || host->arc != 0.0)
            return std::nullopt;
        const Walls::Axis axis = Walls::Axis::Of(*host);
        const double length = axis.Length();
        if (length <= Tol)
            return std::nullopt;
        const P2 a = axis.At(0.0, 0.0), b = axis.At(length, 0.0);
        auto along = [&](P2 p) { return (p - a).Dot(b - a) / length; };
        // 连上它的墙停在它的定位线或某一面上
        const auto [ext, in] = Walls::Faces(*this, *host);
        const double reach = std::max(std::abs(ext), std::abs(in)) + Tol;
        const auto hostId = elements.IdOf(o.host);
        std::vector<double> hits;
        for (const auto& [id, ptr] : elements.Items())
        {
            const Wall* w = ptr->As<Wall>();
            if (!w || (hostId && id == *hostId) || w->arc != 0.0 || !CutBy(*w, host->level))
                continue;
            const Walls::Axis wa = Walls::Axis::Of(*w);
            const auto [we, wi] = Walls::Faces(*this, *w);
            for (const double off : { we, wi })
            {
                const P2 p = wa.At(0.0, off), q = wa.At(wa.Length(), off);
                if (const auto hit = Geom::LineIntersection(a, b, p, q); hit && Geom::DistToSegment(*hit, p, q) <= reach)
                    hits.push_back(along(*hit));
            }
        }
        double start = 0.0, end = length;
        for (const double t : hits)
        {
            if (t < o.offset)
                start = std::max(start, t);
            if (t > o.offset)
                end = std::min(end, t);
        }
        return std::pair{ start, end };
    }

    std::optional<double> Model::Centred(const Opening& o, double near) const
    {
        const auto span = OpeningSpan(o);
        if (!span)
            return std::nullopt;
        const double middle = (span->first + span->second) / 2.0;
        if (std::abs(o.offset - middle) <= near)
            return middle;
        return std::nullopt;
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
        if (const Mass* ms = e.As<Mass>())
            return ms->level;
        if (const Component* c = e.As<Component>())
            return c->level;
        if (const Column* c = e.AsColumn())
            return c->level;
        if (const Run* r = e.AsRun())
            return r->level;
        if (const Opening* o = e.AsOpening())
        {
            const Wall* w = GetWall(o->host);
            if (!w)
                return std::nullopt;
            return w->level;
        }
        if (const Dimension* d = e.As<Dimension>())
            return d->level;
        if (const Tag* t = e.As<Tag>())
        {
            const Element* h = elements.Get(t->host);
            return h && !h->As<Tag>() ? LevelOf(*h) : std::nullopt;
        }
        if (const PlanView* v = e.As<PlanView>())
            return v->level;
        // 文字、高程点、详图线、填充区域在标高的平面或平面视图里
        if (const auto plan = AnnotationPlan(e))
            return PlanLevel(*plan);
        return std::nullopt;
    }

    // ── 柱 ───────────────────────────────────────────────────────
    std::pair<double, double> Model::Span(Guid level, double baseOffset, const std::optional<Guid>& topLevel, double height) const
    {
        const double base = Elevation(level) + baseOffset;
        return { base, topLevel ? Elevation(*topLevel) : base + height };
    }

    std::pair<double, double> Model::ColumnSpan(const Column& c) const
    {
        return Span(c.level, c.baseOffset, c.topLevel, c.height);
    }

    const ColumnType* Model::ColumnTypeOf(const Element& e) const
    {
        const Column* col = e.AsColumn();
        if (!col)
            return nullptr;
        const auto& map = e.As<StructuralColumn>() ? types.structuralColumn : e.As<Footing>() ? types.footing : types.column;
        const auto it = map.find(col->typeName);
        return it == map.end() ? nullptr : &it->second;
    }

    const RunType* Model::RunTypeOf(const Element& e) const
    {
        const Run* r = e.AsRun();
        if (!r)
            return nullptr;
        const auto cat = types.runs.find(e.Category());
        if (cat == types.runs.end())
            return nullptr;
        const auto it = cat->second.find(r->typeName);
        return it == cat->second.end() ? nullptr : &it->second;
    }

    std::optional<Runs::Geom> Model::RunGeom(const Element& e) const
    {
        const Run*     r = e.AsRun();
        const RunType* t = RunTypeOf(e);
        if (!r || !t)
            return std::nullopt;
        return Runs::Geom::Of(*this, *r, *t);
    }

    Ring Model::ColumnOutline(const Element& e) const
    {
        const Column*     col = e.AsColumn();
        const ColumnType* t   = ColumnTypeOf(e);
        if (!col || !t)
            return {};
        auto heading = [](double deg) { const double a = deg * kPi / 180.0; return P2{ std::cos(a), std::sin(a) }; };
        Ring out;
        if (t->round)
        {
            for (int i = 0; i < 24; ++i)
                out.push_back(col->at + heading(i * 15.0) * (t->width / 2.0));
            return out;
        }
        const P2 u = heading(col->angle);
        const double w = t->width / 2.0, d = t->depth / 2.0;
        for (const auto& [a, b] : { std::pair{ -w, -d }, std::pair{ w, -d }, std::pair{ w, d }, std::pair{ -w, d } })
            out.push_back(col->at + u * a + u.Perp() * b);
        return out;
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
    namespace
    {
        // 墙中心线的一段与半个墙厚
        struct HalfWall
        {
            P2     a, b;
            double half;
        };

        // 从 from 经 end 的方向上，end 落在其中的墙（离中心线不到半个墙厚）的中心线在哪里：顺着线的方向接着走到那里；
        // end 不在墙里或已在中心线上时就是 end
        P2 ToCentre(P2 from, P2 end, const std::vector<HalfWall>& walls)
        {
            const P2 way = (end - from).Unit();
            if (!way.IsFinite())
                return end;
            std::optional<double> nearest;
            for (const HalfWall& w : walls)
            {
                if (w.half <= 0.0 || Geom::DistToSegment(end, w.a, w.b) > w.half + Tol)
                    continue;
                // 够穿过斜着碰到的墙，又到不了再过去的墙
                const double reach = 4.0 * w.half + Tol;
                if (const auto t = Geom::Crossing(end, end + way * reach, w.a, w.b); t && (!nearest || *t * reach < *nearest))
                    nearest = *t * reach;
            }
            return nearest ? end + way * *nearest : end;
        }
    }

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
        // 墙中心线各段带上半个墙厚：画到墙面的分隔线（平面上通常这样画）接着走到墙中心线，在中心线上量的房间也被它分开
        std::vector<HalfWall> walls;
        for (const auto& [id, e] : elements.Items())
        {
            const Wall* w = e->As<Wall>();
            if (!w || !Bounding(*w, level))
                continue;
            const double half = Thickness(*w) / 2.0;
            const std::vector<P2> line = Walls::CentreLine(*this, id);
            for (size_t i = 0; i + 1 < line.size(); ++i)
                walls.push_back({ line[i], line[i + 1], half });
        }
        std::vector<Segment> lines;
        for (const HalfWall& w : walls)
            lines.emplace_back(w.a, w.b);
        for (const auto& [a, b] : Separators(level))
            lines.emplace_back(ToCentre(b, a, walls), ToCentre(a, b, walls));
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
        if (WallHolding(r, at))
            return std::nullopt;
        return Geom::RegionAt(RoomLines(r.level, at), r.point, Tol);
    }

    std::optional<Id> Model::WallHolding(const Room& r, RoomArea at) const
    {
        if (at != RoomArea::Finish)
            return std::nullopt;
        for (const auto& [id, e] : elements.Items())
            if (const Wall* w = e->As<Wall>(); w && Bounding(*w, r.level) && Geom::Contains(Walls::Outline(*this, id), r.point))
                return id;
        return std::nullopt;
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
        struct HeldOpening
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
        for (const auto& [cat, map] : { std::pair{ "column", &types.column }, std::pair{ "structural column", &types.structuralColumn },
                                        std::pair{ "footing", &types.footing } })
            for (const auto& [n, t] : *map)
            {
                const std::string who = std::string(cat) + " type `" + n + "`";
                if (!Size(who + " width", t.width, error) || !Size(who + " depth", t.depth, error))
                    return false;
            }
        for (const auto& [cat, map] : types.runs)
        {
            if (!IsRunCategory(cat))
            {
                std::vector<std::string> all(RunCategories.begin(), RunCategories.end());
                std::string list;
                for (size_t i = 0; i < all.size(); ++i)
                    list += (i ? ", " : "") + all[i];
                error = "`" + cat + "` has no run types; there are " + list;
                return false;
            }
            for (const auto& [n, t] : map)
            {
                const std::string who = cat + " type `" + n + "`";
                if (!Size(who + " width", t.width, error) || !Size(who + " depth", t.depth, error))
                    return false;
                if (t.round && t.depth != t.width)
                {
                    error = who + ": a round section is as deep as it is wide (" + Num(t.width) + " mm), not " + Num(t.depth) + " mm";
                    return false;
                }
                if (!TextOk(who, "material", t.material, error))
                    return false;
                if (!t.material.empty() && !materials.count(t.material))
                {
                    error = who + ": material `" + t.material + "` isn't in the library; add it with `material`";
                    return false;
                }
            }
        }
        for (const auto& [n, x] : materials)
        {
            if (!TextOk("material `" + n + "`", "name", n, error))
                return false;
            if (Text::Trim(n).empty())
            {
                error = "a material needs a name";
                return false;
            }
            if (x.transparency > 100)
            {
                error = "material `" + n + "`: transparency is a percent, 0 to 100, not " + std::to_string(x.transparency);
                return false;
            }
        }
        // 族的检查要把每个类型建一遍（含空心形体的切割），很费时。它只看族本身（不看形体与符号的 ID、GUID）和库里有哪些材质：
        // 每个线程记住查过、通过的族，同样的族在同样的材质名称下不再查（BIMCraft 每次都查）
        static thread_local std::map<std::string, Family> passed;
        static thread_local std::vector<std::string> passedMaterials;
        std::vector<std::string> materialNames;
        for (const auto& [n, _] : materials)
            materialNames.push_back(n);
        if (materialNames != passedMaterials)
        {
            passed.clear();
            passedMaterials = materialNames;
        }
        for (const auto& [n, f] : families)
        {
            if (!TextOk("family `" + n + "`", "name", n, error))
                return false;
            if (Text::Trim(n).empty())
            {
                error = "a family needs a name";
                return false;
            }
            if (const auto it = passed.find(n); it != passed.end() && SameShape(it->second, f))
                continue;
            std::string why;
            if (!f.Check([&](const std::string& m) { return materials.count(m) != 0; }, why))
            {
                error = "family `" + n + "`: " + why;
                return false;
            }
            passed[n] = f;
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
        if (!CheckStyles(error))
            return false;
        for (const auto& [n, t] : types.ceiling)
        {
            // 更细的格子每隔几个像素就画一条线，大天花板上有几百万条
            if (!(t.grid == 0.0 || (t.grid >= 100.0 && t.grid <= MaxCoord)))
            {
                error = "ceiling type `" + n + "`: grid " + Num(t.grid) + " mm; its tiles are 100 mm or wider, or 0 for none";
                return false;
            }
        }
        // 元素存的参数值都是它的族的参数，或体量的记号（项目参数还没移植，别的键都不认）
        const auto ParamsOk = [&](const std::string& who, const Element& e, std::string& err)
        {
            const auto* params = e.Params();
            if (!params)
                return true;
            const auto fo = e.FamilyOf();
            const auto fam = fo ? families.find(fo->first) : families.end();
            for (const auto& [k, v] : *params)
            {
                if (Massing::Reserved(k))
                {
                    if (!Massing::CheckValue(*this, who, k, v, e, err))
                        return false;
                    continue;
                }
                if (fam != families.end() && std::any_of(fam->second.parameters.begin(), fam->second.parameters.end(),
                                                         [&](const Fam::Parameter& p) { return p.name == k; }))
                    continue;
                err = who + ": `" + k + "` isn't a parameter of " + e.Category() + "s; add it with `parameter`";
                return false;
            }
            return true;
        };
        std::set<std::string> levelNames, gridNames, viewNames;
        // 每面墙上的门窗，按沿墙的起点排序
        std::map<Guid, std::vector<HeldOpening>> spans;
        for (const auto& [id, ptr] : elements.Items())
        {
            const Element& e = *ptr;
            const std::string who = std::string(e.Category()) + " " + std::to_string(id);
            if (const auto* lines = e.SketchOf(); lines && !Sketch::Check(who, *lines, error))
                return false;
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
                if (!viewNames.insert(l->name).second)
                {
                    error = who + ": another view is already named `" + l->name + "`";
                    return false;
                }
                if (!l->graphics.Check(fillPatterns, error))
                {
                    error = who + ": " + error;
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
            else if (const PlanView* v = e.As<PlanView>())
            {
                if (!TextOk(who, "name", v->name, error))
                    return false;
                if (Text::Trim(v->name).empty())
                {
                    error = who + ": a view needs a name";
                    return false;
                }
                if (!levelOk(v->level) || !RangeOk(who, v->viewRange, error))
                    return false;
                if (v->crop && !Crop(who, (*v->crop)[0], (*v->crop)[1], error))
                    return false;
                if (!v->graphics.Check(fillPatterns, error))
                {
                    error = who + ": " + error;
                    return false;
                }
                // 标高的名称是它自己的平面的；它的平面视图另起名称
                for (const auto& [_, l] : Levels())
                    if (l->name == v->name)
                    {
                        error = who + ": a level is already named `" + v->name + "`; a plan view of it needs another name";
                        return false;
                    }
                if (v->parent)
                {
                    // 父视图是同一标高的平面：标高自己的，或不是相关视图的平面视图
                    const Element* p = elements.Get(*v->parent);
                    bool primary = false;
                    if (p && p->As<Level>())
                        primary = Same(*v->parent, v->level);
                    else if (const PlanView* o = p ? p->As<PlanView>() : nullptr)
                        primary = !o->parent && Same(o->level, v->level);
                    if (!primary)
                    {
                        error = who + ": a dependent view hangs from a plan of its own level that is no dependent itself, not " + Shown(*v->parent);
                        return false;
                    }
                }
                if (!viewNames.insert(v->name).second)
                {
                    error = who + ": another view is already named `" + v->name + "`";
                    return false;
                }
            }
            else if (const View3D* v3 = e.As<View3D>())
            {
                if (!TextOk(who, "name", v3->name, error))
                    return false;
                if (Text::Trim(v3->name).empty())
                {
                    error = who + ": a view needs a name";
                    return false;
                }
                if (!Coord(who + " azimuth", v3->azimuth, error))
                    return false;
                if (!(v3->elevation >= -89.0 && v3->elevation <= 89.0))
                {
                    error = who + ": elevation " + Num(v3->elevation) + "° isn't between -89° and 89°";
                    return false;
                }
                if (!v3->graphics.Check(fillPatterns, error))
                {
                    error = who + ": " + error;
                    return false;
                }
                if (const auto& b = v3->sectionBox)
                    for (size_t i = 0; i < 3; ++i)
                        if (!Coord(who + " section box", b->min[i], error) || !Coord(who + " section box", b->max[i], error))
                            return false;
                if (!viewNames.insert(v3->name).second)
                {
                    error = who + ": another view is already named `" + v3->name + "`";
                    return false;
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
            else if (const Mass* ms = e.As<Mass>())
            {
                if (!levelOk(ms->level) || !Boundary(who, ms->boundary, error) || !TextOk(who, "name", ms->name, error) ||
                    !Coord(who + " base_offset", ms->baseOffset, error) || !ParamsOk(who, e, error))
                    return false;
                if (ms->topLevel ? !levelOk(*ms->topLevel) : !Size(who + " height", ms->height, error))
                    return false;
                const auto [bottom, top] = MassSpan(*ms);
                if (top <= bottom + Tol)
                {
                    error = who + ": its top must be above its base";
                    return false;
                }
                if (ms->top)
                {
                    // 先查它的点：过了这一关，两个轮廓才配得上、截面才算得出
                    if (ms->top->size() == ms->boundary.size() && !Boundary(who + " top", *ms->top, error))
                        return false;
                    if (const auto why = Massing::LoftProblem(ms->boundary, *ms->top))
                    {
                        error = who + ": " + *why;
                        return false;
                    }
                }
            }
            else if (e.IsAnnotation())
            {
                if (!CheckAnnotation(who, e, error))
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
            else if (const Component* c = e.As<Component>())
            {
                const auto it = families.find(c->family);
                if (it == families.end())
                {
                    std::vector<std::string> names;
                    for (const auto& [n, _] : families)
                        names.push_back(n);
                    error = who + ": family `" + c->family + "` doesn't exist (there are: " + Join(names) + ")";
                    return false;
                }
                const Family& f = it->second;
                if (Fam::CategoryOf::Hosted(f.category))
                {
                    error = who + ": family `" + c->family + "` is a door's or a window's: it stands in a wall, placed with `door` or `window`";
                    return false;
                }
                if (!ParamsOk(who, e, error) || !levelOk(c->level) || !Point(who, c->at, error) || !Coord(who + " angle", c->angle, error) ||
                    !Coord(who + " offset", c->offset, error) || !FamilyValuesOk(who, c->family, f, c->params, error))
                    return false;
                Fam::Values values;
                std::vector<Fam::Piece> pieces;
                std::string why;
                if (!f.Values(c->typeName, c->params, values, why) || !f.Pieces(values, pieces, why))
                {
                    error = who + ": " + why;
                    return false;
                }
            }
            else if (const Column* col = e.AsColumn())
            {
                const auto& map = e.As<StructuralColumn>() ? types.structuralColumn : e.As<Footing>() ? types.footing : types.column;
                if (!Known(who, col->typeName, map, error) || !levelOk(col->level) ||
                    !Point(who, col->at, error) || !Coord(who + " angle", col->angle, error) || !Coord(who + " base_offset", col->baseOffset, error))
                    return false;
                if (col->topLevel ? !levelOk(*col->topLevel) : !Size(who + " height", col->height, error))
                    return false;
                const auto [bottom, top] = ColumnSpan(*col);
                if (top <= bottom + Tol)
                {
                    error = who + ": its top must be above its base";
                    return false;
                }
            }
            else if (const Run* r = e.AsRun())
            {
                const std::string category = e.Category();
                static const std::map<std::string, RunType> none;
                const auto cat = types.runs.find(category);
                const auto& map = cat == types.runs.end() ? none : cat->second;
                if (!Known(who, r->typeName, map, error) || !levelOk(r->level) || !Point(who + " start", r->start, error) ||
                    !Point(who + " end", r->end, error) || !Coord(who + " offset", r->offset, error) ||
                    (r->endOffset && !Coord(who + " end_offset", *r->endOffset, error)) || !TextOk(who, "system", r->system, error) ||
                    !Runs::Check(who, category, Runs::Geom::Of(*this, *r, map.at(r->typeName)), error))
                    return false;
            }
            else if (const Roof* r = e.As<Roof>())
            {
                if (!Known(who, r->typeName, types.roof, error) || !levelOk(r->level) || !Coord(who + " overhang", r->overhang, error) ||
                    !Coord(who + " offset", r->offset, error) || !ParamsOk(who, e, error))
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
                    !Coord(who + " base_offset", w->baseOffset, error) || !ParamsOk(who, e, error))
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
                    !Openings(who, f->boundary, f->openings, error) || !Coord(who + " offset", f->offset, error) || !ParamsOk(who, e, error))
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
                if (!ParamsOk(who, e, error))
                    return false;
                if (o->family)
                    if (const auto it = families.find(*o->family); it != families.end())
                    {
                        const Family& f = it->second;
                        if (!FamilyValuesOk(who, *o->family, f, o->params, error))
                            return false;
                        Fam::Values values;
                        std::vector<Fam::Piece> pieces;
                        std::vector<Ring> strokes;
                        if (!f.ValuesIn(o->typeName, o->params, Thickness(*host), values, why) || !f.Pieces(values, pieces, why) ||
                            !f.Strokes(values, strokes, why))
                        {
                            error = who + ": " + why;
                            return false;
                        }
                        if (!Size(who + " width", t.width, error) || !Size(who + " height", t.height, error))
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
                const HeldOpening span{ a, b, o->sill, o->sill + t.height, id };
                const auto at = std::partition_point(list.begin(), list.end(), [&](const HeldOpening& held) { return held.start <= a; });
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
                    const HeldOpening& a = v[i];
                    const HeldOpening& b = v[j];
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
        std::vector<std::string> named;
        for (const Layered& t : LayeredTypes())
            for (const Layer& l : *t.layers)
                named.push_back(l.material);
        for (const auto& [_, f] : families)
            for (std::string& m : f.Materials())
                named.push_back(std::move(m));
        for (const auto& [_, e] : elements.Items())
            for (std::string& m : ComponentMaterials(*e))
                named.push_back(std::move(m));
        for (const std::string& n : named)
            if (!n.empty() && !materials.count(n))
                materials[n] = Material{};
    }

    // ── 族与构件 ─────────────────────────────────────────────────
    std::vector<Fam::Piece> Model::ComponentLocalPieces(const Component& c) const
    {
        const auto it = families.find(c.family);
        if (it == families.end())
            return {};
        Fam::Values values;
        std::vector<Fam::Piece> pieces;
        std::string error;
        if (!it->second.Values(c.typeName, c.params, values, error) || !it->second.Pieces(values, pieces, error))
            return {};
        if (!c.mirrored)
            return pieces;
        // 镜像时 y 绕 x 翻过来，三角形与轮廓的绕向也随之翻
        for (Fam::Piece& pc : pieces)
        {
            for (Tri& t : pc.tris)
            {
                for (P3& q : t)
                    q[1] = -q[1];
                std::swap(t[1], t[2]);
            }
            std::reverse(pc.outline.begin(), pc.outline.end());
            for (P2& q : pc.outline)
                q.y = -q.y;
        }
        return pieces;
    }

    std::vector<Fam::Piece> Model::ComponentPieces(const Component& c) const
    {
        const double base = Elevation(c.level) + c.offset;
        const double a = c.angle * kPi / 180.0, sin = std::sin(a), cos = std::cos(a);
        const auto place = [&](const P3& p) { return P3{ c.at.x + p[0] * cos - p[1] * sin, c.at.y + p[0] * sin + p[1] * cos, base + p[2] }; };
        std::vector<Fam::Piece> out = ComponentLocalPieces(c);
        for (Fam::Piece& pc : out)
        {
            for (Tri& t : pc.tris)
                for (P3& q : t)
                    q = place(q);
            for (P2& q : pc.outline)
            {
                const P3 p = place(P3{ q.x, q.y, 0.0 });
                q = P2(p[0], p[1]);
            }
            pc.bottom = base + pc.bottom;
            pc.top    = base + pc.top;
        }
        return out;
    }

    std::optional<std::pair<std::string, std::string>> Model::DefaultFamily(std::string_view category) const
    {
        const std::string c = NormalizeCategory(category);
        Fam::Category kind = Fam::Category::Door;
        const char* usual = "Single Door";
        const char* size = "900 x 2100";
        if (c == "window")
        {
            kind = Fam::Category::Window;
            usual = "Fixed Window";
            size = "1200 x 1500";
        }
        else if (c != "door")
            return std::nullopt;
        auto it = families.find(LanguageOf::Word(language, usual));
        if (it == families.end() || it->second.category != kind)
            it = std::find_if(families.begin(), families.end(), [&](const auto& f) { return f.second.category == kind; });
        if (it == families.end() || it->second.types.empty())
            return std::nullopt;
        const std::string ty = it->second.types.count(size) ? std::string(size) : it->second.types.begin()->first;
        return std::pair{ it->first, ty };
    }

    std::optional<std::tuple<P2, P2, double>> Model::OpeningFrame(const Opening& o) const
    {
        const Wall* w = GetWall(o.host);
        if (!w)
            return std::nullopt;
        const Walls::Axis axis = Walls::Axis::Of(*w);
        return std::tuple{ axis.At(o.offset, Walls::Centre(*this, *w)), axis.Tangent(o.offset), OwnBase(*w) + o.sill };
    }

    std::optional<std::pair<std::vector<Fam::Piece>, std::vector<Ring>>> Model::OpeningLocal(const Element& e) const
    {
        const Opening* o = e.AsOpening();
        if (!o || !o->family)
            return std::nullopt;
        const auto it = families.find(*o->family);
        const Wall* w = GetWall(o->host);
        if (it == families.end() || !w)
            return std::nullopt;
        const Family& f = it->second;
        Fam::Values values;
        std::vector<Fam::Piece> pieces;
        std::vector<Ring> strokes;
        std::string error;
        if (!f.ValuesIn(o->typeName, o->params, Thickness(*w), values, error) || !f.Pieces(values, pieces, error) || !f.Strokes(values, strokes, error))
            return std::nullopt;
        // 翻转时是镜像：铰链到另一侧门框、开向墙的另一侧
        const double fx = o->flipHand ? -1.0 : 1.0, fy = o->flipFacing ? -1.0 : 1.0;
        const bool mirrored = fx * fy < 0.0;
        for (Fam::Piece& pc : pieces)
        {
            for (Tri& t : pc.tris)
            {
                for (P3& q : t)
                    q = P3{ fx * q[0] + 0.0, fy * q[1] + 0.0, q[2] };
                if (mirrored)
                    std::swap(t[1], t[2]);
            }
            for (P2& q : pc.outline)
                q = P2(fx * q.x + 0.0, fy * q.y + 0.0);
            if (mirrored)
                std::reverse(pc.outline.begin(), pc.outline.end());
        }
        for (Ring& l : strokes)
            for (P2& q : l)
                q = P2(fx * q.x + 0.0, fy * q.y + 0.0);
        return std::pair{ std::move(pieces), std::move(strokes) };
    }

    std::vector<Fam::Piece> Model::OpeningPieces(const Element& e) const
    {
        const Opening* o = e.AsOpening();
        if (!o)
            return {};
        auto local = OpeningLocal(e);
        const auto frame = OpeningFrame(*o);
        if (!local || !frame)
            return {};
        const auto [at, u, z] = *frame;
        const P2 a = at, d = u;
        const double base = z;
        const auto place = [&](const P3& p)
        {
            const P2 q = a + d * p[0] + d.Perp() * p[1];
            return P3{ q.x, q.y, base + p[2] };
        };
        std::vector<Fam::Piece> out = std::move(local->first);
        for (Fam::Piece& pc : out)
        {
            for (Tri& t : pc.tris)
                for (P3& q : t)
                    q = place(q);
            for (P2& q : pc.outline)
            {
                const P3 p = place(P3{ q.x, q.y, 0.0 });
                q = P2(p[0], p[1]);
            }
            pc.bottom = base + pc.bottom;
            pc.top    = base + pc.top;
        }
        return out;
    }

    std::vector<Ring> Model::OpeningStrokes(const Element& e) const
    {
        const Opening* o = e.AsOpening();
        if (!o)
            return {};
        auto local = OpeningLocal(e);
        const auto frame = OpeningFrame(*o);
        if (!local || !frame)
            return {};
        const P2 at = std::get<0>(*frame), u = std::get<1>(*frame);
        std::vector<Ring> out = std::move(local->second);
        for (Ring& l : out)
            for (P2& q : l)
                q = at + u * q.x + u.Perp() * q.y;
        return out;
    }

    std::vector<std::string> Model::ComponentMaterials(const Element& e) const
    {
        const auto fo = e.FamilyOf();
        const auto* params = e.Params();
        if (!fo || !params)
            return {};
        const auto it = families.find(fo->first);
        if (it == families.end())
            return {};
        std::vector<std::string> out;
        for (const auto& [k, v] : *params)
            if (it->second.MaterialParameter(k) && v.is_string())
                out.push_back(v.get<std::string>());
        return out;
    }

    std::optional<std::string> Model::MaterialUser(const std::string& name) const
    {
        for (const auto& [n, f] : families)
        {
            const auto ms = f.Materials();
            if (std::find(ms.begin(), ms.end(), name) != ms.end())
                return "family `" + n + "`";
        }
        for (const auto& [id, e] : elements.Items())
        {
            const auto ms = ComponentMaterials(*e);
            if (std::find(ms.begin(), ms.end(), name) != ms.end())
                return "component " + std::to_string(id);
        }
        return std::nullopt;
    }

    void Model::RenameMaterialInFamilies(const std::string& from, const std::string& to)
    {
        for (auto& [_, f] : families)
            f.RenameMaterial(from, to);
        std::vector<Id> touched;
        for (const auto& [id, e] : elements.Items())
            if (const auto ms = ComponentMaterials(*e); std::find(ms.begin(), ms.end(), from) != ms.end())
                touched.push_back(id);
        for (const Id id : touched)
        {
            Element* e = elements.GetMut(id);
            const auto fo = e->FamilyOf();
            const auto it = fo ? families.find(fo->first) : families.end();
            if (it == families.end())
                continue;
            for (auto& [k, v] : *e->ParamsMut())
                if (it->second.MaterialParameter(k) && v.is_string() && v.get<std::string>() == from)
                    v = to;
        }
    }

    bool Model::FamilyValuesOk(const std::string& who, const std::string& name, const Family& f, const std::map<std::string, Json>& params,
                               std::string& error) const
    {
        for (const auto& [k, v] : params)
        {
            const auto p = std::find_if(f.parameters.begin(), f.parameters.end(), [&](const Fam::Parameter& q) { return q.name == k; });
            if (p == f.parameters.end())
                continue;
            if (!p->instance)
            {
                error = who + ": `" + k + "` is a type parameter of `" + name + "`; change it in its type, with `family`";
                return false;
            }
            if (p->formula)
            {
                error = who + ": `" + k + "` is worked out by its formula";
                return false;
            }
            Fam::Val val;
            std::string why;
            if (!Fam::Read(*p, v, val, why))
            {
                error = who + ": " + why;
                return false;
            }
            if (const std::string* m = std::get_if<std::string>(&val); m && !m->empty() && !materials.count(*m))
            {
                error = who + ": material `" + *m + "` isn't in the library; add it with `material`";
                return false;
            }
        }
        return true;
    }

    void Model::RekeyFamilyParams(const std::function<std::optional<std::string>(const std::string&, const std::string&)>& key,
                                  const std::function<std::optional<std::string>(const std::string&)>& material)
    {
        std::vector<Id> ids;
        for (const auto& [id, e] : elements.Items())
            if (const auto fo = e->FamilyOf(); fo && families.count(fo->first) && e->Params() && !e->Params()->empty())
                ids.push_back(id);
        for (const Id id : ids)
        {
            Element* e = elements.GetMut(id);
            const std::string family = e->FamilyOf()->first;
            const Family& fam = families.at(family);
            std::map<std::string, Json>& params = *e->ParamsMut();
            std::vector<std::string> names;
            for (const auto& [n, _] : params)
                names.push_back(n);
            for (const std::string& n : names)
            {
                const auto k = key(family, n);
                if (!k)
                    continue;
                Json v = params[n];
                params.erase(n);
                // 参数按名称找：给的那个，或键所指的
                const auto named = std::find_if(fam.parameters.begin(), fam.parameters.end(),
                                                [&](const Fam::Parameter& p) { return p.name == n || p.name == *k; });
                if (named != fam.parameters.end() && named->kind == Fam::Kind::Material && v.is_string())
                    if (const auto to = material(v.get<std::string>()))
                        v = *to;
                params[*k] = std::move(v);
            }
        }
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

    std::optional<Id> Culprit(std::string_view error)
    {
        const auto words = Text::SplitWhitespace(error);
        if (words.size() < 2)
            return std::nullopt;
        std::string_view w = words[1];
        while (!w.empty() && w.back() == ':')
            w.remove_suffix(1);
        Id id = 0;
        const auto [end, ec] = std::from_chars(w.data(), w.data() + w.size(), id);
        if (w.empty() || ec != std::errc() || end != w.data() + w.size())
            return std::nullopt;
        return id;
    }
}
