// 移植自 BIMCraft crates/core/src/commands.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
//
// 命令表：脚本、界面（以后还有命令行与 AI 代理）能做的一切。参数是 JSON，长度单位毫米。命令构造元素，
// 然后由 Model::Check 检查整个模型（Session::Execute），所以命令只需要仔细读参数。
// 第 2a 步移植了项目文件、撤销、标高、轴网、墙、门窗、楼板、类型、材质与编辑（修改、移动、复制、删除、翻转），
// 其余命令按 docs/PLAN.md 第 2 阶段的顺序陆续加进来
#include "Commands/Commands.h"
#include "Model/Massing.h"
#include "I18n/Translator.h"
#include "Ifc/Ifc.h"
#include "Model/Align.h"
#include "Model/Sketch.h"
#include "Model/ModelJson.h"
#include "Model/Snap.h"
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

        // 恢复副本（自动保存，Autosave.h）：原子地写，留着项目的路径与撤销历史
        bool AutosaveCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "autosave";
            Params P(c, p, error);
            std::optional<std::string> path;
            if (!P.String("path", path))
                return false;
            if (!path)
                return P.Fail("`path` is required");
            if (s.Path() && *s.Path() == *path)
                return P.Fail("the recovery file must differ from the saved project");
            std::string text, why;
            if (!s.GetModel().ToFile(text, why))
                return P.Fail(why);
            if (text.size() >= kMaxFile)
                return P.Fail("the recovery snapshot exceeds the project file size limit");
            if (!AtomicWrite(*path, text, why))
                return P.Fail(why);
            result = Json{ { "path", *path }, { "format", "project" } };
            return true;
        }

        // 打开并检查一份恢复副本；之后保存存到 project，没有时（没保存过的项目）要另存
        bool Recover(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "recover";
            // 先读完参数再打开（打开会换掉会话）：参数不对时什么都不恢复
            Params P(c, p, error);
            std::optional<std::string> path, project;
            if (!P.String("path", path) || !P.String("project", project))
                return false;
            if (!Open(s, Json{ { "path", path ? Json(*path) : Json() } }, result, error))
            {
                if (error.rfind("open:", 0) == 0)
                    error = "recover:" + error.substr(5);
                return false;
            }
            s.SetPath(project);
            result["project"] = project ? Json(*project) : Json();
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
            std::map<Guid, Guid> hostOf;
            for (const Id id : openings)
            {
                Opening* o = m.elements.GetMut(id)->AsOpening();
                o->offset     = lengths[o->host] - o->offset;
                o->flipFacing = !o->flipFacing;
                o->flipHand   = !o->flipHand;
                hostOf[m.GuidOf(id)] = o->host;
            }
            // 尺寸量着同样的线：翻转的墙左右对调、起点终点对调，它的门窗的两边也对调
            std::vector<Id> dims;
            for (const auto& [id, e] : m.elements.Items())
                if (e->As<Dimension>())
                    dims.push_back(id);
            for (const Id id : dims)
                for (Reference& r : m.elements.GetMut(id)->As<Dimension>()->refs)
                {
                    const Guid g = m.GuidOf(r.id);
                    if (lengths.count(g))
                        r.face = FaceOf::Flipped(r.face);
                    else if (const auto h = hostOf.find(g); h != hostOf.end() && lengths.count(h->second))
                        r.face = FaceOf::Mirrored(r.face);
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

        // ── 草图（移植自 commands.rs 的 sketch_param、sketched、edit_boundary）────────────
        // 命令里的草图线：[[x,y],[x,y]] 直线，[[x,y],[x,y],[x,y]] 从第一点到第二点、经过第三点的圆弧，或 {id?, start, end, arc? | through?}；
        // 带 id 的保留 old（正在编辑的草图）里那条线的 ID 与 GUID
        // styled 时（填充区域的边界）线还可以带 style（线样式）与 invisible
        bool SketchParam(Params& P, const Json& p, const char* key, const std::vector<SketchLine>& old, std::optional<std::vector<SketchLine>>& out,
                         bool styled = false)
        {
            const Json* v = Get(p, key);
            if (!v || v->is_null())
                return true;
            const std::string shape = "`" + std::string(key) + "` is a list of lines, each [[x,y],[x,y]], an arc [[x,y],[x,y],[x,y]] (its ends, then a "
                                      "point it passes through) or {id?, start, end, arc? | through?}";
            if (!v->is_array())
                return P.Fail(shape);
            if (v->size() > MaxPoints)
                return P.Fail("`" + std::string(key) + "` lists more than " + std::to_string(MaxPoints));
            std::vector<SketchLine> lines;
            for (size_t i = 0; i < v->size(); ++i)
            {
                const Json& g = (*v)[i];
                const std::string at = "`" + std::string(key) + "` line " + std::to_string(i);
                std::string why;
                SketchLine l;
                if (g.is_array())
                {
                    std::vector<P2> pts;
                    for (const Json& q : g)
                    {
                        P2 pt;
                        if (!FromJson(q, pt, why))
                            return P.Fail(at + ": " + why);
                        pts.push_back(pt);
                    }
                    if (pts.size() == 2)
                    {
                        l.start = pts[0];
                        l.end   = pts[1];
                    }
                    else if (pts.size() == 3)
                        l = Sketch::ArcThrough(pts[0], pts[1], pts[2]);
                    else
                        return P.Fail(shape);
                }
                else if (g.is_object())
                {
                    for (auto it = g.begin(); it != g.end(); ++it)
                        if (it.key() != "id" && it.key() != "guid" && it.key() != "start" && it.key() != "end" && it.key() != "arc" && it.key() != "through" &&
                            !(styled && (it.key() == "style" || it.key() == "invisible")))
                            return P.Fail(at + ": `" + it.key() + "` isn't one it takes; lines and arcs are ported, other curves aren't yet");
                    if (styled && g.contains("style") && !g["style"].is_null())
                    {
                        if (!g["style"].is_string())
                            return P.Fail(at + ": `style` is a line style's name");
                        l.style = std::string(Text::Trim(g["style"].get<std::string>()));
                    }
                    if (styled && g.contains("invisible") && !g["invisible"].is_null())
                    {
                        if (!g["invisible"].is_boolean())
                            return P.Fail(at + ": `invisible` is true or false");
                        l.invisible = g["invisible"].get<bool>();
                    }
                    if (!g.contains("start") || !g.contains("end") || !FromJson(g["start"], l.start, why) || !FromJson(g["end"], l.end, why))
                        return P.Fail(at + " needs `start` and `end` [x, y]" + (why.empty() ? "" : ": " + why));
                    if (g.contains("arc") && g.contains("through"))
                        return P.Fail(at + ": give an arc's `through` point or its `arc` turn, not both");
                    if (g.contains("arc"))
                    {
                        if (!g["arc"].is_number())
                            return P.Fail(at + ": `arc` is degrees");
                        l.arc = g["arc"].get<double>();
                    }
                    if (g.contains("through"))
                    {
                        P2 t;
                        if (!FromJson(g["through"], t, why))
                            return P.Fail(at + ": `through`: " + why);
                        l.arc = Sketch::ArcThrough(l.start, l.end, t).arc;
                    }
                    if (g.contains("id") && !g["id"].is_null())
                    {
                        const auto id = AsWhole(g["id"]);
                        const auto was = id ? std::find_if(old.begin(), old.end(), [&](const SketchLine& o) { return o.id == static_cast<Id>(*id); })
                                            : old.end();
                        if (was == old.end())
                            return P.Fail(at + ": " + g["id"].dump() + " isn't a line of this sketch; leave `id` out for a new one");
                        l.id   = was->id;
                        l.guid = was->guid;
                    }
                }
                else
                    return P.Fail(shape);
                lines.push_back(l);
            }
            out = std::move(lines);
            return true;
        }

        // `sketch` 解成一个外轮廓（逆时针）与它的洞（顺时针），与线一起；没给时为空。与 boundary 或 holes 一起给时拒绝
        bool Sketched(Params& P, const Json& p, const char* holes, const std::vector<SketchLine>& old,
                      std::optional<std::pair<Geom::Region, std::vector<SketchLine>>>& out)
        {
            std::optional<std::vector<SketchLine>> lines;
            if (!SketchParam(P, p, "sketch", old, lines))
                return false;
            if (!lines)
                return true;
            for (const char* k : { "boundary", holes })
                if (*k && !P.Absent(k))
                    return P.Fail(std::string("give the outline as `sketch` or `boundary`") + (*holes ? std::string(" (with `") + holes + "`)" : std::string()) + ", not both");
            Geom::Region r;
            std::vector<Sketch::Problem> problems;
            if (!Sketch::SolveOne(*lines, r, problems))
                return P.Fail("the sketch isn't one outline: " + Sketch::Say(problems, Sketch::Namer(*lines)));
            out = std::pair{ std::move(r), std::move(*lines) };
            return true;
        }

        // 元素现在的草图：记着的，还是它的轮廓与洞时；否则它们的边，当作新的线
        std::optional<std::vector<SketchLine>> BoundaryNow(const Element& e)
        {
            Ring outline;
            std::vector<Ring> holes;
            if (const Floor* f = e.As<Floor>())
                std::tie(outline, holes) = std::pair{ f->boundary, f->openings };
            else if (const Ceiling* c = e.As<Ceiling>())
                std::tie(outline, holes) = std::pair{ c->boundary, c->openings };
            else if (const Roof* r = e.As<Roof>(); r && !r->extrusion)
                std::tie(outline, holes) = std::pair{ r->boundary, r->courtyards };
            else
                return std::nullopt;
            const auto* kept = e.SketchOf();
            if (kept && !kept->empty() && Sketch::Draws(*kept, outline, holes))
                return *kept;
            return Sketch::LinesOf(outline, holes);
        }

        bool EditBoundaryCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "edit_boundary";
            Params P(c, p, error);
            const Model& m = s.GetModel();
            std::optional<Guid> g;
            if (!P.IdOpt("id", g))
                return false;
            if (!g)
                return P.Fail("`id`, the floor, ceiling, roof or filled region to edit, is required");
            const auto id = m.elements.IdOf(*g);
            if (!id)
                return P.Fail("no element " + g->ToString());
            Element e = *m.elements.Get(*id);
            // 填充区域：任意多个环，线带线样式
            if (FilledRegion* fr = e.As<FilledRegion>())
            {
                std::optional<std::vector<SketchLine>> lines;
                if (!SketchParam(P, p, "sketch", fr->sketch, lines, true))
                    return false;
                if (!lines)
                {
                    result = Json{ { "id", *id }, { "sketch", SketchJson(fr->sketch) } };
                    return true;
                }
                Sketch::Profile profile;
                std::vector<Sketch::Problem> problems;
                if (!Sketch::Solve(*lines, profile, problems))
                    return P.Fail("the sketch doesn't close into loops: " + Sketch::Say(problems, Sketch::Namer(*lines)));
                fr->sketch = std::move(*lines);
                s.ModelMut().elements.Insert(*id, std::move(e));
                result = Json{ { "id", *id }, { "area_m2", M2(profile.Area()) } };
                return true;
            }
            if (const Roof* r = e.As<Roof>(); r && r->extrusion)
                return P.Fail(std::to_string(*id) + " is an extrusion roof: change its profile, `modify` its `extrusion`");
            const auto now = BoundaryNow(e);
            if (!now)
                return P.Fail(std::to_string(*id) + " isn't a floor, a ceiling, a roof's footprint or a filled region");
            std::optional<std::pair<Geom::Region, std::vector<SketchLine>>> sk;
            if (!Sketched(P, p, "", *now, sk))
                return false;
            if (!sk)
            {
                result = Json{ { "id", *id }, { "sketch", SketchJson(*now) } };
                return true;
            }
            auto& [r, lines] = *sk;
            const double area = r.Area();
            if (Floor* f = e.As<Floor>())
                std::tie(f->boundary, f->openings, f->sketch) = std::tuple{ r.outer, r.holes, lines };
            else if (Ceiling* x = e.As<Ceiling>())
                std::tie(x->boundary, x->openings, x->sketch) = std::tuple{ r.outer, r.holes, lines };
            else if (Roof* old = e.As<Roof>())
            {
                // 山墙留在新迹线上还有同一条边的地方，方向随意
                const std::vector<Segment> before = old->Edges();
                std::vector<Segment> kept;
                for (const size_t k : old->gables)
                    if (k < before.size())
                        kept.push_back(before[k]);
                old->boundary   = r.outer;
                old->courtyards = r.holes;
                old->sketch     = lines;
                old->gables.clear();
                const std::vector<Segment> after = old->Edges();
                for (size_t k = 0; k < after.size(); ++k)
                    for (const Segment& was : kept)
                        if ((after[k].first.Dist(was.first) <= Tol && after[k].second.Dist(was.second) <= Tol) ||
                            (after[k].first.Dist(was.second) <= Tol && after[k].second.Dist(was.first) <= Tol))
                        {
                            old->gables.push_back(k);
                            break;
                        }
            }
            s.ModelMut().elements.Insert(*id, std::move(e));
            result = Json{ { "id", *id }, { "area_m2", M2(area) } };
            return true;
        }

        bool SketchCmd(Session&, const Json& p, Json& result, std::string& error)
        {
            const char* c = "sketch";
            Params P(c, p, error);
            std::optional<std::vector<SketchLine>> lines;
            if (!SketchParam(P, p, "lines", {}, lines))
                return false;
            if (!lines)
                return P.Fail("`lines` [[[x,y],[x,y]],…] is required");
            Sketch::Profile profile;
            std::vector<Sketch::Problem> problems;
            if (!Sketch::Solve(*lines, profile, problems))
                return P.Fail("the lines don't make closed loops: " + Sketch::Say(problems, Sketch::Namer(*lines)));
            auto RingJson = [](const Ring& ring)
            {
                Json out = Json::array();
                for (const P2& q : ring)
                    out.push_back(ToJson(q));
                return out;
            };
            Json regions = Json::array();
            for (const Geom::Region& r : profile.regions)
            {
                Json holes = Json::array();
                for (const Ring& h : r.holes)
                    holes.push_back(RingJson(h));
                regions.push_back(Json{ { "outline", RingJson(r.outer) }, { "holes", std::move(holes) }, { "area_m2", M2(r.Area()) } });
            }
            result = Json{ { "regions", std::move(regions) }, { "area_m2", M2(profile.Area()) } };
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
            std::optional<std::pair<Geom::Region, std::vector<SketchLine>>> sketched;
            if (!Sketched(P, p, "openings", {}, sketched))
                return false;
            Ring boundary;
            std::vector<Ring> found;
            if (sketched)
            {
                boundary = sketched->first.outer;
                found    = sketched->first.holes;
            }
            else if (P.Absent("boundary"))
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
            if (sketched)
                f.sketch = sketched->second;
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
            std::optional<std::pair<Geom::Region, std::vector<SketchLine>>> sketched;
            if (!Sketched(P, p, "openings", {}, sketched))
                return false;
            std::optional<P2> at;
            if (!P.PointOpt("point", at))
                return false;
            Ring boundary;
            std::vector<Ring> found;
            if (sketched)
            {
                boundary = sketched->first.outer;
                found    = sketched->first.holes;
            }
            else if (!P.Absent("boundary"))
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
            if (sketched)
                x.sketch = sketched->second;
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
            if (const auto wall = m.WallHolding(r, m.roomArea))
                return P.Fail("[" + Num(point.x) + ", " + Num(point.y) + "] lies inside wall " + m.Shown(*wall) +
                              "; place a room in the space between walls, not in a wall");
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
            std::optional<std::pair<Geom::Region, std::vector<SketchLine>>> sketched;
            if (!Sketched(P, p, "courtyards", {}, sketched))
                return false;
            if (sketched && extrusion)
                return P.Fail("an extrusion roof has no footprint to sketch");
            const bool given = !P.Absent("boundary") || sketched;
            std::optional<double> overhang;
            if (!P.Num("overhang", overhang))
                return false;
            Roof r;
            r.overhang = overhang.value_or(given || extrusion ? 0.0 : 500.0);
            std::vector<Ring> found;
            if (sketched)
            {
                r.boundary = sketched->first.outer;
                found      = sketched->first.holes;
                r.sketch   = sketched->second;
            }
            else if (given)
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

        // 命令要的柱（category 为 column、structuralcolumn 或 footing），还没放进模型：工具预览一次单击会放下的
        bool StoodOfImpl(const Model& m, const Json& p, const std::string& category, Element& out, std::string& error)
        {
            const char* c = CommandOf(category);
            Params P(c, p, error);
            Column col;
            std::optional<double> angle, height, baseOffset;
            if (!P.PointReq("at", col.at) || !LevelParam(c, m, p, "level", col.level, error) || !TypeParam(P, m, category.c_str(), col.typeName) ||
                !P.Num("angle", angle))
                return false;
            // 独立基础是平放的柱：厚 thickness，顶面高出标高 offset
            if (category == "footing")
            {
                std::optional<double> thickness, top;
                if (!P.Num("thickness", thickness) || !P.Num("offset", top))
                    return false;
                const double th = thickness.value_or(400.0);
                if (th <= 0.0)
                    return P.Fail("`thickness` must be above zero, got " + Num(th));
                col.angle      = angle.value_or(0.0);
                col.height     = th;
                col.baseOffset = top.value_or(0.0) - th;
                out = Element(Footing{ std::move(col) });
                return true;
            }
            if (!P.Num("height", height) || !P.Num("base_offset", baseOffset))
                return false;
            // 到上一个标高，或给了 height 时那么高，或到 top_level
            if (P.Absent("top_level"))
            {
                if (!height)
                    col.topLevel = m.LevelAbove(col.level);
            }
            else
            {
                Guid t;
                if (!LevelParam(c, m, p, "top_level", t, error))
                    return false;
                col.topLevel = t;
            }
            col.angle      = angle.value_or(0.0);
            col.height     = height.value_or(DefaultHeight);
            col.baseOffset = baseOffset.value_or(0.0);
            if (category == "column")
                out = Element(std::move(col));
            else
                out = Element(StructuralColumn{ std::move(col) });
            return true;
        }

        bool ColumnCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            Element e;
            if (!StoodOfImpl(s.GetModel(), p, "column", e, error))
                return false;
            result = Json{ { "id", s.ModelMut().Insert(std::move(e)) } };
            return true;
        }

        bool StructuralColumnCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            Element e;
            if (!StoodOfImpl(s.GetModel(), p, "structuralcolumn", e, error))
                return false;
            result = Json{ { "id", s.ModelMut().Insert(std::move(e)) } };
            return true;
        }

        bool FootingCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            Element e;
            if (!StoodOfImpl(s.GetModel(), p, "footing", e, error))
                return false;
            result = Json{ { "id", s.ModelMut().Insert(std::move(e)) } };
            return true;
        }

        // 命令要的 run（类别 category：梁、条形基础……），points 每一段一个，还没放进模型：工具预览一次单击会放下的
        bool RunsOfImpl(const Model& m, const Json& p, const std::string& category, std::vector<Element>& out, std::string& error)
        {
            const char* c = CommandOf(category);
            Params P(c, p, error);
            std::vector<P2> pts;
            bool closed = false;
            if (!P.Points("points", pts) || !P.Flag("closed", closed))
                return false;
            if (pts.size() < 2)
                return P.Fail("`points` needs at least 2 points");
            if (closed)
                pts.push_back(pts.front());
            Run r;
            std::optional<double> offset, endOffset;
            std::optional<std::string> justify, system;
            if (!LevelParam(c, m, p, "level", r.level, error) || !TypeParam(P, m, category.c_str(), r.typeName) || !P.Num("offset", offset) ||
                !P.Num("end_offset", endOffset) || !P.String("justify", justify) || !P.String("system", system))
                return false;
            if (endOffset && pts.size() != 2)
                return P.Fail("`end_offset` is the height of one run's end: give 2 points");
            r.justify = Runs::DefaultJustify(category);
            if (justify)
            {
                const auto j = JustifyOf::FromName(*justify);
                if (!j)
                    return P.Fail("`justify` is top, centre or bottom, not `" + *justify + "`");
                r.justify = *j;
            }
            r.offset    = offset.value_or(0.0);
            r.endOffset = endOffset;
            r.system    = system.value_or("");
            out.clear();
            for (size_t i = 0; i + 1 < pts.size(); ++i)
            {
                Run one   = r;
                one.start = pts[i];
                one.end   = pts[i + 1];
                const auto e = Element::OfRun(category, std::move(one));
                if (!e)
                    return P.Fail("`" + category + "` has no runs");
                out.push_back(*e);
            }
            return true;
        }

        bool Chain(Session& s, const Json& p, const char* category, Json& result, std::string& error)
        {
            std::vector<Element> made;
            if (!RunsOfImpl(s.GetModel(), p, category, made, error))
                return false;
            Json ids = Json::array();
            for (Element& e : made)
                ids.push_back(s.ModelMut().Insert(std::move(e)));
            result = Json{ { "ids", ids } };
            return true;
        }

        bool BeamCmd(Session& s, const Json& p, Json& r, std::string& e) { return Chain(s, p, "beam", r, e); }
        bool StripFootingCmd(Session& s, const Json& p, Json& r, std::string& e) { return Chain(s, p, "stripfooting", r, e); }
        bool PipeCmd(Session& s, const Json& p, Json& r, std::string& e) { return Chain(s, p, "pipe", r, e); }
        bool ConduitCmd(Session& s, const Json& p, Json& r, std::string& e) { return Chain(s, p, "conduit", r, e); }
        bool CableTrayCmd(Session& s, const Json& p, Json& r, std::string& e) { return Chain(s, p, "cabletray", r, e); }
        bool WireCmd(Session& s, const Json& p, Json& r, std::string& e) { return Chain(s, p, "wire", r, e); }
        bool DownConductorCmd(Session& s, const Json& p, Json& r, std::string& e) { return Chain(s, p, "downconductor", r, e); }
        bool GroundConductorCmd(Session& s, const Json& p, Json& r, std::string& e) { return Chain(s, p, "groundconductor", r, e); }
        bool CablingCmd(Session& s, const Json& p, Json& r, std::string& e) { return Chain(s, p, "cable", r, e); }

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
            else if (category == "column" || category == "structuralcolumn" || category == "footing")
            {
                auto& map = category == "column" ? m.types.column : category == "footing" ? m.types.footing : m.types.structuralColumn;
                std::optional<ColumnType> old;
                if (const auto it = map.find(*name); it != map.end())
                    old = it->second;
                if (!width && !old)
                    return need("width");
                bool round = old && old->round;
                if (!P.Absent("round") && !P.Flag("round", round))
                    return false;
                std::optional<double> depth;
                if (!P.Num("depth", depth))
                    return false;
                ColumnType ty;
                ty.width = width ? *width : old->width;
                ty.round = round;
                // 圆柱的进深即直径
                ty.depth = round ? ty.width : depth ? *depth : old ? old->depth : ty.width;
                map[*name] = ty;
                Types one;
                one.column[*name] = ty;
                t = Bim::ToJson(one)["column"][*name];
            }
            // 梁或别的 run 的：截面宽 width、高 depth，或圆的；用库里的一种材质做
            else if (IsRunCategory(category))
            {
                auto& map = m.types.runs[category];
                std::optional<RunType> old;
                if (const auto it = map.find(*name); it != map.end())
                    old = it->second;
                if (!width && !old)
                    return need("width");
                bool round = old && old->round;
                if (!P.Absent("round") && !P.Flag("round", round))
                    return false;
                std::optional<double> depth;
                std::optional<std::string> material;
                if (!P.Num("depth", depth) || !P.String("material", material))
                    return false;
                RunType ty;
                ty.width = width ? *width : old->width;
                ty.round = round;
                if (round)
                    ty.depth = ty.width;
                else if (depth)
                    ty.depth = *depth;
                else if (old)
                    ty.depth = old->depth;
                else
                    return need("depth");
                ty.material = material ? *material : old ? old->material : "";
                map[*name] = ty;
                Types one;
                one.runs[category][*name] = ty;
                t = Bim::ToJson(one)["runs"][category][*name];
            }
            else
                return P.Fail("`" + category + "` has no types here; use wall, floor, ceiling, door, window, column, structural column, footing, roof or a "
                              "run's category (beam, strip footing, pipe, conduit, cable tray, wire, down conductor, ground conductor, cable)");
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
            if (!P.Absent("transparency"))
            {
                const Json& v = *Get(p, "transparency");
                if (!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<int64_t>() > 100)
                    return P.Fail("`transparency` is a whole percent, 0 to 100, not " + Show(v));
                mat.transparency = static_cast<uint8_t>(v.get<int64_t>());
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

        // ── 体量（移植自 BIMCraft commands.rs 的 mass、mass_floors、…_by_face）──────────
        bool DeleteCmd(Session& s, const Json& p, Json& result, std::string& error);

        // 体量的底面积（平方米）、高（毫米）、体积（立方米）
        std::tuple<double, double, double> MassSize(const Model& m, const Mass& ms)
        {
            const auto [bottom, top] = m.MassSpan(ms);
            return { M2(std::abs(Geom::Area(ms.boundary))), top - bottom, std::round(m.MassVolume(ms, bottom, top) / 1e7) / 100.0 };
        }

        bool MassCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "mass";
            Params P(c, p, error);
            const Model& m = s.GetModel();
            Mass ms;
            std::vector<P2> boundary;
            if (!LevelParam(c, m, p, "level", ms.level, error) || !P.Points("boundary", boundary))
                return false;
            std::optional<std::vector<P2>> top;
            const bool hasTop = !P.Absent("top"), hasLean = !P.Absent("lean");
            if (hasTop && hasLean)
                return P.Fail("give `top`, an outline, or `lean`, how far the top moves sideways, not both");
            if (hasTop)
            {
                std::vector<P2> t;
                if (!P.Points("top", t))
                    return false;
                top = std::move(t);
            }
            else if (hasLean)
            {
                P2 by;
                if (!P.PointReq("lean", by))
                    return false;
                std::vector<P2> t;
                for (const P2& q : boundary)
                    t.push_back(q + by);
                top = std::move(t);
            }
            // 逆时针，顶部轮廓同向、点点对应
            if (Geom::Area(boundary) < 0.0)
            {
                std::reverse(boundary.begin(), boundary.end());
                if (top)
                    std::reverse(top->begin(), top->end());
            }
            std::optional<double> height, baseOffset;
            std::optional<std::string> name;
            if (!P.Num("height", height) || !P.Num("base_offset", baseOffset) || !P.String("name", name))
                return false;
            // 到上一个标高（同管井），或给了 height 时那么高，或到 top_level
            if (P.Absent("top_level"))
            {
                if (!height)
                    ms.topLevel = m.LevelAbove(ms.level);
            }
            else
            {
                Guid t;
                if (!LevelParam(c, m, p, "top_level", t, error))
                    return false;
                ms.topLevel = t;
            }
            ms.height     = height.value_or(DefaultHeight);
            ms.baseOffset = baseOffset.value_or(0.0);
            ms.name       = name ? *name : LanguageOf::Word(m.language, "Mass");
            ms.boundary   = std::move(boundary);
            if (top && *top != ms.boundary)
                ms.top = std::move(*top);
            const auto [area, h, volume] = MassSize(m, ms);
            const Id id = s.ModelMut().Insert(std::move(ms));
            result = Json{ { "id", id }, { "area_m2", area }, { "height", h }, { "volume_m3", volume } };
            return true;
        }

        // mass 指的体量
        bool MassOf(const char* c, const Model& m, const Json& p, Id& id, Mass& out, std::string& error)
        {
            if (Absent(p, "mass"))
                return Fail(error, c, "`mass`, the id of a mass, is required");
            Guid g;
            std::string why;
            if (!FromJson(p["mass"], g, why))
                return Fail(error, c, "`mass`: " + why);
            const auto found = m.elements.IdOf(g);
            const Element* e = found ? m.elements.Get(*found) : nullptr;
            if (!e || !e->As<Mass>())
                return Fail(error, c, m.Shown(g) + " isn't a mass");
            id  = *found;
            out = *e->As<Mass>();
            return true;
        }

        // made（一个 ID 或一列）里的元素在 key 下记上 value：它们的 ID
        std::vector<Id> MarkMade(Session& s, const Json& made, const char* key, const Json& value)
        {
            std::vector<Id> ids;
            if (made.is_array())
            {
                for (const Json& v : made)
                    if (v.is_number_integer())
                        ids.push_back(v.get<Id>());
            }
            else if (made.is_number_integer())
                ids.push_back(made.get<Id>());
            for (const Id id : ids)
                if (Element* e = s.ModelMut().elements.GetMut(id); e && e->ParamsMut())
                    (*e->ParamsMut())[key] = value;
            return ids;
        }

        // p 里 keys 给了的参数，交给做元素的命令
        Json Passing(const Json& p, std::initializer_list<const char*> keys, Json to)
        {
            for (const char* k : keys)
                if (!Absent(p, k))
                    to[k] = p[k];
            return to;
        }

        Json RingJsonOf(const Ring& ring)
        {
            Json a = Json::array();
            for (const P2& q : ring)
                a.push_back(ToJson(q));
            return a;
        }

        bool MassFloorsCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "mass_floors";
            Params P(c, p, error);
            std::vector<std::pair<Id, Mass>> masses;
            if (!Absent(p, "mass"))
            {
                Id id;
                Mass ms;
                if (!MassOf(c, s.GetModel(), p, id, ms, error))
                    return false;
                masses.emplace_back(id, std::move(ms));
            }
            else
                for (const auto& [id, e] : s.GetModel().elements.Items())
                    if (const Mass* ms = e->As<Mass>())
                        masses.emplace_back(id, *ms);
            if (masses.empty())
                return P.Fail("no mass; make one with `mass`");
            bool prune = false;
            std::optional<std::string> kind;
            if (!P.Flag("prune", prune) || !P.String("type", kind))
                return false;
            std::vector<Id> ids, made, updated, stale, removed;
            for (const auto& [mass, ms] : masses)
            {
                const Guid massGuid = s.GetModel().GuidOf(mass);
                const auto held     = Massing::FloorsOf(s.GetModel(), massGuid);
                const auto slices   = s.GetModel().MassSlices(ms);
                std::vector<Guid> levels;
                for (const auto& sl : slices)
                {
                    const Model& m   = s.GetModel();
                    const Guid level = m.GuidOf(sl.level);
                    levels.push_back(level);
                    // 体量在这个标高处的轮廓：底面轮廓，放样的在两个轮廓之间
                    const Ring ring = m.MassOutline(ms, sl.z);
                    const auto it = std::find_if(held.begin(), held.end(), [&](const auto& h) { return m.Same(h.second, level); });
                    if (it == held.end())
                    {
                        Json one;
                        if (!FloorCmd(s, Passing(p, { "type" }, Json{ { "boundary", RingJsonOf(ring) }, { "level", ToJson(level) } }), one, error))
                            return false;
                        for (const Id id : MarkMade(s, one["id"], Massing::FloorMark,
                                                    Json{ { "mass", ToJson(massGuid) }, { "level", ToJson(level) } }))
                        {
                            made.push_back(id);
                            ids.push_back(id);
                        }
                        continue;
                    }
                    const Id f = it->first;
                    ids.push_back(f);
                    // 轮廓（与类型）已经对的楼板不动，再跑一次不是一步撤销
                    const Floor* fl = m.elements.Get(f)->As<Floor>();
                    if (fl->boundary != ring || (kind && *kind != fl->typeName))
                    {
                        if (kind && !m.types.floor.count(*kind))
                            return P.Fail("floor type `" + *kind + "` doesn't exist");
                        Floor* mf = s.ModelMut().elements.GetMut(f)->As<Floor>();
                        mf->boundary = ring;
                        if (kind)
                            mf->typeName = *kind;
                        updated.push_back(f);
                    }
                }
                for (const auto& [f, level] : held)
                    if (std::none_of(levels.begin(), levels.end(), [&](const Guid& l) { return s.GetModel().Same(l, level); }))
                        (prune ? removed : stale).push_back(f);
            }
            if (!removed.empty())
            {
                Json gone;
                if (!DeleteCmd(s, Json{ { "ids", removed } }, gone, error))
                    return false;
            }
            result = Json{ { "ids", ids }, { "made", made }, { "updated", updated }, { "stale", stale }, { "removed", removed } };
            return true;
        }

        bool WallByFaceCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "wall_by_face";
            Id mass;
            Mass ms;
            if (!MassOf(c, s.GetModel(), p, mass, ms, error))
                return false;
            const Ring ring = ms.Footprint(), crown = ms.TopRing();
            const size_t n = ring.size();
            // 墙是竖直的，只立在顶边正在底边上方的面上
            std::set<size_t> faces;
            if (Absent(p, "faces"))
            {
                for (size_t f = 0; f < n; ++f)
                    if (Massing::Plumb(ring, crown, f))
                        faces.insert(f);
                if (faces.empty())
                    return Fail(error, c, "every face of the mass leans, and a wall stands plumb; none can stand on one");
            }
            else
            {
                const Json& a = p["faces"];
                if (!a.is_array() || a.size() > MaxPoints)
                    return Fail(error, c, "`faces` must be a list of edge numbers, got " + a.dump());
                for (const Json& v : a)
                {
                    if (!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<uint64_t>() >= n)
                        return Fail(error, c, "face " + v.dump() + " isn't an edge of the footprint: 0 to " + std::to_string(n == 0 ? 0 : n - 1) +
                                                  ", from its first point");
                    const size_t f = v.get<size_t>();
                    if (!Massing::Plumb(ring, crown, f))
                        return Fail(error, c, "face " + std::to_string(f) + " leans, and a wall stands plumb");
                    faces.insert(f);
                }
            }
            const Json top = ms.topLevel ? Json{ { "top_level", ToJson(*ms.topLevel) } } : Json{ { "height", ms.height } };
            const Guid massGuid = s.GetModel().GuidOf(mass);
            Json ids = Json::array();
            for (const size_t f : faces)
            {
                // 逆时针时体量在边的左边；墙的外侧在它的左边，所以反过来走
                const P2 a = ring[f], b = ring[(f + 1) % n];
                Json q{ { "points", Json::array({ ToJson(b), ToJson(a) }) }, { "level", ToJson(ms.level) }, { "base_offset", ms.baseOffset },
                        { "location", "exterior-finish" } };
                q.update(top);
                Json made;
                if (!WallCmd(s, Passing(p, { "type", "location" }, q), made, error))
                    return false;
                for (const Id id : MarkMade(s, made["ids"], Massing::Face, Json{ { "mass", ToJson(massGuid) }, { "face", f } }))
                    ids.push_back(id);
            }
            result = Json{ { "ids", ids } };
            return true;
        }

        // 体量的顶面在哪里：一个标高，高出它多少
        std::pair<Guid, double> TopFace(const Mass& ms)
        {
            return ms.topLevel ? std::pair{ *ms.topLevel, 0.0 } : std::pair{ ms.level, ms.baseOffset + ms.height };
        }

        bool FloorByFaceCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            Id mass;
            Mass ms;
            if (!MassOf("floor_by_face", s.GetModel(), p, mass, ms, error))
                return false;
            const auto [level, offset] = TopFace(ms);
            if (!FloorCmd(s, Passing(p, { "type" }, Json{ { "boundary", RingJsonOf(ms.TopRing()) }, { "level", ToJson(level) }, { "offset", offset } }),
                          result, error))
                return false;
            MarkMade(s, result["id"], Massing::Face, Json{ { "mass", ToJson(s.GetModel().GuidOf(mass)) }, { "face", "top" } });
            return true;
        }

        bool RoofByFaceCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            Id mass;
            Mass ms;
            if (!MassOf("roof_by_face", s.GetModel(), p, mass, ms, error))
                return false;
            const auto [level, offset] = TopFace(ms);
            const Json q{ { "boundary", RingJsonOf(ms.TopRing()) }, { "level", ToJson(level) }, { "offset", offset }, { "slope", 0 }, { "overhang", 0 } };
            if (!RoofCmd(s, Passing(p, { "type", "slope", "overhang" }, q), result, error))
                return false;
            MarkMade(s, result["id"], Massing::Face, Json{ { "mass", ToJson(s.GetModel().GuidOf(mass)) }, { "face", "top" } });
            return true;
        }

        bool UpdateByFaceCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "update_by_face";
            const Model& m = s.GetModel();
            const bool hasIds = !Absent(p, "ids") || !Absent(p, "id"), hasMass = !Absent(p, "mass");
            if (hasIds && hasMass)
                return Fail(error, c, "give `ids` or `mass`, not both");
            std::vector<Id> ids;
            if (hasMass)
            {
                Id mass;
                Mass ms;
                if (!MassOf(c, m, p, mass, ms, error))
                    return false;
                for (const auto& [id, e] : m.elements.Items())
                    if (const auto f = Massing::FaceOf(*e); f && m.Same(f->first, m.GuidOf(mass)))
                        ids.push_back(id);
            }
            else if (hasIds)
            {
                if (!IdsParam(c, m, p, ids, error))
                    return false;
                for (const Id id : ids)
                    if (!Massing::FaceOf(*m.elements.Get(id)))
                        return Fail(error, c, std::to_string(id) + " isn't made by face; `wall_by_face`, `floor_by_face` and `roof_by_face` make them");
            }
            else
                for (const auto& [id, e] : m.elements.Items())
                    if (Massing::FaceOf(*e))
                        ids.push_back(id);
            if (ids.empty())
                return Fail(error, c,
                            "nothing is made by face: `wall_by_face`, `floor_by_face` and `roof_by_face` make walls, floors and roofs that follow a mass");
            std::vector<std::pair<Id, Element>> changed;
            std::vector<Id> same;
            std::vector<std::string> gone;
            for (const Id id : ids)
            {
                const Element* e = m.elements.Get(id);
                const auto face  = Massing::FaceOf(*e);
                if (!face)
                    continue;
                Element now = *e;
                const auto mid = m.elements.IdOf(face->first);
                const Element* me = mid ? m.elements.Get(*mid) : nullptr;
                if (!me || !me->As<Mass>())
                {
                    gone.push_back(std::string(e->Category()) + " " + std::to_string(id) + " was made from mass " + m.Shown(face->first) +
                                   ", which isn't there");
                    continue;
                }
                std::string why;
                if (!Massing::Follow(now, *me->As<Mass>(), *mid, face->second, why))
                    gone.push_back(std::string(e->Category()) + " " + std::to_string(id) + " " + why);
                else if (now == *e)
                    same.push_back(id);
                else
                    changed.emplace_back(id, std::move(now));
            }
            if (!gone.empty())
            {
                std::string all;
                for (const std::string& g : gone)
                    all += (all.empty() ? "" : "; ") + g;
                return Fail(error, c, all);
            }
            std::vector<Id> updated;
            for (auto& [id, e] : changed)
            {
                updated.push_back(id);
                s.ModelMut().elements.Insert(id, std::move(e));
            }
            result = Json{ { "updated", updated }, { "unchanged", same } };
            return true;
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
                    if ((k == "level" || k == "top_level" || (k == "view" && e->IsAnnotation())) && val.is_string())
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

        // 命令怎样带动元素：经 map（平移、旋转或镜像）。平移时单独选中的门窗只沿墙滑动 slide 的那部分；
        // mirror 时是镜像：墙反过来走（外侧仍在镜像过去的那边），楼板、天花板的轮廓反向，构件翻过来，门换到另一侧门框
        struct Motion
        {
            std::function<P2(P2)> map;
            std::optional<P2>     slide;
            bool                  mirror = false;
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
            // 草图随元素走，线保留 ID；镜像时圆弧反过来转
            if (std::vector<SketchLine>* lines = out.SketchOf())
                for (SketchLine& l : *lines)
                {
                    l.start = f(l.start);
                    l.end   = f(l.end);
                    if (mo.mirror)
                        l.arc = -l.arc + 0.0;
                }
            if (Wall* w = out.As<Wall>())
            {
                const P2 a = f(w->start), b = f(w->end);
                w->start = mo.mirror ? b : a;
                w->end   = mo.mirror ? a : b;
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
                // 镜像让轮廓变成顺时针；楼板的轮廓保持逆时针
                if (mo.mirror)
                    std::reverse(fl->boundary.begin(), fl->boundary.end());
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
                if (mo.mirror)
                    std::reverse(cl->boundary.begin(), cl->boundary.end());
            }
            else if (Room* r = out.As<Room>())
                r->point = f(r->point);
            // 顶部轮廓同向、点点对应
            else if (Mass* ms = out.As<Mass>())
            {
                for (Ring* ring : { &ms->boundary, ms->top ? &*ms->top : nullptr })
                {
                    if (!ring)
                        continue;
                    for (P2& q : *ring)
                        q = f(q);
                    if (mo.mirror)
                        std::reverse(ring->begin(), ring->end());
                }
            }
            else if (Column* col = out.AsColumn())
                std::tie(col->at, col->angle) = Turned(f, col->at, col->angle);
            else if (Run* run = out.AsRun())
            {
                run->start = f(run->start);
                run->end   = f(run->end);
            }
            // 镜像是族翻过来
            else if (Component* cp = out.As<Component>())
            {
                std::tie(cp->at, cp->angle) = Turned(f, cp->at, cp->angle);
                cp->mirrored = cp->mirrored != mo.mirror;
            }
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
                {
                    for (P2& q : r->extrusion->line)
                        q = f(q);
                    // 镜像过去扫向另一边
                    if (mo.mirror)
                        std::tie(r->extrusion->from, r->extrusion->to) = std::pair{ -r->extrusion->to + 0.0, -r->extrusion->from + 0.0 };
                }
            }
            else if (Note* n = out.As<Note>())
            {
                // 镜像过去的文字仍然正着读
                const auto [at, angle] = Turned(f, n->at, n->angle);
                n->at = at;
                if (!mo.mirror)
                    n->angle = angle;
                if (n->leader)
                    n->leader = f(*n->leader);
            }
            else if (Spot* sp = out.As<Spot>())
                sp->at = f(sp->at);
            else if (DetailLine* l = out.As<DetailLine>())
                l->curve = l->curve.Map([&](P2 q) { return Micron(f(q)); }, mo.mirror);
            // 草图在上面跟着走了，图案也跟着
            else if (FilledRegion* fr = out.As<FilledRegion>())
                std::tie(fr->patternOrigin, fr->patternAngle) = Turned(f, fr->patternOrigin, fr->patternAngle);
            else if (Dimension* d = out.As<Dimension>())
                d->at = f(d->at);
            // 标记的位置跟着它的宿主：见 Relocate 与 Duplicate
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

        // 选中的标记的头被 mo 带到哪里
        std::vector<std::pair<Id, P2>> TagHeads(const Model& m, const std::vector<Id>& ids, const Motion& mo)
        {
            std::vector<std::pair<Id, P2>> out;
            for (const Id id : ids)
                if (const Tag* t = m.elements.Get(id)->As<Tag>())
                    if (const auto a = m.Anchor(t->host))
                        out.emplace_back(id, mo.map(*a + t->offset));
            return out;
        }

        // 标记 id 的头放到 head，不管它的宿主怎样动了
        void PlaceTag(Model& m, Id id, P2 head)
        {
            const Element* e = m.elements.Get(id);
            const Tag* t = e ? e->As<Tag>() : nullptr;
            const auto a = t ? m.Anchor(t->host) : std::nullopt;
            if (a)
                m.elements.GetMut(id)->As<Tag>()->offset = Micron(head - *a);
        }

        bool Relocate(const char* c, Session& s, const std::vector<Id>& ids, const Motion& mo, std::optional<bool> end, std::string& error)
        {
            const Model& m = s.GetModel();
            const auto& f = mo.map;
            const std::set<Id> moving(ids.begin(), ids.end());
            std::set<Guid> movingGuids;
            for (const Id id : ids)
                movingGuids.insert(m.GuidOf(id));
            const auto heads = TagHeads(m, ids, mo);
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
                // 镜像过去墙反着走：门挂到另一侧门框，仍朝同一边开
                if (mo.mirror && carried(o->host))
                    n->flipHand = !n->flipHand;
                const Id oid = id;
                others.erase(std::remove_if(others.begin(), others.end(), [oid](const auto& x) { return x.first == oid; }), others.end());
                others.emplace_back(oid, std::move(copy));
            }
            Model& mm = s.ModelMut();
            for (auto& [id, w] : walls)
                mm.elements.Insert(id, std::move(w));
            std::vector<Id> flipped;
            for (auto& [id, e] : others)
            {
                if (mo.mirror && e.As<Mass>())
                    flipped.push_back(id);
                mm.elements.Insert(id, std::move(e));
            }
            // 镜像的体量轮廓反着走：按面做的墙记着的边重新编号
            for (const Id id : flipped)
                Massing::Mirrored(mm, id);
            // 尺寸量着镜像过去的墙上同样的线：墙反着走，起点终点对调，它的门窗的两边也对调
            if (mo.mirror && !end)
            {
                std::map<Guid, Guid> hostOf;
                std::vector<Id> dims;
                for (const auto& [id, e] : mm.elements.Items())
                {
                    if (const Opening* o = e->AsOpening())
                        hostOf[mm.GuidOf(id)] = mm.GuidOf(o->host);
                    if (e->As<Dimension>())
                        dims.push_back(id);
                }
                const auto turned = [&](Guid g)
                {
                    const auto h = hostOf.find(g);
                    return movingGuids.count(g) || (h != hostOf.end() && movingGuids.count(h->second));
                };
                for (const Id id : dims)
                    for (Reference& r : mm.elements.GetMut(id)->As<Dimension>()->refs)
                        if (turned(mm.GuidOf(r.id)))
                            r.face = FaceOf::Mirrored(r.face);
            }
            for (const auto& [id, head] : heads)
                PlaceTag(mm, id, head);
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
            // 尺寸、标记、高程点最后复制：它们可能指着副本
            std::vector<std::pair<Id, Element>> notes;
            const auto headList = TagHeads(m, ids, mo);
            const std::map<Id, P2> heads(headList.begin(), headList.end());
            for (const Id id : ids)
            {
                Element e;
                if (!Transformed(c, m, id, mo, copying, e, error))
                    return false;
                // 复制的墙带上它的门窗，见下
                if (const Opening* o = e.AsOpening(); o && copyingGuids.count(o->host))
                    continue;
                if (e.As<Dimension>() || e.As<Tag>() || e.As<Spot>())
                    notes.emplace_back(id, std::move(e));
                else
                    copies.emplace_back(id, std::move(e));
            }
            for (const auto& [id, e] : m.elements.Items())
                if (const Opening* o = e->AsOpening(); o && copyingGuids.count(o->host))
                {
                    Element copy = *e;
                    // 镜像的副本反着走：从另一端量，挂在另一侧门框
                    if (mo.mirror)
                    {
                        Opening* n = copy.AsOpening();
                        if (const Wall* w = m.GetWall(n->host))
                            n->offset = Walls::Axis::Of(*w).Length() - n->offset;
                        n->flipHand = !n->flipHand;
                    }
                    copies.emplace_back(id, std::move(copy));
                }
            Model& mm = s.ModelMut();
            std::vector<Id> made;
            std::map<Guid, Guid> hosts;
            for (auto& [from, e] : copies)
            {
                if (Grid* g = e.As<Grid>())
                    g->name = NextGridName(mm);
                else if (Room* r = e.As<Room>())
                {
                    std::vector<std::string> numbers;
                    for (const auto& [_, x] : mm.elements.Items())
                        if (const Room* rr = x->As<Room>())
                            numbers.push_back(rr->number);
                    r->number = NextNumber(numbers);
                }
                else if (Opening* o = e.AsOpening())
                {
                    if (const auto it = hosts.find(o->host); it != hosts.end())
                        o->host = it->second;
                    o->mark = NextNumber(Marks(mm, e.IsDoor()));
                }
                if (auto* params = e.ParamsMut())
                    Massing::Detach(*params);
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
            // 复制的尺寸、标记、高程点挂到它们量的、写的、读的东西的副本上（有的话）
            const auto copied = [&](Guid g)
            {
                const auto it = hosts.find(mm.GuidOf(g));
                return it != hosts.end() ? std::optional<Guid>(it->second) : std::nullopt;
            };
            for (auto& [from, e] : notes)
            {
                if (Dimension* d = e.As<Dimension>())
                {
                    for (Reference& r : d->refs)
                        if (const auto to = copied(r.id))
                        {
                            r.id = *to;
                            if (mo.mirror)
                                r.face = FaceOf::Mirrored(r.face);
                        }
                }
                else if (Tag* t = e.As<Tag>())
                    t->host = copied(t->host).value_or(t->host);
                else if (Spot* sp = e.As<Spot>(); sp && sp->host)
                    sp->host = copied(*sp->host).value_or(*sp->host);
                const Id id = mm.Insert(std::move(e));
                if (const auto h = heads.find(from); h != heads.end())
                    PlaceTag(mm, id, h->second);
                made.push_back(id);
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

        P2 Rotated(P2 q, P2 about, double angle)
        {
            const double r = angle * kPi / 180.0, sn = std::sin(r), cs = std::cos(r);
            const P2 d = q - about;
            return Micron(about + P2(d.x * cs - d.y * sn, d.x * sn + d.y * cs));
        }

        bool RotateCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "rotate";
            Params P(c, p, error);
            std::vector<Id> ids;
            P2 about;
            std::optional<double> angle;
            bool copy = false;
            if (!IdsParam(c, s.GetModel(), p, ids, error) || !P.PointReq("about", about) || !P.Num("angle", angle) || !P.Flag("copy", copy))
                return false;
            if (!angle)
                return P.Fail("`angle` in degrees, counter-clockwise, is required");
            const double a = *angle;
            const Motion mo{ [about, a](P2 q) { return Rotated(q, about, a); }, std::nullopt, false };
            if (copy)
                return Duplicate(c, s, ids, mo, result, error);
            if (!Relocate(c, s, ids, mo, std::nullopt, error))
                return false;
            result = Json{ { "rotated", ids } };
            return true;
        }

        bool MirrorCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "mirror";
            Params P(c, p, error);
            std::vector<Id> ids;
            std::vector<P2> axis;
            bool copy = false;
            if (!IdsParam(c, s.GetModel(), p, ids, error) || !P.Points("axis", axis) || !P.Flag("copy", copy))
                return false;
            if (axis.size() != 2 || !(axis[0].Dist(axis[1]) > Tol))
                return P.Fail("`axis` is two different points [[x, y], [x, y]]");
            const P2 a = axis[0], u = (axis[1] - axis[0]).Unit();
            const Motion mo{ [a, u](P2 q) { const P2 d = q - a; return Micron(a + u * (2.0 * d.Dot(u)) - d); }, std::nullopt, true };
            if (copy)
                return Duplicate(c, s, ids, mo, result, error);
            if (!Relocate(c, s, ids, mo, std::nullopt, error))
                return false;
            result = Json{ { "mirrored", ids } };
            return true;
        }

        bool CornerCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "corner";
            Params P(c, p, error);
            std::vector<Id> ids;
            if (!IdsParam(c, s.GetModel(), p, ids, error))
                return false;
            const Model& m = s.GetModel();
            if (ids.size() != 2 || !m.GetWall(ids[0]) || !m.GetWall(ids[1]))
                return P.Fail("`ids` takes two walls");
            const Wall wa = *m.GetWall(ids[0]), wb = *m.GetWall(ids[1]);
            if (wa.arc != 0.0 || wb.arc != 0.0)
                return P.Fail("it trims and extends straight walls; a curved one changes by its ends or its `arc`");
            std::vector<P2> keep;
            if (!P.Absent("keep") && !P.Points("keep", keep))
                return false;
            if (!keep.empty() && keep.size() != 2)
                return P.Fail("`keep` is a point on each wall, [[x, y], [x, y]]");
            const auto x = Geom::LineIntersection(wa.start, wa.end, wb.start, wb.end);
            if (!x)
                return P.Fail("the walls are parallel: they never meet");
            const P2 hit = Micron(*x);
            for (size_t i = 0; i < 2; ++i)
            {
                const Wall* now = s.GetModel().GetWall(ids[i]);
                if (!now)
                    continue;
                const Wall w = *now;
                const P2 u = (w.end - w.start).Unit();
                auto at = [&](P2 q) { return (q - w.start).Dot(u); };
                // 去拐角的那一端：离保留的那部分远的一端，否则离拐角近的一端
                const bool start = !keep.empty() ? at(keep[i]) > at(hit) : hit.Dist(w.start) < hit.Dist(w.end);
                const P2 by = hit - (start ? w.start : w.end);
                if (by.Length() > 1e-6 && !Relocate(c, s, { ids[i] }, Motion{ [by](P2 q) { return q + by; }, by, false }, start, error))
                    return false;
            }
            result = Json{ { "corner", ToJson(hit) } };
            return true;
        }

        bool SplitCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "split";
            Params P(c, p, error);
            const Model& m = s.GetModel();
            std::optional<Guid> g;
            P2 at;
            if (!P.IdOpt("id", g))
                return false;
            if (!g)
                return P.Fail("`id`, a wall, is required");
            const Wall* found = m.GetWall(*g);
            if (!found)
                return P.Fail(g->ToString() + " is not a wall");
            if (!P.PointReq("at", at))
                return false;
            const Wall w = *found;
            const Id id = *m.elements.IdOf(*g);
            const Walls::Axis axis = Walls::Axis::Of(w);
            if (axis.Distance(at, 0.0) > m.Thickness(w) / 2.0 + 100.0)
                return P.Fail("[" + Num(at.x) + ", " + Num(at.y) + "] isn't on wall " + std::to_string(id));
            const double len = axis.Length();
            // 直墙沿它取到微米；弧墙按它的比例
            const double along = w.arc == 0.0 ? std::round(axis.Project(at).first * 1e3) / 1e3 : axis.Project(at).first;
            if (along <= Tol || along >= len - Tol)
                return P.Fail("split between the wall's ends");
            const P2 cut = Micron(axis.At(along, 0.0));
            // 门窗随它所在的那一段；跨在切口上的挡着
            std::vector<Id> later;
            for (const auto& [oid, e] : m.elements.Items())
            {
                const Opening* o = e->AsOpening();
                if (!o || !m.Same(o->host, *g))
                    continue;
                const auto t = m.OpeningTypeOf(*e);
                const double half = t ? t->width / 2.0 : 0.0;
                if (o->offset + half <= along + Tol)
                    continue;
                if (o->offset - half < along - Tol)
                    return P.Fail(std::string(e->Category()) + " " + std::to_string(oid) + " is in the way; split elsewhere");
                later.push_back(oid);
            }
            Wall first = w, second = w;
            first.end    = cut;
            second.start = cut;
            Massing::Detach(first.params);
            Massing::Detach(second.params);
            // 弧按长度分转角
            first.arc  = std::round(first.arc * along / len * 1e9) / 1e9;
            second.arc = std::round((w.arc - first.arc) * 1e9) / 1e9;
            Model& mm = s.ModelMut();
            mm.elements.Insert(id, Element{ first });
            const Id made = mm.Insert(Element{ second });
            const Guid host = mm.GuidOf(made);
            for (const Id oid : later)
            {
                Opening* o = mm.elements.GetMut(oid)->AsOpening();
                o->host = host;
                o->offset -= along;
            }
            result = Json{ { "ids", Json::array({ id, made }) } };
            return true;
        }

        bool AlignCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "align";
            Params P(c, p, error);
            std::optional<Guid> g;
            if (!P.IdOpt("id", g))
                return false;
            if (!g)
                return P.Fail("`id` is required");
            std::vector<P2> reference, line;
            if (!P.Points("reference", reference) || !P.Points("line", line))
                return false;
            if (reference.size() != 2)
                return P.Fail("`reference` must contain two points");
            if (line.size() != 2)
                return P.Fail("`line` must contain two points");
            Align::Placement pl;
            std::string why;
            if (!Align::Placement::Make({ reference[0], reference[1] }, { line[0], line[1] }, pl, why))
                return Fail(error, c, why);
            const Model& m = s.GetModel();
            const auto id = m.elements.IdOf(*g);
            if (!id)
                return P.Fail("no element " + g->ToString());
            if (Align::Lines(m, *id).empty())
                return P.Fail("element " + std::to_string(*id) + " has no straight alignment line in plan");
            const Element* e = m.elements.Get(*id);
            if (const Opening* o = e->AsOpening())
                if (const Wall* w = m.GetWall(o->host); w && (std::abs(pl.angle) > 1e-9 || std::abs(pl.by.Cross((w->end - w->start).Unit())) > Tol))
                    return P.Fail("a door or window can only slide along its host wall; align its jambs, or align the wall");
            const std::optional<P2> slide = std::abs(pl.angle) <= 1e-9 ? std::optional<P2>(pl.by) : std::nullopt;
            if (!Relocate(c, s, { *id }, Motion{ [pl](P2 q) { return pl.Map(q); }, slide, false }, std::nullopt, error))
                return false;
            result = Json{ { "aligned", *id }, { "angle", pl.angle }, { "by", ToJson(pl.by) } };
            return true;
        }

        // 名字列表，报错里用：" (there are: a, b)"
        std::string KnownNames(const std::vector<std::string>& names)
        {
            std::string out;
            for (const std::string& n : names)
                out += (out.empty() ? "" : ", ") + n;
            return out.empty() ? std::string() : " (there are: " + out + ")";
        }

        // 参数说的库条目：类别与类型（category、type）、族（family）、族的类型（family、type，库里写作"族: 类型"）
        bool EntryOf(Params& P, const Model& m, Kind& kind, std::string& name)
        {
            std::optional<std::string> category, family, type;
            if (!P.String("category", category) || !P.String("family", family) || !P.String("type", type))
                return false;
            if (category && !family && type)
            {
                const auto names = m.TypeNames(*category);
                if (std::find(names.begin(), names.end(), *type) == names.end())
                    return P.Fail("there is no " + *category + " type `" + *type + "`" + KnownNames(names));
                kind = Kind::Type(*category);
                name = *type;
                return true;
            }
            if (!category && family)
            {
                const auto f = m.families.find(*family);
                if (f == m.families.end())
                {
                    std::vector<std::string> names;
                    for (const auto& [n, _] : m.families)
                        names.push_back(n);
                    return P.Fail("there is no family `" + *family + "`" + KnownNames(names));
                }
                if (!type)
                {
                    kind = Kind::Of(Kind::Tag::Family);
                    name = *family;
                    return true;
                }
                if (!f->second.types.count(*type))
                {
                    std::vector<std::string> names;
                    for (const auto& [n, _] : f->second.types)
                        names.push_back(n);
                    return P.Fail("family `" + *family + "` has no type `" + *type + "`" + KnownNames(names));
                }
                kind = Kind::Of(Kind::Tag::FamilyType);
                name = *family + ": " + *type;
                return true;
            }
            return P.Fail("give `category` and `type`, or `family`, or `family` and `type`");
        }

        // 每个库条目被引用了几次：元素的类型与族、类型的层的材质……没被引用的是没用的
        std::map<std::pair<Kind, std::string>, size_t> LibraryUses(const Model& m)
        {
            std::map<std::pair<Kind, std::string>, size_t> uses;
            Model copy = m;
            copy.LibRefsMut([&](const Kind& k, std::string& n) { ++uses[{ k, n }]; });
            return uses;
        }

        // 一类类型的表：类别的
        template <class F>
        bool WithTypes(Model& m, const std::string& category, F f)
        {
            if (category == "wall") return f(m.types.wall);
            if (category == "floor") return f(m.types.floor);
            if (category == "door") return f(m.types.door);
            if (category == "window") return f(m.types.window);
            if (category == "column") return f(m.types.column);
            if (category == "roof") return f(m.types.roof);
            if (category == "ceiling") return f(m.types.ceiling);
            if (category == "structuralcolumn") return f(m.types.structuralColumn);
            if (category == "footing") return f(m.types.footing);
            if (IsRunCategory(category))
            {
                const auto it = m.types.runs.find(category);
                return it != m.types.runs.end() && f(it->second);
            }
            return false;
        }

        bool DeleteTypeCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "delete_type";
            Params P(c, p, error);
            const Model& m = s.GetModel();
            Kind kind;
            std::string name;
            if (!EntryOf(P, m, kind, name))
                return false;
            const std::string label = kind.Label();
            const auto uses = LibraryUses(m);
            if (const auto u = uses.find({ kind, name }); u != uses.end() && u->second > 0)
                return P.Fail(label + " `" + name + "` is used " + std::to_string(u->second) + " time" + (u->second == 1 ? "" : "s") +
                              ": change or delete what uses it first");
            if (kind.tag == Kind::Tag::Type && m.TypeNames(kind.category).size() <= 1)
                return P.Fail("`" + name + "` is the last " + label + ": a " + kind.category + " needs one; make another first");
            const size_t colon = name.find(": ");
            if (kind.tag == Kind::Tag::FamilyType && m.families.at(name.substr(0, colon)).types.size() <= 1)
                return P.Fail("`" + name + "` is its family's last type: delete the family");
            Model& mm = s.ModelMut();
            if (kind.tag == Kind::Tag::Type)
                WithTypes(mm, kind.category, [&](auto& t) { return t.erase(name) > 0; });
            else if (kind.tag == Kind::Tag::Family)
                mm.families.erase(name);
            else
                mm.families.at(name.substr(0, colon)).types.erase(name.substr(colon + 2));
            result = Json{ { "deleted", Json{ { "kind", label }, { "name", name } } } };
            return true;
        }

        bool RenameTypeCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "rename_type";
            Params P(c, p, error);
            if (p.is_object())
                for (auto it = p.begin(); it != p.end(); ++it)
                    if (it.key() != "category" && it.key() != "family" && it.key() != "type" && it.key() != "name")
                        return P.Fail("`" + it.key() + "` isn't one of its parameters: category, family, type, name");
            std::optional<std::string> given;
            if (!P.String("name", given))
                return false;
            const std::string to = given ? std::string(Text::Trim(*given)) : std::string();
            if (to.empty())
                return P.Fail("`name` is required: the type's new name");
            Kind kind;
            std::string from;
            if (!EntryOf(P, s.GetModel(), kind, from))
                return false;
            if (kind.tag == Kind::Tag::Family)
                return P.Fail("give the family's `type` to rename; families rename with `family` and `rename`");
            const std::string label = kind.Label();
            // 族的类型在库里是"族: 类型"
            const size_t colon = from.find(": ");
            const std::string toEntry = kind.tag == Kind::Tag::FamilyType ? from.substr(0, colon) + ": " + to : to;
            const std::string shownFrom = kind.tag == Kind::Tag::FamilyType ? from.substr(colon + 2) : from;
            if (toEntry == from)
            {
                result = Json{ { "renamed", Json{ { "kind", label }, { "from", shownFrom }, { "to", to } } } };
                return true;
            }
            for (const auto& [k, n] : s.GetModel().LibraryNames())
                if (k == kind && n == toEntry)
                    return P.Fail("there's already a " + label + " `" + to + "`");
            Model& m = s.ModelMut();
            auto move = [&](auto& table, const std::string& a, const std::string& b)
            {
                auto node = table.extract(a);
                if (node.empty())
                    return false;
                node.key() = b;
                table.insert(std::move(node));
                return true;
            };
            const bool moved = kind.tag == Kind::Tag::Type ? WithTypes(m, kind.category, [&](auto& t) { return move(t, from, to); })
                                                           : move(m.families.at(from.substr(0, colon)).types, shownFrom, to);
            if (!moved)
                return P.Fail("there is no " + label + " `" + from + "`");
            // 库里的条目保留 GUID；引用它的元素跟着改名
            m.library.Renamed(kind, from, toEntry);
            m.LibRefsMut([&](const Kind& k, std::string& n)
            {
                if (k == kind && n == from)
                    n = toEntry;
            });
            result = Json{ { "renamed", Json{ { "kind", label }, { "from", shownFrom }, { "to", to } } } };
            return true;
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
                    const auto in = [&](const std::optional<Guid>& x) { return x && gone.count(m.GuidOf(*x)); };
                    bool hangs = false;
                    if (const Opening* o = e->AsOpening())
                        hangs = gone.count(m.GuidOf(o->host)) > 0;
                    // 注释跟着它在的视图（标高的平面）走，标记跟着它的宿主，高程点跟着它读的东西
                    else if (const Tag* t = e->As<Tag>())
                        hangs = in(t->host) || in(t->view);
                    else if (const Dimension* d = e->As<Dimension>())
                        hangs = in(d->level) || in(d->view);
                    else if (const Spot* sp = e->As<Spot>())
                        hangs = in(sp->view) || in(sp->host);
                    else if (e->IsAnnotation())
                        hangs = in(m.AnnotationPlan(*e));
                    if (hangs)
                    {
                        gone.insert(g);
                        more = true;
                    }
                }
            }
            // 尺寸去掉删了的参照；剩下不到两个时也删
            std::vector<std::pair<Id, Dimension>> trimmed;
            for (const auto& [id, e] : m.elements.Items())
            {
                const Dimension* d = e->As<Dimension>();
                if (!d || gone.count(m.GuidOf(id)) ||
                    std::none_of(d->refs.begin(), d->refs.end(), [&](const Reference& r) { return gone.count(m.GuidOf(r.id)) > 0; }))
                    continue;
                Dimension t = *d;
                std::erase_if(t.refs, [&](const Reference& r) { return gone.count(m.GuidOf(r.id)) > 0; });
                if (t.refs.size() < 2)
                    gone.insert(m.GuidOf(id));
                else
                    trimmed.emplace_back(id, std::move(t));
            }
            Json deleted = Json::array();
            for (const auto& [id, _] : m.elements.Items())
                if (gone.count(m.GuidOf(id)))
                    deleted.push_back(id);
            Model& mm = s.ModelMut();
            for (auto& [id, d] : trimmed)
                mm.elements.Insert(id, std::move(d));
            // 记号认体量要它还在：先拿掉记号再删
            Massing::Forget(mm, gone);
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

        // ── 注释（移植自 BIMCraft 的注释选项卡，第 5d 步）─────────────
        // 只收 keys 里的参数
        bool Only(const char* c, const Json& p, std::initializer_list<const char*> keys, std::string& error)
        {
            if (!p.is_object())
                return Fail(error, c, "params must be an object");
            for (auto it = p.begin(); it != p.end(); ++it)
                if (std::none_of(keys.begin(), keys.end(), [&](const char* k) { return it.key() == k; }))
                    return Fail(error, c, "unknown parameter `" + it.key() + "`");
            return true;
        }

        // 注释在的视图：view（标高的名称、ID 或 GUID：它的平面），没给时为 level 的平面（默认最低的标高）。
        // MiniBIM 还没有平面视图、剖面、立面与图纸
        bool ViewParam(const char* c, const Model& m, const Json& p, Guid& out, std::string& error)
        {
            return LevelParam(c, m, p, Absent(p, "view") ? "level" : "view", out, error);
        }

        bool DimensionCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "dimension";
            Params P(c, p, error);
            const Model& m = s.GetModel();
            Guid level;
            P2 at;
            std::optional<std::string> kindName;
            if (!LevelParam(c, m, p, "level", level, error) || !P.PointReq("at", at) || !P.String("kind", kindName))
                return false;
            DimKind kind = DimKind::Aligned;
            if (kindName)
            {
                if (*kindName == "angular")
                    kind = DimKind::Angular;
                else if (*kindName == "radial")
                    kind = DimKind::Radial;
                else if (*kindName != "aligned")
                    return P.Fail("`kind` is aligned, angular or radial, not `" + *kindName + "`");
            }
            const Json* given = Get(p, "refs");
            if (!given || !given->is_array())
                return P.Fail("`refs` [ref, …] is required");
            if (given->size() > MaxPoints)
                return P.Fail("`refs` has more than " + std::to_string(MaxPoints) + " references");
            std::vector<Reference> refs;
            for (const Json& v : *given)
            {
                const auto near = [&](P2 q, const char* what)
                {
                    return P.Fail(std::string("no ") + what + " at [" + Num(q.x) + ", " + Num(q.y) + "] on " + LevelName(m, level));
                };
                if (const auto q = PointOf(v))
                {
                    std::optional<Reference> r;
                    if (kind == DimKind::Radial)
                    {
                        if (!(r = ArcNear(m, level, *q)))
                            return near(*q, "round column's edge");
                    }
                    else
                    {
                        // 第一条之后：对齐的与它平行，角度的与它相交
                        std::optional<Segment> first;
                        Segment l;
                        std::string why;
                        if (!refs.empty() && m.ReferenceLine(refs.front(), l, why))
                            first = l;
                        const auto keep = [&](const Segment& x)
                        {
                            return !first || Geom::Parallel(first->first, first->second, x.first, x.second) == (kind == DimKind::Aligned);
                        };
                        if (!(r = ReferenceNear(m, level, *q, keep)))
                            return near(*q, "wall face, wall centre line, wall end, door, window or grid line");
                    }
                    refs.push_back(*r);
                    continue;
                }
                const std::string shape = Show(v) + " is not a reference {id, face?} or a point [x, y]";
                if (!v.is_object())
                    return P.Fail(shape);
                for (auto it = v.begin(); it != v.end(); ++it)
                    if (it.key() != "id" && it.key() != "face")
                        return P.Fail(shape + ": unknown field `" + it.key() + "`");
                Reference r;
                std::string why;
                if (!v.contains("id") || !FromJson(v["id"], r.id, why))
                    return P.Fail(shape + ": `id` is required");
                if (v.contains("face") && !v["face"].is_null())
                {
                    const auto f = v["face"].is_string() ? FaceOf::FromName(v["face"].get<std::string>()) : std::nullopt;
                    if (!f)
                        return P.Fail(shape + ": `face` is centre, left, right, start or end");
                    r.face = *f;
                }
                refs.push_back(r);
            }
            Dimension d{ level, std::nullopt, kind, std::move(refs), at };
            Json values = Json::array();
            for (const double x : m.DimensionValues(d))
                values.push_back(Hundredth(x));
            const Id id = s.ModelMut().Insert(Element(std::move(d)));
            result = Json{ { "id", id }, { "values", std::move(values) } };
            return true;
        }

        bool TagCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "tag";
            Params P(c, p, error);
            const Model& m = s.GetModel();
            std::optional<P2> offset;
            bool leader = false;
            std::optional<std::string> label, category;
            Guid level;
            if (!P.PointOpt("offset", offset) || !P.Flag("leader", leader) || !P.String("label", label) || !P.String("category", category) ||
                !LevelParam(c, m, p, "level", level, error))
                return false;
            if (label && Text::Trim(*label).empty())
                label.reset();
            std::vector<Id> hosts;
            if (P.Absent("ids") && P.Absent("id"))
            {
                // 全部标记：这个平面上还没有标记的门窗，或 category 的
                const std::optional<std::string> of = category ? std::optional<std::string>(NormalizeCategory(*category)) : std::nullopt;
                std::set<Guid> tagged;
                for (const auto& [_, e] : m.elements.Items())
                    if (const Tag* t = e->As<Tag>(); t && m.AnnotationIn(*e, level))
                        tagged.insert(m.GuidOf(t->host));
                for (const auto& [id, e] : m.elements.Items())
                {
                    const bool wanted = of ? e->Category() == *of : e->AsOpening() != nullptr;
                    if (wanted && !tagged.count(m.GuidOf(id)) && m.LevelOf(*e) == std::optional<Guid>(level))
                        hosts.push_back(id);
                }
                if (hosts.empty())
                    return P.Fail("nothing to tag: " + LevelName(m, level) + " has no " + of.value_or("door and window") + " without a tag");
            }
            else if (!IdsParam(c, m, p, hosts, error))
                return false;
            std::vector<Tag> tags;
            for (const Id h : hosts)
            {
                const Guid g = m.GuidOf(h);
                tags.push_back(Tag{ g, std::nullopt, offset.value_or(m.TagOffset(g)), leader, label });
            }
            Model& mm = s.ModelMut();
            Json ids = Json::array();
            for (Tag& t : tags)
                ids.push_back(mm.Insert(Element(std::move(t))));
            result = Json{ { "ids", std::move(ids) } };
            return true;
        }

        // 文字注释的 runs：[{from, to, bold?, italic?, underline?}, …]
        bool RunsParam(Params& P, const Json& p, std::vector<TextRun>& out)
        {
            out.clear();
            const Json* v = Get(p, "runs");
            if (!v || v->is_null())
                return true;
            const std::string shape = "`runs` is [{from, to, bold?, italic?, underline?}, …], counted in characters";
            if (!v->is_array() || v->size() > TextTypes::MaxRuns)
                return P.Fail(shape);
            for (const Json& r : *v)
            {
                if (!r.is_object())
                    return P.Fail(shape);
                TextRun run;
                for (auto it = r.begin(); it != r.end(); ++it)
                {
                    const std::string& k = it.key();
                    if (k == "from" || k == "to")
                    {
                        const auto n = AsWhole(*it);
                        if (!n || *n > 1000000)
                            return P.Fail(shape);
                        (k == "from" ? run.from : run.to) = static_cast<size_t>(*n);
                    }
                    else if (k == "bold" || k == "italic" || k == "underline")
                    {
                        if (it->is_null())
                            continue;
                        if (!it->is_boolean())
                            return P.Fail("`runs`: `" + k + "` is true or false");
                        (k == "bold" ? run.bold : k == "italic" ? run.italic : run.underline) = it->get<bool>();
                    }
                    else
                        return P.Fail("`runs`: unknown field `" + k + "`");
                }
                if (!r.contains("from") || !r.contains("to"))
                    return P.Fail(shape);
                out.push_back(run);
            }
            return true;
        }

        bool TextCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "text";
            if (!Absent(p, "size"))
                return Fail(error, c, "a note's size is its text type's: give `type` (`text_type` lists them, and makes one of a size)");
            if (!Only(c, p, { "text", "at", "view", "level", "type", "angle", "leader", "override_format", "runs" }, error))
                return false;
            Params P(c, p, error);
            const Model& m = s.GetModel();
            const Json* t = Get(p, "text");
            if (!t || t->is_null())
                return P.Fail("`text` is required");
            if (!t->is_string() || Text::CharCount(t->get_ref<const std::string&>()) > TextTypes::MaxNote)
                return P.Fail("`text` must be text of at most " + std::to_string(TextTypes::MaxNote) + " characters, got " + Show(*t));
            Note n;
            n.text = t->get<std::string>();
            std::optional<std::string> type;
            std::optional<double> angle;
            if (!P.PointReq("at", n.at) || !ViewParam(c, m, p, n.view, error) || !P.String("type", type) || !P.Num("angle", angle) ||
                !P.PointOpt("leader", n.leader) || !P.Flag("override_format", n.overrideFormat) || !RunsParam(P, p, n.runs))
                return false;
            if (type)
                n.typeName = std::string(Text::Trim(*type));
            else if (const std::string* d = TextTypes::Default(m.textTypes))
                n.typeName = *d;
            else
                return P.Fail("the project has no text type; make one first");
            n.angle = angle.value_or(0.0);
            const Id id = s.ModelMut().Insert(Element(std::move(n)));
            result = Json{ { "id", id } };
            return true;
        }

        bool TextTypeCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "text_type";
            if (!Only(c, p, { "name", "like", "rename", "delete", "font", "size", "bold", "italic", "underline", "width_factor", "color", "border",
                              "border_offset", "border_pen" },
                      error))
                return false;
            Params P(c, p, error);
            std::optional<std::string> given, like, font, rename;
            if (!P.String("name", given) || !P.String("like", like) || !P.String("font", font) || !P.String("rename", rename))
                return false;
            if (!given || Text::Trim(*given).empty())
            {
                Json types = Json::object();
                for (const auto& [n, t] : s.GetModel().textTypes)
                    types[n] = ToJson(t);
                result = Json{ { "types", std::move(types) } };
                return true;
            }
            const std::string name(Text::Trim(*given));
            const Kind kind = Kind::Of(Kind::Tag::TextType);
            bool del = false;
            if (!P.Flag("delete", del))
                return false;
            if (del)
            {
                const Model& m0 = s.GetModel();
                if (!m0.textTypes.count(name))
                    return P.Fail("there's no text type `" + name + "`");
                for (const auto& [id, e] : m0.elements.Items())
                    if (const Note* n = e->As<Note>(); n && n->typeName == name)
                        return P.Fail("note " + std::to_string(id) + " is of `" + name + "`; give it another type first");
                s.ModelMut().textTypes.erase(name);
                result = Json{ { "deleted", name } };
                return true;
            }
            const Model& m0 = s.GetModel();
            TextType t;
            if (const auto it = m0.textTypes.find(name); it != m0.textTypes.end())
                t = it->second;
            else if (like)
            {
                const auto from = m0.textTypes.find(std::string(Text::Trim(*like)));
                if (from == m0.textTypes.end())
                    return P.Fail("there's no text type `" + *like + "` to start from");
                t = from->second;
            }
            // 字体名记着（MiniBIM 只有一种单线字，都用它画）
            if (font)
            {
                const std::string f(Text::Trim(*font));
                if (f.empty())
                    return P.Fail("`font` needs a font's name");
                t.font = f;
            }
            for (const auto& [key, slot] : { std::pair{ "size", &t.size }, std::pair{ "width_factor", &t.widthFactor }, std::pair{ "border_offset", &t.borderOffset } })
            {
                std::optional<double> x;
                if (!P.Num(key, x))
                    return false;
                if (x)
                    *slot = *x;
            }
            for (const auto& [key, slot] : { std::pair{ "bold", &t.bold }, std::pair{ "italic", &t.italic }, std::pair{ "underline", &t.underline },
                                             std::pair{ "border", &t.border } })
                if (!P.Absent(key) && !P.Flag(key, *slot))
                    return false;
            if (!P.Absent("color"))
            {
                const auto col = Colour(*Get(p, "color"));
                if (!col)
                    return P.Fail("`color` is \"#rrggbb\" or [r, g, b], each 0 to 255, not " + Show(*Get(p, "color")));
                t.color = *col;
            }
            if (!P.Absent("border_pen"))
            {
                const auto pen = AsWhole(*Get(p, "border_pen"));
                if (!pen || *pen > 16)
                    return P.Fail("`border_pen` is a pen, 0 to 16, not " + Show(*Get(p, "border_pen")));
                t.borderPen = static_cast<uint8_t>(*pen);
            }
            std::string now = name;
            if (rename)
            {
                const std::string to(Text::Trim(*rename));
                if (to.empty())
                    return P.Fail("`rename` needs a name");
                if (to != name && m0.textTypes.count(to))
                    return P.Fail("there's already a text type `" + to + "`");
                now = to;
            }
            if (!t.Check("text type `" + now + "`", error))
            {
                error = Bad(c, error);
                return false;
            }
            if (now != name || m0.textTypes.find(name) == m0.textTypes.end() || !(m0.textTypes.at(name) == t))
            {
                Model& m = s.ModelMut();
                if (now != name && m.textTypes.count(name))
                {
                    m.textTypes.erase(name);
                    m.library.Renamed(kind, name, now);
                    std::vector<Id> notes;
                    for (const auto& [id, e] : m.elements.Items())
                        if (const Note* n = e->As<Note>(); n && n->typeName == name)
                            notes.push_back(id);
                    for (const Id id : notes)
                        m.elements.GetMut(id)->As<Note>()->typeName = now;
                }
                m.textTypes[now] = t;
            }
            result = Json{ { "name", now }, { "type", ToJson(t) } };
            return true;
        }

        bool SpotCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "spot";
            if (!Only(c, p, { "at", "view", "level", "host" }, error))
                return false;
            Params P(c, p, error);
            const Model& m = s.GetModel();
            Spot sp;
            if (!P.PointReq("at", sp.at) || !ViewParam(c, m, p, sp.view, error) || !P.IdOpt("host", sp.host))
                return false;
            if (!sp.host)
            {
                const auto h = m.SpotHost(sp.view, sp.at);
                if (!h)
                    return P.Fail("nothing to read at [" + Num(sp.at.x) + ", " + Num(sp.at.y) + "] on " + LevelName(m, sp.view) +
                                  "; place it on a floor, a roof, a stair, a wall or a column");
                sp.host = m.GuidOf(*h);
            }
            const auto elevation = m.SpotValue(sp);
            const auto host = m.elements.IdOf(*sp.host);
            const Id id = s.ModelMut().Insert(Element(std::move(sp)));
            result = Json{ { "id", id }, { "elevation", elevation ? Json(Hundredth(*elevation)) : Json() }, { "host", host ? Json(*host) : Json() } };
            return true;
        }

        // 点到微米、圆弧的转角到十亿分之一度：本来就整的（三下点出的四分之一圆是 90，不是 90.00000000000001）
        Curve Rounded(Curve cv)
        {
            for (P2& q : cv.points)
                q = Micron(q);
            for (double& a : cv.arcs)
                a = std::round(a * 1e9) / 1e9 + 0.0;
            if (std::all_of(cv.arcs.begin(), cv.arcs.end(), [](double a) { return a == 0.0; }))
                cv.arcs.clear();
            cv.centre = Micron(cv.centre);
            return cv;
        }

        bool DetailLineCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "detail_line";
            if (!Only(c, p, { "view", "level", "style", "insulation", "centre_mark", "cloud", "revision", "start", "end", "through", "arc", "points", "arcs",
                              "closed", "centre", "radius", "radii", "angle", "span", "control", "knots", "fit", "start_tangent", "end_tangent" },
                      error))
                return false;
            Params P(c, p, error);
            const Model& m = s.GetModel();
            Guid view;
            std::optional<std::string> style;
            if (!ViewParam(c, m, p, view, error) || !P.String("style", style))
                return false;
            if (style)
                style = std::string(Text::Trim(*style));
            else if (const std::string* d = Lines::DefaultStyle(m.lineStyles, m.language))
                style = *d;
            else
                return P.Fail("the project has no line style; make one first");
            // 读法与文件里的详图线相同
            Json v = p;
            v.erase("level");
            v["category"] = "line";
            v["view"]     = ToJson(view);
            v["style"]    = *style;
            Element e;
            std::string why;
            if (!FromJson(v, e, why))
            {
                if (Text::StartsWith(why, "element: "))
                    why.erase(0, 9);
                return P.Fail(why);
            }
            DetailLine& l = *e.As<DetailLine>();
            if (!l.curve.Check("the line", why))
                return P.Fail(why);
            l.curve = Rounded(l.curve);
            if (!l.curve.Check("the line", why))
                return P.Fail(why);
            const Id id = s.ModelMut().Insert(std::move(e));
            result = Json{ { "id", id } };
            return true;
        }

        bool FilledRegionCmd(Session& s, const Json& p, Json& result, std::string& error)
        {
            const char* c = "filled_region";
            if (!Only(c, p, { "sketch", "view", "level", "type", "line_style" }, error))
                return false;
            Params P(c, p, error);
            const Model& m = s.GetModel();
            std::optional<std::vector<SketchLine>> lines;
            if (!SketchParam(P, p, "sketch", {}, lines, true))
                return false;
            if (!lines)
                return P.Fail("`sketch`, its outlines' lines, is required");
            Sketch::Profile profile;
            std::vector<Sketch::Problem> problems;
            if (!Sketch::Solve(*lines, profile, problems))
                return P.Fail("the sketch doesn't close into loops: " + Sketch::Say(problems, Sketch::Namer(*lines)));
            FilledRegion r;
            std::optional<std::string> type;
            if (!ViewParam(c, m, p, r.view, error) || !P.String("type", type))
                return false;
            // 实心黑色，项目语言里的或英文的，否则第一个
            if (type)
                r.typeName = std::string(Text::Trim(*type));
            else if (const std::string* d = Fills::DefaultRegionType(m.regionTypes, m.language))
                r.typeName = *d;
            else
                return P.Fail("the project has no filled region type; make one first");
            // 没说时为细线；null（或 Invisible）不画
            const Json* ls = Get(p, "line_style");
            if (!ls)
            {
                if (const std::string* d = Lines::DefaultStyle(m.lineStyles, m.language))
                    r.lineStyle = *d;
            }
            else if (ls->is_string())
            {
                const std::string n(Text::Trim(ls->get<std::string>()));
                if (Text::AsciiLower(n) != "invisible")
                    r.lineStyle = n;
            }
            else if (!ls->is_null())
                return P.Fail("`line_style` is a line style's name, or null for invisible lines, not " + Show(*ls));
            r.sketch = std::move(*lines);
            const Id id = s.ModelMut().Insert(Element(std::move(r)));
            result = Json{ { "id", id }, { "area_m2", M2(profile.Area()) } };
            return true;
        }

        bool MeasureCmd(Session&, const Json& p, Json& result, std::string& error)
        {
            const char* c = "measure";
            Params P(c, p, error);
            P2 a, b;
            if (!P.PointReq("from", a) || !P.PointReq("to", b))
                return false;
            const Snap::Ruler r = Snap::Measure(a, b);
            const auto to = [](double x, double k) { return std::round(x * k) / k + 0.0; };
            result = Json{ { "length", to(r.length, 100.0) }, { "dx", to(r.dx, 100.0) }, { "dy", to(r.dy, 100.0) }, { "angle", to(r.angle, 1e4) } };
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
            if (p.is_object() && p.contains("at") && !p["at"].is_null() && !p["at"].is_string())
                return P.Fail("`at` is centre or finish");
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
                           { "room_area", m.roomArea == RoomArea::Finish ? "finish" : "centre" },
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
            { "autosave", N_("Autosave"), {}, "{path} atomically write a recovery snapshot, keeping the project path and undo history", AutosaveCmd, false },
            { "recover", N_("Recover"), {}, "{path, project?} open and validate a recovery snapshot; Save targets `project`, or asks for Save As without it", Recover, false },
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
            { "column", N_("Column"), { "CL" },
              "{at: [x,y], type?, angle?, level?, top_level?, height?, base_offset?} a column, its width along `angle` degrees; up to "
              "the next level, or `height` mm tall, or up to `top_level`", ColumnCmd, true },
            { "structural_column", N_("Structural Column"), { "CL" },
              "{at: [x,y], type?, angle?, level?, top_level?, height?, base_offset?} a column of the structure, as `column` makes one but "
              "in its own category with its own types (`type` with category structural column): up to the next level, or `height` mm "
              "tall, or up to `top_level`. CL is `column`'s too; on the desktop's Structure tab it is this one", StructuralColumnCmd, true },
            { "footing", N_("Isolated Footing"), { "FI" },
              "{at: [x,y], type?, thickness?, offset?, angle?, level?} an isolated (pad) footing, its plan the type's (category "
              "footing), `thickness` mm thick (400) with its top `offset` mm over the level (0: at it), so it hangs below", FootingCmd, true },
            { "beam", N_("Beam"), { "BM" },
              "{points: [[x,y],…], closed?, type?, level?, offset?, end_offset?, justify?: top|centre|bottom, system?} a beam for each "
              "segment of the points: its section the type's (category beam: `width` across, `depth` deep, or round), its `justify` "
              "line (top by default) `offset` mm over the level at its start (0: at the level, so it hangs below) and `end_offset` at "
              "its end (the same: level), for a single segment; `system` names the system it belongs to. It slopes up to 75°", BeamCmd, true },
            { "strip_footing", N_("Wall Footing"), { "FW" },
              "{points: [[x,y],…], closed?, type?, level?, offset?, system?} a continuous footing under a wall for each segment of the "
              "points, its section the type's (category stripfooting), its top `offset` mm over the level (0: at it)", StripFootingCmd, true },
            { "pipe", N_("Pipe"), { "PI" },
              "{points: [[x,y],…], closed?, type?, level?, offset?, end_offset?, justify?: top|centre|bottom, system?} a pipe for each "
              "segment of the points: its section the type's (category pipe: round, `width` the outside diameter), its `justify` line "
              "(centre by default) `offset` mm over the level at its start (0: at the level) and `end_offset` at its end (the same: level), "
              "for a single segment, so a drain falls and a riser rises; the same point twice and two heights make one stand upright. "
              "`system` names the system it belongs to (\"Água fria\", \"Esgoto\")", PipeCmd, true },
            { "conduit", N_("Conduit"), { "CN" },
              "{points: [[x,y],…], closed?, type?, level?, offset?, end_offset?, justify?: top|centre|bottom, system?} a conduit (eletroduto) "
              "for each segment of the points, as `pipe` lays them: round, `width` its outside diameter; `system` names its circuit", ConduitCmd, true },
            { "cable_tray", N_("Cable Tray"), { "CT" },
              "{points: [[x,y],…], closed?, type?, level?, offset?, end_offset?, justify?: top|centre|bottom, system?} a cable tray (eletrocalha) "
              "for each segment of the points, as `pipe` lays them: its section the type's `width` across by `depth` deep", CableTrayCmd, true },
            { "wire", N_("Wire"), { "EW" },
              "{points: [[x,y],…], closed?, type?, level?, offset?, end_offset?, justify?: top|centre|bottom, system?} a wire (condutor) for "
              "each segment of the points, as `pipe` lays them: round, `width` its outside diameter; `system` names its circuit", WireCmd, true },
            { "down_conductor", N_("Down Conductor"), { "DC" },
              "{points: [[x,y],…], closed?, type?, level?, offset?, end_offset?, justify?: top|centre|bottom, system?} a down conductor "
              "(condutor de descida, NBR 5419) for each segment of the points, as `pipe` lays them: round, or a flat strip; the same point "
              "twice and two heights make one stand upright down a wall; `system` names the protection system it belongs to", DownConductorCmd, true },
            { "ground_conductor", N_("Earthing Conductor"), { "GC" },
              "{points: [[x,y],…], closed?, type?, level?, offset?, end_offset?, justify?: top|centre|bottom, system?} an earthing conductor "
              "(condutor de aterramento, the ring or mesh of NBR 5419) for each segment of the points, as `pipe` lays them, usually under "
              "the level (a negative `offset`)", GroundConductorCmd, true },
            { "cabling", N_("Cabling"), { "CB" },
              "{points: [[x,y],…], closed?, type?, level?, offset?, end_offset?, justify?: top|centre|bottom, system?} structured cabling "
              "(cabeamento estruturado, NBR 14565) for each segment of the points, as `pipe` lays them: round, `width` the cable's outside "
              "diameter; `system` names the link or the network it belongs to", CablingCmd, true },

            { "mass", N_("Mass"), { "MS" },
              "{level?, boundary: [[x,y],…], top?: [[x,y],…] | lean?: [dx,dy], top_level?, height?, base_offset?, name?} a conceptual "
              "mass: the footprint `boundary` on `level`, `base_offset` mm above it, rising to `top_level` or `height` mm (by default to the "
              "level above, or 3000). `top`, a ring with as many points, lofts it to that outline at the top; `lean` moves the top sideways",
              MassCmd, true },
            { "mass_floors", N_("Mass Floors"), { "MF" },
              "{mass?, type?, prune?} a floor on each level the mass (or every mass) crosses, along its outline there; run again it updates "
              "them, and reports the floors on levels it no longer crosses as `stale` (`prune` deletes them)", MassFloorsCmd, true },
            { "wall_by_face", N_("Wall by Face"), { "WB" },
              "{mass, faces?: [edge, …], type?, location?} walls on the mass's upright faces (edge i of its footprint runs from point i to "
              "the next; by default every plumb one), their outside on the face, as high as the mass", WallByFaceCmd, true },
            { "floor_by_face", N_("Floor by Face"), { "FB" }, "{mass, type?} a floor on the mass's top face", FloorByFaceCmd, true },
            { "roof_by_face", N_("Roof by Face"), { "RB" }, "{mass, type?, slope?, overhang?} a roof on the mass's top face, flat by default",
              RoofByFaceCmd, true },
            { "update_by_face", N_("Update to Face"), { "UF" },
              "{mass? | ids?} rebuilds the walls, floors and roofs made by face (of the mass, or `ids`, or all) to the mass as it is now",
              UpdateByFaceCmd, true },
            { "dimension", N_("Aligned Dimension"), { "DI" },
              "{refs: [ref, …], at: [x, y], level?, kind?: aligned|angular|radial} aligned (the default): the distances between parallel references, "
              "along a line through `at`; angular: the angle between two references that meet, on the side of `at`; radial: a round column's radius, "
              "toward `at`. A ref is {id, face?}: a wall's centre (the default), left or right face (of its start-to-end direction), start or end; a "
              "door's or window's centre, or its jamb toward its wall's start or end; a grid's line; a straight detail line, or its start or end "
              "(radial: a circle or an arc of one). Or a point [x, y] on one of those (for radial, on a round column's edge). It follows what it measures",
              DimensionCmd, true },
            { "text", N_("Text"), { "TX" },
              "{text, at: [x, y], view?, type?, angle?, leader?: [x, y], override_format?, runs?: [{from, to, bold?, italic?, underline?}, …]} a text "
              "note, its first line's top left at `at`, in a level's plan (`view`, the lowest by default). Lettered as its text `type` says (2.5mm unless "
              "told; `text_type` lists and makes them), turned `angle` degrees counter-clockwise; with `leader`, a line to that point. Lines break at \\n. "
              "With `override_format`, each run makes characters `from` up to `to` bold, italic or underlined (true), or not (false)",
              TextCmd, true },
            { "text_type", N_("Text Types"), {},
              "{name?, font?, size?, bold?, italic?, underline?, width_factor?, color?, border?, border_offset?, border_pen?, like?, rename?, delete?: true} "
              "a text type of the project's: its font's name (kept; MiniBIM letters every note in its single-stroke font), its capitals' height on paper "
              "(`size`, mm, 2.5 for a new one), bold, italic, underlined, `width_factor`, colour, and a border `border_offset` mm out in pen `border_pen`. "
              "A new one starts as `like` says, or as 2.5mm Arial. `rename` renames it, its notes following; `delete` takes it away while no note is of "
              "it. Without `name`, it lists every type",
              TextTypeCmd, true },
            { "spot", N_("Spot Elevation"), { "EL" },
              "{at: [x, y], view?, host?} a spot elevation in a level's plan (the lowest by default): the height of the top of what stands on the level "
              "at `at` (or of `host`), following it",
              SpotCmd, true },
            { "tag", N_("Tag by Category"), { "TG" },
              "{ids?: [id,…], label?, level?, category?, offset?: [dx, dy], leader?} tags elements with a field of theirs: a door's or window's mark, a "
              "room's name, else the type, or `label`, another field of its category's. `offset` mm from the point it names (beside the wall for a door "
              "or window, else up and right); without ids, every door and window on the level not tagged yet, or of `category`",
              TagCmd, true },
            { "detail_line", N_("Detail Line"), { "DL" },
              "{view?, style?, and a curve: `points` [[x, y], …] (with `arcs`, each segment's turn in degrees, and `closed`), `start` and `end` (with "
              "`arc` degrees or a `through` point), or `centre` and `radius` (a circle)} a detail line in a level's plan (the lowest by default), drawn "
              "in line style `style` (Thin Lines unless told), only there; `insulation` (a width), `cloud` (an arc length) and `revision` are kept; "
              "with `centre_mark` true, a small cross marks the centre of its arcs or circle",
              DetailLineCmd, true },
            { "filled_region", N_("Filled Region"), { "FR" },
              "{sketch: [[[x,y],[x,y]],…], view?, type?, line_style?: name | null} a filled region in a level's plan (the lowest by default): the loops "
              "its sketch closes into (a loop inside one a hole, one inside that an island), filled as type `type` (Solid Black unless told) says, its "
              "boundary in line style `line_style` (Thin Lines unless told; null, unseen). A sketch line may have its own `style`, or be `invisible`. "
              "It draws over the model, under the annotation; edit its sketch with edit_boundary",
              FilledRegionCmd, true },
            { "measure", N_("Measure"), { "ME" },
              "{from: [x, y], to: [x, y]} what a ruler laid between two points reads (mm): its `length`, its parts `dx` and `dy`, and its `angle` in "
              "degrees from east, counter-clockwise; it changes nothing",
              MeasureCmd, false },
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
              "{category: wall|floor|ceiling|door|window|column|structuralcolumn|footing|roof|beam|stripfooting|pipe|conduit|cabletray|wire|"
              "downconductor|groundconductor|cable, name, thickness? | width?, height?, sill?, operation?, grid? | width?, depth?, round?, material?} creates the type, or changes it "
              "everywhere it's used. A wall or floor type may have `layers` [{function?: structure|substrate|thermal|membrane|finish, material?, "
              "thickness}, …], a wall's from its exterior side in, a floor's from the top down: it's as thick as they are, and a new `thickness` "
              "goes to its structure. A roof's or a ceiling's layers too, from the top down; a ceiling type may have `grid`: its square tiles' "
              "width, mm, which reflected ceiling plans draw (0, none). A column's, a structural column's or a footing's: `width` by `depth`, or `round`, "
              "`width` across; a run's (a beam's, a pipe's, a cable tray's…) too, made of a `material` of the library", TypeCmd, true },
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
            { "delete_type", N_("Delete Type"), {},
              "{category?: wall|floor|door|window|column|roof|ceiling|structuralcolumn, family?, type?} deletes a type of a category (`category` and `type`), a "
              "family (`family`) or one of a family's types (`family` and `type`), as Revit's Delete on one in the Project Browser does; "
              "refused while an element uses it (it says how many), and for the last type of a category or of a family", DeleteTypeCmd, true },
            { "rename_type", N_("Rename Type"), {},
              "{category?: wall|floor|door|window|column|roof|ceiling|structuralcolumn, family?, type, name} renames a type of a category (`category` and `type`) "
              "or one of a family's types (`family` and `type`) to `name`, as Rename in Type Properties does: the elements of it follow, "
              "and it keeps its GUID; refused for a name another type of it has", RenameTypeCmd, true },
            { "edit_boundary", N_("Edit Boundary"), {},
              "{id, sketch?: [[[x,y],[x,y]],…]} redraws a floor's, a ceiling's or a roof's footprint's outline and its holes (a floor's or a "
              "ceiling's openings, a roof's courtyards) as a sketch, all at once (see `sketch`); a roof's gable edges stay gables where the new "
              "footprint has the same edge. Without `sketch`, gives the outline and holes it has now, as the lines to edit (a line it kept from "
              "its last sketch with its `id`: send it back with it to keep it)", EditBoundaryCmd, true },
            { "sketch", N_("Sketch"), {},
              "{lines: [[[x,y],[x,y]],…]} solves a sketch without making anything, as Finish Edit Mode does: lines in any order, either way "
              "round, joined end to end (within 1 mm) into closed loops; a loop inside no other (or inside a hole) an outline, counter-clockwise, "
              "one inside an outline a hole in it, clockwise. A line is [[x,y],[x,y]], an arc [[x,y],[x,y],[x,y]] (its ends, then a point it "
              "passes through) or {start, end, arc?}. Gives each region's `outline`, `holes` and `area_m2`, or every problem with the lines it's "
              "about (numbered from 0). `sketch` on floor, ceiling and roof, and `edit_boundary`, take lines the same way", SketchCmd, false },
            { "align", N_("Align"), { "AL" },
              "{id, reference: [[x,y],[x,y]], line: [[x,y],[x,y]]} aligns an element's picked line with a reference in plan: turns by the "
              "smaller angle about the picked line's midpoint, then moves it onto the reference, keeping its size. Doors and windows slide "
              "along their host wall; they cannot turn independently. Returns {aligned, angle, by}", AlignCmd, true },
            { "rotate", N_("Rotate"), { "RO" },
              "{ids: [id,…], about: [x, y], angle, copy?} turns elements about a point by `angle` degrees, counter-clockwise; walls joined "
              "to them stay joined, and doors and windows turn with their walls", RotateCmd, true },
            { "mirror", N_("Mirror"), { "MM", "DM" },
              "{ids: [id,…], axis: [[x, y], [x, y]], copy?} mirrors elements across the line through two points; mirrored doors and "
              "windows face the other way", MirrorCmd, true },
            { "corner", N_("Trim/Extend to Corner"), { "TR" },
              "{ids: [a, b], keep?: [[x, y], [x, y]]} trims or extends two walls to meet where their lines cross; `keep` is a point on "
              "the part of each to keep", CornerCmd, true },
            { "split", N_("Split"), { "SL" },
              "{id, at: [x, y]} splits a wall in two at `at`: its doors and windows go with their part", SplitCmd, true },
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
    // level 的平面剖切到的东西（墙的面、中心线与端头，门窗的中心与两边，轴网）里 keep 认的、离 p 最近的参照线，100 毫米以内
    std::optional<Reference> ReferenceNear(const Model& m, Guid level, P2 p, const std::function<bool(const Segment&)>& keep)
    {
        const auto cut = [&](const Wall* w) { return w && m.CutBy(*w, level); };
        std::optional<std::pair<Reference, double>> best;
        for (const auto& [id, e] : m.elements.Items())
        {
            const Opening* o = e->AsOpening();
            if (!(cut(e->As<Wall>()) || (o && cut(m.GetWall(o->host))) || e->As<Grid>()))
                continue;
            const auto r = m.ReferenceAt(m.GuidOf(id), p, keep);
            if (r && r->second <= 100.0 && (!best || r->second < best->second))
                best = r;
        }
        return best ? std::optional<Reference>(best->first) : std::nullopt;
    }

    // level 上圆柱的边离 p 最近（100 毫米以内）的那根
    std::optional<Reference> ArcNear(const Model& m, Guid level, P2 p)
    {
        std::optional<std::pair<Reference, double>> best;
        for (const auto& [id, e] : m.elements.Items())
        {
            const Column* col = e->AsColumn();
            if (!col || !m.Same(col->level, level))
                continue;
            const Reference r{ m.GuidOf(id), Face::Centre };
            P2 centre;
            double radius = 0.0;
            std::string why;
            if (!m.ReferenceArc(r, centre, radius, why))
                continue;
            const double d = std::abs(p.Dist(centre) - radius);
            if (d <= 100.0 && (!best || d < best->second))
                best = std::pair{ r, d };
        }
        return best ? std::optional<Reference>(best->first) : std::nullopt;
    }

    bool OpeningOf(const Model& m, const Json& p, bool isDoor, Opening& out, std::string& error)
    {
        return BuildOpening(m, p, isDoor, out, error);
    }

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
        else if (const Column* col = e->AsColumn())
        {
            const auto [bottom, top] = m.ColumnSpan(*col);
            v["height"] = top - bottom;
            if (const ColumnType* t = m.ColumnTypeOf(*e))
            {
                v["width"] = t->width;
                v["depth"] = t->depth;
                v["round"] = t->round;
            }
        }
        // 截面、沿轴线多长、中心线两端的高度
        else if (e->AsRun())
        {
            const RunType* t = m.RunTypeOf(*e);
            const auto g = m.RunGeom(*e);
            if (t && g)
            {
                v["width"]  = t->width;
                v["depth"]  = t->depth;
                v["round"]  = t->round;
                v["length"] = Hundredth(g->Length());
                Json axis = Json::array();
                for (const P3& q : { g->a, g->b })
                    axis.push_back(Json::array({ Hundredth(q[0]), Hundredth(q[1]), Hundredth(q[2]) }));
                v["axis"] = axis;
            }
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
        else if (const Mass* ms = e->As<Mass>())
        {
            const auto [area, height, volume] = MassSize(m, *ms);
            v["area_m2"]   = area;
            v["height"]    = height;
            v["volume_m3"] = volume;
            double gross = 0.0;
            Json levels  = Json::array();
            for (const auto& sl : m.MassSlices(*ms))
            {
                gross += std::abs(Geom::Area(m.MassOutline(*ms, sl.z)));
                levels.push_back(sl.level);
            }
            v["gross_floor_area_m2"] = M2(gross);
            v["levels"]              = std::move(levels);
        }
        else if (e->IsAnnotation())
        {
            const auto level = m.LevelOf(*e);
            const auto levelId = level ? m.elements.IdOf(*level) : std::nullopt;
            if (const Dimension* d = e->As<Dimension>())
            {
                Json values = Json::array();
                for (const double x : m.DimensionValues(*d))
                    values.push_back(Hundredth(x));
                v["values"] = std::move(values);
            }
            else if (const Tag* t = e->As<Tag>())
            {
                v["text"]  = m.TagText(*t);
                v["level"] = levelId ? Json(*levelId) : Json();
                const auto a = m.Anchor(t->host);
                v["at"] = a ? ToJson(*a + t->offset) : Json();
            }
            else if (const Note* n = e->As<Note>())
            {
                v["level"] = levelId ? Json(*levelId) : Json();
                const auto it = m.textTypes.find(n->typeName);
                v["size"] = it != m.textTypes.end() ? Json(it->second.size) : Json();
            }
            else if (const FilledRegion* r = e->As<FilledRegion>())
            {
                Sketch::Profile profile;
                std::vector<Sketch::Problem> problems;
                v["area_m2"]        = M2(Sketch::Solve(r->sketch, profile, problems) ? profile.Area() : 0.0);
                v["level"]          = levelId ? Json(*levelId) : Json();
                v["pattern_origin"] = ToJson(r->patternOrigin);
                v["pattern_angle"]  = r->patternAngle;
            }
            else if (const DetailLine* l = e->As<DetailLine>())
            {
                v["shape"]  = l->curve.Name();
                v["length"] = Hundredth(l->curve.Length());
                v["level"]  = levelId ? Json(*levelId) : Json();
            }
            else if (const Spot* sp = e->As<Spot>())
            {
                const auto z = m.SpotValue(*sp);
                v["elevation"] = z ? Json(Hundredth(*z)) : Json();
                v["level"]     = levelId ? Json(*levelId) : Json();
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

    const char* CommandOf(std::string_view category)
    {
        static const std::pair<const char*, const char*> table[] = {
            { "structuralcolumn", "structural_column" }, { "stripfooting", "strip_footing" }, { "beam", "beam" }, { "pipe", "pipe" },
            { "conduit", "conduit" }, { "cabletray", "cable_tray" }, { "wire", "wire" }, { "downconductor", "down_conductor" },
            { "groundconductor", "ground_conductor" }, { "cable", "cabling" }, { "footing", "footing" },
        };
        for (const auto& [cat, command] : table)
            if (category == cat)
                return command;
        return "column";
    }

    std::optional<const char*> RunCategoryOf(std::string_view command)
    {
        for (const char* c : RunCategories)
            if (command == CommandOf(c))
                return c;
        return std::nullopt;
    }

    bool RunsOf(const Model& m, const Json& p, std::string_view category, std::vector<Element>& out, std::string& error)
    {
        return RunsOfImpl(m, p, std::string(category), out, error);
    }

    bool StoodOf(const Model& m, const Json& p, std::string_view category, Element& out, std::string& error)
    {
        return StoodOfImpl(m, p, std::string(category), out, error);
    }
}
