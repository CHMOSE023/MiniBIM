// 移植自 BIMCraft crates/core/src/model.rs、ids.rs、library.rs、lenient.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/ModelJson.h"
#include "Model/Massing.h"
#include "Model/Sketch.h"
#include "Model/Text.h"
#include <algorithm>
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
            // 草图线：{id?, guid?, start, end, arc? | through?}。BIMCraft 别的曲线（多段线、圆、椭圆、样条）还没移植：
            // 宽松时整个草图略去（元素的轮廓照旧，编辑边界从轮廓开始），严格时报错
            bool SketchLines(const char* key, std::vector<SketchLine>& out)
            {
                const Json* v = Raw(key);
                if (!v)
                    return true;
                if (!v->is_array())
                    return Fail(std::string("`") + key + "` must be a list of lines");
                for (size_t i = 0; i < v->size(); ++i)
                {
                    const Json& o = (*v)[i];
                    const std::string at = std::string("`") + key + "` line " + std::to_string(i);
                    if (!o.is_object())
                        return Fail(at + " must be an object");
                    for (auto it = o.begin(); it != o.end(); ++it)
                        if (it.key() != "id" && it.key() != "guid" && it.key() != "start" && it.key() != "end" && it.key() != "arc" &&
                            it.key() != "through" && it.key() != "style" && it.key() != "invisible")
                        {
                            if (!m_leftOut)
                                return Fail(at + ": `" + it.key() + "` isn't ported yet; lines and arcs are");
                            m_leftOut->push_back(m_path + "/" + key);
                            out.clear();
                            return true;
                        }
                    SketchLine l;
                    std::string why;
                    if (o.contains("id"))
                    {
                        const auto n = AsWhole(o["id"]);
                        if (!n)
                            return Fail(at + ": `id` must be a whole number");
                        l.id = static_cast<Bim::Id>(*n);
                    }
                    if (o.contains("guid") && !FromJson(o["guid"], l.guid, why))
                        return Fail(at + ": `guid`: " + why);
                    if (!o.contains("start") || !o.contains("end") || !FromJson(o["start"], l.start, why) || !FromJson(o["end"], l.end, why))
                        return Fail(at + " needs `start` and `end` [x, y]" + (why.empty() ? "" : ": " + why));
                    if (o.contains("arc") && o.contains("through"))
                        return Fail(at + ": give an arc's `through` point or its `arc` turn, not both");
                    if (o.contains("arc"))
                    {
                        if (!o["arc"].is_number())
                            return Fail(at + ": `arc` must be a number");
                        l.arc = o["arc"].get<double>();
                    }
                    if (o.contains("through"))
                    {
                        P2 t;
                        if (!FromJson(o["through"], t, why))
                            return Fail(at + ": `through`: " + why);
                        l.arc = Sketch::ArcThrough(l.start, l.end, t).arc;
                    }
                    if (o.contains("style") && !o["style"].is_null())
                    {
                        if (!o["style"].is_string())
                            return Fail(at + ": `style` must be a line style's name");
                        l.style = o["style"].get<std::string>();
                    }
                    if (o.contains("invisible") && !o["invisible"].is_null())
                    {
                        if (!o["invisible"].is_boolean())
                            return Fail(at + ": `invisible` must be true or false");
                        l.invisible = o["invisible"].get<bool>();
                    }
                    out.push_back(l);
                }
                return true;
            }

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

        // 参数值：{名称: 值}
        bool ReadParams(Fields& f, std::map<std::string, Json>& out)
        {
            const Json* v = f.Raw("params");
            if (!v)
                return true;
            if (!v->is_object())
                return f.Fail("`params` must be an object of values by name");
            out.clear();
            for (auto it = v->begin(); it != v->end(); ++it)
                out[it.key()] = it.value();
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

        // [r, g, b]，各 0 到 255
        bool ReadColor(Fields& f, const char* key, Rgb3& out)
        {
            const Json* c = f.Raw(key);
            if (!c)
                return true;
            if (!c->is_array() || c->size() != 3)
                return f.Fail(std::string("`") + key + "` must be [r, g, b]");
            for (size_t i = 0; i < 3; ++i)
            {
                const auto byte = AsWhole((*c)[i]);
                if (!byte || *byte > 255)
                    return f.Fail(std::string("`") + key + "` must be [r, g, b], each 0 to 255");
                out[i] = static_cast<uint8_t>(*byte);
            }
            return true;
        }

        bool ReadPen(Fields& f, const char* key, uint8_t& out)
        {
            const Json* v = f.Raw(key);
            if (!v)
                return true;
            const auto n = AsWhole(*v);
            if (!n || *n > 255)
                return f.Fail(std::string("`") + key + "` must be a pen, 0 to 16");
            out = static_cast<uint8_t>(*n);
            return true;
        }

        // 详图线的曲线：{start, end, arc? | through?}、{points, arcs?, closed?} 或 {centre, radius}。椭圆与样条还没移植：
        // 宽松时设 unsupported（整个略去），严格时报错
        bool ReadCurve(Fields& f, Curve& out, bool& unsupported)
        {
            unsupported = false;
            for (const char* k : { "radii", "angle", "span", "control", "knots", "fit", "start_tangent", "end_tangent" })
                if (f.Raw(k))
                {
                    if (f.LeftOut())
                        return unsupported = true, true;
                    return f.Fail(std::string("`") + k + "` isn't ported yet; lines, arcs, polylines and circles are");
                }
            const Json* start = f.Raw("start");
            const Json* points = f.Raw("points");
            const Json* centre = f.Raw("centre");
            const Json* through = f.Raw("through");
            const Json* arc = f.Raw("arc");
            const Json* arcs = f.Raw("arcs");
            const Json* radius = f.Raw("radius");
            bool closed = false;
            if (!f.Bool("closed", closed))
                return false;
            std::string why;
            const auto only = [&](std::initializer_list<std::pair<const char*, bool>> others, const char* what)
            {
                for (const auto& [k, on] : others)
                    if (on)
                        return f.Fail(std::string("`") + k + "` doesn't go with the " + what + " given");
                return true;
            };
            if (start)
            {
                if (!only({ { "points", points != nullptr }, { "arcs", arcs != nullptr }, { "closed", closed }, { "centre", centre != nullptr },
                            { "radius", radius != nullptr } }, "start, end"))
                    return false;
                P2 a, b;
                if (!f.Point("start", a, true) || !f.Point("end", b, true))
                    return false;
                if (arc && through)
                    return f.Fail("give an arc's `through` point or its `arc` turn, not both");
                if (through)
                {
                    P2 t;
                    if (!f.Point("through", t, true))
                        return false;
                    out = Curve::Arc(a, b, Sketch::ArcThrough(a, b, t).arc);
                    return true;
                }
                double sweep = 0.0;
                if (!f.Number("arc", sweep, false))
                    return false;
                out = Curve::Arc(a, b, sweep);
                return true;
            }
            if (points)
            {
                if (!only({ { "end", f.Raw("end") != nullptr }, { "arc", arc != nullptr }, { "through", through != nullptr },
                            { "centre", centre != nullptr }, { "radius", radius != nullptr } }, "points"))
                    return false;
                Curve c;
                if (!f.Points("points", c.points, true))
                    return false;
                if (arcs)
                {
                    if (!arcs->is_array())
                        return f.Fail("`arcs` must be a list of turns in degrees");
                    for (const Json& a : *arcs)
                    {
                        if (!a.is_number())
                            return f.Fail("`arcs` must be a list of turns in degrees");
                        c.arcs.push_back(a.get<double>());
                    }
                    if (std::all_of(c.arcs.begin(), c.arcs.end(), [](double a) { return a == 0.0; }))
                        c.arcs.clear();
                }
                c.closed = closed;
                out = std::move(c);
                return true;
            }
            if (centre && radius)
            {
                if (!only({ { "end", f.Raw("end") != nullptr }, { "arc", arc != nullptr }, { "through", through != nullptr },
                            { "closed", closed } }, "centre, radius"))
                    return false;
                P2 c;
                double r = 0.0;
                if (!f.Point("centre", c, true) || !f.Number("radius", r, true))
                    return false;
                out = Curve::CircleOf(c, r);
                return true;
            }
            return f.Fail("a line is {start, end, arc? or through?}, {points, arcs?, closed?} or {centre, radius}");
        }

        void CurveJson(const Curve& c, Json& j)
        {
            if (c.kind == Curve::Kind::Circle)
            {
                j["centre"] = ToJson(c.centre);
                j["radius"] = c.radius;
                return;
            }
            if (!c.closed && c.points.size() == 2)
            {
                j["start"] = ToJson(c.points[0]);
                j["end"]   = ToJson(c.points[1]);
                if (!c.arcs.empty() && c.arcs[0] != 0.0)
                    j["arc"] = c.arcs[0];
                return;
            }
            Json pts = Json::array();
            for (const P2& p : c.points)
                pts.push_back(ToJson(p));
            j["points"] = std::move(pts);
            if (!c.arcs.empty())
                j["arcs"] = c.arcs;
            if (c.closed)
                j["closed"] = true;
        }

        Json ColorJson(const Rgb3& c) { return Json::array({ c[0], c[1], c[2] }); }

        // ── 视图的图形（graphics.rs）──────────────────────────────
        // 颜色：[r, g, b]（各 0 到 255）或 "#rrggbb"
        bool ReadAnyColor(Fields& f, const char* key, std::optional<Rgb3>& out)
        {
            const Json* c = f.Raw(key);
            if (!c)
                return out.reset(), true;
            const std::string bad = c->dump() + " isn't a colour: [r, g, b], each 0 to 255, or \"#rrggbb\"";
            Rgb3 rgb{};
            if (c->is_string())
            {
                const std::string s = c->get<std::string>();
                const auto hex = [](char ch) -> int
                {
                    if (ch >= '0' && ch <= '9') return ch - '0';
                    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                    return -1;
                };
                bool ok = s.size() == 7 && s[0] == '#';
                for (size_t i = 0; ok && i < 3; ++i)
                {
                    const int hi = hex(s[1 + 2 * i]), lo = hex(s[2 + 2 * i]);
                    ok = hi >= 0 && lo >= 0;
                    rgb[i] = static_cast<uint8_t>(hi * 16 + lo);
                }
                if (!ok)
                    return f.Fail(bad);
            }
            else if (!ReadColor(f, key, rgb))
                return f.Fail(bad);
            out = rgb;
            return true;
        }

        bool ReadPatternFill(Fields& f, const char* key, std::optional<PatternFill>& out)
        {
            const Json* v = f.Raw(key);
            if (!v)
                return out.reset(), true;
            Fields p(*v, f.Who() + " " + key, f.Path() + "/" + key, f.LeftOut(), f.Error());
            PatternFill pf;
            if (!p.IsObject() || !p.OptString("pattern", pf.pattern) || !ReadColor(p, "color", pf.color) || !p.Finish())
                return false;
            out = std::move(pf);
            return true;
        }

        bool ReadStyle(Fields& f, const char* key, Style& out)
        {
            const Json* v = f.Raw(key);
            out = Style{};
            if (!v)
                return true;
            Fields s(*v, f.Who() + " " + key, f.Path() + "/" + key, f.LeftOut(), f.Error());
            if (!s.IsObject() || !ReadAnyColor(s, "color", out.color) || !ReadAnyColor(s, "fill", out.fill) ||
                !ReadPatternFill(s, "foreground", out.foreground) || !ReadPatternFill(s, "background", out.background))
                return false;
            if (s.Raw("weight"))
            {
                uint8_t pen = 0;
                if (!ReadPen(s, "weight", pen))
                    return false;
                out.weight = pen;
            }
            std::optional<std::string> dash;
            if (!s.OptString("dash", dash))
                return false;
            if (dash)
            {
                out.dash = DashOf::Parse(*dash);
                if (!out.dash)
                    return s.Fail("`dash`: unknown variant `" + *dash + "`, expected one of solid, dashed, dotted");
            }
            return s.Finish();
        }

        bool ReadOverride(const Json& v, const std::string& who, const std::string& path, std::vector<std::string>* leftOut, std::string& error,
                          Override& out)
        {
            Fields f(v, who, path, leftOut, error);
            out = Override{};
            if (!f.IsObject() || !f.Bool("hidden", out.hidden) || !ReadStyle(f, "projection", out.projection) || !ReadStyle(f, "cut", out.cut) ||
                !f.Bool("halftone", out.halftone))
                return false;
            if (const Json* t = f.Raw("transparency"))
            {
                const auto n = AsWhole(*t);
                if (!n || *n > 255)
                    return f.Fail("`transparency` must be a whole number, 0 to 100");
                out.transparency = static_cast<uint8_t>(*n);
            }
            return f.Finish();
        }

        std::optional<VisualStyle> StyleKey(std::string_view s)
        {
            for (const VisualStyle v : { VisualStyle::Shaded, VisualStyle::HiddenLine, VisualStyle::Wireframe })
                if (s == VisualStyleOf::Key(v))
                    return v;
            return std::nullopt;
        }

        std::optional<Detail> DetailKey(std::string_view s)
        {
            for (const Detail d : { Detail::Coarse, Detail::Medium, Detail::Fine })
                if (s == Text::AsciiLower(DetailOf::Name(d)))
                    return d;
            return std::nullopt;
        }

        // 视图的图形：视图样板、过滤器、阶段、设计选项、Linework 与 CAD 图层还没移植，读文件时略去
        bool ReadGraphics(Fields& f, Graphics& out)
        {
            const Json* v = f.Raw("graphics");
            out = Graphics{};
            if (!v)
                return true;
            const std::string path = f.Path() + "/graphics";
            Fields g(*v, f.Who() + " graphics", path, f.LeftOut(), f.Error());
            if (!g.IsObject())
                return false;
            if (const Json* cats = g.Raw("categories"))
            {
                if (!cats->is_object())
                    return g.Fail("`categories` must be {category: override}");
                for (auto it = cats->begin(); it != cats->end(); ++it)
                {
                    Override o;
                    if (!ReadOverride(it.value(), f.Who() + " graphics category `" + it.key() + "`", path + "/categories/" + Escaped(it.key()),
                                      f.LeftOut(), f.Error(), o))
                        return false;
                    out.categories[it.key()] = std::move(o);
                }
            }
            if (const Json* els = g.Raw("elements"))
            {
                if (!els->is_object())
                    return g.Fail("`elements` must be {GUID: override}");
                for (auto it = els->begin(); it != els->end(); ++it)
                {
                    const auto id = Guid::Parse(it.key());
                    if (!id)
                        return g.Fail("`elements`: `" + it.key() + "` is not an element id or GUID");
                    Override o;
                    if (!ReadOverride(it.value(), f.Who() + " graphics element " + it.key(), path + "/elements/" + Escaped(it.key()), f.LeftOut(),
                                      f.Error(), o))
                        return false;
                    out.elements[*id] = std::move(o);
                }
            }
            if (!g.OptNumber("scale", out.scale) || !EnumName(g, "detail", out.detail, DetailKey, "coarse, medium, fine") ||
                !EnumName(g, "style", out.style, StyleKey, "shaded, hidden_line, wireframe"))
                return false;
            return g.Finish();
        }

        Json PatternFillJson(const PatternFill& p)
        {
            return Json{ { "pattern", p.pattern ? Json(*p.pattern) : Json(nullptr) }, { "color", ColorJson(p.color) } };
        }

        Json StyleJson(const Style& s)
        {
            Json j = Json::object();
            if (s.color)
                j["color"] = ColorJson(*s.color);
            if (s.weight)
                j["weight"] = *s.weight;
            if (s.dash)
                j["dash"] = DashOf::Name(*s.dash);
            if (s.fill)
                j["fill"] = ColorJson(*s.fill);
            if (s.foreground)
                j["foreground"] = PatternFillJson(*s.foreground);
            if (s.background)
                j["background"] = PatternFillJson(*s.background);
            return j;
        }

        Json ViewRangeJson(const ViewRange& v)
        {
            Json r = Json::object();
            r["top"]    = v.top ? Json(*v.top) : Json(nullptr);
            r["cut"]    = v.cut;
            r["bottom"] = v.bottom;
            r["depth"]  = v.depth;
            return r;
        }

        bool ReadViewRange(Fields& f, ViewRange& out)
        {
            const Json* range = f.Raw("view_range");
            if (!range)
                return true;
            Fields r(*range, f.Who() + " view_range", f.Path() + "/view_range", f.LeftOut(), f.Error());
            return r.IsObject() && r.OptNumber("top", out.top) && r.Number("cut", out.cut, false) && r.Number("bottom", out.bottom, false) &&
                   r.Number("depth", out.depth, false) && r.Finish();
        }

        bool ReadP3(Fields& f, const char* key, P3& out)
        {
            const Json* v = f.Raw(key);
            if (!v)
                return f.Missing(key);
            if (!v->is_array() || v->size() != 3 || !std::all_of(v->begin(), v->end(), [](const Json& x) { return x.is_number(); }))
                return f.Fail(std::string("`") + key + "` must be [x, y, z]");
            for (size_t i = 0; i < 3; ++i)
                out[i] = (*v)[i].get<double>();
            return true;
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
                if (!f.Allow({ "name", "elevation", "view_range", "crop", "extent", "section_box", "graphics" }) || !f.String("name", l.name, true) ||
                    !f.Number("elevation", l.elevation, true) || !ReadViewRange(f, l.viewRange))
                    return false;
                if (!f.Corners("crop", l.crop) || !f.Corners("extent", l.extent) || !f.Bool("section_box", l.sectionBox) || !ReadGraphics(f, l.graphics) ||
                    !f.Finish())
                    return false;
                out = Element(std::move(l));
                return true;
            }
            if (category == "planview")
            {
                PlanView v;
                if (!f.Allow({ "level", "name", "view_range", "crop", "graphics", "parent", "section_box" }) || !f.Id("level", v.level, true) ||
                    !f.String("name", v.name, true) || !ReadViewRange(f, v.viewRange) || !f.Corners("crop", v.crop) || !ReadGraphics(f, v.graphics) ||
                    !f.OptId("parent", v.parent) || !f.Bool("section_box", v.sectionBox) || !f.Finish())
                    return false;
                out = Element(std::move(v));
                return true;
            }
            if (category == "view3d")
            {
                View3D v;
                if (!f.Allow({ "name", "azimuth", "elevation", "graphics", "section_box" }) || !f.String("name", v.name, true) ||
                    !f.Number("azimuth", v.azimuth, true) || !f.Number("elevation", v.elevation, true) || !ReadGraphics(f, v.graphics))
                    return false;
                if (const Json* b = f.Raw("section_box"))
                {
                    Fields s(*b, who + " section_box", path + "/section_box", leftOut, error);
                    SectionBox box;
                    if (!s.IsObject() || !ReadP3(s, "min", box.min) || !ReadP3(s, "max", box.max) || !s.Bool("on", box.on) || !s.Finish())
                        return false;
                    v.sectionBox = box;
                }
                if (!f.Finish())
                    return false;
                out = Element(std::move(v));
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
                if (!f.Allow({ "type", "level", "start", "end", "top_level", "height", "base_offset", "arc", "location", "attach_top", "attach_base", "params" }))
                    return false;
                ReadWallLike(f, w, ok);
                if (!ok || !ReadParams(f, w.params) || !f.Finish())
                    return false;
                out = Element(std::move(w));
                return true;
            }
            if (category == "floor")
            {
                Floor x;
                if (!f.Allow({ "type", "level", "boundary", "openings", "offset", "slope", "arrow", "structural", "pattern_origin", "pattern_angle", "sketch", "params" }) ||
                    !ReadParams(f, x.params) ||
                    !f.SketchLines("sketch", x.sketch) ||
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
                if (!f.Allow({ "type", "level", "boundary", "openings", "offset", "sketch" }) || !f.SketchLines("sketch", x.sketch) ||
                    !f.String("type", x.typeName, true) ||
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
                if (!f.Allow({ "type", "level", "boundary", "courtyards", "slope", "overhang", "gable", "gables", "offset", "openings", "join", "extrusion", "sketch", "params" }) ||
                    !f.SketchLines("sketch", x.sketch) || !ReadParams(f, x.params) ||
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
                if (!f.Allow({ "type", "family", "host", "offset", "sill", "mark", "flip_facing", "flip_hand", "params" }) || !f.String("type", o.typeName, true) || !f.OptString("family", o.family) || !f.Id("host", o.host, true) ||
                    !f.Number("offset", o.offset, true) || !f.Number("sill", o.sill, false) || !f.String("mark", o.mark, false) ||
                    !f.Bool("flip_facing", o.flipFacing) || !f.Bool("flip_hand", o.flipHand) || !ReadParams(f, o.params) || !f.Finish())
                    return false;
                if (category == "door")
                    out = Element(Door{ std::move(o) });
                else
                    out = Element(Window{ std::move(o) });
                return true;
            }
            if (category == "component")
            {
                Component c;
                if (!f.Allow({ "family", "type", "level", "at", "angle", "offset", "mirrored", "params" }) || !f.String("family", c.family, true) ||
                    !f.String("type", c.typeName, true) || !f.Id("level", c.level, true) || !f.Point("at", c.at, true) ||
                    !f.Number("angle", c.angle, false) || !f.Number("offset", c.offset, false) || !f.Bool("mirrored", c.mirrored) ||
                    !ReadParams(f, c.params) || !f.Finish())
                    return false;
                out = Element(std::move(c));
                return true;
            }
            if (category == "column" || category == "structuralcolumn" || category == "footing")
            {
                Column c;
                if (!f.Allow({ "type", "level", "at", "angle", "top_level", "height", "base_offset" }) || !f.String("type", c.typeName, true) ||
                    !f.Id("level", c.level, true) || !f.Point("at", c.at, true) || !f.Number("angle", c.angle, false) ||
                    !f.OptId("top_level", c.topLevel) || !f.Number("height", c.height, false) || !f.Number("base_offset", c.baseOffset, false) ||
                    !f.Finish())
                    return false;
                if (category == "column")
                    out = Element(std::move(c));
                else if (category == "footing")
                    out = Element(Footing{ std::move(c) });
                else
                    out = Element(StructuralColumn{ std::move(c) });
                return true;
            }
            if (category == "mass")
            {
                Mass x;
                if (!f.Allow({ "name", "level", "boundary", "top_level", "height", "base_offset", "top", "params" }) ||
                    !f.String("name", x.name, false) || !f.Id("level", x.level, true) || !f.Points("boundary", x.boundary, true) ||
                    !f.OptId("top_level", x.topLevel) || !f.Number("height", x.height, false) || !f.Number("base_offset", x.baseOffset, false) ||
                    !ReadParams(f, x.params))
                    return false;
                if (f.Raw("top") && !f.Raw("top")->is_null())
                {
                    Ring top;
                    if (!f.Points("top", top, true))
                        return false;
                    x.top = std::move(top);
                }
                if (!f.Finish())
                    return false;
                out = Element(std::move(x));
                return true;
            }
            if (category == "dimension")
            {
                Dimension d;
                std::string kind;
                if (!f.Allow({ "level", "view", "kind", "refs", "at" }) || !f.Id("level", d.level, true) || !f.OptId("view", d.view) ||
                    !f.String("kind", kind, false) || !f.Point("at", d.at, true))
                    return false;
                if (kind == "angular")
                    d.kind = DimKind::Angular;
                else if (kind == "radial")
                    d.kind = DimKind::Radial;
                else if (!kind.empty() && kind != "aligned")
                    return f.Fail("`kind`: unknown variant `" + kind + "`, expected one of aligned, angular, radial");
                const Json* refs = f.Raw("refs");
                if (!refs)
                    return f.Missing("refs");
                if (!refs->is_array())
                    return f.Fail("`refs` must be a list of references {id, face?}");
                for (const Json& rv : *refs)
                {
                    Fields g(rv, who + " ref", path + "/refs", leftOut, error);
                    Reference r;
                    std::string face;
                    if (!g.IsObject() || !g.Id("id", r.id, true) || !g.String("face", face, false) || !g.Finish())
                        return false;
                    if (!face.empty())
                    {
                        const auto fc = FaceOf::FromName(face);
                        if (!fc)
                            return g.Fail("`face`: unknown variant `" + face + "`, expected one of centre, left, right, start, end");
                        r.face = *fc;
                    }
                    d.refs.push_back(r);
                }
                if (!f.Finish())
                    return false;
                out = Element(std::move(d));
                return true;
            }
            if (category == "text")
            {
                Note n;
                if (!f.Allow({ "view", "at", "text", "type", "angle", "leader", "override_format", "runs" }) || !f.Id("view", n.view, true) ||
                    !f.Point("at", n.at, true) || !f.String("text", n.text, true) || !f.String("type", n.typeName, false) ||
                    !f.Number("angle", n.angle, false) || !f.Bool("override_format", n.overrideFormat))
                    return false;
                if (f.Raw("leader"))
                {
                    P2 l;
                    if (!f.Point("leader", l, true))
                        return false;
                    n.leader = l;
                }
                if (const Json* runs = f.Raw("runs"))
                {
                    if (!runs->is_array())
                        return f.Fail("`runs` must be a list of {from, to, bold?, italic?, underline?}");
                    for (const Json& rv : *runs)
                    {
                        Fields g(rv, who + " run", path + "/runs", leftOut, error);
                        TextRun r;
                        double from = -1.0, to = -1.0;
                        if (!g.IsObject() || !g.Number("from", from, true) || !g.Number("to", to, true))
                            return false;
                        if (!(from >= 0.0 && to >= 0.0 && from == std::floor(from) && to == std::floor(to) && to < 1e9))
                            return g.Fail("`from` and `to` are counts of characters");
                        r.from = static_cast<size_t>(from);
                        r.to   = static_cast<size_t>(to);
                        for (const auto& [k, slot] : { std::pair{ "bold", &r.bold }, std::pair{ "italic", &r.italic }, std::pair{ "underline", &r.underline } })
                            if (g.Raw(k))
                            {
                                bool b = false;
                                if (!g.Bool(k, b))
                                    return false;
                                *slot = b;
                            }
                        if (!g.Finish())
                            return false;
                        n.runs.push_back(r);
                    }
                }
                if (!f.Finish())
                    return false;
                out = Element(std::move(n));
                return true;
            }
            if (category == "spot")
            {
                Spot sp;
                if (!f.Allow({ "view", "at", "host" }) || !f.Id("view", sp.view, true) || !f.Point("at", sp.at, true) || !f.OptId("host", sp.host) ||
                    !f.Finish())
                    return false;
                out = Element(std::move(sp));
                return true;
            }
            if (category == "tag")
            {
                Tag t;
                if (!f.Allow({ "host", "view", "offset", "leader", "label" }) || !f.Id("host", t.host, true) || !f.OptId("view", t.view) ||
                    !f.Point("offset", t.offset, true) || !f.Bool("leader", t.leader) || !f.OptString("label", t.label) || !f.Finish())
                    return false;
                out = Element(std::move(t));
                return true;
            }
            if (category == "line")
            {
                DetailLine l;
                bool unsupported = false;
                if (!f.Allow({ "view", "style", "insulation", "centre_mark", "cloud", "revision", "start", "end", "through", "arc", "points", "arcs",
                               "closed", "centre", "radius", "radii", "angle", "span", "control", "knots", "fit", "start_tangent", "end_tangent" }) ||
                    !f.Id("view", l.view, true) || !f.String("style", l.style, true) || !f.OptNumber("insulation", l.insulation) ||
                    !f.Bool("centre_mark", l.centreMark) || !f.OptNumber("cloud", l.cloud) || !f.OptString("revision", l.revision) ||
                    !ReadCurve(f, l.curve, unsupported))
                    return false;
                if (unsupported)
                {
                    skip = true;
                    return true;
                }
                if (!f.Finish())
                    return false;
                out = Element(std::move(l));
                return true;
            }
            if (category == "region")
            {
                FilledRegion r;
                const size_t before = leftOut ? leftOut->size() : 0;
                if (!f.Allow({ "view", "type", "line_style", "sketch", "pattern_origin", "pattern_angle" }) || !f.Id("view", r.view, true) ||
                    !f.String("type", r.typeName, true) || !f.OptString("line_style", r.lineStyle) || !f.SketchLines("sketch", r.sketch) ||
                    !f.Point("pattern_origin", r.patternOrigin, false) || !f.Number("pattern_angle", r.patternAngle, false) || !f.Finish())
                    return false;
                // 草图里有还没移植的曲线：区域没有轮廓，整个略去
                if (r.sketch.empty() && leftOut && leftOut->size() > before)
                {
                    leftOut->resize(before);
                    skip = true;
                    return true;
                }
                out = Element(std::move(r));
                return true;
            }
            if (IsRunCategory(category))
            {
                Run r;
                if (!f.Allow({ "type", "level", "start", "end", "offset", "end_offset", "justify", "system" }) || !f.String("type", r.typeName, true) ||
                    !f.Id("level", r.level, true) || !f.Point("start", r.start, true) || !f.Point("end", r.end, true) ||
                    !f.Number("offset", r.offset, false) || !f.OptNumber("end_offset", r.endOffset) ||
                    !EnumName(f, "justify", r.justify, [](std::string_view s) { return JustifyOf::FromName(s); }, "top, centre, bottom") ||
                    !f.String("system", r.system, false) || !f.Finish())
                    return false;
                out = *Element::OfRun(category, std::move(r));
                return true;
            }
            if (leftOut)
            {
                skip = true;
                return true;
            }
            error = who + ": unknown variant `" + category +
                    "`, expected one of level, grid, roomseparator, wall, floor, ceiling, door, window, room, roof, component, column, structuralcolumn, footing, "
                    "beam, stripfooting, pipe, conduit, cabletray, wire, downconductor, groundconductor, cable, mass, dimension, text, spot, tag, line, region";
            return false;
        }

        Json ParamsJson(const std::map<std::string, Json>& params)
        {
            Json out = Json::object();
            for (const auto& [k, v] : params)
                out[k] = v;
            return out;
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

        // 线型图案的一段：{"dash": 3}、{"space": 1.5}、"dot"、{"dot_length": 1}、{"offset": {length, count, distance}}、
        // {"wave" | "zigzag": {length, frequency, amplitude}}、{"text": {length, size, text}}
        bool ReadSegment(const Json& v, LineSegment& s, std::string& why)
        {
            using K = LineSegment::Kind;
            if (v.is_string() && v.get<std::string>() == "dot")
            {
                s.kind = K::Dot;
                return true;
            }
            const char* expected = "a segment is {\"dash\": mm}, {\"space\": mm}, \"dot\", {\"dot_length\": mm}, {\"offset\": {...}}, "
                                   "{\"wave\": {...}}, {\"zigzag\": {...}} or {\"text\": {...}}";
            if (!v.is_object() || v.size() != 1)
                return why = expected, false;
            const std::string key = v.begin().key();
            const Json& x = v.begin().value();
            const auto number = [&](const Json& o, const char* k, double& out)
            {
                const auto it = o.find(k);
                if (it == o.end() || !it->is_number())
                    return why = std::string("`") + key + "` needs a number `" + k + "`", false;
                out = it->get<double>();
                return true;
            };
            const auto only = [&](std::initializer_list<const char*> keys)
            {
                for (auto it = x.begin(); it != x.end(); ++it)
                    if (std::none_of(keys.begin(), keys.end(), [&](const char* k) { return it.key() == k; }))
                        return why = "`" + key + "` has an unknown field `" + it.key() + "`", false;
                return true;
            };
            if (key == "dash" || key == "space" || key == "dot_length")
            {
                if (!x.is_number())
                    return why = "`" + key + "` must be a length in mm", false;
                s.kind   = key == "dash" ? K::Dash : key == "space" ? K::Space : K::DotLength;
                s.length = x.get<double>();
                return true;
            }
            if (!x.is_object())
                return why = expected, false;
            if (key == "offset")
            {
                double count = 0.0;
                if (!only({ "length", "count", "distance" }) || !number(x, "length", s.length) || !number(x, "count", count) ||
                    !number(x, "distance", s.distance))
                    return false;
                if (!(count >= 0.0 && count <= 255.0 && count == std::floor(count)))
                    return why = "`offset` has a whole `count` of lines, 0 to 255", false;
                s.kind  = K::Offset;
                s.count = static_cast<uint8_t>(count);
                return true;
            }
            if (key == "wave" || key == "zigzag")
            {
                s.kind = key == "wave" ? K::Wave : K::Zigzag;
                return only({ "length", "frequency", "amplitude" }) && number(x, "length", s.length) && number(x, "frequency", s.frequency) &&
                       number(x, "amplitude", s.amplitude);
            }
            if (key == "text")
            {
                s.kind = K::Text;
                const auto t = x.find("text");
                if (t == x.end() || !t->is_string())
                    return why = "`text` needs a string `text`", false;
                s.text = t->get<std::string>();
                return only({ "length", "size", "text" }) && number(x, "length", s.length) && number(x, "size", s.size);
            }
            return why = expected, false;
        }

        Json SegmentJson(const LineSegment& s)
        {
            using K = LineSegment::Kind;
            switch (s.kind)
            {
            case K::Dash: return Json{ { "dash", s.length } };
            case K::Space: return Json{ { "space", s.length } };
            case K::Dot: return "dot";
            case K::DotLength: return Json{ { "dot_length", s.length } };
            case K::Offset: return Json{ { "offset", { { "length", s.length }, { "count", s.count }, { "distance", s.distance } } } };
            case K::Wave:
            case K::Zigzag:
                return Json{ { s.kind == K::Wave ? "wave" : "zigzag", { { "length", s.length }, { "frequency", s.frequency }, { "amplitude", s.amplitude } } } };
            case K::Text: return Json{ { "text", { { "length", s.length }, { "size", s.size }, { "text", s.text } } } };
            }
            return nullptr;
        }

        // 注释的库条目：line_patterns、line_styles、fill_patterns、region_types、text_types（没有的键保留模型起步的）
        bool ReadStyles(Fields& f, Model& m)
        {
            if (const Json* lp = f.Raw("line_patterns"))
            {
                if (!lp->is_object())
                    return f.Fail("`line_patterns` must be an object of patterns by name");
                m.linePatterns.clear();
                for (auto it = lp->begin(); it != lp->end(); ++it)
                {
                    const std::string at = "line pattern `" + it.key() + "`";
                    if (!it->is_array())
                        return f.Fail(at + " must be a list of segments");
                    LinePattern p;
                    for (const Json& sv : *it)
                    {
                        LineSegment s;
                        std::string why;
                        if (!ReadSegment(sv, s, why))
                            return f.Fail(at + ": " + why);
                        p.segments.push_back(std::move(s));
                    }
                    m.linePatterns[it.key()] = std::move(p);
                }
            }
            return ReadNamed(f, "line_styles", m.lineStyles, false, [](Fields& g, LineStyle& s)
                   {
                       s.pen = 1;
                       if (!g.Raw("pen"))
                           return g.Missing("pen");
                       return ReadPen(g, "pen", s.pen) && ReadColor(g, "color", s.color) && g.OptString("pattern", s.pattern);
                   }) &&
                   ReadNamed(f, "fill_patterns", m.fillPatterns, false, [](Fields& g, FillPattern& p)
                   {
                       std::string target;
                       if (!g.String("target", target, false))
                           return false;
                       if (target == "model")
                           p.model = true;
                       else if (!target.empty() && target != "drafting")
                           return g.Fail("`target`: unknown variant `" + target + "`, expected `drafting` or `model`");
                       const Json* ls = g.Raw("lines");
                       if (!ls)
                           return true;
                       if (!ls->is_array())
                           return g.Fail("`lines` must be a list of {angle, origin?, shift?, spacing, dashes?}");
                       for (const Json& lv : *ls)
                       {
                           Fields h(lv, g.Who() + " line", g.Path() + "/lines", g.LeftOut(), g.Error());
                           HatchLine l;
                           if (!h.IsObject() || !h.Number("angle", l.angle, true) || !h.Point("origin", l.origin, false) ||
                               !h.Number("shift", l.shift, false) || !h.Number("spacing", l.spacing, true))
                               return false;
                           if (const Json* d = h.Raw("dashes"))
                           {
                               if (!d->is_array())
                                   return h.Fail("`dashes` must be a list of lengths");
                               for (const Json& x : *d)
                               {
                                   if (!x.is_number())
                                       return h.Fail("`dashes` must be a list of lengths");
                                   l.dashes.push_back(x.get<double>());
                               }
                           }
                           if (!h.Finish())
                               return false;
                           p.lines.push_back(std::move(l));
                       }
                       return true;
                   }) &&
                   ReadNamed(f, "region_types", m.regionTypes, false, [](Fields& g, RegionType& t)
                   {
                       return g.OptString("foreground", t.foreground) && ReadColor(g, "foreground_color", t.foregroundColor) &&
                              g.OptString("background", t.background) && ReadColor(g, "background_color", t.backgroundColor) &&
                              g.Bool("masking", t.masking);
                   }) &&
                   ReadNamed(f, "text_types", m.textTypes, false, [](Fields& g, TextType& t)
                   {
                       t = TextType{};
                       return g.String("font", t.font, false) && g.Number("size", t.size, false) && g.Bool("bold", t.bold) &&
                              g.Bool("italic", t.italic) && g.Bool("underline", t.underline) && g.Number("width_factor", t.widthFactor, false) &&
                              ReadColor(g, "color", t.color) && g.Bool("border", t.border) && g.Number("border_offset", t.borderOffset, false) &&
                              ReadPen(g, "border_pen", t.borderPen);
                   });
        }

        // run 的类型：{类别: {名称: {width, depth, round?, material?}}}
        bool ReadRunTypes(Fields& parent, std::map<std::string, std::map<std::string, RunType>>& out)
        {
            const Json* v = parent.Raw("runs");
            if (!v)
                return true;
            if (!v->is_object())
                return parent.Fail("`runs` must be an object of categories");
            out.clear();
            for (auto it = v->begin(); it != v->end(); ++it)
            {
                Fields cat(*v, parent.Who(), parent.Path() + "/runs", parent.LeftOut(), parent.Error());
                if (!ReadNamed(cat, it.key().c_str(), out[it.key()], true, [](Fields& f, RunType& r)
                    {
                        return f.Number("width", r.width, true) && f.Number("depth", r.depth, true) && f.Bool("round", r.round) &&
                               f.String("material", r.material, false);
                    }))
                    return false;
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

        bool ReadColumnType(Fields& f, ColumnType& t)
        {
            return f.Number("width", t.width, true) && f.Number("depth", t.depth, true) && f.Bool("round", t.round);
        }

        Json ColumnTypesJson(const std::map<std::string, ColumnType>& map)
        {
            Json out = Json::object();
            for (const auto& [n, t] : map)
                out[n] = Json{ { "width", t.width }, { "depth", t.depth }, { "round", t.round } };
            return out;
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
    // ── 族（family.rs 的 serde）────────────────────────────────────
    namespace
    {
        Json ExprJson(const Fam::Expr& e)
        {
            if (const double* d = std::get_if<double>(&e))
                return *d;
            return std::get<std::string>(e);
        }

        Json PairJson(const std::array<Fam::Expr, 2>& p) { return Json::array({ ExprJson(p[0]), ExprJson(p[1]) }); }

        Json PairsJson(const std::vector<std::array<Fam::Expr, 2>>& ps)
        {
            Json out = Json::array();
            for (const auto& p : ps)
                out.push_back(PairJson(p));
            return out;
        }

        // 数或公式（serde 的 untagged）
        bool ExprOf(Fields& f, const std::string& what, const Json& v, Fam::Expr& out)
        {
            if (v.is_number())
                out = v.get<double>();
            else if (v.is_string())
                out = v.get<std::string>();
            else
                return f.Fail("`" + what + "`: data did not match any variant of untagged enum Expr");
            return true;
        }

        bool ReadExpr(Fields& f, const char* key, Fam::Expr& out, bool required)
        {
            const Json* v = f.Raw(key);
            if (!v)
                return required ? f.Missing(key) : true;
            return ExprOf(f, key, *v, out);
        }

        bool ReadOptExpr(Fields& f, const char* key, std::optional<Fam::Expr>& out)
        {
            out.reset();
            const Json* v = f.Raw(key);
            if (!v)
                return true;
            Fam::Expr e;
            if (!ExprOf(f, key, *v, e))
                return false;
            out = std::move(e);
            return true;
        }

        bool PairOf(Fields& f, const std::string& what, const Json& v, std::array<Fam::Expr, 2>& out)
        {
            if (!v.is_array() || v.size() != 2)
                return f.Fail("`" + what + "` is a pair [x, y], got " + v.dump());
            return ExprOf(f, what, v[0], out[0]) && ExprOf(f, what, v[1], out[1]);
        }

        bool ReadPairs(Fields& f, const char* key, std::vector<std::array<Fam::Expr, 2>>& out)
        {
            out.clear();
            const Json* v = f.Raw(key);
            if (!v)
                return true;
            if (!v->is_array())
                return f.Fail(std::string("`") + key + "` is a list of pairs [x, y]");
            for (const Json& p : *v)
            {
                std::array<Fam::Expr, 2> pair{ 0.0, 0.0 };
                if (!PairOf(f, key, p, pair))
                    return false;
                out.push_back(std::move(pair));
            }
            return true;
        }

        bool ReadPartIds(Fields& f, Id& id, Guid& guid)
        {
            if (const Json* v = f.Raw("id"))
            {
                const auto whole = AsWhole(*v);
                if (!whole)
                    return f.Fail("`id` must be a whole number");
                id = static_cast<Id>(*whole);
            }
            return f.Id("guid", guid, false);
        }

        bool ReadFamily(Fields& f, Family& fam)
        {
            if (!f.IsObject() || !f.Allow({ "category", "parameters", "types", "forms", "symbols", "operation" }))
                return false;
            if (const Json* c = f.Raw("category"))
            {
                const auto cat = c->is_string() ? Fam::CategoryOf::FromName(c->get<std::string>()) : std::nullopt;
                if (!cat)
                    return f.Fail("`category`: unknown variant " + c->dump());
                fam.category = *cat;
            }
            if (const Json* ps = f.Raw("parameters"))
            {
                if (!ps->is_array())
                    return f.Fail("`parameters` must be a list");
                for (size_t i = 0; i < ps->size(); ++i)
                {
                    Fields g((*ps)[i], f.Who().empty() ? "parameter " + std::to_string(i + 1) : f.Who() + " parameter " + std::to_string(i + 1),
                             f.Path() + "/parameters/" + std::to_string(i), f.LeftOut(), f.Error());
                    Fam::Parameter p;
                    if (!g.IsObject() || !g.Allow({ "name", "kind", "instance", "value", "formula" }) || !g.String("name", p.name, true))
                        return false;
                    if (const Json* k = g.Raw("kind"))
                    {
                        const auto kind = k->is_string() ? Fam::KindOf::FromName(k->get<std::string>()) : std::nullopt;
                        if (!kind)
                            return g.Fail("`kind`: unknown variant " + k->dump() + ", expected one of length, number, angle, yesno, material");
                        p.kind = *kind;
                    }
                    if (!g.Bool("instance", p.instance))
                        return false;
                    if (const Json* v = g.Raw("value"))
                        p.value = *v;
                    if (!g.OptString("formula", p.formula) || !g.Finish())
                        return false;
                    fam.parameters.push_back(std::move(p));
                }
            }
            if (const Json* ts = f.Raw("types"))
            {
                if (!ts->is_object())
                    return f.Fail("`types` must be an object of types by name");
                for (auto it = ts->begin(); it != ts->end(); ++it)
                {
                    if (!it.value().is_object())
                        return f.Fail("`types`: type `" + it.key() + "` must be an object of values by parameter");
                    auto& set = fam.types[it.key()];
                    for (auto v = it.value().begin(); v != it.value().end(); ++v)
                        set[v.key()] = v.value();
                }
            }
            if (const Json* fs = f.Raw("forms"))
            {
                if (!fs->is_array())
                    return f.Fail("`forms` must be a list");
                for (size_t i = 0; i < fs->size(); ++i)
                {
                    Fields g((*fs)[i], f.Who().empty() ? "form " + std::to_string(i + 1) : f.Who() + " form " + std::to_string(i + 1),
                             f.Path() + "/forms/" + std::to_string(i), f.LeftOut(), f.Error());
                    Fam::Form x;
                    if (!g.IsObject() ||
                        !g.Allow({ "id", "guid", "plane", "profile", "circle", "from", "to", "axis", "void", "material", "visible", "mesh" }) ||
                        !ReadPartIds(g, x.id, x.guid))
                        return false;
                    if (const Json* pl = g.Raw("plane"))
                    {
                        const std::string n = pl->is_string() ? pl->get<std::string>() : std::string();
                        if (n == "xy")
                            x.plane = Fam::SketchPlane::Xy;
                        else if (n == "xz")
                            x.plane = Fam::SketchPlane::Xz;
                        else if (n == "yz")
                            x.plane = Fam::SketchPlane::Yz;
                        else
                            return g.Fail("`plane`: unknown variant " + pl->dump() + ", expected one of xy, xz, yz");
                    }
                    if (!ReadPairs(g, "profile", x.profile))
                        return false;
                    if (const Json* c = g.Raw("circle"))
                    {
                        Fields h(*c, g.Who() + " circle", g.Path() + "/circle", g.LeftOut(), g.Error());
                        Fam::Circle circle;
                        const Json* centre = h.IsObject() && h.Allow({ "centre", "radius" }) ? h.Raw("centre") : nullptr;
                        if (!centre)
                            return h.IsObject() ? h.Missing("centre") : false;
                        if (!PairOf(h, "centre", *centre, circle.centre) || !ReadExpr(h, "radius", circle.radius, true) || !h.Finish())
                            return false;
                        x.circle = std::move(circle);
                    }
                    if (!ReadExpr(g, "from", x.from, false) || !ReadExpr(g, "to", x.to, false))
                        return false;
                    if (const Json* a = g.Raw("axis"))
                    {
                        if (!a->is_array() || a->size() != 2)
                            return g.Fail("`axis` is two points [[x, y], [x, y]]");
                        std::array<std::array<Fam::Expr, 2>, 2> axis{};
                        if (!PairOf(g, "axis", (*a)[0], axis[0]) || !PairOf(g, "axis", (*a)[1], axis[1]))
                            return false;
                        x.axis = std::move(axis);
                    }
                    if (!g.Bool("void", x.isVoid) || !g.String("material", x.material, false) || !ReadOptExpr(g, "visible", x.visible))
                        return false;
                    if (const Json* m = g.Raw("mesh"))
                    {
                        if (!m->is_array())
                            return g.Fail("`mesh` is a list of triangles [[x, y, z], [x, y, z], [x, y, z]]");
                        auto tris = std::make_shared<std::vector<Tri>>();
                        for (const Json& t : *m)
                        {
                            if (!t.is_array() || t.size() != 3)
                                return g.Fail("`mesh` is a list of triangles [[x, y, z], [x, y, z], [x, y, z]]");
                            Tri tri{};
                            for (size_t k = 0; k < 3; ++k)
                            {
                                if (!t[k].is_array() || t[k].size() != 3 || !t[k][0].is_number() || !t[k][1].is_number() || !t[k][2].is_number())
                                    return g.Fail("`mesh` is a list of triangles [[x, y, z], [x, y, z], [x, y, z]]");
                                tri[k] = P3{ t[k][0].get<double>(), t[k][1].get<double>(), t[k][2].get<double>() };
                            }
                            tris->push_back(tri);
                        }
                        x.mesh = std::move(tris);
                    }
                    if (!g.Finish())
                        return false;
                    fam.forms.push_back(std::move(x));
                }
            }
            if (const Json* ss = f.Raw("symbols"))
            {
                if (!ss->is_array())
                    return f.Fail("`symbols` must be a list");
                for (size_t i = 0; i < ss->size(); ++i)
                {
                    Fields g((*ss)[i], f.Who().empty() ? "symbol " + std::to_string(i + 1) : f.Who() + " symbol " + std::to_string(i + 1),
                             f.Path() + "/symbols/" + std::to_string(i), f.LeftOut(), f.Error());
                    Fam::Symbol y;
                    if (!g.IsObject() || !g.Allow({ "id", "guid", "line", "arc", "visible" }) || !ReadPartIds(g, y.id, y.guid) ||
                        !ReadPairs(g, "line", y.line))
                        return false;
                    if (const Json* a = g.Raw("arc"))
                    {
                        Fields h(*a, g.Who() + " arc", g.Path() + "/arc", g.LeftOut(), g.Error());
                        Fam::Curve c;
                        const Json* centre = h.IsObject() && h.Allow({ "centre", "radius", "from", "to" }) ? h.Raw("centre") : nullptr;
                        if (!centre)
                            return h.IsObject() ? h.Missing("centre") : false;
                        if (!PairOf(h, "centre", *centre, c.centre) || !ReadExpr(h, "radius", c.radius, true) || !ReadExpr(h, "from", c.from, false) ||
                            !ReadExpr(h, "to", c.to, true) || !h.Finish())
                            return false;
                        y.arc = std::move(c);
                    }
                    if (!ReadOptExpr(g, "visible", y.visible) || !g.Finish())
                        return false;
                    fam.symbols.push_back(std::move(y));
                }
            }
            if (const Json* op = f.Raw("operation"))
            {
                const auto o = op->is_string() ? OperationOf::Parse(op->get<std::string>()) : std::nullopt;
                if (!o || op->get<std::string>() != OperationOf::Name(*o))
                    return f.Fail("`operation`: unknown variant " + op->dump() +
                                  ", expected one of swing, double-swing, sliding, double-sliding, folding, fixed");
                fam.operation = o;
            }
            return f.Finish();
        }

        Json FamilyJson(const Family& fam)
        {
            Json j = Json::object();
            j["category"] = Fam::CategoryOf::Name(fam.category);
            Json ps = Json::array();
            for (const Fam::Parameter& p : fam.parameters)
            {
                Json q = Json::object();
                q["name"] = p.name;
                q["kind"] = Fam::KindOf::Name(p.kind);
                if (p.instance)
                    q["instance"] = true;
                q["value"] = p.value;
                if (p.formula)
                    q["formula"] = *p.formula;
                ps.push_back(std::move(q));
            }
            j["parameters"] = std::move(ps);
            Json ts = Json::object();
            for (const auto& [n, set] : fam.types)
            {
                Json t = Json::object();
                for (const auto& [k, v] : set)
                    t[k] = v;
                ts[n] = std::move(t);
            }
            j["types"] = std::move(ts);
            Json fs = Json::array();
            for (const Fam::Form& x : fam.forms)
            {
                Json g = Json::object();
                if (x.id != 0)
                    g["id"] = x.id;
                if (!x.guid.IsNil())
                    g["guid"] = ToJson(x.guid);
                g["plane"] = x.plane == Fam::SketchPlane::Xy ? "xy" : (x.plane == Fam::SketchPlane::Xz ? "xz" : "yz");
                if (!x.profile.empty())
                    g["profile"] = PairsJson(x.profile);
                if (x.circle)
                    g["circle"] = Json{ { "centre", PairJson(x.circle->centre) }, { "radius", ExprJson(x.circle->radius) } };
                g["from"] = ExprJson(x.from);
                g["to"]   = ExprJson(x.to);
                if (x.axis)
                    g["axis"] = Json::array({ PairJson((*x.axis)[0]), PairJson((*x.axis)[1]) });
                if (x.isVoid)
                    g["void"] = true;
                if (!x.material.empty())
                    g["material"] = x.material;
                if (x.visible)
                    g["visible"] = ExprJson(*x.visible);
                if (x.HasMesh())
                {
                    Json m = Json::array();
                    for (const Tri& t : *x.mesh)
                        m.push_back(Json::array({ Json::array({ t[0][0], t[0][1], t[0][2] }), Json::array({ t[1][0], t[1][1], t[1][2] }),
                                                  Json::array({ t[2][0], t[2][1], t[2][2] }) }));
                    g["mesh"] = std::move(m);
                }
                fs.push_back(std::move(g));
            }
            j["forms"] = std::move(fs);
            if (!fam.symbols.empty())
            {
                Json ss = Json::array();
                for (const Fam::Symbol& y : fam.symbols)
                {
                    Json g = Json::object();
                    if (y.id != 0)
                        g["id"] = y.id;
                    if (!y.guid.IsNil())
                        g["guid"] = ToJson(y.guid);
                    if (!y.line.empty())
                        g["line"] = PairsJson(y.line);
                    if (y.arc)
                        g["arc"] = Json{ { "centre", PairJson(y.arc->centre) },
                                         { "radius", ExprJson(y.arc->radius) },
                                         { "from", ExprJson(y.arc->from) },
                                         { "to", ExprJson(y.arc->to) } };
                    if (y.visible)
                        g["visible"] = ExprJson(*y.visible);
                    ss.push_back(std::move(g));
                }
                j["symbols"] = std::move(ss);
            }
            if (fam.operation)
                j["operation"] = OperationOf::Name(*fam.operation);
            return j;
        }
    }

    Json ToJson(const Family& f)
    {
        return FamilyJson(f);
    }

    bool FromJson(const Json& v, Family& out, std::string& error)
    {
        Fields f(v, "", "", nullptr, error);
        Family fam;
        if (!ReadFamily(f, fam))
            return false;
        out = std::move(fam);
        return true;
    }


    Json SketchJson(const std::vector<SketchLine>& lines)
    {
        Json out = Json::array();
        for (const SketchLine& l : lines)
        {
            Json o = Json::object();
            if (l.id != 0)
                o["id"] = l.id;
            if (!l.guid.IsNil())
                o["guid"] = ToJson(l.guid);
            o["start"] = ToJson(l.start);
            o["end"]   = ToJson(l.end);
            if (l.arc != 0.0)
                o["arc"] = l.arc;
            if (l.style)
                o["style"] = *l.style;
            if (l.invisible)
                o["invisible"] = true;
            out.push_back(std::move(o));
        }
        return out;
    }

    Json ToJson(const Element& e)
    {
        Json j = Json::object();
        j["category"] = e.Category();
        if (const Level* l = e.As<Level>())
        {
            j["name"]      = l->name;
            j["elevation"] = l->elevation;
            j["view_range"] = ViewRangeJson(l->viewRange);
            if (l->crop)
                j["crop"] = Json::array({ ToJson((*l->crop)[0]), ToJson((*l->crop)[1]) });
            if (l->extent)
                j["extent"] = Json::array({ ToJson((*l->extent)[0]), ToJson((*l->extent)[1]) });
            if (l->sectionBox)
                j["section_box"] = true;
            if (!l->graphics.IsDefault())
                j["graphics"] = ToJson(l->graphics);
        }
        else if (const PlanView* v = e.As<PlanView>())
        {
            j["level"]      = ToJson(v->level);
            j["name"]       = v->name;
            j["view_range"] = ViewRangeJson(v->viewRange);
            if (v->crop)
                j["crop"] = Json::array({ ToJson((*v->crop)[0]), ToJson((*v->crop)[1]) });
            if (!v->graphics.IsDefault())
                j["graphics"] = ToJson(v->graphics);
            if (v->parent)
                j["parent"] = ToJson(*v->parent);
            if (v->sectionBox)
                j["section_box"] = true;
        }
        else if (const View3D* v3 = e.As<View3D>())
        {
            j["name"]      = v3->name;
            j["azimuth"]   = v3->azimuth;
            j["elevation"] = v3->elevation;
            if (!v3->graphics.IsDefault())
                j["graphics"] = ToJson(v3->graphics);
            if (v3->sectionBox)
            {
                const SectionBox& b = *v3->sectionBox;
                j["section_box"] = Json{ { "min", Json::array({ b.min[0], b.min[1], b.min[2] }) },
                                         { "max", Json::array({ b.max[0], b.max[1], b.max[2] }) },
                                         { "on", b.on } };
            }
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
            if (!w->params.empty())
                j["params"] = ParamsJson(w->params);
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
            if (!f->sketch.empty())
                j["sketch"] = SketchJson(f->sketch);
            if (!f->params.empty())
                j["params"] = ParamsJson(f->params);
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
            if (!c->sketch.empty())
                j["sketch"] = SketchJson(c->sketch);
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
            if (!r->sketch.empty())
                j["sketch"] = SketchJson(r->sketch);
            if (!r->params.empty())
                j["params"] = ParamsJson(r->params);
        }
        else if (const Mass* ms = e.As<Mass>())
        {
            j["name"]     = ms->name;
            j["level"]    = ToJson(ms->level);
            j["boundary"] = RingJson(ms->boundary);
            if (ms->topLevel)
                j["top_level"] = ToJson(*ms->topLevel);
            j["height"]      = ms->height;
            j["base_offset"] = ms->baseOffset;
            if (ms->top)
                j["top"] = RingJson(*ms->top);
            if (!ms->params.empty())
                j["params"] = ParamsJson(ms->params);
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
            if (!o->params.empty())
                j["params"] = ParamsJson(o->params);
        }
        else if (const Column* col = e.AsColumn())
        {
            j["type"]        = col->typeName;
            j["level"]       = ToJson(col->level);
            j["at"]          = ToJson(col->at);
            j["angle"]       = col->angle;
            j["top_level"]   = col->topLevel ? ToJson(*col->topLevel) : Json(nullptr);
            j["height"]      = col->height;
            j["base_offset"] = col->baseOffset;
        }
        else if (const Run* r = e.AsRun())
        {
            j["type"]   = r->typeName;
            j["level"]  = ToJson(r->level);
            j["start"]  = ToJson(r->start);
            j["end"]    = ToJson(r->end);
            j["offset"] = r->offset;
            if (r->endOffset)
                j["end_offset"] = *r->endOffset;
            j["justify"] = JustifyOf::Name(r->justify);
            if (!r->system.empty())
                j["system"] = r->system;
        }
        else if (const Component* c = e.As<Component>())
        {
            j["family"] = c->family;
            j["type"]   = c->typeName;
            j["level"]  = ToJson(c->level);
            j["at"]     = ToJson(c->at);
            j["angle"]  = c->angle;
            j["offset"] = c->offset;
            if (c->mirrored)
                j["mirrored"] = true;
            if (!c->params.empty())
                j["params"] = ParamsJson(c->params);
        }
        else if (const Dimension* d = e.As<Dimension>())
        {
            j["level"] = ToJson(d->level);
            if (d->view)
                j["view"] = ToJson(*d->view);
            j["kind"] = d->kind == DimKind::Angular ? "angular" : d->kind == DimKind::Radial ? "radial" : "aligned";
            Json refs = Json::array();
            for (const Reference& r : d->refs)
                refs.push_back(Json{ { "id", ToJson(r.id) }, { "face", FaceOf::Name(r.face) } });
            j["refs"] = std::move(refs);
            j["at"]   = ToJson(d->at);
        }
        else if (const Note* n = e.As<Note>())
        {
            j["view"]  = ToJson(n->view);
            j["at"]    = ToJson(n->at);
            j["text"]  = n->text;
            j["type"]  = n->typeName;
            j["angle"] = n->angle;
            if (n->leader)
                j["leader"] = ToJson(*n->leader);
            if (n->overrideFormat)
                j["override_format"] = true;
            if (!n->runs.empty())
            {
                Json runs = Json::array();
                for (const TextRun& r : n->runs)
                {
                    Json o{ { "from", r.from }, { "to", r.to } };
                    if (r.bold)
                        o["bold"] = *r.bold;
                    if (r.italic)
                        o["italic"] = *r.italic;
                    if (r.underline)
                        o["underline"] = *r.underline;
                    runs.push_back(std::move(o));
                }
                j["runs"] = std::move(runs);
            }
        }
        else if (const Spot* sp = e.As<Spot>())
        {
            j["view"] = ToJson(sp->view);
            j["at"]   = ToJson(sp->at);
            if (sp->host)
                j["host"] = ToJson(*sp->host);
        }
        else if (const Tag* t = e.As<Tag>())
        {
            j["host"] = ToJson(t->host);
            if (t->view)
                j["view"] = ToJson(*t->view);
            j["offset"] = ToJson(t->offset);
            j["leader"] = t->leader;
            if (t->label)
                j["label"] = *t->label;
        }
        else if (const DetailLine* l = e.As<DetailLine>())
        {
            j["view"]  = ToJson(l->view);
            j["style"] = l->style;
            if (l->insulation)
                j["insulation"] = *l->insulation;
            if (l->centreMark)
                j["centre_mark"] = true;
            if (l->cloud)
                j["cloud"] = *l->cloud;
            if (l->revision)
                j["revision"] = *l->revision;
            CurveJson(l->curve, j);
        }
        else if (const FilledRegion* r = e.As<FilledRegion>())
        {
            j["view"] = ToJson(r->view);
            j["type"] = r->typeName;
            if (r->lineStyle)
                j["line_style"] = *r->lineStyle;
            j["sketch"] = SketchJson(r->sketch);
            if (r->patternOrigin != P2{})
                j["pattern_origin"] = ToJson(r->patternOrigin);
            if (r->patternAngle != 0.0)
                j["pattern_angle"] = r->patternAngle;
        }
        return j;
    }

    Json ToJson(const Override& o)
    {
        Json j = Json::object();
        if (o.hidden)
            j["hidden"] = true;
        if (!o.projection.Empty())
            j["projection"] = StyleJson(o.projection);
        if (!o.cut.Empty())
            j["cut"] = StyleJson(o.cut);
        if (o.halftone)
            j["halftone"] = true;
        if (o.transparency)
            j["transparency"] = o.transparency;
        return j;
    }

    bool FromJson(const Json& v, Override& out, std::string& error)
    {
        return ReadOverride(v, "", "", nullptr, error, out);
    }

    Json ToJson(const Graphics& g)
    {
        Json j = Json::object();
        if (!g.categories.empty())
        {
            Json c = Json::object();
            for (const auto& [name, o] : g.categories)
                c[name] = ToJson(o);
            j["categories"] = std::move(c);
        }
        if (!g.elements.empty())
        {
            Json e = Json::object();
            for (const auto& [id, o] : g.elements)
                e[id.ToString()] = ToJson(o);
            j["elements"] = std::move(e);
        }
        if (g.scale)
            j["scale"] = *g.scale;
        if (g.detail != Detail::Fine)
            j["detail"] = Text::AsciiLower(DetailOf::Name(g.detail));
        if (g.style != VisualStyle::Shaded)
            j["style"] = VisualStyleOf::Key(g.style);
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
        t["column"] = ColumnTypesJson(types.column);
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
        if (!types.structuralColumn.empty())
            t["structural_column"] = ColumnTypesJson(types.structuralColumn);
        if (!types.footing.empty())
            t["footing"] = ColumnTypesJson(types.footing);
        if (!types.runs.empty())
        {
            Json runs = Json::object();
            for (const auto& [cat, map] : types.runs)
            {
                Json one = Json::object();
                for (const auto& [n, r] : map)
                {
                    Json x = Json{ { "width", r.width }, { "depth", r.depth }, { "round", r.round } };
                    if (!r.material.empty())
                        x["material"] = r.material;
                    one[n] = std::move(x);
                }
                runs[cat] = std::move(one);
            }
            t["runs"] = std::move(runs);
        }
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
            if (x.transparency != 0)
                y["transparency"] = x.transparency;
            mats[n] = std::move(y);
        }
        return mats;
    }

    Json ToJson(const TextType& t)
    {
        Json y{ { "font", t.font }, { "size", t.size }, { "color", ColorJson(t.color) }, { "border_offset", t.borderOffset },
                { "border_pen", t.borderPen } };
        if (t.bold)
            y["bold"] = true;
        if (t.italic)
            y["italic"] = true;
        if (t.underline)
            y["underline"] = true;
        if (t.widthFactor != 1.0)
            y["width_factor"] = t.widthFactor;
        if (t.border)
            y["border"] = true;
        return y;
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
        Json fams = Json::object();
        for (const auto& [n, f] : families)
            fams[n] = Bim::ToJson(f);
        j["families"] = std::move(fams);
        Json lp = Json::object();
        for (const auto& [n, p] : linePatterns)
        {
            Json segs = Json::array();
            for (const LineSegment& s : p.segments)
                segs.push_back(SegmentJson(s));
            lp[n] = std::move(segs);
        }
        j["line_patterns"] = std::move(lp);
        Json ls = Json::object();
        for (const auto& [n, s] : lineStyles)
        {
            Json y{ { "pen", s.pen }, { "color", ColorJson(s.color) } };
            if (s.pattern)
                y["pattern"] = *s.pattern;
            ls[n] = std::move(y);
        }
        j["line_styles"] = std::move(ls);
        Json fp = Json::object();
        for (const auto& [n, p] : fillPatterns)
        {
            Json y{ { "target", p.model ? "model" : "drafting" } };
            if (!p.lines.empty())
            {
                Json lines = Json::array();
                for (const HatchLine& l : p.lines)
                {
                    Json o{ { "angle", l.angle }, { "spacing", l.spacing } };
                    if (l.origin != P2{})
                        o["origin"] = Bim::ToJson(l.origin);
                    if (l.shift != 0.0)
                        o["shift"] = l.shift;
                    if (!l.dashes.empty())
                        o["dashes"] = l.dashes;
                    lines.push_back(std::move(o));
                }
                y["lines"] = std::move(lines);
            }
            fp[n] = std::move(y);
        }
        j["fill_patterns"] = std::move(fp);
        Json rt = Json::object();
        for (const auto& [n, t] : regionTypes)
        {
            Json y{ { "foreground_color", ColorJson(t.foregroundColor) }, { "background_color", ColorJson(t.backgroundColor) } };
            if (t.foreground)
                y["foreground"] = *t.foreground;
            if (t.background)
                y["background"] = *t.background;
            if (t.masking)
                y["masking"] = true;
            rt[n] = std::move(y);
        }
        j["region_types"] = std::move(rt);
        Json tt = Json::object();
        for (const auto& [n, t] : textTypes)
            tt[n] = Bim::ToJson(t);
        j["text_types"] = std::move(tt);
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
        // 族还用名称时：实例参数的值按参数的 GUID 存
        m.RekeyFamilyParams(
            [&](const std::string& family, const std::string& n) -> std::optional<std::string>
            {
                const auto g = lib.GuidOf(Kind::Of(Kind::Tag::FamilyParameter), family + ": " + n);
                return g ? std::optional<std::string>(g->ToString()) : std::nullopt;
            },
            [&](const std::string& name) -> std::optional<std::string>
            {
                const auto g = lib.GuidOf(Kind::Of(Kind::Tag::Material), name);
                return g ? std::optional<std::string>(g->ToString()) : std::nullopt;
            });
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
        // 文件格式 11 的项目语言里还没移植的（BIMCraft 的瑞典语、捷克语、法语）：宽松读时按英语，记为略去
        bool otherLanguage = false;
        if (const auto it = v.find("language"); it != v.end() && it->is_string() && f.LeftOut())
            if (const std::string code = it->get<std::string>(); code == "sv" || code == "cs" || code == "fr")
            {
                otherLanguage = true;
                f.Raw("language");
                f.LeftOut()->push_back("/language");
            }
        if (!EnumName(f, "units", m.units, [](std::string_view s) { return UnitsOf::FromName(s); }, "mm, cm, m, in, ft, ft-in") ||
            (!otherLanguage && !EnumName(f, "language", m.language, [](std::string_view s) -> std::optional<Language>
            {
                for (Language l : LanguageOf::All)
                    if (s == LanguageOf::Code(l))
                        return l;
                return std::nullopt;
            }, "en, pt, es")) ||
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
                !ReadNamed(t, "column", m.types.column, false, ReadColumnType) ||
                !ReadNamed(t, "roof", m.types.roof, false, [&](Fields& g, RoofType& x) { return layered(g, x); }) ||
                !ReadNamed(t, "ceiling", m.types.ceiling, false, [&](Fields& g, CeilingType& x)
                {
                    return layered(g, x) && g.Number("grid", x.grid, false);
                }) || !ReadNamed(t, "structural_column", m.types.structuralColumn, false, ReadColumnType) ||
                !ReadNamed(t, "footing", m.types.footing, false, ReadColumnType) || !ReadRunTypes(t, m.types.runs) || !t.Finish())
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
            // 没有 transparency 的（BIMCraft 第 8 版之前的文件）读成不透明
            if (const Json* t = g.Raw("transparency"))
            {
                const auto whole = AsWhole(*t);
                if (!whole || *whole > 255)
                    return g.Fail("`transparency` must be a whole percent");
                x.transparency = static_cast<uint8_t>(*whole);
            }
            const char* patterns = "none, solid, diagonal, crosshatch, horizontal, vertical, grid, brick, concrete, insulation";
            auto parse = [](std::string_view s) { return PatternOf::FromName(s); };
            return EnumName(g, "cut", x.cut, parse, patterns) && EnumName(g, "surface", x.surface, parse, patterns);
        }))
            return false;

        // 注释的库条目；BIMCraft 第 6 版之前的文件没有线样式与线型图案：按项目的语言起名（有它们的文件按 GUID 记着图案，不会相等）
        if (!ReadStyles(f, m))
            return false;
        if (m.language != Language::English && m.lineStyles == Lines::Styles(Language::English) &&
            m.linePatterns == Lines::Patterns(Language::English))
        {
            m.lineStyles   = Lines::Styles(m.language);
            m.linePatterns = Lines::Patterns(m.language);
        }

        // 族
        if (const Json* fams = f.Raw("families"))
        {
            if (!fams->is_object())
                return f.Fail("`families` must be an object of families by name");
            for (auto it = fams->begin(); it != fams->end(); ++it)
            {
                Fields g(it.value(), "family `" + it.key() + "`", "/families/" + Escaped(it.key()), &leftOut, error);
                Family fam;
                if (!ReadFamily(g, fam))
                    return false;
                m.families[it.key()] = std::move(fam);
            }
        }

        // 元素：没有 GUID 的（旧文件）读的时候各给一个
        std::map<Id, double> noteSizes;
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
            // BIMCraft 第 8 版之前的文字注释记着字高，没有文字类型：读完后给它那个字高的类型
            std::optional<double> noteSize;
            if (bare.is_object() && bare.value("category", "") == "text")
                if (const auto s = bare.find("size"); s != bare.end())
                {
                    if (!s->is_number() || !(s->get<double>() > 0.0))
                    {
                        error = who + ": `size` must be a text height in mm";
                        return false;
                    }
                    noteSize = s->get<double>();
                    bare.erase("size");
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
            if (noteSize)
                noteSizes[eid] = *noteSize;
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

        // 注释：MiniBIM 还没有剖面、立面、图纸，在那些视图里的注释（视图不是标高的平面或平面视图）略去；宿主、参照的东西不在了的
        // 标记、高程点略去，尺寸去掉那些参照，剩下的不够量时也略去
        for (bool removed = true; removed;)
        {
            removed = false;
            std::vector<Id> gone;
            std::vector<std::pair<Id, Dimension>> trimmed;
            const auto here = [&](Guid g) { return m.elements.Contains(g); };
            for (const auto& [id, e] : m.elements.Items())
            {
                if (!e->IsAnnotation())
                    continue;
                bool keep = true;
                if (const Dimension* d = e->As<Dimension>())
                {
                    Dimension t = *d;
                    std::erase_if(t.refs, [&](const Reference& r) { return !here(r.id); });
                    keep = (!d->view || m.GetPlanView(*d->view)) && m.GetLevel(d->level) && t.refs.size() >= (d->kind == DimKind::Radial ? 1u : 2u);
                    if (keep && t.refs.size() != d->refs.size())
                        trimmed.emplace_back(id, std::move(t));
                }
                else if (const Tag* t = e->As<Tag>())
                    keep = (!t->view || m.GetPlanView(*t->view)) && here(t->host);
                else
                {
                    const auto plan = m.AnnotationPlan(*e);
                    keep = plan && m.PlanLevel(*plan);
                    if (const Spot* sp = e->As<Spot>(); keep && sp && sp->host)
                        keep = here(*sp->host);
                }
                if (!keep)
                    gone.push_back(id);
            }
            for (auto& [id, d] : trimmed)
            {
                leftOut.push_back("/elements/" + std::to_string(id) + "/refs");
                m.elements.Insert(id, std::move(d));
            }
            for (const Id id : gone)
            {
                m.elements.Remove(id);
                leftOut.push_back("/elements/" + std::to_string(id));
                removed = true;
            }
        }

        m.Migrate();
        m.NamesFromFile();
        // 没有文字类型的文字注释（第 8 版之前的文件）：给它字高的类型
        std::vector<Id> notes;
        for (const auto& [id, e] : m.elements.Items())
            if (const Note* n = e->As<Note>(); n && (n->typeName.empty() || noteSizes.count(id)))
                notes.push_back(id);
        for (const Id id : notes)
        {
            Note n = *m.elements.Get(id)->As<Note>();
            const auto s = noteSizes.find(id);
            n.typeName = TextTypes::ForSize(m.textTypes, s != noteSizes.end() ? s->second : 2.5);
            m.elements.Insert(id, std::move(n));
        }
        if (!notes.empty())
            m.SyncLibrary();
        // 参数值里不是它的族的参数、也不是体量记号的（项目参数还没移植）略去
        std::vector<Id> withParams;
        for (const auto& [id, e] : m.elements.Items())
            if (e->Params() && !e->Params()->empty())
                withParams.push_back(id);
        for (const Id id : withParams)
        {
            Element* e = m.elements.GetMut(id);
            const auto fo = e->FamilyOf();
            const auto fam = fo ? m.families.find(fo->first) : m.families.end();
            auto& params = *e->ParamsMut();
            for (auto it = params.begin(); it != params.end();)
            {
                const bool own = Massing::Reserved(it->first) ||
                                 (fam != m.families.end() && std::any_of(fam->second.parameters.begin(), fam->second.parameters.end(),
                                                                         [&](const Fam::Parameter& p) { return p.name == it->first; }));
                if (own)
                {
                    ++it;
                    continue;
                }
                leftOut.push_back("/elements/" + std::to_string(id) + "/params/" + Escaped(it->first));
                it = params.erase(it);
            }
        }
        out = std::move(m);
        return true;
    }
}
