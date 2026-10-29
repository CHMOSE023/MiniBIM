// 移植自 BIMCraft crates/core/src/model.rs、ids.rs、library.rs、lenient.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/ModelJson.h"
#include "Model/Text.h"
#include <cmath>
#include <initializer_list>
#include <set>

namespace MiniBIM::Bim
{
    namespace
    {
        // JSON 指针里的键：~ 写作 ~0，/ 写作 ~1
        std::string Escaped(std::string_view key)
        {
            std::string out;
            for (const char c : key)
            {
                if (c == '~') out += "~0";
                else if (c == '/') out += "~1";
                else out += c;
            }
            return out;
        }

        // 读一个对象的各个字段：记下用到的键，读完后（Finish）没用到的键在严格模式下报错，宽松模式下记进 leftOut。
        // 类型不对的值两种模式都报错（BIMCraft 读文件时也一样：只略去不认识的键）
        class Fields
        {
        public:
            Fields(const Json& obj, std::string who, std::string path, std::vector<std::string>* leftOut, std::string& error)
                : m_obj(obj), m_who(std::move(who)), m_path(std::move(path)), m_leftOut(leftOut), m_error(error) {}

            bool IsObject()
            {
                if (m_obj.is_object())
                    return true;
                return Fail("expected an object, got " + m_obj.dump());
            }

            // 键的原值（并记为用到）；没有或为 null 时为 nullptr
            const Json* Raw(const char* key)
            {
                m_used.insert(key);
                const auto it = m_obj.find(key);
                if (it == m_obj.end() || it->is_null())
                    return nullptr;
                return &*it;
            }

            bool Missing(const char* key) { return Fail(std::string("missing field `") + key + "`"); }

            bool Number(const char* key, double& out, bool required)
            {
                const Json* v = Raw(key);
                if (!v)
                    return required ? Missing(key) : true;
                if (!v->is_number())
                    return Fail(std::string("`") + key + "` must be a number, got " + v->dump());
                out = v->get<double>();
                return true;
            }

            bool OptNumber(const char* key, std::optional<double>& out)
            {
                double x = 0.0;
                if (!Raw(key))
                    return out.reset(), true;
                if (!Number(key, x, true))
                    return false;
                out = x;
                return true;
            }

            bool Bool(const char* key, bool& out)
            {
                const Json* v = Raw(key);
                if (!v)
                    return true;
                if (!v->is_boolean())
                    return Fail(std::string("`") + key + "` must be true or false, got " + v->dump());
                out = v->get<bool>();
                return true;
            }

            bool String(const char* key, std::string& out, bool required)
            {
                const Json* v = Raw(key);
                if (!v)
                    return required ? Missing(key) : true;
                if (!v->is_string())
                    return Fail(std::string("`") + key + "` must be text, got " + v->dump());
                out = v->get<std::string>();
                return true;
            }

            bool OptString(const char* key, std::optional<std::string>& out)
            {
                std::string s;
                if (!Raw(key))
                    return out.reset(), true;
                if (!String(key, s, true))
                    return false;
                out = std::move(s);
                return true;
            }

            bool Point(const char* key, P2& out, bool required)
            {
                const Json* v = Raw(key);
                if (!v)
                    return required ? Missing(key) : true;
                std::string why;
                if (!FromJson(*v, out, why))
                    return Fail(std::string("`") + key + "`: " + why);
                return true;
            }

            bool Points(const char* key, Ring& out, bool required)
            {
                const Json* v = Raw(key);
                if (!v)
                    return required ? Missing(key) : true;
                return ReadRing(key, *v, out);
            }

            bool Rings(const char* key, std::vector<Ring>& out)
            {
                const Json* v = Raw(key);
                if (!v)
                    return true;
                if (!v->is_array())
                    return Fail(std::string("`") + key + "` must be a list of outlines [[[x, y], …], …]");
                out.clear();
                for (const Json& r : *v)
                {
                    Ring ring;
                    if (!ReadRing(key, r, ring))
                        return false;
                    out.push_back(std::move(ring));
                }
                return true;
            }

            bool Corners(const char* key, std::optional<std::array<P2, 2>>& out)
            {
                const Json* v = Raw(key);
                if (!v)
                    return out.reset(), true;
                Ring ring;
                if (!ReadRing(key, *v, ring))
                    return false;
                if (ring.size() != 2)
                    return Fail(std::string("`") + key + "` is two points, [[x, y], [x, y]]");
                out = std::array<P2, 2>{ ring[0], ring[1] };
                return true;
            }

            bool Id(const char* key, Guid& out, bool required)
            {
                const Json* v = Raw(key);
                if (!v)
                    return required ? Missing(key) : true;
                std::string why;
                if (!FromJson(*v, out, why))
                    return Fail(std::string("`") + key + "`: " + why);
                return true;
            }

