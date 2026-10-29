// 移植自 BIMCraft crates/core/src/commands.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
//
// 命令表：脚本、界面（以后还有命令行与 AI 代理）能做的一切。参数是 JSON，长度单位毫米。命令构造元素，
// 然后由 Model::Check 检查整个模型（Session::Execute），所以命令只需要仔细读参数。
// 第 2a 步移植了项目文件、撤销、标高、轴网、墙、门窗、楼板、类型、材质与编辑（修改、移动、复制、删除、翻转），
// 其余命令按 docs/PLAN.md 第 2 阶段的顺序陆续加进来
#include "Commands/Commands.h"
#include "I18n/Translator.h"
#include "Ifc/Ifc.h"
#include "Model/ModelJson.h"
#include "Model/Text.h"
#include "Model/Walls.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace MiniBIM::Bim
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        // 这么大或更大的项目文件拒绝
        constexpr uintmax_t kMaxFile = uintmax_t(256) << 20;
        // open 最多列出这么多略去的字段（再说一共几个）
        constexpr size_t kMaxLeftOut = 100;

        std::string Bad(const char* c, const std::string& msg) { return std::string(c) + ": " + msg; }
        bool Fail(std::string& error, const char* c, const std::string& msg)
        {
            error = Bad(c, msg);
            return false;
        }

        // 报错里显示参数值：不合法的 UTF-8 换成替换字符，不抛异常
        std::string Show(const Json& v) { return v.dump(-1, ' ', false, Json::error_handler_t::replace); }
        std::string Num(double x) { return Text::Shortest(x); }

        std::filesystem::path ToPath(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

        bool Absent(const Json& p, const char* key)
        {
            if (!p.is_object())
                return true;
            const auto it = p.find(key);
            return it == p.end() || it->is_null();
        }

        const Json* Get(const Json& p, const char* key)
        {
            if (!p.is_object())
                return nullptr;
            const auto it = p.find(key);
            return it == p.end() ? nullptr : &*it;
        }

        // [x, y]，毫米：有限、在模型的范围以内
        std::optional<P2> PointOf(const Json& v)
        {
            if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number())
                return std::nullopt;
            const P2 p(v[0].get<double>(), v[1].get<double>());
            if (!p.IsFinite() || std::abs(p.x) > MaxCoord || std::abs(p.y) > MaxCoord)
                return std::nullopt;
            return p;
        }

        // 读一条命令的参数：出错时 error 写明是哪条命令、哪个参数，返回 false
        class Params
        {
        public:
            Params(const char* c, const Json& p, std::string& error) : m_c(c), m_p(p), m_error(error) {}

            const Json& Raw() const { return m_p; }
            bool Absent(const char* key) const { return Bim::Absent(m_p, key); }
            bool Fail(const std::string& msg) { return Bim::Fail(m_error, m_c, msg); }

            bool PointOpt(const char* key, std::optional<P2>& out)
            {
                out.reset();
                if (Absent(key))
                    return true;
                const Json& v = *Get(m_p, key);
                out = PointOf(v);
                return out ? true : Fail("`" + std::string(key) + "` must be a point [x, y] in mm, got " + Show(v));
            }

            bool PointReq(const char* key, P2& out)
            {
                std::optional<P2> p;
                if (!PointOpt(key, p))
                    return false;
                if (!p)
                    return Fail("`" + std::string(key) + "` [x, y] is required");
                out = *p;
                return true;
            }

            bool Points(const char* key, std::vector<P2>& out)
            {
                const Json* a = Get(m_p, key);
                if (!a || !a->is_array())
                    return Fail("`" + std::string(key) + "` [[x, y], …] is required");
                if (a->size() > MaxPoints)
                    return Fail("`" + std::string(key) + "` has more than " + std::to_string(MaxPoints) + " points");
                out.clear();
                for (const Json& v : *a)
                {
                    const auto p = PointOf(v);
                    if (!p)
                        return Fail("`" + std::string(key) + "`: " + Show(v) + " is not a point [x, y] in mm");
                    out.push_back(*p);
                }
                return true;
            }

            bool Num(const char* key, std::optional<double>& out)
            {
                out.reset();
                if (Absent(key))
                    return true;
                const Json& v = *Get(m_p, key);
                if (v.is_number())
                {
                    const double x = v.get<double>();
                    if (std::isfinite(x) && std::abs(x) <= MaxCoord)
                    {
                        out = x;
                        return true;
                    }
                }
                return Fail("`" + std::string(key) + "` must be a number in mm, got " + Show(v));
            }

            bool Flag(const char* key, bool& out)
            {
                out = false;
                if (Absent(key))
                    return true;
                const Json& v = *Get(m_p, key);
                if (!v.is_boolean())
                    return Fail("`" + std::string(key) + "` must be true or false, got " + Show(v));
                out = v.get<bool>();
                return true;
            }

            bool String(const char* key, std::optional<std::string>& out)
            {
                out.reset();
                if (Absent(key))
                    return true;
                const Json& v = *Get(m_p, key);
                if (v.is_string() && Text::CharCount(v.get_ref<const std::string&>()) <= MaxText)
                {
                    out = v.get<std::string>();
                    return true;
                }
                return Fail("`" + std::string(key) + "` must be text of at most " + std::to_string(MaxText) + " characters, got " + Show(v));
            }

            // key 给出的元素（ID 或 GUID）：给 GUID；给的是 ID 时先原样保留，由模型解析
            bool IdOpt(const char* key, std::optional<Guid>& out)
            {
                out.reset();
                if (Absent(key))
                    return true;
                const Json& v = *Get(m_p, key);
                Guid g;
                std::string why;
                if (!FromJson(v, g, why))
                    return Fail("`" + std::string(key) + "` must be an element id or GUID, got " + Show(v));
                out = g;
                return true;
            }

            bool Count(const char* key, size_t def, size_t& out)
            {
                out = def;
                if (Absent(key))
                    return true;
                const Json& v = *Get(m_p, key);
                const auto whole = AsWhole(v);
                if (!whole)
                    return Fail("`" + std::string(key) + "` must be a whole number, got " + Show(v));
                out = static_cast<size_t>(*whole);
                return true;
            }

        private:
            const char*  m_c;
            const Json&  m_p;
            std::string& m_error;
        };

        // 模型里的元素，按 ID 或 GUID
        bool ElementId(const char* c, const Model& m, const Json& v, Id& out, std::string& error)
        {
            Guid g;
            std::string why;
            if (!FromJson(v, g, why))
                return Fail(error, c, Show(v) + " is not an element id or GUID");
            const auto id = m.elements.IdOf(g);
            if (!id)
                return Fail(error, c, "no element " + g.ToString());
            out = *id;
            return true;
        }

        // ids [id, …] 或一个 id：模型的元素，按 ID 或 GUID，各一次，按给出的顺序
        bool IdsParam(const char* c, const Model& m, const Json& p, std::vector<Id>& out, std::string& error)
        {
            out.clear();
            const Json* ids = Get(p, "ids");
            const Json* one = Get(p, "id");
            std::vector<Id> given;
            if (ids && ids->is_array())
            {
                if (ids->size() > MaxPoints)
                    return Fail(error, c, "`ids` lists more than " + std::to_string(MaxPoints) + " elements");
                for (const Json& v : *ids)
                {
                    Id id = 0;
                    if (!ElementId(c, m, v, id, error))
                        return false;
                    given.push_back(id);
                }
            }
            else if ((!ids || ids->is_null()) && one)
            {
                Id id = 0;
                if (!ElementId(c, m, *one, id, error))
                    return false;
                given.push_back(id);
            }
            else
                return Fail(error, c, "`ids` [id, …] is required");
            std::set<Id> seen;
            for (const Id id : given)
                if (seen.insert(id).second)
                    out.push_back(id);
            return true;
        }

        // key 指的标高（名称、ID 或 GUID），给它的 GUID；没给时为最低的标高
        bool LevelParam(const char* c, const Model& m, const Json& p, const char* key, Guid& out, std::string& error)
        {
            const auto levels = m.Levels();
            Id id = 0;
            const Json* v = Get(p, key);
            if (!v || v->is_null())
            {
                if (levels.empty())
                    return Fail(error, c, "the model has no levels");
                id = levels.front().first;
            }
            else if (v->is_string() && !Guid::Parse(v->get<std::string>()))
            {
                const std::string n = v->get<std::string>();
                const auto it = std::find_if(levels.begin(), levels.end(), [&](const auto& l) { return l.second->name == n; });
                if (it == levels.end())
                {
                    std::string names;
                    for (const auto& [_, l] : levels)
                        names += (names.empty() ? "" : ", ") + l->name;
                    return Fail(error, c, "no level or plan view named `" + n + "` (the levels are: " + names + ")");
                }
                id = it->first;
            }
            else
            {
                Guid g;
                std::string why;
                const auto found = FromJson(*v, g, why) ? m.elements.IdOf(g) : std::nullopt;
                if (!found || !m.GetLevel(*found))
                    return Fail(error, c, "`" + std::string(key) + "`: " + Show(*v) + " is not a level or a plan view");
                id = *found;
            }
            out = m.GuidOf(id);
            return true;
        }

        // type，没给时为类别的默认类型（Model::DefaultType）。不存在的名称由 Model::Check 报出并列出已有的
        bool TypeParam(Params& P, const Model& m, const char* category, std::string& out)
        {
            std::optional<std::string> n;
            if (!P.String("type", n))
                return false;
            if (n)
            {
                out = *n;
                return true;
            }
            const auto d = m.DefaultType(category);
            if (!d)
                return P.Fail("the model has no types for this");
            out = *d;
            return true;
        }

        double Hundredth(double x) { return std::round(x * 100.0) / 100.0; }
        // 平方毫米换成平方米，到百分位
        double M2(double mm2) { return std::round(mm2 / 1e4) / 100.0; }

        std::string LevelName(const Model& m, Guid id)
        {
            const Level* l = m.GetLevel(id);
            return l ? l->name : "level " + m.Shown(id);
        }

        // 点在墙定位线上离起点多远
        double Along(const Wall& w, P2 p) { return Walls::Axis::Of(w).Project(p).first; }
        // 点到墙中心线的距离
        double OffCentre(const Model& m, const Wall& w, P2 p) { return Walls::Axis::Of(w).Distance(p, Walls::Centre(m, w)); }

        // level 的平面剖切到的、离 at 最近的墙（在中心线两侧半个墙厚再加 100 毫米以内），以及 at 沿它多远
        std::optional<std::pair<Guid, double>> WallAt(const Model& m, Guid level, P2 at)
        {
            std::optional<std::tuple<Guid, const Wall*, double>> best;
            for (const auto& [id, e] : m.elements.Items())
            {
                const Wall* w = e->As<Wall>();
                if (!w || !m.CutBy(*w, level))
                    continue;
                const double d = OffCentre(m, *w, at);
                if (d > m.Thickness(*w) / 2.0 + 100.0)
                    continue;
                if (!best || d < std::get<2>(*best))
                    best = std::tuple{ m.GuidOf(id), w, d };
            }
            if (!best)
                return std::nullopt;
            return std::pair{ std::get<0>(*best), Along(*std::get<1>(*best), at) };
        }


        // 给出的环：去掉连续重复的点和末尾回到起点的点
        Ring CleanRing(const std::vector<P2>& given)
        {
            Ring r;
            for (const P2& q : given)
                if (r.empty() || r.back().Dist(q) > Tol)
                    r.push_back(q);
            while (r.size() > 1 && r.front().Dist(r.back()) <= Tol)
                r.pop_back();
            return r;
        }

        // key 下给出的环（楼板的洞口），各按 CleanRing 清理
        bool Rings(Params& P, const char* key, std::optional<std::vector<Ring>>& out)
        {
            out.reset();
            if (P.Absent(key))
                return true;
            const Json& v = *Get(P.Raw(), key);
            if (!v.is_array())
                return P.Fail("`" + std::string(key) + "` must be a list of outlines [[[x,y],…],…]");
            std::vector<Ring> rings;
            for (const Json& r : v)
            {
                if (!r.is_array())
                    return P.Fail("`" + std::string(key) + "` must be a list of outlines [[[x,y],…],…]");
                std::vector<P2> pts;
                for (const Json& q : r)
                {
                    P2 p;
                    std::string why;
                    if (!FromJson(q, p, why))
                        return P.Fail("`" + std::string(key) + "` must be a list of outlines [[[x,y],…],…]");
                    pts.push_back(p);
                }
                rings.push_back(CleanRing(pts));
            }
            out = std::move(rings);
            return true;
        }

        // 照 Revit 给轴网起名：上一个轴网的字母的下一个，否则下一个数
        std::string NextGridName(const Model& m)
        {
            std::vector<std::string> names;
            for (const auto& [_, e] : m.elements.Items())
                if (const Grid* g = e->As<Grid>())
                    names.push_back(g->name);
            if (!names.empty() && names.back().size() == 1)
            {
                const char ch = names.back()[0];
                if (ch >= 'A' && ch < 'Z')
                {
                    const std::string next(1, static_cast<char>(ch + 1));
                    if (std::find(names.begin(), names.end(), next) == names.end())
                        return next;
                }
            }
            return NextNumber(names);
        }

        std::vector<std::string> Marks(const Model& m, bool doors)
        {
            std::vector<std::string> out;
            for (const auto& [_, e] : m.elements.Items())
                if ((doors && e->IsDoor()) || (!doors && e->IsWindow()))
                    out.push_back(e->AsOpening()->mark);
            return out;
        }

        // 读整个文件（UTF-8 路径）
        bool ReadFile(const std::string& path, std::string& out, std::string& error)
        {
            std::error_code ec;
            const uintmax_t size = std::filesystem::file_size(ToPath(path), ec);
            if (ec)
            {
                error = path + ": " + ec.message();
                return false;
            }
            if (size >= kMaxFile)
            {
                error = path + " is too large (" + std::to_string(size) + " bytes)";
                return false;
            }
            std::ifstream in(ToPath(path), std::ios::binary);
            if (!in)
            {
                error = path + ": can't be read";
                return false;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        }

        // 原子地写文件：先写旁边的临时文件，再换过去，写到一半失败时原来的文件还在
        bool AtomicWrite(const std::string& path, const std::string& text, std::string& error)
        {
            const std::filesystem::path target = ToPath(path);
            std::filesystem::path temp = target;
            temp += ".tmp";
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out << text;
                out.flush();
                if (!out)
                {
                    error = path + ": can't be written";
                    std::error_code ec;
                    std::filesystem::remove(temp, ec);
                    return false;
                }
            }
            std::error_code ec;
            std::filesystem::rename(temp, target, ec);
            if (ec)
            {
                error = path + ": " + ec.message();
                std::filesystem::remove(temp, ec);
                return false;
            }
            return true;
        }

        std::string Extension(const std::string& path)
        {
            const std::u8string e = ToPath(path).extension().u8string();
            std::string ext(e.begin(), e.end());
            if (!ext.empty() && ext.front() == '.')
                ext.erase(0, 1);
            return Text::AsciiLower(ext);
        }

        // ── 文件与历史 ───────────────────────────────────────────
        bool New(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "new";
            Params P(c, p, error);
            std::optional<std::string> code, name;
            if (!P.String("language", code) || !P.String("name", name))
                return false;
            Language language = Language::English;
            if (code)
            {
                const auto l = LanguageOf::FromCode(*code);
                if (!l)
                    return P.Fail("`" + *code + "` is no language here; there are en (English), pt (Português) and es (Español)");
                language = *l;
            }
            const std::string n = name.value_or(LanguageOf::Word(language, "Project"));
            s.Reset(Model::New(n, language), std::nullopt);
            result = Json{ { "name", n }, { "language", LanguageOf::Code(language) } };
            return true;
        }

        bool Open(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "open";
            Params P(c, p, error);
            std::optional<std::string> path;
            if (!P.String("path", path))
                return false;
            if (!path)
                return P.Fail("`path` is required");
            std::string text, why;
            // IFC 作为新项目进来：保存时再问存到哪里
            if (Extension(*path) == "ifc")
            {
                if (!ReadFile(*path, text, why))
                    return P.Fail(why);
                const std::u8string stem = ToPath(*path).stem().u8string();
                Ifc::Imported i;
                if (!Ifc::Import(text, stem.empty() ? std::string("Project") : std::string(stem.begin(), stem.end()), i, why))
                    return P.Fail(*path + ": " + why);
                std::map<std::string, size_t> counts;
                for (const auto& [_, e] : i.model.elements.Items())
                    ++counts[e->Category()];
                Json elements = Json::object();
                for (const auto& [category, n] : counts)
                    elements[category] = n;
                result = Json{ { "path", *path },
                               { "name", i.model.name },
                               { "format", i.schema },
                               { "elements", std::move(elements) },
                               { "skipped", i.skipped },
                               { "moved", i.moved ? ToJson(*i.moved) : Json() } };
                s.Reset(std::move(i.model), std::nullopt);
                return true;
            }
            if (!ReadFile(*path, text, why))
                return P.Fail(why);
            Model m;
            std::vector<std::string> leftOut;
            if (!Model::Read(text, m, leftOut, why))
                return P.Fail(*path + " is not a project file: " + why);
            m.AdoptMaterials();
            m.SyncLibrary();
            if (!m.Check(why))
                return P.Fail(*path + ": " + why);
            const std::string name = m.name;
            s.Reset(std::move(m), *path);
            result = Json{ { "path", *path }, { "name", name } };
            if (!leftOut.empty())
            {
                // 恶意的文件可能列出上百万个键：结果只列前面一些，再说一共几个
                Json list = Json::array();
                for (size_t i = 0; i < leftOut.size() && i < kMaxLeftOut; ++i)
                    list.push_back(leftOut[i]);
                result["left_out"]       = std::move(list);
                result["left_out_count"] = leftOut.size();
            }
            return true;
        }

        bool Save(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "save";
            Params P(c, p, error);
            std::optional<std::string> given;
            if (!P.String("path", given))
                return false;
            if (!given && !s.Path())
                return P.Fail("`path` is required: this project hasn't been saved yet");
            const std::string path = given ? *given : *s.Path();
            const std::string ext = Extension(path);
            std::string text, why;
            if (ext == "ifc")
            {
                if (!AtomicWrite(path, Ifc::Export(s.GetModel()), why))
                    return P.Fail(why);
                result = Json{ { "path", path }, { "format", "IFC4" } };
                return true;
            }
            if (ext == "pdf" || ext == "svg" || ext == "png")
                return P.Fail("exporting ." + ext + " isn't ported yet (docs/PLAN.md, phase 2)");
            if (!s.GetModel().ToFile(text, why) || !AtomicWrite(path, text, why))
                return P.Fail(why);
            s.SetPath(path);
            result = Json{ { "path", path }, { "format", "project" } };
            return true;
        }

        // undo、redo 走几步：count，默认 1
        bool StepsParam(const char* c, const Json& p, size_t& out, std::string& error)
        {
            if (p.is_object())
                for (auto it = p.begin(); it != p.end(); ++it)
                    if (it.key() != "count")
                        return Fail(error, c, "unknown parameter `" + it.key() + "`");
            Params P(c, p, error);
            return P.Count("count", 1, out);
        }

        bool Undo(Session& s, const Json& p, Json& result, std::string& error)
        {
            size_t n = 1;
            return StepsParam("undo", p, n, error) && s.StepBack(false, n, result, error);
        }

        bool Redo(Session& s, const Json& p, Json& result, std::string& error)
        {
            size_t n = 1;
            return StepsParam("redo", p, n, error) && s.StepBack(true, n, result, error);
        }

        // ── 建模 ─────────────────────────────────────────────────
        bool LevelCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "level";
            Params P(c, p, error);
            std::optional<double> elevation;
            std::optional<std::string> name;
            if (!P.Num("elevation", elevation) || !P.String("name", name))
                return false;
            if (!elevation)
                return P.Fail("`elevation` in mm is required");
            const Model& m = s.GetModel();
            std::set<std::string> taken;
            for (const auto& [_, l] : m.Levels())
                taken.insert(l->name);
            if (!name)
            {
                const std::string word = LanguageOf::Word(m.language, "Level");
                for (size_t n = taken.size() + 1;; ++n)
                    if (!taken.count(word + " " + std::to_string(n)))
                    {
                        name = word + " " + std::to_string(n);
                        break;
                    }
            }
            std::optional<std::array<P2, 2>> extent;
            if (!P.Absent("extent"))
            {
                std::vector<P2> pts;
                if (!P.Points("extent", pts))
                    return false;
                if (pts.size() != 2)
                    return P.Fail("`extent` is two corners, [[x, y], [x, y]]: south-west, then north-east");
                extent = std::array<P2, 2>{ pts[0], pts[1] };
            }
            Level l;
            l.name      = *name;
            l.elevation = *elevation;
            l.extent    = extent;
            const Id id = s.ModelMut().Insert(std::move(l));
            result = Json{ { "id", id }, { "name", *name } };
            return true;
        }

        bool GridCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "grid";
            Params P(c, p, error);
            P2 start, end;
            std::optional<std::string> name;
            if (!P.PointReq("start", start) || !P.PointReq("end", end) || !P.String("name", name))
                return false;
            const std::string n = name ? *name : NextGridName(s.GetModel());
            const Id id = s.ModelMut().Insert(Grid{ n, start, end });
            result = Json{ { "id", id }, { "name", n } };
            return true;
        }

        std::string LocationNames(const std::string& n)
        {
            std::string names;
            for (Location l : LocationOf::All)
                names += (names.empty() ? "" : ", ") + std::string(LocationOf::Name(l));
            return "`location` is one of " + names + ", not `" + n + "`";
        }

        bool WallCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "wall";
            Params P(c, p, error);
            std::vector<P2> pts;
            if (!P.Points("points", pts))
                return false;
            if (pts.size() < 2)
                return P.Fail("`points` needs at least 2 points");
            bool closed = false;
            if (!P.Flag("closed", closed))
                return false;
            if (closed)
                pts.push_back(pts.front());
            const Model& m = s.GetModel();
            Guid level;
            std::string typeName;
            std::optional<double> height, baseOffset, arc;
            if (!LevelParam(c, m, p, "level", level, error) || !TypeParam(P, m, "wall", typeName) || !P.Num("height", height))
                return false;
            std::optional<Guid> topLevel;
            if (P.Absent("top_level"))
            {
                if (!height)
                    topLevel = m.LevelAbove(level);
            }
            else
            {
                Guid t;
                if (!LevelParam(c, m, p, "top_level", t, error))
                    return false;
                topLevel = t;
            }
            std::optional<std::string> location;
            std::optional<Guid> attachTop, attachBase;
            if (!P.Num("base_offset", baseOffset) || !P.Num("arc", arc) || !P.String("location", location) ||
                !P.IdOpt("attach_top", attachTop) || !P.IdOpt("attach_base", attachBase))
                return false;
            Location loc = Location::Centre;
            if (location)
            {
                const auto l = LocationOf::FromName(*location);
                if (!l)
                    return P.Fail(LocationNames(*location));
                loc = *l;
            }
            Model& mm = s.ModelMut();
            Json ids = Json::array();
            for (size_t i = 0; i + 1 < pts.size(); ++i)
            {
                Wall w;
                w.typeName   = typeName;
                w.level      = level;
                w.start      = pts[i];
                w.end        = pts[i + 1];
                w.topLevel   = topLevel;
                w.height     = height.value_or(DefaultHeight);
                w.baseOffset = baseOffset.value_or(0.0);
                w.arc        = arc.value_or(0.0);
                w.location   = loc;
                w.attachTop  = attachTop;
                w.attachBase = attachBase;
                ids.push_back(mm.Insert(std::move(w)));
            }
            result = Json{ { "ids", ids } };
            return true;
        }

        // 墙绕定位线翻过来：反向，外侧到了另一边；门窗留在原处，开向与铰链照旧
        bool Flip(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "flip";
            const Model& m0 = s.GetModel();
            std::vector<Id> ids;
            if (!IdsParam(c, m0, p, ids, error))
                return false;
            for (const Id id : ids)
                if (!m0.GetWall(id))
                    return Fail(error, c, std::string(m0.elements.Get(id)->Category()) + " " + std::to_string(id) + " isn't a wall");
            std::map<Guid, double> lengths;
            for (const Id id : ids)
                lengths[m0.GuidOf(id)] = Walls::Axis::Of(*m0.GetWall(id)).Length();
            std::vector<Id> openings;
            for (const auto& [id, e] : m0.elements.Items())
                if (const Opening* o = e->AsOpening(); o && lengths.count(o->host))
                    openings.push_back(id);
            Model& m = s.ModelMut();
            for (const Id id : ids)
            {
                Wall* w = m.elements.GetMut(id)->As<Wall>();
                std::swap(w->start, w->end);
                w->arc = -w->arc + 0.0;
            }
            for (const Id id : openings)
            {
                Opening* o = m.elements.GetMut(id)->AsOpening();
                o->offset     = lengths[o->host] - o->offset;
                o->flipFacing = !o->flipFacing;
                o->flipHand   = !o->flipHand;
            }
            result = Json{ { "flipped", ids } };
            return true;
        }

        // p 要的门或窗，还没放进模型：door、window 命令放进去的
        bool BuildOpening(const Model& m, const Json& p, bool isDoor, Opening& o, std::string& error)
        {
            const char* c = isDoor ? "door" : "window";
            Params P(c, p, error);
            // 族的类型，或项目自己的类型
            std::optional<std::string> family;
            if (!P.String("family", family))
                return false;
            std::string typeName;
            std::optional<double> familySill;
            if (family)
            {
                const Fam::Category kind = isDoor ? Fam::Category::Door : Fam::Category::Window;
                const auto f = m.families.find(*family);
                if (f == m.families.end() || f->second.category != kind)
                {
                    std::string names;
                    for (const auto& [n, x] : m.families)
                        if (x.category == kind)
                            names += (names.empty() ? "" : ", ") + n;
                    return P.Fail("there's no " + std::string(c) + " family `" + *family + "` (there are: " + names + ")");
                }
                std::optional<std::string> t;
                if (!P.String("type", t))
                    return false;
                if (t)
                    typeName = *t;
                else if (!f->second.types.empty())
                    typeName = f->second.types.begin()->first;
                else
                    return P.Fail("family `" + *family + "` has no type");
                Fam::Values values;
                std::string why;
                if (!f->second.Values(typeName, {}, values, why))
                    return P.Fail(why);
                if (const auto v = values.find(Fam::Sill); v != values.end())
                    if (const double* x = std::get_if<double>(&v->second))
                        familySill = *x;
            }
            else if (!TypeParam(P, m, c, typeName))
                return false;
            std::optional<P2> at;
            std::optional<Guid> wall;
            if (!P.PointOpt("at", at) || !P.IdOpt("wall", wall))
                return false;
            Guid host;
            double alongAt = 0.0;
            if (wall)
            {
                const Wall* w = m.GetWall(*wall);
                if (!w)
                    return P.Fail(wall->ToString() + " is not a wall");
                host    = m.GuidOf(*wall);
                alongAt = at ? Along(*w, *at) : w->start.Dist(w->end) / 2.0;
            }
            else
            {
                if (!at)
                    return P.Fail("give `at` [x, y] on a wall, or `wall` with `offset`");
                Guid level;
                if (!LevelParam(c, m, p, "level", level, error))
                    return false;
                const auto found = WallAt(m, level, *at);
                if (!found)
                    return P.Fail("no wall at [" + Num(at->x) + ", " + Num(at->y) + "] on " + LevelName(m, level));
                host    = found->first;
                alongAt = found->second;
            }
            std::optional<double> offset, sill;
            std::optional<std::string> mark;
            if (!P.Num("offset", offset) || !P.Num("sill", sill) || !P.String("mark", mark))
                return false;
            const auto& types = isDoor ? m.types.door : m.types.window;
            const auto t = types.find(typeName);
            const double defaultSill = familySill ? *familySill : (family ? (isDoor ? 0.0 : 900.0) : (t == types.end() ? 0.0 : t->second.sill));
            o.family   = family;
            o.typeName = typeName;
            o.host     = host;
            o.offset   = offset.value_or(alongAt);
            o.sill     = isDoor ? 0.0 : sill.value_or(defaultSill);
            o.mark     = mark ? *mark : NextNumber(Marks(m, isDoor));
            return P.Flag("flip_facing", o.flipFacing) && P.Flag("flip_hand", o.flipHand);
        }

        bool OpeningCmd(Session& s, const Json& p, Json& result, std::string& error, bool isDoor)
        {
            Opening o;
            if (!BuildOpening(s.GetModel(), p, isDoor, o, error))
                return false;
            const auto host = s.GetModel().elements.IdOf(o.host);
            const double offset = o.offset;
            const std::string mark = o.mark;
            const Id id = isDoor ? s.ModelMut().Insert(Door{ std::move(o) }) : s.ModelMut().Insert(Window{ std::move(o) });
            result = Json{ { "id", id }, { "host", host ? Json(*host) : Json(nullptr) }, { "offset", offset }, { "mark", mark } };
            return true;
        }

        bool DoorCmd(Session& s, const Json& p, Json& result, std::string& error) { return OpeningCmd(s, p, result, error, true); }
        bool WindowCmd(Session& s, const Json& p, Json& result, std::string& error) { return OpeningCmd(s, p, result, error, false); }

        bool FloorCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "floor";
            Params P(c, p, error);
            const Model& m = s.GetModel();
            Guid level;
            std::string typeName;
            if (!LevelParam(c, m, p, "level", level, error) || !TypeParam(P, m, "floor", typeName))
                return false;
            if (!P.Absent("sketch"))
                return P.Fail("`sketch` isn't ported yet (docs/PLAN.md, phase 2); give `boundary`");
            Ring boundary;
            std::vector<Ring> found;
            if (P.Absent("boundary"))
            {
                const auto outline = FloorOutline(m, level);
                if (!outline)
                    return P.Fail("no walls enclose an area on " + LevelName(m, level) + "; give `boundary`");
                boundary = outline->first;
                found    = outline->second;
            }
            else
            {
                std::vector<P2> pts;
                if (!P.Points("boundary", pts))
                    return false;
                boundary = CleanRing(pts);
            }
            std::optional<std::vector<Ring>> openings;
            if (!Rings(P, "openings", openings))
                return false;
            if (Geom::Area(boundary) < 0.0)
                std::reverse(boundary.begin(), boundary.end());
            std::optional<std::array<P2, 2>> arrow;
            if (!P.Absent("arrow"))
            {
                std::vector<P2> pts;
                if (!P.Points("arrow", pts))
                    return false;
                if (pts.size() != 2)
                    return P.Fail("`arrow` is [[x, y], [x, y]]: its tail, then its head");
                arrow = std::array<P2, 2>{ pts[0], pts[1] };
            }
            std::optional<double> offset, slope;
            bool structural = false;
            if (!P.Num("offset", offset) || !P.Num("slope", slope) || !P.Flag("structural", structural))
                return false;
            Floor f;
            f.typeName   = typeName;
            f.level      = level;
            f.boundary   = boundary;
            f.openings   = openings ? *openings : found;
            f.offset     = offset.value_or(0.0);
            f.slope      = slope.value_or(0.0);
            f.arrow      = arrow;
            f.structural = structural;
            // 洞口在它里面、互不相交，否则模型拒绝
            double area = Geom::Area(f.boundary);
            for (const Ring& o : f.openings)
                area -= std::abs(Geom::Area(o));
            const Id id = s.ModelMut().Insert(std::move(f));
            result = Json{ { "id", id }, { "area_m2", M2(area) } };
            return true;
        }

        bool CeilingCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "ceiling";
            Params P(c, p, error);
            const Model& m = s.GetModel();
            Guid level;
            std::string typeName;
            if (!LevelParam(c, m, p, "level", level, error) || !TypeParam(P, m, "ceiling", typeName))
                return false;
            if (!P.Absent("sketch"))
                return P.Fail("`sketch` isn't ported yet (docs/PLAN.md, phase 2); give `point` or `boundary`");
            std::optional<P2> at;
            if (!P.PointOpt("point", at))
                return false;
            Ring boundary;
            std::vector<Ring> found;
            if (!P.Absent("boundary"))
            {
                std::vector<P2> pts;
                if (!P.Points("boundary", pts))
                    return false;
                boundary = CleanRing(pts);
            }
            else if (at)
            {
                // 房间的空地：墙面围着这一点的部分，以及立在其中的墙（顺时针，同楼板的洞口）
                const auto r = Geom::RegionAt(m.FaceLines(level), *at, Tol);
                if (!r)
                    return P.Fail("no walls on " + LevelName(m, level) + " close around [" + Num(at->x) + ", " + Num(at->y) + "]; give `boundary`");
                for (const P2& q : Geom::Corners(r->outer))
                    boundary.push_back(Micron(q));
                for (const Ring& h : r->holes)
                    found.push_back(Tidy(Ring(h.rbegin(), h.rend())));
            }
            else
                return P.Fail("`point` [x, y] in a room, `boundary` [[x, y], …] or `sketch` is required");
            if (Geom::Area(boundary) < 0.0)
                std::reverse(boundary.begin(), boundary.end());
            std::optional<std::vector<Ring>> openings;
            std::optional<double> offset;
            if (!Rings(P, "openings", openings) || !P.Num("offset", offset))
                return false;
            Ceiling x;
            x.typeName = typeName;
            x.level    = level;
            x.boundary = boundary;
            x.openings = openings ? *openings : found;
            x.offset   = offset.value_or(CeilingHeight);
            // 洞口在它里面、互不相交，否则模型拒绝
            double area = Geom::Area(x.boundary);
            for (const Ring& o : x.openings)
                area -= std::abs(Geom::Area(o));
            const Id id = s.ModelMut().Insert(std::move(x));
            result = Json{ { "id", id }, { "area_m2", M2(area) } };
            return true;
        }

        bool RoomCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "room";
            Params P(c, p, error);
            const Model& m = s.GetModel();
            P2 point;
            Guid level;
            if (!P.PointReq("point", point) || !LevelParam(c, m, p, "level", level, error))
                return false;
            std::optional<std::string> name, number;
            if (!P.String("name", name) || !P.String("number", number))
                return false;
            Room r;
            r.level = level;
            r.point = point;
            r.name  = name ? *name : LanguageOf::Word(m.language, "Room");
            if (number)
                r.number = *number;
            else
            {
                std::vector<std::string> taken;
                for (const auto& [_, e] : m.elements.Items())
                    if (const Room* x = e->As<Room>())
                        taken.push_back(x->number);
                r.number = NextNumber(taken);
            }
            const auto region = m.RoomRegion(r);
            if (!region)
                return P.Fail("[" + Num(point.x) + ", " + Num(point.y) + "] isn't enclosed by walls on " + LevelName(m, level) +
                              "; place rooms inside a closed loop of walls");
            const double area = M2(region->Area());
            const std::string n = r.number;
            const Id id = s.ModelMut().Insert(std::move(r));
            result = Json{ { "id", id }, { "number", n }, { "area_m2", area } };
            return true;
        }

        bool RoomSeparatorCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "room_separator";
            Params P(c, p, error);
            std::vector<P2> pts;
            if (!P.Points("points", pts))
                return false;
            if (pts.size() < 2)
                return P.Fail("`points` needs at least 2 points");
            bool closed = false;
            if (!P.Flag("closed", closed))
                return false;
            if (closed)
                pts.push_back(pts.front());
            Guid level;
            if (!LevelParam(c, s.GetModel(), p, "level", level, error))
                return false;
            Model& m = s.ModelMut();
            Json ids = Json::array();
            for (size_t i = 0; i + 1 < pts.size(); ++i)
                ids.push_back(m.Insert(RoomSeparator{ level, pts[i], pts[i + 1] }));
            result = Json{ { "ids", std::move(ids) } };
            return true;
        }

        // extrusion：{line: [[x,y],[x,y]], profile: [[along, up],…], from?, to}
        bool ExtrusionParam(Params& P, std::optional<Extrusion>& out)
        {
            out.reset();
            if (P.Absent("extrusion"))
                return true;
            const Json& v = *Get(P.Raw(), "extrusion");
            const std::string shape = "`extrusion` is {line: [[x,y],[x,y]], profile: [[along, up],…], from?, to}: ";
            if (!v.is_object())
                return P.Fail(shape + "got " + Show(v));
            for (auto it = v.begin(); it != v.end(); ++it)
                if (it.key() != "line" && it.key() != "profile" && it.key() != "from" && it.key() != "to")
                    return P.Fail(shape + "unknown field `" + it.key() + "`");
            std::string why;
            Params X("roof", v, why);
            std::vector<P2> line;
            Extrusion x;
            std::optional<double> from, to;
            if (!X.Points("line", line) || !X.Points("profile", x.profile) || !X.Num("from", from) || !X.Num("to", to))
                return P.Fail(shape + why.substr(std::string("roof: ").size()));
            if (line.size() != 2)
                return P.Fail(shape + "`line` is two points [[x,y],[x,y]]");
            if (!to)
                return P.Fail(shape + "missing field `to`");
            x.line = { line[0], line[1] };
            x.from = from.value_or(0.0);
            x.to   = *to;
            out = std::move(x);
            return true;
        }

        // gables：边的编号
        bool GablesParam(Params& P, std::vector<size_t>& out)
        {
            out.clear();
            if (P.Absent("gables"))
                return true;
            const Json& v = *Get(P.Raw(), "gables");
            if (!v.is_array())
                return P.Fail("`gables` is a list of edge numbers: got " + Show(v));
            if (v.size() > MaxPoints)
                return P.Fail("`gables` lists more than " + std::to_string(MaxPoints));
            for (const Json& n : v)
            {
                const auto k = AsWhole(n);
                if (!k)
                    return P.Fail("`gables` is a list of edge numbers: got " + Show(n));
                out.push_back(static_cast<size_t>(*k));
            }
            return true;
        }

        bool RoofCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "roof";
            Params P(c, p, error);
            const Model& m = s.GetModel();
            Guid level;
            if (P.Absent("level"))
            {
                const auto levels = m.Levels();
                if (levels.empty())
                    return P.Fail("the model has no levels");
                level = m.GuidOf(levels.size() > 1 ? levels[1].first : levels[0].first);
            }
            else if (!LevelParam(c, m, p, "level", level, error))
                return false;
            std::string typeName;
            std::optional<Extrusion> extrusion;
            if (!TypeParam(P, m, "roof", typeName) || !ExtrusionParam(P, extrusion))
                return false;
            if (!P.Absent("sketch"))
                return P.Fail("`sketch` isn't ported yet (docs/PLAN.md, phase 2); give `boundary`");
            const bool given = !P.Absent("boundary");
            std::optional<double> overhang;
            if (!P.Num("overhang", overhang))
                return false;
            Roof r;
            r.overhang = overhang.value_or(given || extrusion ? 0.0 : 500.0);
            std::vector<Ring> found;
            if (given)
            {
                std::vector<P2> pts;
                if (!P.Points("boundary", pts))
                    return false;
                r.boundary = CleanRing(pts);
            }
            else if (!extrusion)
            {
                std::string why;
                const auto footprint = RoofFootprint(m, level, r.overhang, why);
                if (!footprint)
                    return P.Fail(why);
                r.boundary = footprint->first;
                found      = footprint->second;
            }
            std::optional<std::vector<Ring>> courtyards, openings;
            std::optional<double> slope, offset;
            if (!Rings(P, "courtyards", courtyards))
                return false;
            if (!extrusion && !P.Num("slope", slope))
                return false;
            if (!P.Flag("gable", r.gable) || !GablesParam(P, r.gables) || !P.Num("offset", offset) || !Rings(P, "openings", openings) ||
                !P.IdOpt("join", r.join))
                return false;
            r.typeName   = typeName;
            r.level      = level;
            r.courtyards = courtyards ? *courtyards : found;
            r.slope      = extrusion ? 0.0 : slope.value_or(30.0);
            r.offset     = offset.value_or(0.0);
            r.openings   = openings ? *openings : std::vector<Ring>{};
            r.extrusion  = std::move(extrusion);
            const Id id = s.ModelMut().Insert(std::move(r));
            result = Json{ { "id", id } };
            return true;
        }

        // ── 族与构件 ─────────────────────────────────────────────
        void Merge(Json& into, const Json& patch);

        // old 里同一位置的部件把 ID 和 GUID 交给 fresh 里自己没有的：原地不动的保留它们
        template <class T>
        void KeepParts(const std::vector<T>& old, std::vector<T>& fresh)
        {
            for (size_t i = 0; i < old.size() && i < fresh.size(); ++i)
                if (fresh[i].id == 0 && fresh[i].guid.IsNil())
                {
                    fresh[i].id   = old[i].id;
                    fresh[i].guid = old[i].guid;
                }
        }

        bool FamilyCmd(Session& s, const Json& given, Json& result, std::string& error)
        {
            const char* c = "family";
            Json p = given;
            // 从文件读：文件里的字段，名称取给出的、文件自己的，或文件名
            {
                Params P(c, p, error);
                std::optional<std::string> path;
                if (!P.String("path", path))
                    return false;
                if (path)
                {
                    std::string text, why;
                    if (!ReadFile(*path, text, why))
                        return P.Fail(why);
                    Json loaded = Json::parse(text, nullptr, false);
                    if (loaded.is_discarded())
                        return P.Fail(*path + " is not a family: it isn't JSON");
                    if (!loaded.is_object())
                        return P.Fail(*path + " is not a family: it isn't an object");
                    const std::u8string stem = ToPath(*path).stem().u8string();
                    Json name = !Absent(p, "name") ? p["name"] : (loaded.contains("name") ? loaded["name"] : Json(std::string(stem.begin(), stem.end())));
                    // 只取它带的族：文件说要改名或删除不算数
                    Json fam = Json::object();
                    for (const char* k : { "category", "parameters", "types", "forms", "symbols", "operation" })
                        if (loaded.contains(k))
                            fam[k] = loaded[k];
                    fam["name"] = name;
                    p = std::move(fam);
                }
            }
            Params P(c, p, error);
            std::optional<std::string> nameGiven;
            if (!P.String("name", nameGiven))
                return false;
            const std::string name = nameGiven ? std::string(Text::Trim(*nameGiven)) : std::string();
            if (name.empty())
                return P.Fail("`name` is required");
            // 项目的族存成文件：path 能把它读回来
            std::optional<std::string> saveTo;
            if (!P.String("save", saveTo))
                return false;
            if (saveTo)
            {
                const auto f = s.GetModel().families.find(name);
                if (f == s.GetModel().families.end())
                    return P.Fail("there's no family `" + name + "` to save");
                Json v = ToJson(f->second);
                v["name"] = name;
                std::string why;
                if (!AtomicWrite(*saveTo, v.dump(2), why))
                    return P.Fail(*saveTo + ": " + why);
                result = Json{ { "saved", name }, { "path", *saveTo } };
                return true;
            }
            Model& m = s.ModelMut();
            const auto users = [&](const std::string& n)
            {
                std::vector<Id> out;
                for (const auto& [id, e] : m.elements.Items())
                    if (const auto fo = e->FamilyOf(); fo && fo->first == n)
                        out.push_back(id);
                return out;
            };
            bool del = false;
            if (!P.Flag("delete", del))
                return false;
            if (del)
            {
                if (!m.families.count(name))
                    return P.Fail("there's no family `" + name + "`");
                const auto used = users(name);
                if (!used.empty())
                {
                    std::string ids;
                    for (const Id id : used)
                        ids += (ids.empty() ? "" : ", ") + std::to_string(id);
                    return P.Fail("elements " + ids + " are of `" + name + "`; delete them first");
                }
                m.families.erase(name);
                result = Json{ { "deleted", name } };
                return true;
            }
            Family f = m.families.count(name) ? m.families.at(name) : Family{};
            // 给出的每个字段按族读：读不懂的说是哪个字段
            const auto part = [&](const char* key, Family& out) -> bool
            {
                Json one = Json::object();
                one[key] = *Get(p, key);
                std::string why;
                if (!FromJson(one, out, why))
                    return P.Fail("`" + std::string(key) + "`: " + why);
                return true;
            };
            if (!Absent(p, "category"))
            {
                Family x;
                if (!part("category", x))
                    return false;
                f.category = x.category;
            }
            if (!Absent(p, "parameters"))
            {
                Family x;
                if (!part("parameters", x))
                    return false;
                f.parameters = std::move(x.parameters);
            }
            if (!Absent(p, "forms"))
            {
                Family x;
                if (!part("forms", x))
                    return false;
                KeepParts(f.forms, x.forms);
                f.forms = std::move(x.forms);
            }
            if (!Absent(p, "symbols"))
            {
                Family x;
                if (!part("symbols", x))
                    return false;
                KeepParts(f.symbols, x.symbols);
                f.symbols = std::move(x.symbols);
            }
            std::optional<std::string> op;
            if (!P.String("operation", op))
                return false;
            if (op)
            {
                if (Text::Trim(*op).empty())
                    f.operation.reset();
                else
                {
                    f.operation = OperationOf::Parse(*op);
                    if (!f.operation)
                        return P.Fail("`operation` is one of swing, double-swing, sliding, double-sliding, folding, fixed, not `" + *op + "`");
                }
            }
            if (!Absent(p, "types"))
            {
                Json types = ToJson(f)["types"];
                Merge(types, *Get(p, "types"));
                Family x;
                Json one = Json{ { "types", types } };
                std::string why;
                if (!FromJson(one, x, why))
                    return P.Fail("`types`: " + why);
                f.types = std::move(x.types);
            }
            std::string now = name;
            std::optional<std::string> rename;
            if (!P.String("rename", rename))
                return false;
            if (rename)
            {
                const std::string to(Text::Trim(*rename));
                if (to.empty())
                    return P.Fail("`rename` needs a name");
                if (to != name && m.families.count(to))
                    return P.Fail("there's a family `" + to + "` already");
                m.families.erase(name);
                m.library.FamilyRenamed(name, to);
                for (const Id id : users(name))
                    *m.elements.GetMut(id)->FamilyMut() = to;
                now = to;
            }
            m.families[now] = f;
            result = Json{ { "name", now }, { "family", ToJson(f) } };
            return true;
        }

        bool ComponentCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "component";
            Params P(c, p, error);
            const Model& m = s.GetModel();
            std::optional<std::string> family;
            if (!P.String("family", family))
                return false;
            if (!family)
                return P.Fail("`family` is required; `inspect` lists the families");
            const auto f = m.families.find(*family);
            if (f == m.families.end())
            {
                std::string names;
                for (const auto& [n, _] : m.families)
                    names += (names.empty() ? "" : ", ") + n;
                return P.Fail("family `" + *family + "` doesn't exist (there are: " + names + ")");
            }
            std::optional<std::string> type;
            if (!P.String("type", type))
                return false;
            Component comp;
            comp.family = *family;
            if (type)
                comp.typeName = *type;
            else if (!f->second.types.empty())
                comp.typeName = f->second.types.begin()->first;
            else
                return P.Fail("family `" + *family + "` has no type");
            if (!Absent(p, "params"))
            {
                const Json& v = *Get(p, "params");
                if (!v.is_object())
                    return P.Fail("`params` is {parameter: value}, not " + Show(v));
                for (auto it = v.begin(); it != v.end(); ++it)
                    if (!it.value().is_null())
                        comp.params[it.key()] = it.value();
            }
            std::optional<double> angle, offset;
            if (!LevelParam(c, m, p, "level", comp.level, error) || !P.PointReq("at", comp.at) || !P.Num("angle", angle) || !P.Num("offset", offset) ||
                !P.Flag("mirrored", comp.mirrored))
                return false;
            comp.angle  = angle.value_or(0.0);
            comp.offset = offset.value_or(0.0);
            const Id id = s.ModelMut().Insert(std::move(comp));
            result = Json{ { "id", id } };
            return true;
        }

        // ── 类型与材质 ───────────────────────────────────────────
        // 一层，严格地读（拼错的键报错）
        bool LayerParam(const char* c, const Json& v, Layer& l, std::string& error)
        {
            if (!v.is_object())
                return Fail(error, c, "`layers`: a layer is {function?, material?, thickness}, not " + Show(v));
            for (auto it = v.begin(); it != v.end(); ++it)
            {
                const std::string& k = it.key();
                if (k != "id" && k != "guid" && k != "function" && k != "material" && k != "thickness")
                    return Fail(error, c, "`layers`: unknown field `" + k + "`, expected one of id, guid, function, material, thickness");
            }
            const auto th = v.find("thickness");
            if (th == v.end() || !th->is_number())
                return Fail(error, c, "`layers`: missing field `thickness`");
            l.thickness = th->get<double>();
            if (const auto f = v.find("function"); f != v.end() && !f->is_null())
            {
                const auto fn = f->is_string() ? FunctionOf::FromName(f->get<std::string>()) : std::nullopt;
                if (!fn)
                    return Fail(error, c, "`layers`: unknown variant " + Show(*f) + ", expected one of structure, substrate, thermal, membrane, finish");
                l.function = *fn;
            }
            if (const auto mat = v.find("material"); mat != v.end() && !mat->is_null())
            {
                if (!mat->is_string())
                    return Fail(error, c, "`layers`: `material` must be text, got " + Show(*mat));
                l.material = mat->get<std::string>();
            }
            if (const auto id = v.find("id"); id != v.end() && !id->is_null())
            {
                const auto whole = AsWhole(*id);
                if (!whole)
                    return Fail(error, c, "`layers`: `id` must be a whole number");
                l.id = *whole;
            }
            if (const auto g = v.find("guid"); g != v.end() && !g->is_null())
            {
                std::string why;
                if (!FromJson(*g, l.guid, why))
                    return Fail(error, c, "`layers`: " + why);
            }
            return true;
        }

        // 带层的类型的厚度与各层：type 命令的参数，在同名的已有类型（old）之上。给了层时为各层之和；
        // 只给新厚度时差值加到第一个结构层（没有时为最厚的层）
        bool Layered(const char* c, Params& P, const std::string& category, const std::optional<std::pair<double, std::vector<Layer>>>& old,
                     std::optional<double> thickness, double& outThickness, std::vector<Layer>& outLayers, std::string& error)
        {
            auto need = [&](const char* what) { return Fail(error, c, "a new " + category + " type needs `" + what + "`"); };
            std::optional<std::vector<Layer>> layers;
            if (!P.Absent("layers"))
            {
                const Json& v = *Get(P.Raw(), "layers");
                if (!v.is_array() || v.size() > MaxLayers)
                    return Fail(error, c, "`layers` is a list of at most " + std::to_string(MaxLayers) + " layers");
                std::vector<Layer> list;
                for (const Json& x : v)
                {
                    Layer l;
                    if (!LayerParam(c, x, l, error))
                        return false;
                    list.push_back(std::move(l));
                }
                // 位置没变的层保留它的 ID 和 GUID
                if (old)
                    for (size_t i = 0; i < list.size() && i < old->second.size(); ++i)
                        if (list[i].id == 0 && list[i].guid.IsNil())
                        {
                            list[i].id   = old->second[i].id;
                            list[i].guid = old->second[i].guid;
                        }
                layers = std::move(list);
            }
            if (layers && !layers->empty())
            {
                double sum = 0.0;
                for (const Layer& l : *layers)
                    sum += l.thickness;
                if (thickness && std::abs(*thickness - sum) > 1e-6)
                    return Fail(error, c, "its layers add up to " + Num(sum) + " mm, not the " + Num(*thickness) + " mm given");
                outThickness = sum;
                outLayers    = std::move(*layers);
                return true;
            }
            if (layers)
            {
                if (!thickness)
                    return need("thickness");
                outThickness = *thickness;
                outLayers.clear();
                return true;
            }
            if (thickness)
            {
                std::vector<Layer> list = old ? old->second : std::vector<Layer>{};
                if (old && !old->second.empty())
                {
                    std::optional<size_t> k;
                    for (size_t i = 0; i < list.size() && !k; ++i)
                        if (list[i].function == Function::Structure)
                            k = i;
                    if (!k)
                    {
                        // 最厚的层（相同时取后面的，照 Rust 的 max_by）
                        size_t best = 0;
                        for (size_t i = 1; i < list.size(); ++i)
                            if (list[i].thickness >= list[best].thickness)
                                best = i;
                        k = best;
                    }
                    Layer& l = list[*k];
                    l.thickness += *thickness - old->first;
                    if (l.thickness <= 0.0)
                        return Fail(error, c, "at " + Num(*thickness) + " mm its structure would be " + Num(l.thickness) + " mm thick; change its layers");
                }
                outThickness = *thickness;
                outLayers    = std::move(list);
                return true;
            }
            if (!old)
                return need("thickness");
            outThickness = old->first;
            outLayers    = old->second;
            return true;
        }

        bool TypeCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "type";
            Params P(c, p, error);
            std::optional<std::string> cat, name;
            if (!P.String("category", cat))
                return false;
            if (!cat)
                return P.Fail("`category` is required: wall, floor, ceiling, door, window or roof");
            const std::string category = NormalizeCategory(*cat);
            if (!P.String("name", name))
                return false;
            if (!name || Text::Trim(*name).empty())
                return P.Fail("`name` is required");
            std::optional<double> thickness, width, height, sill;
            if (!P.Num("thickness", thickness) || !P.Num("width", width) || !P.Num("height", height) || !P.Num("sill", sill))
                return false;
            auto need = [&](const char* what) { return P.Fail("a new " + category + " type needs `" + what + "`"); };
            Model& m = s.ModelMut();
            Json t;
            if (category == "wall" || category == "floor")
            {
                std::optional<std::pair<double, std::vector<Layer>>> old;
                if (category == "wall")
                {
                    if (const auto it = m.types.wall.find(*name); it != m.types.wall.end())
                        old = std::pair{ it->second.thickness, it->second.layers };
                }
                else if (const auto it = m.types.floor.find(*name); it != m.types.floor.end())
                    old = std::pair{ it->second.thickness, it->second.layers };
                double th = 0.0;
                std::vector<Layer> layers;
                if (!Layered(c, P, category, old, thickness, th, layers, error))
                    return false;
                if (category == "wall")
                    m.types.wall[*name] = WallType{ th, layers };
                else
                    m.types.floor[*name] = FloorType{ th, layers };
                Types one;
                one.wall[*name] = WallType{ th, layers };
                t = Bim::ToJson(one)["wall"][*name];
            }
            else if (category == "roof" || category == "ceiling")
            {
                std::optional<std::pair<double, std::vector<Layer>>> old;
                std::optional<double> oldGrid;
                if (category == "roof")
                {
                    if (const auto it = m.types.roof.find(*name); it != m.types.roof.end())
                        old = std::pair{ it->second.thickness, it->second.layers };
                }
                else if (const auto it = m.types.ceiling.find(*name); it != m.types.ceiling.end())
                {
                    old = std::pair{ it->second.thickness, it->second.layers };
                    oldGrid = it->second.grid;
                }
                double th = 0.0;
                std::vector<Layer> layers;
                if (!Layered(c, P, category, old, thickness, th, layers, error))
                    return false;
                Types one;
                if (category == "roof")
                {
                    m.types.roof[*name] = RoofType{ th, layers };
                    one.roof[*name] = m.types.roof[*name];
                }
                else
                {
                    std::optional<double> grid;
                    if (!P.Num("grid", grid))
                        return false;
                    m.types.ceiling[*name] = CeilingType{ th, grid ? *grid : oldGrid.value_or(0.0), layers };
                    one.ceiling[*name] = m.types.ceiling[*name];
                }
                t = Bim::ToJson(one)[category][*name];
            }
            else if (category == "door" || category == "window")
            {
                auto& map = category == "door" ? m.types.door : m.types.window;
                std::optional<OpeningType> old;
                if (const auto it = map.find(*name); it != map.end())
                    old = it->second;
                std::optional<std::string> op;
                if (!P.String("operation", op))
                    return false;
                std::optional<Operation> operation = old ? old->operation : std::nullopt;
                if (op)
                {
                    if (Text::Trim(*op).empty())
                        operation.reset();
                    else
                    {
                        operation = OperationOf::Parse(*op);
                        if (!operation)
                            return P.Fail("`operation` is one of swing, double-swing, sliding, double-sliding, folding, fixed, not `" + *op + "`");
                    }
                }
                OpeningType ty;
                if (!width && !old)
                    return need("width");
                if (!height && !old)
                    return need("height");
                ty.width     = width ? *width : old->width;
                ty.height    = height ? *height : old->height;
                ty.sill      = sill ? *sill : (old ? old->sill : (category == "window" ? 900.0 : 0.0));
                ty.operation = operation;
                map[*name] = ty;
                Types one;
                one.door[*name] = ty;
                t = Bim::ToJson(one)["door"][*name];
            }
            else
                return P.Fail("`" + category + "` has no types here; use wall, floor, ceiling, door, window or roof (the other categories come with docs/PLAN.md, phase 2)");
            result = Json{ { "category", category }, { "name", *name }, { "type", t } };
            return true;
        }

        // "#rrggbb" 或 [r, g, b]
        std::optional<std::array<uint8_t, 3>> Colour(const Json& v)
        {
            if (v.is_string())
            {
                const std::string hex = v.get<std::string>();
                if (hex.size() != 7 || hex[0] != '#')
                    return std::nullopt;
                std::array<uint8_t, 3> out{};
                for (size_t i = 0; i < 3; ++i)
                {
                    int x = 0;
                    for (size_t k = 0; k < 2; ++k)
                    {
                        const char ch = hex[1 + i * 2 + k];
                        const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                        if (d < 0)
                            return std::nullopt;
                        x = x * 16 + d;
                    }
                    out[i] = static_cast<uint8_t>(x);
                }
                return out;
            }
            if (v.is_array() && v.size() == 3)
            {
                std::array<uint8_t, 3> out{};
                for (size_t i = 0; i < 3; ++i)
                {
                    const auto byte = AsWhole(v[i]);
                    if (!byte || *byte > 255)
                        return std::nullopt;
                    out[i] = static_cast<uint8_t>(*byte);
                }
                return out;
            }
            return std::nullopt;
        }

        bool MaterialCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "material";
            Params P(c, p, error);
            std::optional<std::string> given;
            bool del = false;
            if (!P.String("name", given) || !P.Flag("delete", del))
                return false;
            const std::string name = given ? std::string(Text::Trim(*given)) : std::string();
            if (name.empty())
                return P.Fail("`name` is required");
            Model& m = s.ModelMut();
            if (del)
            {
                if (!m.materials.count(name))
                    return P.Fail("the library has no `" + name + "`");
                for (const Model::Layered& t : m.LayeredTypes())
                    for (const Layer& l : *t.layers)
                        if (l.material == name)
                            return P.Fail("`" + name + "` is in " + t.category + " type `" + *t.name + "`'s layers; take it out of them first");
                if (const auto user = m.MaterialUser(name))
                    return P.Fail("`" + name + "` is in " + *user + "; take it out of it first");
                m.materials.erase(name);
                result = Json{ { "deleted", name } };
                return true;
            }
            if (!P.Absent("texture"))
                return P.Fail("textures aren't ported yet (docs/PLAN.md, phase 2)");
            Material mat = m.materials.count(name) ? m.materials[name] : Material{};
            if (!P.Absent("color"))
            {
                const Json& v = *Get(p, "color");
                const auto colour = Colour(v);
                if (!colour)
                    return P.Fail("`color` is \"#rrggbb\" or [r, g, b], each 0 to 255, not " + Show(v));
                mat.color = *colour;
            }
            for (const auto& [key, slot] : { std::pair{ "cut", &mat.cut }, std::pair{ "surface", &mat.surface } })
            {
                if (P.Absent(key))
                    continue;
                const Json& v = *Get(p, key);
                const auto pattern = v.is_string() ? PatternOf::FromName(v.get<std::string>()) : std::nullopt;
                if (!pattern)
                    return P.Fail("`" + std::string(key) + "`: " + Show(v) +
                                  " isn't a pattern: one of none, solid, diagonal, crosshatch, horizontal, vertical, grid, brick, concrete, insulation");
                *slot = *pattern;
            }
            std::optional<std::string> rename;
            if (!P.String("rename", rename))
                return false;
            std::string now = name;
            if (rename)
            {
                const std::string to(Text::Trim(*rename));
                if (to.empty())
                    return P.Fail("`rename` needs a name");
                if (to != name && m.materials.count(to))
                    return P.Fail("the library already has `" + to + "`");
                m.materials.erase(name);
                m.library.Renamed(Kind::Of(Kind::Tag::Material), name, to);
                for (auto& [_, t] : m.types.wall)
                    for (Layer& l : t.layers)
                        if (l.material == name)
                            l.material = to;
                for (auto& [_, t] : m.types.floor)
                    for (Layer& l : t.layers)
                        if (l.material == name)
                            l.material = to;
                for (auto& [_, t] : m.types.roof)
                    for (Layer& l : t.layers)
                        if (l.material == name)
                            l.material = to;
                for (auto& [_, t] : m.types.ceiling)
                    for (Layer& l : t.layers)
                        if (l.material == name)
                            l.material = to;
                m.RenameMaterialInFamilies(name, to);
                now = to;
            }
            m.materials[now] = mat;
            std::map<std::string, Material> one{ { now, mat } };
            result = Json{ { "name", now }, { "material", Bim::ToJson(one)[now] } };
            return true;
        }

        // ── 编辑 ─────────────────────────────────────────────────
        // patch 合并进 into：对象逐键合并，null 是去掉那个键
        void Merge(Json& into, const Json& patch)
        {
            if (!patch.is_object())
            {
                into = patch;
                return;
            }
            if (!into.is_object())
                into = Json::object();
            for (auto it = patch.begin(); it != patch.end(); ++it)
            {
                if (it->is_null())
                    into.erase(it.key());
                else
                {
                    if (!into.contains(it.key()))
                        into[it.key()] = nullptr;
                    Merge(into[it.key()], *it);
                }
            }
        }

        bool Modify(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "modify";
            if (!p.is_object())
                return Fail(error, c, "params must be {id | ids, …fields}");
            const Model& m = s.GetModel();
            std::vector<Id> ids;
            if (!IdsParam(c, m, p, ids, error))
                return false;
            std::vector<std::pair<Id, Element>> edited, rescaled;
            for (const Id id : ids)
            {
                const Element* e = m.elements.Get(id);
                Json v = ToJson(*e);
                for (auto it = p.begin(); it != p.end(); ++it)
                {
                    const std::string& k = it.key();
                    if (k == "id" || k == "ids")
                        continue;
                    Json val = *it;
                    if (k == "category")
                    {
                        if (v["category"] == val)
                            continue;
                        return Fail(error, c, "`category` can't change; delete the element and create another");
                    }
                    if ((k == "level" || k == "top_level") && val.is_string())
                    {
                        Guid g;
                        if (!LevelParam(c, m, p, k.c_str(), g, error))
                            return false;
                        val = ToJson(g);
                    }
                    if (v.contains(k) && v[k].is_object() && val.is_object())
                        Merge(v[k], val);       // 对象字段（平面的视图范围）合并
                    else
                    {
                        Json fresh = nullptr;
                        Merge(fresh, val);
                        v[k] = std::move(fresh);
                    }
                }
                Element e2;
                std::string why;
                if (!FromJson(v, e2, why))
                {
                    if (Text::StartsWith(why, "element: "))
                        why.erase(0, 9);
                    return Fail(error, c, "element " + std::to_string(id) + ": " + why);
                }
                // 换了定位线的墙留在原处：定位线横移
                const Wall* old = e->As<Wall>();
                Wall* w = e2.As<Wall>();
                if (old && w && old->location != w->location && !p.contains("start") && !p.contains("end"))
                {
                    Wall was = *w;
                    was.location = old->location;
                    const double before = Walls::Faces(m, was).first, after = Walls::Faces(m, *w).first;
                    const Walls::Axis axis = Walls::Axis::Of(*w);
                    const double length = axis.Length();
                    w->start = Micron(axis.At(0.0, before - after));
                    w->end   = Micron(axis.At(length, before - after));
                    // 圆弧上，门窗的偏移随它变长变短
                    const double k = Walls::Axis::Of(*w).Length() / std::max(length, 1e-9);
                    if (std::abs(k - 1.0) > 1e-12)
                        for (const auto& [oid, oe] : m.elements.Items())
                            if (const Opening* o = oe->AsOpening(); o && m.Same(o->host, m.GuidOf(id)))
                            {
                                Element copy = *oe;
                                copy.AsOpening()->offset *= k;
                                rescaled.emplace_back(oid, std::move(copy));
                            }
                }
                edited.emplace_back(id, std::move(e2));
            }
            for (auto& r : rescaled)
                edited.push_back(std::move(r));
            Model& mm = s.ModelMut();
            for (auto& [id, e] : edited)
                mm.elements.Insert(id, std::move(e));
            if (!p.contains("ids") && ids.size() == 1)
                result = ElementJson(s.GetModel(), ids.front());
            else
                result = Json{ { "modified", ids } };
            return true;
        }

        // 命令怎样带动元素：经 map（平移）。平移时单独选中的门窗只沿墙滑动 slide 的那部分
        struct Motion
        {
            std::function<P2(P2)> map;
            std::optional<P2>     slide;
        };

        // 方向 angle 度
        P2 Heading(double angle) { return P2(std::cos(angle * kPi / 180.0), std::sin(angle * kPi / 180.0)); }

        // f 把点 at 和从它出发的方向 angle 带到哪里
        std::pair<P2, double> Turned(const std::function<P2(P2)>& f, P2 at, double angle)
        {
            const P2 a = f(at), b = f(at + Heading(angle) * 1000.0);
            return { a, std::round(std::atan2(b.y - a.y, b.x - a.x) * 180.0 / kPi * 1e9) / 1e9 };
        }

        // 被 mo 带动后的元素。标高在平面上没有位置；单独选中的门窗只能沿墙平移，否则随墙动
        bool Transformed(const char* c, const Model& m, Id id, const Motion& mo, const std::set<Id>& with, Element& out, std::string& error)
        {
            out = *m.elements.Get(id);
            const auto& f = mo.map;
            if (out.As<Level>())
                return Fail(error, c, "level " + std::to_string(id) + " has no place in plan; change its elevation, or add a level");
            if (Wall* w = out.As<Wall>())
            {
                w->start = f(w->start);
                w->end   = f(w->end);
            }
            else if (Grid* g = out.As<Grid>())
            {
                g->start = f(g->start);
                g->end   = f(g->end);
            }
            else if (Floor* fl = out.As<Floor>())
            {
                for (P2& q : fl->boundary)
                    q = f(q);
                for (Ring& r : fl->openings)
                    for (P2& q : r)
                        q = f(q);
                if (fl->arrow)
                    for (P2& q : *fl->arrow)
                        q = f(q);
                std::tie(fl->patternOrigin, fl->patternAngle) = Turned(f, fl->patternOrigin, fl->patternAngle);
            }
            else if (RoomSeparator* rs = out.As<RoomSeparator>())
            {
                rs->start = f(rs->start);
                rs->end   = f(rs->end);
            }
            else if (Ceiling* cl = out.As<Ceiling>())
            {
                for (P2& q : cl->boundary)
                    q = f(q);
                for (Ring& r : cl->openings)
                    for (P2& q : r)
                        q = f(q);
            }
            else if (Room* r = out.As<Room>())
                r->point = f(r->point);
            else if (Component* cp = out.As<Component>())
                std::tie(cp->at, cp->angle) = Turned(f, cp->at, cp->angle);
            else if (Roof* r = out.As<Roof>())
            {
                // 边保持编号，山墙随之
                for (P2& q : r->boundary)
                    q = f(q);
                for (std::vector<Ring>* rings : { &r->courtyards, &r->openings })
                    for (Ring& ring : *rings)
                        for (P2& q : ring)
                            q = f(q);
                if (r->extrusion)
                    for (P2& q : r->extrusion->line)
                        q = f(q);
            }
            else if (Opening* o = out.AsOpening())
            {
                const auto host = m.elements.IdOf(o->host);
                const bool within = host && with.count(*host);
                if (!within)
                {
                    const Wall* hostWall = m.GetWall(o->host);
                    if (!mo.slide || !hostWall)
                        return Fail(error, c, std::string(out.Category()) + " " + std::to_string(id) + " turns and mirrors only with its wall; select the wall too");
                    o->offset += mo.slide->Dot((hostWall->end - hostWall->start).Unit());
                }
            }
            return true;
        }

        bool Relocate(const char* c, Session& s, const std::vector<Id>& ids, const Motion& mo, std::optional<bool> end, std::string& error)
        {
            const Model& m = s.GetModel();
            const auto& f = mo.map;
            const std::set<Id> moving(ids.begin(), ids.end());
            std::set<Guid> movingGuids;
            for (const Id id : ids)
                movingGuids.insert(m.GuidOf(id));
            // 整个带走的墙上的门窗随墙走
            auto carried = [&](Guid host) { return movingGuids.count(host) && !end; };
            std::map<Id, Wall> walls;
            std::vector<std::pair<Id, Element>> others;
            for (const Id id : ids)
            {
                Element e;
                if (!Transformed(c, m, id, mo, moving, e, error))
                    return false;
                if (Wall* w = e.As<Wall>())
                {
                    const Wall* old = m.GetWall(id);
                    if (end && *end)
                        w->end = old->end;
                    else if (end && !*end)
                        w->start = old->start;
                    walls[id] = *w;
                }
                else if (const Opening* o = e.AsOpening(); o && movingGuids.count(o->host))
                    continue;
                else
                    others.emplace_back(id, std::move(e));
            }
            // 带走的墙原来落在不动的墙上的端头，沿带走的墙滑回那面墙上（单独移动的端头去它该去的地方）
            if (!end)
            {
                for (auto& [id, w] : walls)
                {
                    const Wall* before = m.GetWall(id);
                    if (!before || before->arc != 0.0)
                        continue;
                    const P2 l0 = w.start, l1 = w.end;
                    for (const auto& [endRef, was] : { std::pair{ &w.start, before->start }, std::pair{ &w.end, before->end } })
                    {
                        for (const auto& [other, oe] : m.elements.Items())
                        {
                            const Wall* o = oe->As<Wall>();
                            if (!o || moving.count(other) || o->arc != 0.0 || !Walls::Stacked(m, *o, *before) ||
                                Geom::DistToSegment(was, o->start, o->end) > Tol)
                                continue;
                            if (const auto x = Geom::LineIntersection(l0, l1, o->start, o->end))
                                *endRef = Micron(*x);
                            break;
                        }
                    }
                }
            }
            // 不动的墙伸缩，保持与带走的墙相接：端头在带走的墙的端头上的跟着走；在它墙身上的沿自己的方向滑到它上面
            std::vector<std::pair<Wall, Wall>> moved;
            for (const auto& [id, w] : walls)
                if (const Wall* old = m.GetWall(id))
                    moved.emplace_back(*old, w);
            for (const auto& [id, e] : m.elements.Items())
            {
                const Wall* w = e->As<Wall>();
                if (!w || moving.count(id))
                    continue;
                Wall n = *w;
                for (const auto& [endRef, at] : { std::pair{ &n.start, w->start }, std::pair{ &n.end, w->end } })
                {
                    for (const auto& [before, after] : moved)
                    {
                        if (!Walls::Stacked(m, *w, before))
                            continue;
                        if (at.Dist(before.start) <= Tol || at.Dist(before.end) <= Tol)
                        {
                            *endRef = at.Dist(before.start) <= Tol ? after.start : after.end;
                            break;
                        }
                        if (Walls::Axis::Of(before).Distance(at, 0.0) <= Tol)
                        {
                            const bool straight = w->arc == 0.0 && after.arc == 0.0;
                            const auto x = straight ? Geom::LineIntersection(w->start, w->end, after.start, after.end) : std::nullopt;
                            *endRef = x ? Micron(*x) : f(at);
                            break;
                        }
                    }
                }
                if (!(n == *w))
                    walls[id] = n;
            }
            // 变了的墙上的门窗留在原处，或随墙走
            for (const auto& [id, e] : m.elements.Items())
            {
                const Opening* o = e->AsOpening();
                if (!o)
                    continue;
                const auto host = m.elements.IdOf(o->host);
                if (!host)
                    continue;
                const Wall* before = m.GetWall(*host);
                const auto after = walls.find(*host);
                if (!before || after == walls.end())
                    continue;
                const bool along = carried(o->host) || moving.count(id);
                const P2 spot = Walls::Axis::Of(*before).At(o->offset, 0.0);
                const P2 at = along ? f(spot) : spot;
                Element copy = *e;
                Opening* n = copy.AsOpening();
                const Walls::Axis axis = Walls::Axis::Of(after->second);
                n->offset = after->second.arc == 0.0 ? (at - after->second.start).Dot(axis.Tangent(0.0)) : axis.Project(at).first;
                const Id oid = id;
                others.erase(std::remove_if(others.begin(), others.end(), [oid](const auto& x) { return x.first == oid; }), others.end());
                others.emplace_back(oid, std::move(copy));
            }
            Model& mm = s.ModelMut();
            for (auto& [id, w] : walls)
                mm.elements.Insert(id, std::move(w));
            for (auto& [id, e] : others)
                mm.elements.Insert(id, std::move(e));
            return true;
        }

        // 被 mo 带动的元素的副本，带上复制的墙上的门窗；副本的 ID
        bool Duplicate(const char* c, Session& s, const std::vector<Id>& ids, const Motion& mo, Json& result, std::string& error)
        {
            const Model& m = s.GetModel();
            const std::set<Id> copying(ids.begin(), ids.end());
            std::set<Guid> copyingGuids;
            for (const Id id : ids)
                copyingGuids.insert(m.GuidOf(id));
            std::vector<std::pair<Id, Element>> copies;
            for (const Id id : ids)
            {
                Element e;
                if (!Transformed(c, m, id, mo, copying, e, error))
                    return false;
                // 复制的墙带上它的门窗，见下
                if (const Opening* o = e.AsOpening(); o && copyingGuids.count(o->host))
                    continue;
                copies.emplace_back(id, std::move(e));
            }
            for (const auto& [id, e] : m.elements.Items())
                if (const Opening* o = e->AsOpening(); o && copyingGuids.count(o->host))
                    copies.emplace_back(id, *e);
            Model& mm = s.ModelMut();
            std::vector<Id> made;
            std::map<Guid, Guid> hosts;
            for (auto& [from, e] : copies)
            {
                if (Grid* g = e.As<Grid>())
                    g->name = NextGridName(mm);
                else if (Opening* o = e.AsOpening())
                {
                    if (const auto it = hosts.find(o->host); it != hosts.end())
                        o->host = it->second;
                    o->mark = NextNumber(Marks(mm, e.IsDoor()));
                }
                const Guid original = mm.GuidOf(from);
                const Id id = mm.Insert(std::move(e));
                hosts[original] = mm.GuidOf(id);
                made.push_back(id);
            }
            // 与屋顶或楼板一起复制的墙贴着副本
            for (const Id id : made)
            {
                const Wall* w = mm.GetWall(id);
                if (!w || (!w->attachTop && !w->attachBase))
                    continue;
                Wall* ww = mm.elements.GetMut(id)->As<Wall>();
                for (std::optional<Guid>* at : { &ww->attachTop, &ww->attachBase })
                    if (*at)
                        if (const auto it = hosts.find(**at); it != hosts.end())
                            *at = it->second;
            }
            result = Json{ { "ids", made } };
            return true;
        }

        bool MoveCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "move";
            Params P(c, p, error);
            std::vector<Id> ids;
            P2 by;
            std::optional<std::string> endName;
            if (!IdsParam(c, s.GetModel(), p, ids, error) || !P.PointReq("by", by) || !P.String("end", endName))
                return false;
            // 只移动一端：true 为起点，false 为终点
            std::optional<bool> end;
            if (endName)
            {
                if (*endName == "start")
                    end = true;
                else if (*endName == "end")
                    end = false;
                else
                    return P.Fail("`end` is `start` or `end`, not `" + *endName + "`");
            }
            if (end && !(ids.size() == 1 && s.GetModel().GetWall(ids.front())))
                return P.Fail("`end` moves an end of one wall; give `ids` one wall");
            if (!Relocate(c, s, ids, Motion{ [by](P2 q) { return q + by; }, by }, end, error))
                return false;
            result = Json{ { "moved", ids } };
            return true;
        }

        bool CopyCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "copy";
            Params P(c, p, error);
            std::vector<Id> ids;
            P2 by;
            if (!IdsParam(c, s.GetModel(), p, ids, error) || !P.PointReq("by", by))
                return false;
            return Duplicate(c, s, ids, Motion{ [by](P2 q) { return q + by; }, by }, result, error);
        }

        bool DeleteCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "delete";
            const Model& m = s.GetModel();
            std::vector<Id> ids;
            if (!IdsParam(c, m, p, ids, error))
                return false;
            std::set<Guid> gone;
            for (const Id id : ids)
                gone.insert(m.GuidOf(id));
            // 挂在要删的东西上的也删：墙的门窗
            for (bool more = true; more;)
            {
                more = false;
                for (const auto& [id, e] : m.elements.Items())
                {
                    const Guid g = m.GuidOf(id);
                    if (gone.count(g))
                        continue;
                    if (const Opening* o = e->AsOpening(); o && gone.count(o->host))
                    {
                        gone.insert(g);
                        more = true;
                    }
                }
            }
            Json deleted = Json::array();
            for (const auto& [id, _] : m.elements.Items())
                if (gone.count(m.GuidOf(id)))
                    deleted.push_back(id);
            Model& mm = s.ModelMut();
            for (const Guid g : gone)
                mm.elements.Remove(g);
            // 贴着的屋顶或楼板删了的墙松开，插进的屋顶删了的屋顶不再插进
            const auto goneRef = [&](const std::optional<Guid>& at) { return at && gone.count(*at); };
            std::vector<Id> loose;
            for (const auto& [id, e] : mm.elements.Items())
            {
                const Wall* w = e->As<Wall>();
                const Roof* r = e->As<Roof>();
                if ((w && (goneRef(w->attachTop) || goneRef(w->attachBase))) || (r && goneRef(r->join)))
                    loose.push_back(id);
            }
            for (const Id id : loose)
            {
                Element* e = mm.elements.GetMut(id);
                if (Wall* w = e->As<Wall>())
                {
                    if (goneRef(w->attachTop))
                        w->attachTop.reset();
                    if (goneRef(w->attachBase))
                        w->attachBase.reset();
                }
                else if (Roof* r = e->As<Roof>())
                    r->join.reset();
            }
            result = Json{ { "deleted", deleted } };
            return true;
        }

        // ── 查询与设置 ───────────────────────────────────────────
        bool UnitsCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "units";
            Params P(c, p, error);
            std::optional<std::string> name;
            if (!P.String("length", name))
                return false;
            if (name)
            {
                const auto u = UnitsOf::FromName(*name);
                if (!u)
                    return P.Fail("`length` is mm, cm, m, in, ft or ft-in, not `" + *name + "`");
                if (*u != s.GetModel().units)
                    s.ModelMut().units = *u;
            }
            result = Json{ { "length", UnitsOf::Name(s.GetModel().units) } };
            return true;
        }

        bool RoomAreaCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "room_area";
            Params P(c, p, error);
            if (p.is_object())
                for (auto it = p.begin(); it != p.end(); ++it)
                    if (it.key() != "at")
                        return P.Fail("`" + it.key() + "` isn't one of its parameters: at");
            std::optional<std::string> name;
            if (!P.String("at", name))
                return false;
            const auto nameOf = [](RoomArea a) { return a == RoomArea::Centre ? "centre" : "finish"; };
            if (name)
            {
                std::optional<RoomArea> at;
                if (*name == "centre")
                    at = RoomArea::Centre;
                else if (*name == "finish")
                    at = RoomArea::Finish;
                else
                    return P.Fail("`at` is centre or finish, not `" + *name + "`");
                if (*at != s.GetModel().roomArea)
                    s.ModelMut().roomArea = *at;
            }
            result = Json{ { "at", nameOf(s.GetModel().roomArea) } };
            return true;
        }

        Json ValuesJson(const Fam::Values& values)
        {
            Json out = Json::object();
            for (const auto& [k, x] : values)
            {
                if (const double* d = std::get_if<double>(&x))
                    out[k] = *d;
                else
                    out[k] = std::get<std::string>(x);
            }
            return out;
        }

        // 族：各自的类别、类型与参数名
        Json FamiliesJson(const Model& m)
        {
            Json all = Json::object();
            for (const auto& [n, f] : m.families)
            {
                Json types = Json::array(), params = Json::array();
                for (const auto& [t, _] : f.types)
                    types.push_back(t);
                for (const Fam::Parameter& q : f.parameters)
                    params.push_back(q.name);
                all[n] = Json{ { "category", Fam::CategoryOf::Name(f.category) }, { "types", types }, { "parameters", params } };
            }
            return all;
        }

        bool Inspect(Session& s, const Json&, Json& result, std::string&)
        {
            const Model& m = s.GetModel();
            std::map<std::string, uint64_t> counts;
            for (const auto& [_, e] : m.elements.Items())
                ++counts[e->Category()];
            Json levels = Json::array();
            for (const auto& [id, l] : m.Levels())
                levels.push_back(Json{ { "id", id }, { "name", l->name }, { "elevation", l->elevation } });
            Json countsJson = Json::object();
            for (const auto& [k, n] : counts)
                countsJson[k] = n;
            result = Json{ { "name", m.name },
                           { "units", "mm" },
                           { "project_units", UnitsOf::Name(m.units) },
                           { "language", LanguageOf::Code(m.language) },
                           { "path", s.Path() ? Json(*s.Path()) : Json(nullptr) },
                           { "levels", levels },
                           { "counts", countsJson },
                           { "types", Bim::ToJson(m.types) },
                           { "materials", Bim::ToJson(m.materials) },
                           { "families", FamiliesJson(m) },
                           { "warnings", Json::array() },
                           { "history", s.History() } };
            return true;
        }

        bool ElementsCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "elements";
            Params P(c, p, error);
            const Model& m = s.GetModel();
            std::optional<std::string> category;
            if (!P.String("category", category))
                return false;
            const std::optional<std::string> cat = category ? std::optional<std::string>(NormalizeCategory(*category)) : std::nullopt;
            std::optional<Guid> level;
            if (!P.Absent("level"))
            {
                Guid g;
                if (!LevelParam(c, m, p, "level", g, error))
                    return false;
                level = g;
            }
            size_t limit = 500, offset = 0;
            if (!P.Count("limit", 500, limit) || !P.Count("offset", 0, offset))
                return false;
            std::vector<Id> hits;
            for (const auto& [id, e] : m.elements.Items())
            {
                if (cat && e->Category() != *cat)
                    continue;
                if (level && m.LevelOf(*e) != level)
                    continue;
                hits.push_back(id);
            }
            Json page = Json::array();
            for (size_t i = offset; i < hits.size() && page.size() < limit; ++i)
                page.push_back(ElementJson(m, hits[i]));
            result = Json{ { "count", hits.size() }, { "returned", page.size() }, { "elements", page } };
            return true;
        }
    }

    // ── 命令表 ───────────────────────────────────────────────────
    const std::vector<Command>& Commands()
    {
        // edit：可撤销；other：不算撤销步骤（只读，或换掉整个项目）
        static const std::vector<Command> all = {
            { "new", N_("New"), {}, "{name?, language?: en|pt|es} an empty project with Level 1 (0) and Level 2 (3000), named in its language", New, false },
            { "open", N_("Open"), {}, "{path} a .minibim (or BIMCraft .bimcraft) project; what this version doesn't know of it is left out, listed in `left_out`. An .ifc (IFC2x3 or IFC4) comes in as a new project: what MiniBIM has no element for, or what breaks a rule, is listed in `skipped`", Open, false },
            { "save", N_("Save"), {}, "{path?} saves the project (Save As with a path) as .minibim; the format is BIMCraft's, which opens it too. A path ending .ifc exports IFC4 and leaves the project's own path as it was", Save, false },
            { "undo", N_("Undo"), {}, "{count?} takes back the last `count` steps (1 by default)", Undo, false },
            { "redo", N_("Redo"), {}, "{count?} does again the last `count` steps undone (1 by default)", Redo, false },
            { "level", N_("Level"), { "LL" },
              "{elevation, name?, extent?: [[x,y],[x,y]]} a level at an elevation in mm; `extent`, the rectangle in plan its plane stretches over "
              "(south-west corner, then north-east)", LevelCmd, true },
            { "grid", N_("Grid"), { "GR" }, "{start: [x,y], end: [x,y], name?} names follow the last grid: 1, 2… or A, B…", GridCmd, true },
            { "wall", N_("Wall"), { "WA" },
              "{points: [[x,y],…], closed?, type?, level?, top_level?, height?, base_offset?, arc?, location?, attach_top?, attach_base?} one wall "
              "per segment of its location line; up to the next level, or `height` mm tall (unconnected), or up to `top_level`. `arc`: degrees "
              "each turns from its start to its end, counter-clockwise. `location`: centre (the default), core-centre, exterior-finish, "
              "interior-finish, exterior-core or interior-core; its exterior side is to the left of its start to its end, so walls drawn "
              "clockwise face out. `attach_top`, `attach_base`: a roof or floor its top follows from below, or its base stands on", WallCmd, true },
            { "door", N_("Door"), { "DR" },
              "{at: [x,y] | wall: id, offset?, type?, family?, level?, mark?, flip_facing?, flip_hand?} in the wall under `at`, or in `wall` "
              "centred `offset` mm from its start; of the project's door `type`, or of a door `family` and its `type`", DoorCmd, true },
            { "window", N_("Window"), { "WN" },
              "{at: [x,y] | wall: id, offset?, sill?, type?, level?, mark?, flip_facing?} like door; `sill` defaults to the type's", WindowCmd, true },
            { "floor", N_("Floor"), {},
              "{boundary?: [[x,y],…], openings?: [[[x,y],…],…], level?, type?, offset?, slope?, arrow?: [[x,y],[x,y]], structural?} a slab, its top "
              "at the level, or `offset` mm above it, open through `openings`; no boundary = the outline of the level's walls along their centre "
              "lines, open over each loop of them standing free inside (a courtyard). Sloped: its top rises `slope` degrees along the slope "
              "arrow, from its tail toward its head", FloorCmd, true },
            { "ceiling", N_("Ceiling"), {},
              "{point: [x,y] | boundary: [[x,y],…], openings?: [[[x,y],…],…], level?, type?, offset?} a ceiling, its underside `offset` mm "
              "(2600) above the level, open through `openings` (holes inside it, apart): in the room around `point`, out to its walls' faces "
              "and round the walls standing inside it, or along `boundary`", CeilingCmd, true },
            { "room", N_("Room"), { "RM" }, "{point: [x,y], level?, name?, number?} bounded by the walls and the room separation lines around the point",
              RoomCmd, true },
            { "room_separator", N_("Room Separator"), {},
              "{points: [[x,y],…], level?, closed?} room separation lines along `points`, one per segment (`closed` joins the last to the "
              "first): they bound rooms as walls do, to split an open space (a living room and its corridor) into rooms, and are nothing "
              "else, with no solid, no cut and no area. Returns their ids", RoomSeparatorCmd, true },
            { "roof", N_("Roof"), {},
              "{level?, boundary?: [[x,y],…], courtyards?: [[[x,y],…],…], slope?, overhang?, gables?: [edge, …], gable?, offset?, extrusion?, "
              "openings?: [[[x,y],…],…], join?, type?} a roof on `level` (by default the one above the lowest), its base `offset` mm above it. "
              "Over `boundary` or else the walls of the level below, `overhang` mm (500) past their faces, round any `courtyards` (holes in "
              "it, inside it and apart, with eaves of their own; over the walls, by default each loop of them standing free inside, joined "
              "to none of the outer ones and with no room in it): flat at `slope` 0, else every edge sloping up at `slope` degrees (30), "
              "hips, valleys and ridges where the slopes meet, but the edges in `gables` (edge i runs from point i to the next, the "
              "courtyards' edges numbered on after the boundary's) left upright, gable ends; `gable` leaves a rectangle's short ends "
              "upright. Or with `extrusion` {line: [[x,y],[x,y]], profile: [[along, up],…], from?, to}: its underside's profile in the "
              "upright plane through `line`, along it from its start and up from the base, swept square to it from `from` (0) to `to` mm "
              "to its left. `openings` are holes through it in plan; with `join`, a roof it runs into, it ends where it meets that one's top",
              RoofCmd, true },
            { "type", N_("Edit Type"), {},
              "{category: wall|floor|ceiling|door|window|roof, name, thickness? | width?, height?, sill?, operation?, grid?} creates the type, or changes it "
              "everywhere it's used. A wall or floor type may have `layers` [{function?: structure|substrate|thermal|membrane|finish, material?, "
              "thickness}, …], a wall's from its exterior side in, a floor's from the top down: it's as thick as they are, and a new `thickness` "
              "goes to its structure. A roof's or a ceiling's layers too, from the top down; a ceiling type may have `grid`: its square tiles' "
              "width, mm, which reflected ceiling plans draw (0, none)", TypeCmd, true },
            { "material", N_("Materials"), {},
              "{name, color?: \"#rrggbb\" | [r, g, b], cut?, surface?, rename?, delete?: true} a material of the project's library, created or "
              "changed: its colour in shaded 3D views, the pattern its cut shows and the one its surfaces show", MaterialCmd, true },
            { "family", N_("Load Family"), {},
              "{name, category?: furniture|casework|plumbing|lighting|generic|door|window, parameters?: [{name, kind?: length|number|"
              "angle|yesno|material, instance?, value?, formula?}], types?: {type: {parameter: value}}, forms?: [{plane?: xy|xz|yz, "
              "profile?: [[a, b], …], circle?: {centre: [a, b], radius}, axis?: [[a, b], [a, b]], from?, to, material?, visible?, void?}], "
              "symbols?: [{line?: [[x, y], …] | arc?: {centre: [x, y], radius, from?, to}, visible?}], operation?: swing|double-swing|sliding|"
              "double-sliding|folding|fixed (a door's or window's, for IFC and schedules), rename?, delete?: true, path?, save?} a family of the "
              "project's, MiniBIM's own parametric component (ported from BIMCraft): created, changed, or loaded from the .json file at `path` "
              "(a family as `inspect` shows it, with its `name`). Its forms are profiles drawn in its own axes (x along a component's angle, y "
              "to its left, z up from its base) and swept: `xy` up from `from` to `to`, `xz` along y, `yz` along x. With an `axis`, a line in "
              "that plane, the profile is turned about it instead, `from` to `to` degrees (up to 360). A `void` is no solid: it cuts the solid "
              "forms listed before it that it passes through, and is never seen itself. A `door` or `window` family stands in a wall, placed "
              "with `door` or `window` and a `family`: its forms are drawn in the opening's axes (x along the wall from the middle of the "
              "opening, y across it, 0 in the middle of the wall and + toward its exterior, z up from the sill), it must have length "
              "parameters `Width` and `Height`, and its formulas read `Wall Thickness`, the wall's. Its `symbols` are the lines its plan "
              "draws. Sizes are mm, or formulas over its parameters as Revit's are: `Width / 2`, `if(Depth > 600, 4, 2)`. A parameter is "
              "each type's, or with `instance` each component's. `types` merges into its types (a null takes one away); `parameters` and "
              "`forms` replace its own", FamilyCmd, true },
            { "component", N_("Component"), { "CM" },
              "{family, type?, at: [x,y], level?, angle?, offset?, mirrored?, params?: {parameter: value}} a family's type placed, its first "
              "by default: at a point on a level, turned `angle` degrees counter-clockwise, `offset` mm up from the level, and with `mirrored` "
              "flipped across its own x axis; `params` sets its instance parameters (`modify` merges into them, a null taking one back to the "
              "family's)", ComponentCmd, true },
            { "flip", N_("Flip"), {},
              "{ids: [wall,…]} turns walls round about their location lines: their exterior sides go to the other side, their doors and "
              "windows stay where they are", Flip, true },
            { "modify", N_("Modify"), {},
              "{id | ids: [id,…], …fields} change fields as `elements` shows them, of one element or several; `level` and `top_level` may be names",
              Modify, true },
            { "move", N_("Move"), { "MV" },
              "{ids: [id,…], by: [dx, dy], end?: start|end} moves elements in plan; walls joined to a moved wall stretch to stay joined, and "
              "doors and windows slide along their walls. With `end`, only that end of one wall moves", MoveCmd, true },
            { "copy", N_("Copy"), { "CO" }, "{ids: [id,…], by: [dx, dy]} copies elements, with the doors and windows of copied walls; returns the copies' ids",
              CopyCmd, true },
            { "delete", N_("Delete"), { "DE" }, "{ids: [id,…]} deleting a wall deletes its doors and windows", DeleteCmd, true },
            { "units", N_("Project Units"), { "UN" },
              "{length?: mm|cm|m|in|ft|ft-in} how lengths are shown and typed; the model, and every command, keep millimetres", UnitsCmd, true },
            { "room_area", N_("Area and Volume Computations"), {},
              "{at?: centre|finish} where the project measures its rooms' areas and perimeters, as Revit's Area and Volume Computations: "
              "at the walls' centre lines (`centre`, as a project starts) or at their finish faces (`finish`, the floor inside the walls). "
              "Rooms keep their points; only their measured outline changes", RoomAreaCmd, true },
            { "inspect", N_("Inspect"), {}, "{} levels, element counts, types, materials and undo history", Inspect, false },
            { "elements", N_("Elements"), {},
              "{category?, level?, limit?, offset?} elements with their `id` (six digits, what commands take) and `guid` (made once, never "
              "changed), fields and derived values (length, height, area_m2…); what an element points at it holds by GUID", ElementsCmd, false },
        };
        return all;
    }

    // ── 给别处用的 ───────────────────────────────────────────────
    Json ElementJson(const Model& m, Id id)
    {
        const Element* e = m.elements.Get(id);
        if (!e)
            return nullptr;
        Json v = ToJson(*e);
        v["id"]   = id;
        v["guid"] = ToJson(m.GuidOf(id));
        // 类型的 GUID，放在名称旁边
        Element copy = *e;
        copy.LibRefsMut([&](const Kind& kind, std::string& name)
        {
            if (kind.tag == Kind::Tag::Type)
                if (const auto g = m.library.GuidOf(kind, name))
                    v["type_guid"] = ToJson(*g);
        });
        if (const Wall* w = e->As<Wall>())
        {
            const Walls::Axis axis = Walls::Axis::Of(*w);
            v["length"]    = Hundredth(axis.Length() * 1e4) / 1e4;
            v["height"]    = Hundredth(m.TopZ(*w) - m.BaseZ(*w));
            v["thickness"] = m.Thickness(*w);
            if (const auto r = axis.Radius())
                v["radius"] = Hundredth(*r);
        }
        else if (const Opening* o = e->AsOpening())
        {
            const auto t = m.OpeningTypeOf(*e);
            const Wall* host = m.GetWall(o->host);
            if (t && host)
            {
                v["width"]  = t->width;
                v["height"] = t->height;
                if (t->operation)
                    v["operation"] = OperationOf::Name(*t->operation);
                v["level"] = ToJson(host->level);
                v["at"]    = ToJson(Walls::Axis::Of(*host).At(o->offset, 0.0));
                // 族的：每个参数算出的值
                if (o->family)
                    if (const auto f = m.families.find(*o->family); f != m.families.end())
                    {
                        Fam::Values values;
                        std::string why;
                        if (f->second.ValuesIn(o->typeName, o->params, m.Thickness(*host), values, why))
                            v["values"] = ValuesJson(values);
                    }
            }
        }
        else if (const Component* cp = e->As<Component>())
        {
            // 每个参数算出的值，以及它高出底部多少
            if (const auto f = m.families.find(cp->family); f != m.families.end())
            {
                Fam::Values values;
                std::string why;
                if (f->second.Values(cp->typeName, cp->params, values, why))
                    v["values"] = ValuesJson(values);
            }
            const double base = m.Elevation(cp->level) + cp->offset;
            double top = base;
            for (const Fam::Piece& pc : m.ComponentPieces(*cp))
                top = std::max(top, pc.top);
            v["height"] = Hundredth(top - base);
        }
        else if (const Floor* f = e->As<Floor>())
        {
            double left = 0.0;
            for (const auto& [face, _] : m.FloorSlabs(*f))
            {
                Ring ring;
                for (const P3& q : face)
                    ring.emplace_back(q[0], q[1]);
                left += std::abs(Geom::Area(ring));
            }
            v["area_m2"] = M2(left);
            const auto t = m.types.floor.find(f->typeName);
            v["thickness"] = t == m.types.floor.end() ? 0.0 : t->second.thickness;
            // 顶面最低与最高处的高度
            const Plane top = m.FloorTop(*f);
            double lo = INFINITY, hi = -INFINITY;
            for (const P2& q : f->boundary)
            {
                lo = std::min(lo, top.At(q));
                hi = std::max(hi, top.At(q));
            }
            v["top"] = Json::array({ Hundredth(lo), Hundredth(hi) });
            // 顶面模型图案的起点与转角：默认值也显示，供 modify 设置
            v["pattern_origin"] = ToJson(f->patternOrigin);
            v["pattern_angle"]  = f->patternAngle;
        }
        else if (const RoomSeparator* rs = e->As<RoomSeparator>())
            v["length"] = Hundredth(rs->start.Dist(rs->end));
        else if (const Ceiling* cl = e->As<Ceiling>())
        {
            // 竖井还没移植：天花板的面积即各板块的面积
            double left = 0.0;
            for (const auto& [face, _] : m.CeilingSlabs(*cl))
            {
                Ring ring;
                for (const P3& q : face)
                    ring.emplace_back(q[0], q[1]);
                left += std::abs(Geom::Area(ring));
            }
            v["area_m2"] = M2(left);
            if (const auto t = m.types.ceiling.find(cl->typeName); t != m.types.ceiling.end())
            {
                v["thickness"] = t->second.thickness;
                v["grid"]      = t->second.grid;
            }
        }
        else if (const Room* r = e->As<Room>())
        {
            const auto region = m.RoomRegion(*r);
            v["enclosed"]    = region.has_value();
            v["area_m2"]     = region ? M2(region->Area()) : 0.0;
            v["perimeter_m"] = region ? std::round(region->Perimeter() / 10.0) / 100.0 : 0.0;
        }
        else if (const Roof* r = e->As<Roof>())
        {
            std::vector<Slope> faces;
            std::string why;
            if (!m.RoofFaces(*r, faces, why))
                faces.clear();
            double ridge = m.Elevation(r->level) + r->offset;
            for (const auto& [loops, p] : faces)
                for (const Ring& l : loops)
                    for (const P2& q : l)
                        ridge = std::max(ridge, p.At(q));
            const Ring outline = m.RoofOutline(*r);
            double courtyards = 0.0;
            for (const Ring& c : r->courtyards)
                courtyards += std::abs(Geom::Area(c));
            v["ridge"]   = Hundredth(ridge);
            v["area_m2"] = M2(std::abs(Geom::Area(outline)) - courtyards);
            Json o = Json::array();
            for (const P2& q : outline)
                o.push_back(ToJson(Micron(q)));
            v["outline"] = std::move(o);
        }
        return v;
    }

    std::optional<std::pair<Ring, std::vector<Ring>>> FloorOutline(const Model& m, Guid level)
    {
        // 各连通块的外轮廓（顺时针），大的在前
        std::vector<Ring> pieces;
        for (Ring& f : Geom::Faces(m.WallLines(level), Tol))
            if (Geom::Area(f) < -Tol * Tol)
                pieces.push_back(std::move(f));
        std::stable_sort(pieces.begin(), pieces.end(), [](const Ring& a, const Ring& b) { return Geom::Area(a) < Geom::Area(b); });
        if (pieces.empty())
            return std::nullopt;
        const Ring& outer = pieces.front();
        // 轮廓里面独立的环：其余在它里面的块
        std::vector<const Ring*> loops;
        for (size_t i = 1; i < pieces.size(); ++i)
            if (std::all_of(pieces[i].begin(), pieces[i].end(), [&](const P2& q) { return Geom::Contains(outer, q); }))
                loops.push_back(&pieces[i]);
        auto nested = [&](size_t a)
        {
            for (size_t b = 0; b < loops.size(); ++b)
                if (b != a && std::all_of(loops[a]->begin(), loops[a]->end(), [&](const P2& q) { return Geom::Contains(*loops[b], q); }))
                    return true;
            return false;
        };
        // 围着一点的最里面的环（环是顺时针的，面积为负，最大的即最小的环），也就是房间所在的环
        auto around = [&](P2 p) -> std::optional<size_t>
        {
            std::optional<size_t> best;
            for (size_t a = 0; a < loops.size(); ++a)
                if (Geom::Contains(*loops[a], p) && (!best || Geom::Area(*loops[a]) >= Geom::Area(*loops[*best])))
                    best = a;
            return best;
        };
        std::vector<size_t> indoors;
        for (const auto& [_, e] : m.elements.Items())
            if (const Room* r = e->As<Room>(); r && r->level == level)
                if (const auto a = around(r->point))
                    indoors.push_back(*a);
        // 天井：最外层、没有房间的环
        std::vector<Ring> courtyards;
        for (size_t a = 0; a < loops.size(); ++a)
            if (!nested(a) && std::find(indoors.begin(), indoors.end(), a) == indoors.end())
                courtyards.push_back(Tidy(*loops[a]));
        return std::pair{ Ring(outer.rbegin(), outer.rend()), std::move(courtyards) };
    }

    std::optional<std::pair<Ring, std::vector<Ring>>> RoofFootprint(const Model& m, Guid level, double overhang, std::string& error)
    {
        const double z = m.Elevation(level);
        std::optional<Id> below;
        for (const auto& [id, l] : m.Levels())
            if (l->elevation < z - Tol)
                below = id;
        if (!below)
        {
            error = LevelName(m, level) + " has no level below to roof over; give `boundary`";
            return std::nullopt;
        }
        const Guid under = m.GuidOf(*below);
        const auto outline = FloorOutline(m, under);
        if (!outline)
        {
            error = "no walls enclose an area on " + LevelName(m, under) + "; give `boundary`";
            return std::nullopt;
        }
        // 这一层最厚的墙定墙面在哪里（BIMCraft 的简化：外轮廓混用几种墙时也只取最厚的）
        double face = 0.0;
        for (const auto& [_, e] : m.elements.Items())
            if (const Wall* w = e->As<Wall>(); w && m.CutBy(*w, under))
                face = std::max(face, m.Thickness(*w) / 2.0);
        // 檐口离墙中心线多远：逆时针的环向外，顺时针的向里
        const double d = face + overhang;
        // 伸出后仍敞开的天井：往里收，太小的会翻过来，边反着走
        std::vector<Ring> courtyards;
        for (const Ring& l : outline->second)
        {
            const Ring grown = Geom::Grow(l, d);
            const auto before = Geom::RingEdges(l), after = Geom::RingEdges(grown);
            bool open = true;
            for (size_t i = 0; open && i < std::min(before.size(), after.size()); ++i)
                open = (before[i].second - before[i].first).Dot(after[i].second - after[i].first) > 0.0;
            if (open)
                courtyards.push_back(Tidy(grown));
        }
        return std::pair{ Tidy(Geom::Grow(outline->first, d)), std::move(courtyards) };
    }

    Ring Tidy(const Ring& ring)
    {
        Ring out;
        for (const P2& p : Geom::Corners(ring))
            out.push_back(Micron(p));
        if (out.empty())
            return out;
        size_t first = 0;
        for (size_t i = 1; i < out.size(); ++i)
            if (out[i].y < out[first].y || (out[i].y == out[first].y && out[i].x < out[first].x))
                first = i;
        std::rotate(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
        return out;
    }

    std::string NextNumber(const std::vector<std::string>& takenIn)
    {
        std::vector<std::string> taken;
        for (const std::string& t : takenIn)
            taken.emplace_back(Text::Trim(t));
        // 每个序列按前缀：有几个、最大的数、补零的位数
        struct Series
        {
            size_t   count = 0;
            uint64_t last  = 0;
            size_t   width = 0;
        };
        std::map<std::string, Series> series;
        for (const std::string& t : taken)
        {
            if (t.size() > MaxText)
                continue;
            size_t digits = 0;
            while (digits < t.size() && t[t.size() - 1 - digits] >= '0' && t[t.size() - 1 - digits] <= '9')
                ++digits;
            const std::string prefix = t.substr(0, t.size() - digits), number = t.substr(t.size() - digits);
            const auto n = Guid::Parse(number);
            if (number.empty() || !n || !n->Pending())
                continue;
            Series& s = series[prefix];
            ++s.count;
            s.last = std::max(s.last, *n->Pending());
            if (number.size() > 1 && number[0] == '0')
                s.width = std::max(s.width, digits);
        }
        const size_t plain = series.count("") ? series[""].count : 0;
        std::string prefix;
        Series best;
        bool found = false;
        for (const auto& [p, s] : series)
        {
            if (!(p.empty() || (s.count >= 2 && s.count > plain)))
                continue;
            // 数目多的、数大的优先；都一样时前缀小的
            if (!found || std::tie(s.count, s.last) > std::tie(best.count, best.last) ||
                (std::tie(s.count, s.last) == std::tie(best.count, best.last) && p < prefix))
            {
                prefix = p;
                best   = s;
                found  = true;
            }
        }
        // 太长的标记不开始序列，所以位数不超过 MaxText
        const size_t width = prefix.empty() ? 0 : std::min(best.width, MaxText);
        uint64_t n = best.last;
        for (;;)
        {
            n = n == UINT64_MAX ? n : n + 1;
            std::string digits = std::to_string(n);
            if (digits.size() < width)
                digits.insert(0, width - digits.size(), '0');
            const std::string next = prefix + digits;
            if (std::find(taken.begin(), taken.end(), next) == taken.end() || n == UINT64_MAX)
                return next;
        }
    }
}