            bool OptId(const char* key, std::optional<Guid>& out)
            {
                Guid g;
                if (!Raw(key))
                    return out.reset(), true;
                if (!Id(key, g, true))
                    return false;
                out = g;
                return true;
            }

            // 严格时先查不认识的键：照 serde 的 deny_unknown_fields，拼错的键比缺的键先报出来
            bool Allow(std::initializer_list<const char*> keys)
            {
                if (m_leftOut)
                    return true;
                for (auto it = m_obj.begin(); it != m_obj.end(); ++it)
                {
                    bool known = it.key() == "category";
                    for (const char* k : keys)
                        known = known || it.key() == k;
                    if (!known)
                        return Fail("unknown field `" + it.key() + "`");
                }
                return true;
            }

            // 没用到的键：严格时报错，宽松时记下
            bool Finish()
            {
                for (auto it = m_obj.begin(); it != m_obj.end(); ++it)
                {
                    if (m_used.count(it.key()))
                        continue;
                    if (!m_leftOut)
                        return Fail("unknown field `" + it.key() + "`");
                    m_leftOut->push_back(m_path + "/" + Escaped(it.key()));
                }
                return true;
            }

            bool Fail(const std::string& why)
            {
                m_error = m_who.empty() ? why : m_who + ": " + why;
                return false;
            }

            const std::string& Path() const { return m_path; }
            std::vector<std::string>* LeftOut() const { return m_leftOut; }
            std::string& Error() const { return m_error; }
            const std::string& Who() const { return m_who; }

        private:
            bool ReadRing(const char* key, const Json& v, Ring& out)
            {
                if (!v.is_array())
                    return Fail(std::string("`") + key + "` must be a list of points [[x, y], …]");
                out.clear();
                for (const Json& p : v)
                {
                    P2 q;
                    std::string why;
                    if (!FromJson(p, q, why))
                        return Fail(std::string("`") + key + "`: " + why);
                    out.push_back(q);
                }
                return true;
            }

            const Json&               m_obj;
            std::string               m_who;
            std::string               m_path;
            std::vector<std::string>* m_leftOut;
            std::string&              m_error;
            std::set<std::string>     m_used;
        };

        template <class E, class NameFn>
        bool EnumName(Fields& f, const char* key, E& out, NameFn parse, const char* expected)
        {
            std::string s;
            if (!f.Raw(key))
                return true;
            if (!f.String(key, s, true))
                return false;
            const auto e = parse(s);
            if (!e)
                return f.Fail(std::string("`") + key + "`: unknown variant `" + s + "`, expected one of " + expected);
            out = *e;
            return true;
        }

        void ReadWallLike(Fields& f, Wall& w, bool& ok)
        {
            ok = f.String("type", w.typeName, true) && f.Id("level", w.level, true) && f.Point("start", w.start, true) &&
                 f.Point("end", w.end, true) && f.OptId("top_level", w.topLevel) && f.Number("height", w.height, false) &&
                 f.Number("base_offset", w.baseOffset, false) && f.Number("arc", w.arc, false) &&
                 EnumName(f, "location", w.location, [](std::string_view s) { return LocationOf::FromName(s); },
                          "centre, core-centre, exterior-finish, interior-finish, exterior-core, interior-core") &&
                 f.OptId("attach_top", w.attachTop) && f.OptId("attach_base", w.attachBase);
        }

        // 读一个元素。宽松模式下整个略去的（还没移植的类别、带族的门窗）返回 true 并设 skip
        bool ReadElement(const Json& v, Element& out, const std::string& who, const std::string& path, std::vector<std::string>* leftOut,
                         std::string& error, bool& skip)
        {
            skip = false;
            Fields f(v, who, path, leftOut, error);
            if (!f.IsObject())
                return false;
            std::string category;
            if (!f.String("category", category, true))
                return false;
            if (category == "level")
            {
                Level l;
                const Json* range = nullptr;
                if (!f.Allow({ "name", "elevation", "view_range", "crop", "extent", "section_box" }) || !f.String("name", l.name, true) || !f.Number("elevation", l.elevation, true))
                    return false;
                if ((range = f.Raw("view_range")) != nullptr)
                {
                    Fields r(*range, who + " view_range", path + "/view_range", leftOut, error);
                    if (!r.IsObject() || !r.OptNumber("top", l.viewRange.top) || !r.Number("cut", l.viewRange.cut, false) ||
                        !r.Number("bottom", l.viewRange.bottom, false) || !r.Number("depth", l.viewRange.depth, false) || !r.Finish())
                        return false;
                }
                if (!f.Corners("crop", l.crop) || !f.Corners("extent", l.extent) || !f.Bool("section_box", l.sectionBox) || !f.Finish())
                    return false;
                out = Element(std::move(l));
                return true;
            }
            if (category == "grid")
            {
                Grid g;
                if (!f.Allow({ "name", "start", "end" }) || !f.String("name", g.name, true) || !f.Point("start", g.start, true) || !f.Point("end", g.end, true) || !f.Finish())
                    return false;
                out = Element(std::move(g));
                return true;
            }
            if (category == "wall")
            {
                Wall w;
                bool ok = false;
                if (!f.Allow({ "type", "level", "start", "end", "top_level", "height", "base_offset", "arc", "location", "attach_top", "attach_base" }))
                    return false;
                ReadWallLike(f, w, ok);
                if (!ok || !f.Finish())
                    return false;
                out = Element(std::move(w));
                return true;
            }
            if (category == "floor")
            {
                Floor x;
                if (!f.Allow({ "type", "level", "boundary", "openings", "offset", "slope", "arrow", "structural", "pattern_origin", "pattern_angle" }) ||
                    !f.String("type", x.typeName, true) || !f.Id("level", x.level, true) || !f.Points("boundary", x.boundary, true) ||
                    !f.Rings("openings", x.openings) || !f.Number("offset", x.offset, false) || !f.Number("slope", x.slope, false) ||
                    !f.Corners("arrow", x.arrow) || !f.Bool("structural", x.structural) || !f.Point("pattern_origin", x.patternOrigin, false) ||
                    !f.Number("pattern_angle", x.patternAngle, false) || !f.Finish())
                    return false;
                out = Element(std::move(x));
                return true;
            }
            if (category == "roomseparator")
            {
                RoomSeparator x;
                if (!f.Allow({ "level", "start", "end" }) || !f.Id("level", x.level, true) || !f.Point("start", x.start, true) ||
                    !f.Point("end", x.end, true) || !f.Finish())
                    return false;
                out = Element(std::move(x));
                return true;
            }
            if (category == "ceiling")
            {
                Ceiling x;
                if (!f.Allow({ "type", "level", "boundary", "openings", "offset" }) || !f.String("type", x.typeName, true) ||
                    !f.Id("level", x.level, true) || !f.Points("boundary", x.boundary, true) || !f.Rings("openings", x.openings) ||
                    !f.Number("offset", x.offset, true) || !f.Finish())
                    return false;
                out = Element(std::move(x));
                return true;
            }
            if (category == "room")
            {
                Room x;
                if (!f.Allow({ "level", "point", "name", "number" }) || !f.Id("level", x.level, true) || !f.Point("point", x.point, true) ||
                    !f.String("name", x.name, true) || !f.String("number", x.number, true) || !f.Finish())
                    return false;
                out = Element(std::move(x));
                return true;
            }
            if (category == "roof")
            {
                Roof x;
                if (!f.Allow({ "type", "level", "boundary", "courtyards", "slope", "overhang", "gable", "gables", "offset", "openings", "join", "extrusion" }) ||
                    !f.String("type", x.typeName, true) || !f.Id("level", x.level, true) || !f.Points("boundary", x.boundary, false) ||
                    !f.Rings("courtyards", x.courtyards) || !f.Number("slope", x.slope, false) || !f.Number("overhang", x.overhang, false) ||
                    !f.Bool("gable", x.gable))
                    return false;
                if (const Json* g = f.Raw("gables"))
                {
                    if (!g->is_array())
                        return f.Fail("`gables` must be a list of edge numbers");
                    for (const Json& n : *g)
                    {
                        const auto k = AsWhole(n);
                        if (!k)
                            return f.Fail("`gables` must be a list of edge numbers, got " + n.dump());
                        x.gables.push_back(static_cast<size_t>(*k));
                    }
                }
                if (!f.Number("offset", x.offset, false) || !f.Rings("openings", x.openings) || !f.OptId("join", x.join))
                    return false;
                if (const Json* xv = f.Raw("extrusion"))
                {
                    Fields g(*xv, who + " extrusion", path + "/extrusion", leftOut, error);
                    Extrusion ex;
                    std::optional<std::array<P2, 2>> line;
                    if (!g.IsObject() || !g.Allow({ "line", "profile", "from", "to" }) || !g.Corners("line", line))
                        return false;
                    if (!line)
                        return g.Missing("line");
                    ex.line = *line;
                    if (!g.Points("profile", ex.profile, true) || !g.Number("from", ex.from, false) || !g.Number("to", ex.to, true) ||
                        !g.Finish())
                        return false;
                    x.extrusion = std::move(ex);
                }
                if (!f.Finish())
                    return false;
                out = Element(std::move(x));
                return true;
            }
            if (category == "door" || category == "window")
            {
                Opening o;
                if (!f.Allow({ "type", "family", "host", "offset", "sill", "mark", "flip_facing", "flip_hand" }) || !f.String("type", o.typeName, true) || !f.OptString("family", o.family) || !f.Id("host", o.host, true) ||
                    !f.Number("offset", o.offset, true) || !f.Number("sill", o.sill, false) || !f.String("mark", o.mark, false) ||
                    !f.Bool("flip_facing", o.flipFacing) || !f.Bool("flip_hand", o.flipHand))
                    return false;
                // 族还没移植：读文件时带族的门窗整个略去
                if (o.family && leftOut)
                {
                    skip = true;
                    return true;
                }
                if (!f.Finish())
                    return false;
                if (category == "door")
                    out = Element(Door{ std::move(o) });
                else
                    out = Element(Window{ std::move(o) });
                return true;
            }
            if (leftOut)
            {
                skip = true;
                return true;
            }
            error = who + ": unknown variant `" + category +
                    "`, expected one of level, grid, roomseparator, wall, floor, ceiling, door, window, room, roof";
            return false;
        }

        Json RingJson(const Ring& ring)
        {
            Json out = Json::array();
            for (const P2& p : ring)
                out.push_back(ToJson(p));
            return out;
        }

        Json RingsJson(const std::vector<Ring>& rings)
        {
            Json out = Json::array();
            for (const Ring& r : rings)
                out.push_back(RingJson(r));
            return out;
        }

        Json LayersJson(const std::vector<Layer>& layers)
        {
            Json out = Json::array();
            for (const Layer& l : layers)
            {
                Json j = Json::object();
                if (l.id != 0)
                    j["id"] = l.id;
                if (!l.guid.IsNil())
                    j["guid"] = ToJson(l.guid);
                j["function"]  = FunctionOf::Name(l.function);
                j["material"]  = l.material;
                j["thickness"] = l.thickness;
                out.push_back(std::move(j));
            }
            return out;
        }

        bool ReadLayers(Fields& f, std::vector<Layer>& out)
        {
            const Json* v = f.Raw("layers");
            if (!v)
                return true;
            if (!v->is_array())
                return f.Fail("`layers` must be a list");
            out.clear();
            for (size_t i = 0; i < v->size(); ++i)
            {
                Layer l;
                Fields g((*v)[i], f.Who() + " layer " + std::to_string(i + 1), f.Path() + "/layers/" + std::to_string(i), f.LeftOut(), f.Error());
                double id = 0.0;
                if (!g.IsObject() || !g.Number("id", id, false))
                    return false;
                if (id < 0.0 || id != std::floor(id))
                    return g.Fail("`id` must be a whole number");
                l.id = static_cast<Id>(id);
                if (!g.Id("guid", l.guid, false) ||
                    !EnumName(g, "function", l.function, [](std::string_view s) { return FunctionOf::FromName(s); },
                              "structure, substrate, thermal, membrane, finish") ||
                    !g.String("material", l.material, false) || !g.Number("thickness", l.thickness, true) || !g.Finish())
                    return false;
                out.push_back(std::move(l));
            }
            return true;
        }

        template <class T, class ReadFn>
        bool ReadNamed(Fields& parent, const char* key, std::map<std::string, T>& out, bool required, ReadFn read)
        {
            const Json* v = parent.Raw(key);
            if (!v)
                return required ? parent.Missing(key) : true;
            if (!v->is_object())
                return parent.Fail(std::string("`") + key + "` must be an object of names");
            out.clear();
            for (auto it = v->begin(); it != v->end(); ++it)
            {
                T t{};
                Fields f(it.value(), parent.Who().empty() ? std::string(key) + " `" + it.key() + "`" : parent.Who() + " " + key + " `" + it.key() + "`",
                         parent.Path() + "/" + key + "/" + Escaped(it.key()), parent.LeftOut(), parent.Error());
                if (!f.IsObject() || !read(f, t) || !f.Finish())
                    return false;
                out[it.key()] = std::move(t);
            }
            return true;
        }

        bool ReadOpeningType(Fields& f, OpeningType& t)
        {
            return f.Number("width", t.width, true) && f.Number("height", t.height, true) && f.Number("sill", t.sill, false) &&
                   [&] {
                       std::optional<std::string> op;
                       if (!f.OptString("operation", op))
                           return false;
                       if (!op)
                           return true;
                       const auto o = OperationOf::Parse(*op);
                       if (!o || *op != OperationOf::Name(*o))
                           return f.Fail("`operation`: unknown variant `" + *op + "`, expected one of swing, double-swing, sliding, double-sliding, folding, fixed");
                       t.operation = o;
                       return true;
                   }();
        }

        Json OpeningTypesJson(const std::map<std::string, OpeningType>& map)
        {
            Json out = Json::object();
            for (const auto& [n, t] : map)
            {
                Json j = Json::object();
                j["width"]  = t.width;
                j["height"] = t.height;
                j["sill"]   = t.sill;
                if (t.operation)
                    j["operation"] = OperationOf::Name(*t.operation);
                out[n] = std::move(j);
            }
            return out;
        }

        const char* RoomAreaName(RoomArea a) { return a == RoomArea::Finish ? "finish" : "centre"; }

        // 对象的键里的 ID：一串数字，不超出 64 位
        std::optional<Id> ParseId(std::string_view key)
        {
            if (key.empty() || key.find_first_not_of("0123456789") != std::string_view::npos)
                return std::nullopt;
            const auto g = Guid::Parse(key);
            return g ? g->Pending() : std::nullopt;
        }
    }

    // ── 基本值 ───────────────────────────────────────────────────
    Json ToJson(Guid g)
    {
        if (const auto id = g.Pending())
            return Json(*id);
        return Json(g.ToString());
    }

    Json ToJson(P2 p)
    {
        return Json::array({ p.x, p.y });
    }

    bool FromJson(const Json& v, Guid& out, std::string& error)
    {
        if (const auto id = AsWhole(v))
        {
            out = Guid::OfId(*id);
            return true;
        }
        if (v.is_number_integer())
        {
            error = v.dump() + " is not an element id";
            return false;
        }
        if (v.is_number_float())
        {
            // JSON 带小数点写的整数
            const double x = v.get<double>();
            if (x == std::floor(x) && x >= 0.0 && x < 9e15)
            {
                out = Guid::OfId(static_cast<Id>(x));
                return true;
            }
            error = Text::Shortest(x) + " is not an element id";
            return false;
        }
        if (v.is_string())
        {
            const std::string s = v.get<std::string>();
            if (const auto g = Guid::Parse(s))
            {
                out = *g;
                return true;
            }
            error = "`" + s + "` is not a GUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx) or an id";
            return false;
        }
        error = "expected an element's GUID, or its id, got " + v.dump();
        return false;
    }

    bool FromJson(const Json& v, P2& out, std::string& error)
    {
        if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number())
        {
            error = v.dump() + " is not a point [x, y]";
            return false;
        }
        out = P2(v[0].get<double>(), v[1].get<double>());
        return true;
    }

    // ── 元素 ─────────────────────────────────────────────────────
    Json ToJson(const Element& e)
    {
        Json j = Json::object();
        j["category"] = e.Category();
        if (const Level* l = e.As<Level>())
        {
            j["name"]      = l->name;
            j["elevation"] = l->elevation;
            Json r = Json::object();
            r["top"]    = l->viewRange.top ? Json(*l->viewRange.top) : Json(nullptr);
            r["cut"]    = l->viewRange.cut;
            r["bottom"] = l->viewRange.bottom;
            r["depth"]  = l->viewRange.depth;
            j["view_range"] = std::move(r);
            if (l->crop)
                j["crop"] = Json::array({ ToJson((*l->crop)[0]), ToJson((*l->crop)[1]) });
            if (l->extent)
                j["extent"] = Json::array({ ToJson((*l->extent)[0]), ToJson((*l->extent)[1]) });
            if (l->sectionBox)
                j["section_box"] = true;
        }
        else if (const Grid* g = e.As<Grid>())
        {
            j["name"]  = g->name;
            j["start"] = ToJson(g->start);
            j["end"]   = ToJson(g->end);
        }
        else if (const Wall* w = e.As<Wall>())
        {
            j["type"]        = w->typeName;
            j["level"]       = ToJson(w->level);
            j["start"]       = ToJson(w->start);
            j["end"]         = ToJson(w->end);
            j["top_level"]   = w->topLevel ? ToJson(*w->topLevel) : Json(nullptr);
            j["height"]      = w->height;
            j["base_offset"] = w->baseOffset;
            if (w->arc != 0.0)
                j["arc"] = w->arc;
            if (w->location != Location::Centre)
                j["location"] = LocationOf::Name(w->location);
            if (w->attachTop)
                j["attach_top"] = ToJson(*w->attachTop);
            if (w->attachBase)
                j["attach_base"] = ToJson(*w->attachBase);
        }
        else if (const Floor* f = e.As<Floor>())
        {
            j["type"]  = f->typeName;
            j["level"] = ToJson(f->level);
            Json b = Json::array();
            for (const P2& p : f->boundary)
                b.push_back(ToJson(p));
            j["boundary"] = std::move(b);
            if (!f->openings.empty())
            {
                Json os = Json::array();
                for (const Ring& r : f->openings)
                {
                    Json o = Json::array();
                    for (const P2& p : r)
                        o.push_back(ToJson(p));
                    os.push_back(std::move(o));
                }
                j["openings"] = std::move(os);
            }
            if (f->offset != 0.0)
                j["offset"] = f->offset;
            if (f->slope != 0.0)
                j["slope"] = f->slope;
            if (f->arrow)
                j["arrow"] = Json::array({ ToJson((*f->arrow)[0]), ToJson((*f->arrow)[1]) });
            if (f->structural)
                j["structural"] = true;
            if (!(f->patternOrigin == P2{}))
                j["pattern_origin"] = ToJson(f->patternOrigin);
            if (f->patternAngle != 0.0)
                j["pattern_angle"] = f->patternAngle;
        }
        else if (const RoomSeparator* rs = e.As<RoomSeparator>())
        {
            j["level"] = ToJson(rs->level);
            j["start"] = ToJson(rs->start);
            j["end"]   = ToJson(rs->end);
        }
        else if (const Ceiling* c = e.As<Ceiling>())
        {
            j["type"]     = c->typeName;
            j["level"]    = ToJson(c->level);
            j["boundary"] = RingJson(c->boundary);
            if (!c->openings.empty())
                j["openings"] = RingsJson(c->openings);
            j["offset"] = c->offset;
        }
        else if (const Room* r = e.As<Room>())
        {
            j["level"]  = ToJson(r->level);
            j["point"]  = ToJson(r->point);
            j["name"]   = r->name;
            j["number"] = r->number;
        }
        else if (const Roof* r = e.As<Roof>())
        {
            j["type"]  = r->typeName;
            j["level"] = ToJson(r->level);
            if (!r->boundary.empty())
                j["boundary"] = RingJson(r->boundary);
            if (!r->courtyards.empty())
                j["courtyards"] = RingsJson(r->courtyards);
            j["slope"]    = r->slope;
            j["overhang"] = r->overhang;
            if (r->gable)
                j["gable"] = true;
            if (!r->gables.empty())
            {
                Json g = Json::array();
                for (const size_t k : r->gables)
                    g.push_back(static_cast<uint64_t>(k));
                j["gables"] = std::move(g);
            }
            j["offset"] = r->offset;
            if (!r->openings.empty())
                j["openings"] = RingsJson(r->openings);
            if (r->join)
                j["join"] = ToJson(*r->join);
            if (r->extrusion)
            {
                const Extrusion& x = *r->extrusion;
                Json ex = Json::object();
                ex["line"]    = Json::array({ ToJson(x.line[0]), ToJson(x.line[1]) });
                ex["profile"] = RingJson(x.profile);
                ex["from"]    = x.from;
                ex["to"]      = x.to;
                j["extrusion"] = std::move(ex);
            }
        }
        else if (const Opening* o = e.AsOpening())
        {
            j["type"] = o->typeName;
            if (o->family)
                j["family"] = *o->family;
            j["host"]        = ToJson(o->host);
            j["offset"]      = o->offset;
            j["sill"]        = o->sill;
            j["mark"]        = o->mark;
            j["flip_facing"] = o->flipFacing;
            j["flip_hand"]   = o->flipHand;
        }
        return j;
    }

    bool FromJson(const Json& v, Element& out, std::string& error)
    {
        bool skip = false;
        return ReadElement(v, out, "element", "", nullptr, error, skip);
    }

    Json ToJson(const Types& types)
    {
        auto layered = [](const auto& map)
        {
            Json out = Json::object();
            for (const auto& [n, x] : map)
            {
                Json y = Json::object();
                y["thickness"] = x.thickness;
                if (!x.layers.empty())
                    y["layers"] = LayersJson(x.layers);
                out[n] = std::move(y);
            }
            return out;
        };
        Json t = Json::object();
        t["wall"]   = layered(types.wall);
        t["floor"]  = layered(types.floor);
        t["door"]   = OpeningTypesJson(types.door);
        t["window"] = OpeningTypesJson(types.window);
        t["roof"]   = layered(types.roof);
        Json ceilings = Json::object();
        for (const auto& [n, x] : types.ceiling)
        {
            Json y = Json::object();
            y["thickness"] = x.thickness;
            y["grid"]      = x.grid;
            if (!x.layers.empty())
                y["layers"] = LayersJson(x.layers);
            ceilings[n] = std::move(y);
        }
        t["ceiling"] = std::move(ceilings);
        return t;
    }

    Json ToJson(const std::map<std::string, Material>& materials)
    {
        Json mats = Json::object();
        for (const auto& [n, x] : materials)
        {
            Json y = Json::object();
            y["color"]   = Json::array({ x.color[0], x.color[1], x.color[2] });
            y["cut"]     = PatternOf::Name(x.cut);
            y["surface"] = PatternOf::Name(x.surface);
            mats[n] = std::move(y);
        }
        return mats;
    }

    // ── 模型 ─────────────────────────────────────────────────────
    Json Model::ToJson() const
    {
        Json j = Json::object();
        j["format"]   = format;
        j["name"]     = name;
        j["uid"]      = uid;
        j["units"]    = UnitsOf::Name(units);
        j["language"] = LanguageOf::Code(language);
        if (roomArea != RoomArea::Centre)
            j["room_area"] = RoomAreaName(roomArea);
        j["next_id"] = nextId;
        j["types"]     = Bim::ToJson(types);
        j["materials"] = Bim::ToJson(materials);
        Json es = Json::object();
        for (const auto& [id, e] : elements.Items())
        {
            Json y = Json::object();
            y["guid"] = Bim::ToJson(GuidOf(id));
            // 先存下来：range-for 里直接对临时对象调 items()，临时对象会先析构
            const Json fields = Bim::ToJson(*e);
            for (auto it = fields.begin(); it != fields.end(); ++it)
                y[it.key()] = it.value();
            es[std::to_string(id)] = std::move(y);
        }
        j["elements"] = std::move(es);
        Json lib = Json::object();
        for (const auto& [id, e] : library.Entries())
        {
            Json y = Json::object();
            y["kind"] = e.kind.ToString();
            y["name"] = e.name;
            y["guid"] = Bim::ToJson(e.guid);
            lib[std::to_string(id)] = std::move(y);
        }
        j["library"] = std::move(lib);
        return j;
    }

    bool Model::ToFile(std::string& out, std::string& error) const
    {
        Model m = *this;
        m.SyncLibrary();
        const Library lib = m.library;
        m.LibRefsMut([&](const Kind& kind, std::string& text)
        {
            if (const auto g = lib.GuidOf(kind, text))
                text = g->ToString();
        });
        try
        {
            out = m.ToJson().dump(2);
        }
        catch (const std::exception& e)
        {
            // 名称里有不合法的 UTF-8 时 dump 会抛异常
            error = e.what();
            return false;
        }
        return true;
    }

    bool Model::Read(std::string_view text, Model& out, std::vector<std::string>& leftOut, std::string& error)
    {
        const Json v = Json::parse(text.begin(), text.end(), nullptr, false);
        if (v.is_discarded())
        {
            error = "it isn't JSON";
            return false;
        }
        if (!v.is_object())
        {
            error = "a project is a JSON object";
            return false;
        }
        Model m;
        m.format = 0;
        if (const auto it = v.find("format"); it != v.end())
        {
            const auto whole = AsWhole(*it);
            if (!whole)
            {
                error = "`format` must be a whole number";
                return false;
            }
            const uint64_t f = *whole;
            if (f > FileFormat)
            {
                error = "it was saved by a newer version (file format " + std::to_string(f) + "; this one reads up to " +
                        std::to_string(FileFormat) + "): update MiniBIM to open it";
                return false;
            }
            m.format = static_cast<uint32_t>(f);
        }
        Fields f(v, "", "", &leftOut, error);
        f.Raw("format");
        double uid = 0.0;
        if (!f.String("name", m.name, true))
            return false;
        if (const Json* u = f.Raw("uid"); !u)
            return f.Missing("uid");
        else if (AsWhole(*u))
            m.uid = *AsWhole(*u);
        else if (u->is_number() && (uid = u->get<double>()) >= 0.0 && uid == std::floor(uid) && uid < 1.8e19)
            m.uid = static_cast<uint64_t>(uid);
        else
            return f.Fail("`uid` must be a whole number");
        if (!EnumName(f, "units", m.units, [](std::string_view s) { return UnitsOf::FromName(s); }, "mm, cm, m, in, ft, ft-in") ||
            !EnumName(f, "language", m.language, [](std::string_view s) -> std::optional<Language>
            {
                for (Language l : LanguageOf::All)
                    if (s == LanguageOf::Code(l))
                        return l;
                return std::nullopt;
            }, "en, pt, es") ||
            !EnumName(f, "room_area", m.roomArea, [](std::string_view s) -> std::optional<RoomArea>
            {
                if (s == "centre") return RoomArea::Centre;
                if (s == "finish") return RoomArea::Finish;
                return std::nullopt;
            }, "centre, finish"))
            return false;
        if (const Json* n = f.Raw("next_id"); !n)
            return f.Missing("next_id");
        else if (!AsWhole(*n))
            return f.Fail("`next_id` must be a whole number");
        else
            m.nextId = *AsWhole(*n);

        // 类型
        const Json* types = f.Raw("types");
        if (!types)
            return f.Missing("types");
        {
            Fields t(*types, "", "/types", &leftOut, error);
            auto layered = [](auto& f2, auto& x) { return f2.Number("thickness", x.thickness, true) && ReadLayers(f2, x.layers); };
            if (!t.IsObject() ||
                !ReadNamed(t, "wall", m.types.wall, true, [&](Fields& g, WallType& x) { return layered(g, x); }) ||
                !ReadNamed(t, "floor", m.types.floor, true, [&](Fields& g, FloorType& x) { return layered(g, x); }) ||
                !ReadNamed(t, "door", m.types.door, true, ReadOpeningType) ||
                !ReadNamed(t, "window", m.types.window, true, ReadOpeningType) ||
                !ReadNamed(t, "roof", m.types.roof, false, [&](Fields& g, RoofType& x) { return layered(g, x); }) ||
                !ReadNamed(t, "ceiling", m.types.ceiling, false, [&](Fields& g, CeilingType& x)
                {
                    return layered(g, x) && g.Number("grid", x.grid, false);
                }) || !t.Finish())
                return false;
        }

        // 材质
        if (!ReadNamed(f, "materials", m.materials, false, [](Fields& g, Material& x)
        {
            const Json* c = g.Raw("color");
            if (!c)
                return g.Missing("color");
            if (!c->is_array() || c->size() != 3)
                return g.Fail("`color` must be [r, g, b]");
            for (size_t i = 0; i < 3; ++i)
            {
                const auto byte = AsWhole((*c)[i]);
                if (!byte || *byte > 255)
                    return g.Fail("`color` must be [r, g, b], each 0 to 255");
                x.color[i] = static_cast<uint8_t>(*byte);
            }
            const char* patterns = "none, solid, diagonal, crosshatch, horizontal, vertical, grid, brick, concrete, insulation";
            auto parse = [](std::string_view s) { return PatternOf::FromName(s); };
            return EnumName(g, "cut", x.cut, parse, patterns) && EnumName(g, "surface", x.surface, parse, patterns);
        }))
            return false;

        // 元素：没有 GUID 的（旧文件）读的时候各给一个
        const Json* es = f.Raw("elements");
        if (!es)
            return f.Missing("elements");
        if (!es->is_object())
            return f.Fail("`elements` must be an object of ids");
        for (auto it = es->begin(); it != es->end(); ++it)
        {
            const std::string& key = it.key();
            const auto id = ParseId(key);
            if (!id)
            {
                error = "`" + key + "` isn't an element id";
                return false;
            }
            const Id eid = *id;
            const std::string who = "element " + key, path = "/elements/" + key;
            std::optional<Guid> guid;
            Json bare = it.value();
            if (bare.is_object())
            {
                if (const auto g = bare.find("guid"); g != bare.end())
                {
                    Guid parsed;
                    std::string why;
                    if (!FromJson(*g, parsed, why))
                    {
                        error = who + ": " + why;
                        return false;
                    }
                    guid = parsed;
                    bare.erase("guid");
                }
            }
            Element e;
            bool skip = false;
            if (!ReadElement(bare, e, who, path, &leftOut, error, skip))
                return false;
            if (skip)
            {
                leftOut.push_back(path);
                continue;
            }
            if (!m.elements.InsertAs(eid, guid.value_or(m.elements.Fresh()), std::move(e), error))
                return false;
        }

        // 库
        if (const Json* lib = f.Raw("library"))
        {
            if (!lib->is_object())
                return f.Fail("`library` must be an object of ids");
            for (auto it = lib->begin(); it != lib->end(); ++it)
            {
                const std::string& key = it.key();
                const auto id = ParseId(key);
                if (!id)
                {
                    error = "`" + key + "` isn't a library id";
                    return false;
                }
                Fields g(it.value(), "library entry " + key, "/library/" + key, &leftOut, error);
                std::string kind;
                Entry entry;
                if (!g.IsObject() || !g.String("kind", kind, true) || !g.String("name", entry.name, true) || !g.Id("guid", entry.guid, true) || !g.Finish())
                    return false;
                const auto k = Kind::Parse(kind);
                if (!k)
                    return g.Fail("`" + kind + "` isn't a kind of library entry");
                entry.kind = *k;
                if (entry.guid.Pending())
                    return g.Fail("its GUID is a number");
                if (!m.library.Insert(*id, std::move(entry), error))
                    return false;
            }
        }
        if (!f.Finish())
            return false;

        // 宽松读入后清理：宿主略去了的门窗也略去；贴着的东西不在了的墙松开（屋顶插进的总是屋顶，不会被略去）
        for (bool removed = true; removed;)
        {
            removed = false;
            std::vector<Id> gone;
            for (const auto& [id, e] : m.elements.Items())
                if (const Opening* o = e->AsOpening(); o && !m.elements.Contains(o->host))
                    gone.push_back(id);
            for (const Id id : gone)
            {
                m.elements.Remove(id);
                leftOut.push_back("/elements/" + std::to_string(id));
                removed = true;
            }
        }
        std::vector<Id> walls;
        for (const auto& [id, e] : m.elements.Items())
            if (e->As<Wall>())
                walls.push_back(id);
        for (const Id id : walls)
        {
            Wall w = *m.elements.Get(id)->As<Wall>();
            bool changed = false;
            for (auto [key, at] : { std::pair{ "attach_top", &w.attachTop }, std::pair{ "attach_base", &w.attachBase } })
            {
                const Element* to = *at ? m.elements.Get(**at) : nullptr;
                if (*at && !(to && (to->As<Floor>() || to->As<Roof>())))
                {
                    at->reset();
                    leftOut.push_back("/elements/" + std::to_string(id) + "/" + key);
                    changed = true;
                }
            }
            if (changed)
                m.elements.Insert(id, std::move(w));
        }

        m.Migrate();
        m.NamesFromFile();
        out = std::move(m);
        return true;
    }
}
