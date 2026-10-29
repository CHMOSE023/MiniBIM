// IFC 交换（src/Ifc）：STEP 文件的读法、IFC4 导出，以及 IFC2x3 / IFC4 的导入与往返。
// 移植自 BIMCraft crates/core/src/step.rs、ifc.rs、import.rs、tests.rs 的测试（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md。
// 柱、楼梯、坡道、栏杆、管井、体量、管线、阶段与项目参数还没移植：用到它们的断言先略去，往返的建筑里也不放它们；
// 楼板里被洞口元素切出的洞，BIMCraft 读作管井，这里读作楼板自己的洞
#include "Commands/Commands.h"
#include "Commands/Sample.h"
#include "Commands/Session.h"
#include "Ifc/Ifc.h"
#include "Ifc/Step.h"
#include "Model/ModelJson.h"
#include "Model/Text.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define MINIBIM_GETPID _getpid
#else
#include <unistd.h>
#define MINIBIM_GETPID getpid
#endif

using namespace MiniBIM::Bim;

namespace
{
    int g_failed = 0;
    void Check(bool ok, const std::string& label)
    {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
        g_failed += ok ? 0 : 1;
    }

    bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // 执行命令；失败时打印原因、返回 null
    Json Run(Session& s, const char* id, const Json& p)
    {
        Json out;
        std::string error;
        if (!s.Execute(id, p, out, error))
        {
            std::printf("  %s %s 失败：%s\n", id, p.dump().c_str(), error.c_str());
            return nullptr;
        }
        return out;
    }

    Json Run(Session& s, const char* id, const char* p) { return Run(s, id, Json::parse(p)); }

    void Script(Session& s, const char* text)
    {
        std::vector<Json> results;
        std::string error;
        if (!s.Script(text, results, error))
            std::printf("  脚本失败：%s\n", error.c_str());
    }

    std::vector<Id> House(Session& s)
    {
        const Json r = Run(s, "wall", R"({"points": [[0, 0], [8000, 0], [8000, 6000], [0, 6000]], "closed": true})");
        std::vector<Id> ids;
        for (const Json& v : r["ids"])
            ids.push_back(v.get<Id>());
        return ids;
    }

    std::string Temp(const std::string& name)
    {
        const std::filesystem::path p = std::filesystem::temp_directory_path() / ("minibim-test-" + std::to_string(MINIBIM_GETPID()) + "-" + name);
        const std::u8string u8 = p.u8string();
        return std::string(u8.begin(), u8.end());
    }

    std::filesystem::path PathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

    void WriteText(const std::string& path, const std::string& text)
    {
        std::ofstream out(PathOf(path), std::ios::binary | std::ios::trunc);
        out << text;
    }

    std::string ReadText(const std::string& path)
    {
        std::ifstream in(PathOf(path), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::string Replace(std::string s, const std::string& from, const std::string& to, bool once = false)
    {
        for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        {
            s.replace(at, from.size(), to);
            if (once)
                break;
        }
        return s;
    }

    // 文件里写了几个 `=ENTITY(`
    size_t Count(const std::string& f, const std::string& entity)
    {
        size_t n = 0;
        const std::string key = "=" + entity + "(";
        for (size_t at = f.find(key); at != std::string::npos; at = f.find(key, at + 1))
            ++n;
        return n;
    }

    size_t Occurrences(const std::string& f, const std::string& part)
    {
        size_t n = 0;
        for (size_t at = f.find(part); at != std::string::npos; at = f.find(part, at + 1))
            ++n;
        return n;
    }

    // f 里每个引用都指向只写了一次的实体
    bool Dangling(const std::string& f)
    {
        std::set<uint64_t> defined, referenced;
        bool twice = false;
        size_t start = 0;
        while (start < f.size())
        {
            size_t end = f.find('\n', start);
            if (end == std::string::npos)
                end = f.size();
            const std::string line = f.substr(start, end - start);
            start = end + 1;
            if (line.empty() || line[0] != '#')
                continue;
            const size_t eq = line.find('=');
            twice |= !defined.insert(std::stoull(line.substr(1, eq - 1))).second;
            for (size_t at = line.find('#', eq); at != std::string::npos; at = line.find('#', at + 1))
            {
                size_t k = at + 1;
                while (k < line.size() && std::isdigit(static_cast<unsigned char>(line[k])))
                    ++k;
                if (k > at + 1)
                    referenced.insert(std::stoull(line.substr(at + 1, k - at - 1)));
            }
        }
        for (const uint64_t r : referenced)
            if (!defined.count(r))
                return true;
        return twice;
    }

    bool Import(const std::string& text, Ifc::Imported& out)
    {
        std::string error;
        if (!Ifc::Import(text, "t", out, error))
        {
            std::printf("  导入失败：%s\n", error.c_str());
            return false;
        }
        return true;
    }

    std::string Skipped(const Ifc::Imported& i)
    {
        std::string out;
        for (const std::string& s : i.skipped)
            out += "  " + s + "\n";
        return out;
    }

    std::vector<const Element*> Only(const Model& m, std::string_view category)
    {
        std::vector<const Element*> out;
        for (const auto& [_, e] : m.elements.Items())
            if (category == e->Category())
                out.push_back(e.get());
        return out;
    }

    // ── 往返比较用：元素是什么、在哪里 ──────────────────────────
    // 数到毫米，所以描述同一座房子的两个模型写出来一样
    std::string Mm(const Json& v)
    {
        if (v.is_number())
            return Text::Shortest(std::round(v.get<double>()) + 0.0);
        if (v.is_array())
        {
            std::string out = "[";
            for (size_t i = 0; i < v.size(); ++i)
                out += (i ? "," : "") + Mm(v[i]);
            return out + "]";
        }
        return v.dump();
    }

    std::string Sorted(const Json& pts)
    {
        std::vector<std::string> all;
        for (const Json& p : pts)
            all.push_back(Mm(p));
        std::sort(all.begin(), all.end());
        std::string out;
        for (size_t i = 0; i < all.size(); ++i)
            out += (i ? " " : "") + all[i];
        return out;
    }

    std::string Describe(const Model& m, Id id)
    {
        const Json e = ElementJson(m, id);
        std::vector<const char*> keys;
        const std::string cat = e.value("category", "");
        if (cat == "wall")
            keys = { "start", "end", "arc", "thickness", "base_offset", "height", "level", "top_level", "attach_top" };
        else if (cat == "door" || cat == "window")
            keys = { "host", "offset", "width", "height", "sill" };
        else if (cat == "floor")
            keys = { "level", "area_m2", "slope", "top", "openings" };
        else if (cat == "ceiling")
            keys = { "level", "offset", "thickness", "area_m2", "openings" };
        else if (cat == "room")
            keys = { "level", "name", "number", "area_m2" };
        else if (cat == "roof")
            keys = { "level", "slope", "ridge", "area_m2", "outline", "gables", "openings", "courtyards" };
        else if (cat == "grid")
            keys = { "name", "start", "end" };
        const auto guid = [](const Json& v) -> std::optional<Guid>
        {
            Guid g;
            std::string error;
            if (v.is_null() || !FromJson(v, g, error))
                return std::nullopt;
            return g;
        };
        const auto text = [&](const std::string& k, const Json& v) -> std::string
        {
            if (k == "level" || k == "top_level")
            {
                const auto g = guid(v);
                const Level* l = g ? m.GetLevel(*g) : nullptr;
                return l ? l->name : "-";
            }
            if (k == "host")
            {
                const auto g = guid(v);
                const auto h = g ? m.elements.IdOf(*g) : std::nullopt;
                return h ? "(" + Describe(m, *h) + ")" : "-";
            }
            if (k == "boundary" || k == "outline")
                return Sorted(v);
            if (k == "openings" || k == "courtyards")
            {
                std::vector<std::string> holes;
                for (const Json& h : v)
                    holes.push_back(Sorted(h));
                std::sort(holes.begin(), holes.end());
                std::string out;
                for (size_t i = 0; i < holes.size(); ++i)
                    out += (i ? " | " : "") + holes[i];
                return out;
            }
            // 屋顶的山墙，按位置：每条竖直的边的中点，天井的边接着外轮廓编号
            if (k == "gables")
            {
                std::vector<std::pair<Json, Json>> edges;
                std::vector<Json> rings = { e.contains("boundary") ? e["boundary"] : Json::array() };
                if (e.contains("courtyards"))
                    for (const Json& c : e["courtyards"])
                        rings.push_back(c);
                for (const Json& r : rings)
                    for (size_t i = 0; i < r.size(); ++i)
                        edges.emplace_back(r[i], r[(i + 1) % r.size()]);
                std::vector<std::string> mids;
                for (const Json& i : v)
                {
                    const size_t n = i.get<size_t>();
                    if (n < edges.size())
                        mids.push_back(Mm(Json::array({ (edges[n].first[0].get<double>() + edges[n].second[0].get<double>()) / 2.0,
                                                        (edges[n].first[1].get<double>() + edges[n].second[1].get<double>()) / 2.0 })));
                }
                std::sort(mids.begin(), mids.end());
                std::string out;
                for (size_t i = 0; i < mids.size(); ++i)
                    out += (i ? " " : "") + mids[i];
                return out;
            }
            if (k == "area_m2" || k == "name" || k == "number")
                return v.dump();
            if (k == "slope")
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.4f", v.is_number() ? v.get<double>() : NAN);
                return buf;
            }
            // 贴着什么：屋顶或楼板，不管 ID
            if (k == "attach_top")
            {
                const auto g = guid(v);
                const Element* x = g ? m.elements.Get(*g) : nullptr;
                return x ? x->Category() : "-";
            }
            return Mm(v);
        };
        // 直墙不说弧；贴着屋顶的墙顶跟着屋顶，不管它的顶部约束（IFC 不带）
        const auto untold = [&](const std::string& k)
        {
            const bool optional = k == "arc" || k == "attach_top" || k == "slope" || k == "host" || k == "gables" || k == "openings" || k == "courtyards";
            return (optional && (!e.contains(k) || e[k].is_null() || (e[k].is_array() && e[k].empty()) || (k == "arc" && e[k] == 0) ||
                                 (k == "slope" && e[k] == 0))) ||
                   (k == "top_level" && e.contains("attach_top") && !e["attach_top"].is_null());
        };
        std::string out;
        for (const char* k : keys)
        {
            if (untold(k))
                continue;
            out += (out.empty() ? "" : " ") + std::string(k) + "=" + text(k, e.contains(k) ? e[k] : Json());
        }
        return out;
    }

    // 一个类别的每个元素，描述后排好
    std::vector<std::string> Building(const Model& m, std::string_view category)
    {
        std::vector<std::string> all;
        for (const auto& [id, e] : m.elements.Items())
            if (category == e->Category())
                all.push_back(Describe(m, id));
        std::sort(all.begin(), all.end());
        return all;
    }

    std::string Join(const std::vector<std::string>& v)
    {
        std::string out;
        for (const std::string& s : v)
            out += "    " + s + "\n";
        return out;
    }

    // ── STEP ─────────────────────────────────────────────────
    void TestStep()
    {
        using Ifc::Step::V;
        Ifc::Step::File f;
        std::string error;
        const bool ok = Ifc::Step::Parse("ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('a;b'),'2;1');\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n"
                                         "#1=IFCX('it''s',$,*,-1.5E2,3.,7,.T.,#22,(1,(2,'a')),IFCLABEL('b'),1.E-05);\n"
                                         "/* a note; with ) */ #2 = IFCY ( ) ;\n#3=(IFCA()IFCB(1));\nENDSEC;\nEND-ISO-10303-21;\n",
                                         f, error);
        const std::vector<V> want = { V::Str("it's"), V(),       V(),       V::Real(-150.0), V::Real(3.0), V::Int(7), V::Enum("T"), V::Ref(22),
                                      V::List({ V::Int(1), V::List({ V::Int(2), V::Str("a") }) }), V::Typed("IFCLABEL", V::Str("b")), V::Real(1e-5) };
        Check(ok && f.schema == "IFC4" && f.entities[1].name == "IFCX" && f.entities[1].args == want && f.entities[2].name == "IFCY" &&
                  f.entities[2].args.empty() && !f.entities.count(3),
              "STEP：实体与参数照写的读出，复合实例跳过");

        Check(Ifc::Step::Decode("\\X2\\00D6\\X0\\ffice") == "\xC3\x96" "ffice" && Ifc::Step::Decode("caf\\X\\E9") == "caf\xC3\xA9" &&
                  Ifc::Step::Decode("a\\\\b") == "a\\b" && Ifc::Step::Decode("\\X4\\0001F600\\X0\\!") == "\xF0\x9F\x98\x80!" &&
                  Ifc::Step::Decode("\\S\\D") == "\xC3\x84" && Ifc::Step::Decode("\\PA\\x") == "x" && Ifc::Step::Decode("\\X2\\D83D\\X0\\") == "\xEF\xBF\xBD" &&
                  Ifc::Step::Decode("\\X2\\00").empty(),
              "STEP：字符串的转义还原（UTF-16、UTF-32、ISO 8859-1、代码页上半、单独的代理、截断）");

        const std::string head = "ISO-10303-21;HEADER;FILE_SCHEMA(('IFC4'));ENDSEC;DATA;";
        const std::string broken[] = {
            "",
            "HEADER;",
            head + "#1=IFCX('open",
            head + "#1=IFCX(1,2",
            head + "#99999999999999999999999=IFCX();ENDSEC;",
            head + "#1=IFCX(.T);ENDSEC;",
            head + "#1=IFCX(1.2.3);ENDSEC;",
            head + "#1=IFCX(@);ENDSEC;",
            head + "#1=IFCX();",
            head + "#1=IFCX(" + std::string(500, '(') + "1" + std::string(500, ')') + ");ENDSEC;",
        };
        bool allFail = true;
        for (const std::string& text : broken)
        {
            Ifc::Step::File g;
            std::string why;
            if (Ifc::Step::Parse(text, g, why) || why.empty())
            {
                std::printf("  读出来了：%s\n", text.substr(0, 80).c_str());
                allFail = false;
            }
        }
        Check(allFail, "STEP：坏文件是错误，不会崩溃");
    }

    // ── 导出 ─────────────────────────────────────────────────
    void TestGlobalIds()
    {
        bool ok = true;
        std::set<std::string> all;
        for (uint64_t i = 0; i < 2000; ++i)
        {
            const Guid g = Guid::Seeded(7, i);
            const std::string c = Ifc::Compress(g);
            ok &= c.size() == 22 && c[0] >= '0' && c[0] <= '3' && Ifc::Expand(c) == g;
            for (const char ch : c)
                ok &= std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '$';
            all.insert(c);
        }
        Check(ok && all.size() == 2000 && !Ifc::Expand("4zzzzzzzzzzzzzzzzzzzzz") && !Ifc::Expand("0000000000000000000000x") &&
                  !Ifc::Expand("00000000000000000000.0") && Ifc::Expand("0000000000000000000000") == Guid{},
              "GlobalId：22 位 64 进制、第一位不超过 3，与 GUID 来回不变");
    }

    void TestExportStrings()
    {
        Session s;
        Model m = s.GetModel();
        m.name = "it's \xC3\x81rea \xF0\x9F\x98\x80 a\\b";
        const std::string f = Ifc::Export(m);
        const size_t project = f.find("=IFCPROJECT(");
        Check(Contains(f, "'it''s \\X2\\00C1\\X0\\rea \\X4\\0001F600\\X0\\ a\\\\b'") && Text::StartsWith(f, "ISO-10303-21;") &&
                  Contains(f, "FILE_SCHEMA(('IFC4'));") && Text::EndsWith(f, "END-ISO-10303-21;\n"),
              "导出：名称里的引号、反斜杠与别的文字照 STEP 转义\n  " + f.substr(project, f.find('\n', project) - project));
        const size_t at = f.find("FILE_NAME(");
        const std::string date = at == std::string::npos ? std::string() : f.substr(f.find(",'", at) + 2, 19);
        Check(date.size() == 19 && date[4] == '-' && date[7] == '-' && date[10] == 'T' && date[13] == ':' && date[16] == ':',
              "导出：文件头写 UTC 时间（ISO 8601）");
    }

    void TestExportSample()
    {
        Session s;
        Script(s, SampleScript);
        const Model& m = s.GetModel();
        const std::string f = Ifc::Export(m);
        const auto n = [&](const char* cat) { return Only(m, cat).size(); };
        const size_t openings = n("door") + n("window");
        Check(Count(f, "IFCPROJECT") == 1 && Count(f, "IFCSITE") == 1 && Count(f, "IFCBUILDING") == 1 && Count(f, "IFCBUILDINGSTOREY") == m.Levels().size() &&
                  Count(f, "IFCWALL") == n("wall") && Count(f, "IFCDOOR") == n("door") && Count(f, "IFCWINDOW") == n("window") &&
                  Count(f, "IFCSLAB") == n("floor") && Count(f, "IFCSPACE") == n("room") && Count(f, "IFCROOF") == n("roof") &&
                  Count(f, "IFCCOVERING") == n("ceiling") && Count(f, "IFCOPENINGELEMENT") == openings && Count(f, "IFCRELVOIDSELEMENT") == openings &&
                  Count(f, "IFCRELFILLSELEMENT") == openings && Contains(f, "'Kitchen'"),
              "导出：示例房屋的空间结构与每个元素都在，门窗各开一个洞口，房间保留名称");
        Check(!Dangling(f), "导出：每个引用都指向只写了一次的实体");
        Check(Count(f, "IFCELEMENTQUANTITY") >= n("wall") + openings && Contains(f, "'Qto_WallBaseQuantities'") &&
                  Contains(f, "'Qto_SlabBaseQuantities'") && Contains(f, "'Qto_DoorBaseQuantities'") && Contains(f, "'Qto_SpaceBaseQuantities'"),
              "导出：墙、楼板、门窗、房间带基本数量");
    }

    void TestExportComponents()
    {
        Session s;
        Script(s, R"(component {"family": "Table", "type": "1600 x 900", "at": [2000, 3000], "angle": 30}
component {"family": "WC", "at": [500, 0], "mirrored": true})");
        const std::string f = Ifc::Export(s.GetModel());
        Check(Count(f, "IFCFURNITURE") == 1 && Count(f, "IFCSANITARYTERMINAL") == 1 && Contains(f, "'Table:1600 x 900'"),
              "导出：构件按族的类别写成元素，按 族:类型 命名");
        Check(Count(f, "IFCTRIANGULATEDFACESET") == 8 && Count(f, "IFCMATERIAL") == 2 && Contains(f, "IFCMATERIAL('Timber'") &&
                  Contains(f, "IFCMATERIAL('Porcelain'") && Count(f, "IFCSTYLEDITEM") == 2 + 8 && Count(f, "IFCRELASSOCIATESMATERIAL") == 2,
              "导出：每个形体一个闭合三角面集，每种材质写一次，它的颜色给用它的形体着色");
        // 在它的楼层里
        const size_t at = f.find("=IFCFURNITURE(");
        const std::string table = at == std::string::npos ? std::string("#?") : f.substr(f.rfind('#', at), at - f.rfind('#', at));
        bool contained = false;
        for (size_t p = f.find("IFCRELCONTAINEDINSPATIALSTRUCTURE("); p != std::string::npos; p = f.find("IFCRELCONTAINEDINSPATIALSTRUCTURE(", p + 1))
        {
            const std::string line = f.substr(p, f.find('\n', p) - p);
            contained |= Contains(line, table + ",") || Contains(line, table + ")");
        }
        Check(contained && !Dangling(f), "导出：构件在它的楼层里");
    }

    void TestExportFamilyOpenings()
    {
        Session s;
        Script(s, R"(wall {"points": [[0, 0], [8000, 0], [8000, 6000], [0, 6000]], "closed": true}
door {"at": [2000, 0], "family": "Single Door", "type": "800 x 2100"}
window {"at": [8000, 3000], "family": "Casement Window"}
door {"at": [5000, 0]})");
        const std::string f = Ifc::Export(s.GetModel());
        Check(Count(f, "IFCDOOR") == 2 && Count(f, "IFCWINDOW") == 1 && Count(f, "IFCOPENINGELEMENT") == 3 && Contains(f, "'Single Door:800 x 2100'") &&
                  Contains(f, "'Casement Window:1200 x 1500'") && Contains(f, "'Single 900 x 2100'"),
              "导出：族的门窗按 族:类型 命名，项目自己的类型按它的名称");
        bool materials = true;
        for (const char* mat : { "Timber", "Metal", "Glass" })
            materials &= Contains(f, std::string("IFCMATERIAL('") + mat + "'");
        Check(Count(f, "IFCTRIANGULATEDFACESET") == 6 + 4 + 4 && materials && Count(f, "IFCRELASSOCIATESMATERIAL") == 2 && !Dangling(f),
              "导出：族的门窗写出它的形体（三角面集）与材质，普通门没有");
    }

    void TestExportLayers()
    {
        Session s;
        Script(s, R"(type {"category": "wall", "name": "Cavity", "layers": [{"function": "finish", "material": "Brick", "thickness": 100}, {"function": "structure", "material": "Concrete block", "thickness": 200}]}
wall {"points": [[0, 0], [0, 6000], [8000, 6000], [8000, 0]], "type": "Cavity"}
wall {"points": [[8000, 0], [0, 0]], "arc": -60, "type": "Cavity", "location": "interior-finish"}
window {"at": [4000, -1100]}
roof {"gable": true, "boundary": [[-600, -1800], [8600, -1800], [8600, 6600], [-600, 6600]]}
modify {"ids": [100002, 100004], "attach_top": 100007}
door {"at": [4000, 6000]})");
        const std::string f = Ifc::Export(s.GetModel());
        Check(Count(f, "IFCMATERIAL") == 2 && Count(f, "IFCMATERIALLAYER") == 2 && Count(f, "IFCMATERIALLAYERSET") == 1 &&
                  Count(f, "IFCMATERIALLAYERSETUSAGE") == 2 && Count(f, "IFCRELASSOCIATESMATERIAL") == 2 && Contains(f, ".AXIS2.,.NEGATIVE.,150.,$)") &&
                  Contains(f, ".AXIS2.,.NEGATIVE.,300.,$)") && Contains(f, "IFCMATERIALLAYER(#") && Contains(f, "'LoadBearing'") && Contains(f, "'Brick'"),
              "导出：复合墙的层写成材质层组，中心线上的墙与立在内面的弧墙各一个用法");
        bool sized = true;
        for (size_t p = f.find("=IFCRECTANGLEPROFILEDEF("); p != std::string::npos; p = f.find("=IFCRECTANGLEPROFILEDEF(", p + 1))
        {
            const std::string line = f.substr(p, f.find('\n', p) - p);
            sized &= !Contains(line, ",0.)") && !Contains(line, ",0.,");
        }
        Check(Occurrences(f, "'Body','Brep'") == 3 && Count(f, "IFCWALL") == 4 && Count(f, "IFCOPENINGELEMENT") == 2 && Count(f, "IFCWINDOW") == 1 &&
                  sized && !Dangling(f),
              "导出：贴着屋顶的墙与屋顶是闭合壳，其余拉伸；穿过直墙与弧墙的洞都有尺寸");
    }

    void TestSaveIfc()
    {
        Session s;
        House(s);
        const std::string path = Temp("house.ifc");
        const Json r = Run(s, "save", Json{ { "path", path } });
        Check(r["format"] == "IFC4" && Contains(ReadText(path), "FILE_SCHEMA(('IFC4'));") && !s.Path(), "保存为 .ifc：导出 IFC4，不改项目自己的路径");
        std::error_code ec;
        std::filesystem::remove(PathOf(path), ec);
    }

    // ── 导入 ─────────────────────────────────────────────────
    // 毫米的 IFC4 文件：墙是没有定位线、朝北的三角网格，柱经映射项取它的类型的形状；site 放置这一切
    std::string Meshes(const std::string& site)
    {
        const std::string pts = "(0.,-100.,0.),(5000.,-100.,0.),(5000.,100.,0.),(0.,100.,0.),(0.,-100.,2700.),(5000.,-100.,2700.),(5000.,100.,2700.),(0.,100.,2700.)";
        const std::string tris = "(1,3,2),(1,4,3),(5,6,7),(5,7,8),(1,2,6),(1,6,5),(2,3,7),(2,7,6),(3,4,8),(3,8,7),(4,1,5),(4,5,8)";
        return "ISO-10303-21;HEADER;FILE_SCHEMA(('IFC4'));ENDSEC;DATA;\n"
               "#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);\n#2=IFCUNITASSIGNMENT((#1));\n#3=IFCPROJECT('p',$,'Meshes',$,$,$,$,$,#2);\n"
               "#4=IFCCARTESIANPOINT((0.,0.,0.));\n#5=IFCAXIS2PLACEMENT3D(#4,$,$);\n#6=IFCLOCALPLACEMENT(#62,#5);\n"
               "#7=IFCBUILDINGSTOREY('s',$,'Level 0',$,$,#6,$,$,.ELEMENT.,0.);\n"
               "#10=IFCCARTESIANPOINTLIST3D((" + pts + "));\n#11=IFCTRIANGULATEDFACESET(#10,$,.T.,(" + tris + "),$);\n"
               "#12=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#11));\n#13=IFCPRODUCTDEFINITIONSHAPE($,$,(#12));\n"
               "#14=IFCCARTESIANPOINT((1000.,2000.,0.));\n#15=IFCDIRECTION((0.,1.,0.));\n#16=IFCDIRECTION((0.,0.,1.));\n"
               "#17=IFCAXIS2PLACEMENT3D(#14,#16,#15);\n#18=IFCLOCALPLACEMENT(#6,#17);\n#19=IFCWALL('w',$,'Wall',$,$,#18,#13,$,$);\n"
               "#30=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,300.,500.);\n#31=IFCEXTRUDEDAREASOLID(#30,$,#16,3000.);\n"
               "#32=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#31));\n#33=IFCREPRESENTATIONMAP(#5,#32);\n"
               "#34=IFCCARTESIANTRANSFORMATIONOPERATOR3D($,$,#4,$,$);\n#35=IFCMAPPEDITEM(#33,#34);\n"
               "#36=IFCSHAPEREPRESENTATION($,'Body','MappedRepresentation',(#35));\n#37=IFCPRODUCTDEFINITIONSHAPE($,$,(#36));\n"
               "#38=IFCCARTESIANPOINT((3000.,4000.,0.));\n#39=IFCAXIS2PLACEMENT3D(#38,#16,$);\n#40=IFCLOCALPLACEMENT(#6,#39);\n"
               "#41=IFCCOLUMN('c',$,'Column',$,$,#40,#37,$,$);\n#50=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#19,#41),#7);\n"
               "#60=IFCCARTESIANPOINT((" + site + "));\n#61=IFCAXIS2PLACEMENT3D(#60,$,$);\n#62=IFCLOCALPLACEMENT($,#61);\n"
               "ENDSEC;END-ISO-10303-21;";
    }

    void TestImportMeshes()
    {
        Ifc::Imported i;
        const bool ok = Import(Meshes("0.,0.,0."), i);
        const auto walls = Only(i.model, "wall");
        const Wall* w = walls.empty() ? nullptr : walls.front()->As<Wall>();
        Check(ok && w && w->start == P2(1000, 2000) && w->end == P2(1000, 7000) && i.model.Thickness(*w) == 200.0 && w->height == 2700.0 && !i.moved,
              "导入：三角网格的墙与拉伸体一样按几何量出");
        Check(i.skipped == std::vector<std::string>{ "1 IFCCOLUMN: MiniBIM has no element for it" }, "导入：MiniBIM 还没有的元素（柱）略去并列出\n" + Skipped(i));

        Ifc::Imported far;
        Import(Meshes("500000000.,5000000000.,0."), far);
        const auto farWalls = Only(far.model, "wall");
        const Wall* fw = farWalls.empty() ? nullptr : farWalls.front()->As<Wall>();
        Check(fw && fw->start == P2(0, 0) && fw->end == P2(0, 5000) && far.moved == P2(-500001000.0, -5000002000.0), "导入：地图坐标上的模型移到原点附近");
    }

    // 毫米的 IFC4 文件：6 × 4 m、200 mm 厚的板，轮廓绕着两个洞，一个洞口元素穿过东边那个（网格会带着这个洞）：
    // slab 写出板 #35 与它属于什么；楼层容纳 within
    std::string Holed(const std::string& slab, int within, bool eastHole = true)
    {
        const auto rect = [](int id, double x0, double y0, double x1, double y1)
        {
            std::string out;
            const double xy[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
            for (int i = 0; i < 4; ++i)
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "#%d=IFCCARTESIANPOINT((%.1f,%.1f));\n", id + 1 + i, xy[i][0], xy[i][1]);
                out += buf;
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), "#%d=IFCPOLYLINE((#%d,#%d,#%d,#%d,#%d));", id, id + 1, id + 2, id + 3, id + 4, id + 1);
            return out + buf;
        };
        return "ISO-10303-21;HEADER;FILE_SCHEMA(('IFC4'));ENDSEC;DATA;\n"
               "#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);\n#2=IFCUNITASSIGNMENT((#1));\n#3=IFCPROJECT('p',$,'Holes',$,$,$,$,$,#2);\n"
               "#4=IFCCARTESIANPOINT((0.,0.,-200.));\n#5=IFCAXIS2PLACEMENT3D(#4,$,$);\n#6=IFCLOCALPLACEMENT($,#5);\n"
               "#7=IFCBUILDINGSTOREY('s',$,'Level 0',$,$,$,$,$,.ELEMENT.,0.);\n" +
               rect(10, 0, 0, 6000, 4000) + "\n" + rect(20, 1000, 1000, 2500, 2000) + "\n" + rect(25, 4000, 1000, 5000, 2000) + "\n" +
               (eastHole ? "#30=IFCARBITRARYPROFILEDEFWITHVOIDS(.AREA.,$,#10,(#20,#25));\n" : "#30=IFCARBITRARYPROFILEDEFWITHVOIDS(.AREA.,$,#10,(#20));\n") +
               "#31=IFCDIRECTION((0.,0.,1.));\n#32=IFCEXTRUDEDAREASOLID(#30,$,#31,200.);\n#33=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#32));\n"
               "#34=IFCPRODUCTDEFINITIONSHAPE($,$,(#33));\n" + slab + "\n"
               "#40=IFCCARTESIANPOINT((4500.,1500.));\n#41=IFCAXIS2PLACEMENT2D(#40,$);\n#42=IFCRECTANGLEPROFILEDEF(.AREA.,$,#41,1000.,1000.);\n"
               "#43=IFCEXTRUDEDAREASOLID(#42,$,#31,200.);\n#44=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#43));\n"
               "#45=IFCPRODUCTDEFINITIONSHAPE($,$,(#44));\n#46=IFCOPENINGELEMENT('o',$,'Hole',$,$,#6,#45,$,.OPENING.);\n"
               "#47=IFCRELVOIDSELEMENT('v',$,$,$,#35,#46);\n#50=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(#" + std::to_string(within) + "),#7);\n"
               "ENDSEC;END-ISO-10303-21;";
    }

    const std::vector<Ring> kBothHoles = { { P2(1000, 1000), P2(1000, 2000), P2(2500, 2000), P2(2500, 1000) },
                                           { P2(4000, 1000), P2(4000, 2000), P2(5000, 2000), P2(5000, 1000) } };

    void TestImportHoles()
    {
        Ifc::Imported i;
        Import(Holed("#35=IFCSLAB('f',$,'Slab',$,$,#6,#34,$,.FLOOR.);", 35), i);
        const auto floors = Only(i.model, "floor");
        const Floor* f = floors.empty() ? nullptr : floors.front()->As<Floor>();
        double area = 0.0;
        for (const auto& [id, e] : i.model.elements.Items())
            if (e->As<Floor>())
                area = ElementJson(i.model, id)["area_m2"].get<double>();
        Check(i.skipped.empty() && f && f->openings == kBothHoles && area == 21.5, "导入：楼板的洞（包括洞口元素穿过的）读作它的开洞\n" + Skipped(i));

        Ifc::Imported c;
        Import(Holed("#35=IFCCOVERING('f',$,'Ceiling',$,$,#6,#34,$,.CEILING.);", 35), c);
        const auto ceilings = Only(c.model, "ceiling");
        const Ceiling* cl = ceilings.empty() ? nullptr : ceilings.front()->As<Ceiling>();
        Check(c.skipped.empty() && cl && cl->openings == kBothHoles, "导入：天花板的洞读作它的开洞\n" + Skipped(c));

        // 屋顶，或者屋顶的一部分：洞口一样穿过它
        const std::string part = "#35=IFCSLAB('f',$,'Slab',$,$,#6,#34,$,.ROOF.);\n#36=IFCROOF('r',$,'Roof',$,$,#6,$,$,.FLAT_ROOF.);\n"
                                 "#37=IFCRELAGGREGATES('a',$,$,$,#36,(#35));";
        bool roofs = true;
        for (const auto& [slab, within] : { std::pair<std::string, int>{ "#35=IFCROOF('f',$,'Roof',$,$,#6,#34,$,.FLAT_ROOF.);", 35 }, { part, 36 } })
        {
            Ifc::Imported r;
            Import(Holed(slab, within), r);
            const auto rs = Only(r.model, "roof");
            const Roof* roof = rs.empty() ? nullptr : rs.front()->As<Roof>();
            roofs &= r.skipped.empty() && roof && roof->openings == kBothHoles;
            if (!r.skipped.empty())
                std::printf("%s", Skipped(r).c_str());
        }
        Check(roofs, "导入：屋顶（或屋顶的一部分）的洞读作它的开洞");

        // 实体里没有的洞：MiniBIM 还没有管井，说出来
        Ifc::Imported cut;
        Import(Holed("#35=IFCSLAB('f',$,'Slab',$,$,#6,#34,$,.FLOOR.);", 35, false), cut);
        const auto cutFloors = Only(cut.model, "floor");
        Check(cutFloors.size() == 1 && cut.skipped.size() == 1 && Contains(cut.skipped.front(), "IFCOPENINGELEMENT #46") &&
                  Contains(cut.skipped.front(), "no shafts"),
              "导入：洞口元素切出的洞不在楼板的实体里时列入略去（还没有管井）\n" + Skipped(cut));
    }

    // file 里给元素 #id 150 mm 砖再 50 mm 抹灰，它的用法从 offset 起沿它自己的 axis 叠层
    std::string Layered(const std::string& file, int id, const std::string& axis, double offset)
    {
        const std::string set = "#80=IFCMATERIAL('Brick',$,$);\n#81=IFCMATERIAL('Plaster',$,$);\n#82=IFCMATERIALLAYER(#80,150.,$,$,$,'Finish',$);\n"
                                "#83=IFCMATERIALLAYER(#81,50.,$,$,$,'LoadBearing',$);\n#84=IFCMATERIALLAYERSET((#82,#83),'Brick and plaster',$);";
        char usage[256];
        std::snprintf(usage, sizeof(usage), "#85=IFCMATERIALLAYERSETUSAGE(#84,.%s.,.POSITIVE.,%.1f,$);\n#86=IFCRELASSOCIATESMATERIAL('m',$,$,$,(#%d),#85);",
                      axis.c_str(), offset, id);
        return Replace(file, "ENDSEC;END", set + "\n" + usage + "\nENDSEC;END");
    }

    std::vector<std::string> Materials(const std::vector<Layer>& layers)
    {
        std::vector<std::string> out;
        for (const Layer& l : layers)
            out.push_back(l.material);
        return out;
    }

    void TestImportLayers()
    {
        // 墙朝北，它自己的 y 朝西：第一层在东边，所以它朝南走
        Ifc::Imported i;
        Import(Layered(Meshes("0.,0.,0."), 19, "AXIS2", -100.0), i);
        const auto walls = Only(i.model, "wall");
        const Wall* w = walls.empty() ? nullptr : walls.front()->As<Wall>();
        Check(w && w->start == P2(1000, 7000) && w->end == P2(1000, 2000) &&
                  Materials(i.model.types.wall.at(w->typeName).layers) == std::vector<std::string>{ "Brick", "Plaster" },
              "导入：沿元素自己的轴往上叠的层从它的背面起（墙）");
        Ifc::Imported s;
        Import(Layered(Holed("#35=IFCSLAB('f',$,'Slab',$,$,#6,#34,$,.FLOOR.);", 35), 35, "AXIS3", 0.0), s);
        const auto floors = Only(s.model, "floor");
        const Floor* f = floors.empty() ? nullptr : floors.front()->As<Floor>();
        Check(f && Materials(s.model.types.floor.at(f->typeName).layers) == std::vector<std::string>{ "Plaster", "Brick" },
              "导入：沿元素自己的轴往上叠的层从它的背面起（板的第一层在底面）");

        // 着色样式引用自己：材质只着一次色，不会死循环
        const std::string style = "#90=IFCMATERIALDEFINITIONREPRESENTATION($,$,(#91),#80);\n#91=IFCSTYLEDREPRESENTATION($,'Style','Material',(#92));\n"
                                  "#92=IFCSTYLEDITEM($,(#93),$);\n#93=IFCSURFACESTYLE('Brick',.BOTH.,(#93,#94));\n#94=IFCSURFACESTYLESHADING(#95,$);\n"
                                  "#95=IFCCOLOURRGB($,1.,0.,0.);";
        Ifc::Imported c;
        Import(Replace(Layered(Meshes("0.,0.,0."), 19, "AXIS2", -100.0), "ENDSEC;END", style + "\nENDSEC;END"), c);
        Check(c.model.materials.count("Brick") && c.model.materials.at("Brick").color == std::array<uint8_t, 3>{ 255, 0, 0 },
              "导入：引用自己的样式只给材质着一次色");

        const std::string membrane = Replace(Replace(Layered(Meshes("0.,0.,0."), 19, "AXIS2", -100.0), "IFCMATERIALLAYERSET((#82,#83)",
                                                     "IFCMATERIALLAYERSET((#82,#87,#83)"),
                                             "ENDSEC;END", "#87=IFCMATERIALLAYER(#81,0.,$,$,$,'Finish',$);\nENDSEC;END");
        Ifc::Imported mb;
        Import(membrane, mb);
        const auto mw = Only(mb.model, "wall");
        bool layersOk = false;
        if (!mw.empty())
        {
            const auto& layers = mb.model.types.wall.at(mw.front()->As<Wall>()->typeName).layers;
            layersOk = layers.size() == 3 && layers[0].function == Function::Finish && layers[0].thickness == 150.0 &&
                       layers[1].function == Function::Membrane && layers[1].thickness == 0.0 && layers[2].function == Function::Structure &&
                       layers[2].thickness == 50.0;
        }
        Check(layersOk, "导入：厚度为 0 的层是膜");
    }

    // 构件在模型里的范围：轮廓的角点与高度，到毫米
    std::array<long long, 6> Extent(const Model& m, const Component& c)
    {
        double x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY, z0 = INFINITY, z1 = -INFINITY;
        for (const Fam::Piece& p : m.ComponentPieces(c))
        {
            for (const P2& q : p.outline)
            {
                x0 = std::min(x0, q.x);
                x1 = std::max(x1, q.x);
                y0 = std::min(y0, q.y);
                y1 = std::max(y1, q.y);
            }
            z0 = std::min(z0, p.bottom);
            z1 = std::max(z1, p.top);
        }
        const auto r = [](double v) { return std::isfinite(v) ? std::llround(v) : 0LL; };
        return { r(x0), r(x1), r(y0), r(y1), r(z0), r(z1) };
    }

    std::vector<Component> Components(const Model& m)
    {
        std::vector<Component> out;
        for (const auto& [_, e] : m.elements.Items())
            if (const Component* c = e->As<Component>())
                out.push_back(*c);
        std::stable_sort(out.begin(), out.end(), [](const Component& a, const Component& b) { return a.family < b.family; });
        return out;
    }

    void TestImportComponents()
    {
        Session s;
        Script(s, R"(component {"family": "Table", "type": "1600 x 900", "at": [2000, 3000], "angle": 30}
component {"family": "WC", "at": [-500, 0]}
component {"family": "WC", "at": [500, 0], "mirrored": true, "offset": 50}
component {"family": "Bed", "at": [6000, 4000], "angle": -90}
level {"elevation": 3000, "name": "Level 3"})");
        Ifc::Imported i;
        Import(Ifc::Export(s.GetModel()), i);
        const auto before = Components(s.GetModel()), after = Components(i.model);
        bool same = i.skipped.empty() && after.size() == 4 && before.size() == 4;
        for (size_t k = 0; same && k < 4; ++k)
        {
            const Level* la = s.GetModel().GetLevel(before[k].level);
            const Level* lb = i.model.GetLevel(after[k].level);
            same &= before[k].family == after[k].family && before[k].typeName == after[k].typeName && la && lb && la->name == lb->name &&
                    Extent(s.GetModel(), before[k]) == Extent(i.model, after[k]);
            if (!same)
                std::printf("  %s %s 不一样\n", before[k].family.c_str(), before[k].typeName.c_str());
        }
        Check(same, "导入：构件从 IFC 回来时立在原处，族、类型、标高与范围不变\n" + Skipped(i));
        const auto of = [&](const char* family) -> const Component*
        {
            const Component* last = nullptr;
            for (const Component& c : after)
                if (c.family == family)
                    last = &c;
            return last;
        };
        std::vector<std::pair<std::string, bool>> wc;
        for (const Component& c : after)
            if (c.family == "WC")
                wc.emplace_back(c.typeName, c.mirrored);
        Check(of("Table") && of("Table")->angle == 30.0 && of("WC") && of("WC")->offset == 50.0 &&
                  wc == std::vector<std::pair<std::string, bool>>{ { "400 x 650", false }, { "400 x 650", true } },
              "导入：构件的转角与高度不变，镜像的是同一类型的镜像");
        std::set<std::string> madeOf;
        if (i.model.families.count("Bed"))
            for (const Fam::Form& f : i.model.families.at("Bed").forms)
                madeOf.insert(f.material);
        Check(madeOf == std::set<std::string>{ "Fabric", "Timber" } && i.model.materials.count("Fabric") &&
                  i.model.materials.at("Fabric").color == s.GetModel().materials.at("Fabric").color,
              "导入：形体照文件里的材质，材质带着颜色回来");
    }

    // 毫米的 IFC4 文件：n 个代理名为 name，各是过 ring 角点（按它的序号缩放）的轮廓沿 dir 拉伸 depth
    std::string Proxies(const std::string& name, size_t n, const std::vector<std::pair<double, double>>& ring, const std::string& dir, double depth)
    {
        std::string out = "ISO-10303-21;HEADER;FILE_SCHEMA(('IFC4'));ENDSEC;DATA;\n#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);\n"
                          "#2=IFCUNITASSIGNMENT((#1));\n#3=IFCPROJECT('p',$,'Proxies',$,$,$,$,$,#2);\n#4=IFCCARTESIANPOINT((0.,0.,0.));\n"
                          "#5=IFCAXIS2PLACEMENT3D(#4,$,$);\n#6=IFCLOCALPLACEMENT($,#5);\n#7=IFCBUILDINGSTOREY('s',$,'Level 0',$,$,#6,$,$,.ELEMENT.,0.);\n";
        std::string ids;
        char buf[512];
        for (size_t k = 0; k < n; ++k)
        {
            const size_t base = 100 + k * 20;
            const double scale = 1.0 + k / 10.0;
            std::string refs;
            for (size_t i = 0; i < ring.size(); ++i)
            {
                std::snprintf(buf, sizeof(buf), "#%zu=IFCCARTESIANPOINT((%.1f,%.1f));\n", base + i, ring[i].first * scale, ring[i].second * scale);
                out += buf;
                refs += "#" + std::to_string(base + i) + ",";
            }
            refs += "#" + std::to_string(base);
            std::snprintf(buf, sizeof(buf),
                          "#%zu=IFCPOLYLINE((%s));\n#%zu=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#%zu);\n#%zu=IFCDIRECTION((%s));\n"
                          "#%zu=IFCEXTRUDEDAREASOLID(#%zu,$,#%zu,%.1f);\n",
                          base + 10, refs.c_str(), base + 11, base + 10, base + 12, dir.c_str(), base + 13, base + 11, base + 12, depth);
            out += buf;
            std::snprintf(buf, sizeof(buf),
                          "#%zu=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#%zu));\n#%zu=IFCPRODUCTDEFINITIONSHAPE($,$,(#%zu));\n"
                          "#%zu=IFCCARTESIANPOINT((%zu.,0.,0.));\n#%zu=IFCAXIS2PLACEMENT3D(#%zu,$,$);\n#%zu=IFCLOCALPLACEMENT(#6,#%zu);\n"
                          "#%zu=IFCBUILDINGELEMENTPROXY('g%zu',$,'%s',$,$,#%zu,#%zu,$,.ELEMENT.);\n",
                          base + 14, base + 13, base + 15, base + 14, base + 16, k * 3000, base + 17, base + 16, base + 18, base + 17, base + 19, k,
                          name.c_str(), base + 18, base + 15);
            out += buf;
            ids += (ids.empty() ? "#" : ",#") + std::to_string(base + 19);
        }
        return out + "#99=IFCRELCONTAINEDINSPATIALSTRUCTURE('r',$,$,$,(" + ids + "),#7);\nENDSEC;END-ISO-10303-21;";
    }

    // 闭合面的体积：各三角形的有向四面体之和，面朝外时为正
    double Volume(const std::vector<Fam::Piece>& pieces)
    {
        double v = 0.0;
        for (const Fam::Piece& p : pieces)
            for (const auto& [a, b, c] : p.tris)
                v += V3::Dot(a, V3::Cross(b, c)) / 6.0;
        return v;
    }

    void TestImportProxies()
    {
        const std::vector<std::pair<double, double>> clockwise = { { 0, 0 }, { 0, 300 }, { 500, 300 }, { 500, 0 } };
        const std::vector<std::pair<double, double>> ccw = { { 0, 0 }, { 500, 0 }, { 500, 300 }, { 0, 300 } };
        bool ok = true;
        for (const auto& [dir, ring] : { std::pair{ std::string("0.,0.,1."), clockwise }, std::pair{ std::string("0.,0.,-1."), ccw } })
        {
            Ifc::Imported i;
            Import(Proxies("Box", 1, ring, dir, 450.0), i);
            const auto cs = Only(i.model, "component");
            const double v = cs.empty() ? 0.0 : Volume(i.model.ComponentLocalPieces(*cs.front()->As<Component>()));
            ok &= std::abs(v - 500.0 * 300.0 * 450.0) < 1.0;
            if (!ok)
                std::printf("  %s：%f\n", dir.c_str(), v);
        }
        Check(ok, "导入：顺时针的轮廓、向下的拉伸进来后面都朝外");

        const std::vector<std::pair<double, double>> square = { { 0, 0 }, { 300, 0 }, { 300, 300 }, { 0, 300 } };
        Ifc::Imported i;
        Import(Proxies("Thing:A", Fam::MaxForms + 3, square, "0.,0.,1.", 400.0), i);
        bool named = i.skipped.size() == 3;
        for (const std::string& s : i.skipped)
            named &= Contains(s, "Thing");
        Check(i.model.families.count("Thing") && i.model.families.at("Thing").types.size() == Fam::MaxForms && Only(i.model, "component").size() == Fam::MaxForms &&
                  named,
              "导入：一个族放不下的形状只略去多出的那些");
    }

    void TestImportSame()
    {
        // 板的环，以及在它之后的第二个项目（米制）：每次读都重新哈希，按哈希顺序进来的东西会每次不同
        const std::string more = "#60=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);\n#61=IFCUNITASSIGNMENT((#60));\n#62=IFCPROJECT('q',$,'Other',$,$,$,$,$,#61);";
        const std::string file = Replace(Holed("#35=IFCSLAB('f',$,'Slab',$,$,#6,#34,$,.FLOOR.);", 35), "ENDSEC;END", more + "\nENDSEC;END");
        // 每次读出的都是自己的项目，它的 uid 是自己的 GlobalId 种子；其余一样
        const auto read = [&]()
        {
            Ifc::Imported i;
            Import(file, i);
            i.model.uid = 0;
            std::string text, error;
            i.model.ToFile(text, error);
            return text;
        };
        const std::string first = read();
        bool same = !first.empty();
        for (int k = 0; k < 4; ++k)
            same &= read() == first;
        Check(same, "导入：同一个文件每次读出一样的模型（含 GUID）");
    }

    // ── 往返 ─────────────────────────────────────────────────
    void TestRoundTrip()
    {
        // 示例房屋加上弧墙、带坡度与洞的楼板、上层带洞的楼板、带洞的天花板、L 形带山墙与天窗的屋顶、拉伸屋顶、绕天井的屋顶，到 IFC 再回来
        Session s;
        Script(s, SampleScript);
        Script(s, R"(wall {"points": [[14000, 0], [18000, 0]], "arc": 90, "type": "Exterior - 300mm"}
floor {"boundary": [[11000, 0], [13000, 0], [13000, 4000], [11000, 4000]], "slope": 4, "arrow": [[12000, 0], [12000, 4000]], "openings": [[[11500, 1500], [12500, 1500], [12500, 2500], [11500, 2500]]]}
floor {"level": "Level 2", "boundary": [[0, 0], [6000, 0], [6000, 4000], [0, 4000]], "openings": [[[1000, 1500], [3500, 1500], [3500, 2500], [1000, 2500]]]}
ceiling {"boundary": [[80000, 0], [84000, 0], [84000, 4000], [80000, 4000]], "openings": [[[81000, 1000], [82500, 1000], [82500, 2000], [81000, 2000]]]}
roof {"level": "Level 2", "boundary": [[30000, 0], [42000, 0], [42000, 6000], [36000, 6000], [36000, 12000], [30000, 12000]], "slope": 25, "gables": [1], "openings": [[[33000, 1000], [35000, 1000], [35000, 2000], [33000, 2000]]]}
roof {"level": "Level 2", "extrusion": {"line": [[50000, 0], [58000, 0]], "profile": [[0, 0], [3000, 2000], [8000, 0]], "to": 6000}}
roof {"level": "Level 2", "boundary": [[60000, 0], [76000, 0], [76000, 16000], [60000, 16000]], "courtyards": [[[66000, 6000], [70000, 6000], [70000, 10000], [66000, 10000]]], "gables": [5]})");
        const std::string path = Temp("round-trip.ifc");
        Run(s, "save", Json{ { "path", path } });
        Session t;
        const Json r = Run(t, "open", Json{ { "path", path } });
        std::error_code ec;
        std::filesystem::remove(PathOf(path), ec);
        Check(r["format"] == "IFC4" && r["skipped"] == Json::array() && !t.Path(), "往返：IFC 作为新项目打开，没有略去的东西\n" + r.dump());
        const auto levels = [](const Model& m)
        {
            std::vector<std::pair<std::string, double>> out;
            for (const auto& [_, l] : m.Levels())
                out.emplace_back(l->name, l->elevation);
            return out;
        };
        Check(levels(t.GetModel()) == levels(s.GetModel()), "往返：标高不变");
        for (const char* category : { "wall", "door", "window", "floor", "room", "roof", "ceiling" })
        {
            const auto before = Building(s.GetModel(), category), after = Building(t.GetModel(), category);
            Check(!before.empty() && after == before,
                  std::string("往返：") + category + " 回来时一样" + (after == before ? "" : "\n  原来：\n" + Join(before) + "  回来：\n" + Join(after)));
        }
        // 像别的项目一样能保存
        const std::string project = Temp("imported.minibim");
        Check(!Run(t, "save", Json{ { "path", project } }).is_null(), "往返：导入的项目能存成项目文件");
        std::filesystem::remove(PathOf(project), ec);
    }

    // 米制的小 IFC2x3 文件，照别的 BIM 软件的写法：3 m 高的楼层；画在外面上、转成朝北、带定位线的墙；
    // 停在墙面上、没有定位线的隔墙；侧着切过墙的洞口里的窗；L 形的板；一个房间；两条轴线；一张桌子
    const char* const kOffice = R"(ISO-10303-21;
HEADER;
FILE_DESCRIPTION(('ViewDefinition [CoordinationView_V2.0]'),'2;1');
FILE_NAME('office.ifc','2026-10-15T10:00:00',(''),(''),'hand','hand','');
FILE_SCHEMA(('IFC2X3'));
ENDSEC;
DATA;
/* metres */
#1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);
#2=IFCUNITASSIGNMENT((#1));
#3=IFCCARTESIANPOINT((0.,0.,0.));
#4=IFCAXIS2PLACEMENT3D(#3,$,$);
#5=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-05,#4,$);
#6=IFCPROJECT('0000000000000000000001',$,'Small \X2\00D6\X0\ffice',$,$,$,$,(#5),#2);
#10=IFCLOCALPLACEMENT($,#4);
#11=IFCSITE('0000000000000000000002',$,'Site',$,$,#10,$,$,.ELEMENT.,$,$,$,$,$);
#12=IFCLOCALPLACEMENT(#10,#4);
#13=IFCBUILDING('0000000000000000000003',$,'Building',$,$,#12,$,$,.ELEMENT.,$,$,$);
#20=IFCLOCALPLACEMENT(#12,#4);
#21=IFCBUILDINGSTOREY('0000000000000000000004',$,'Ground',$,$,#20,$,$,.ELEMENT.,0.);
#22=IFCCARTESIANPOINT((0.,0.,3.));
#23=IFCAXIS2PLACEMENT3D(#22,$,$);
#24=IFCLOCALPLACEMENT(#12,#23);
#25=IFCBUILDINGSTOREY('0000000000000000000005',$,'First',$,$,#24,$,$,.ELEMENT.,3.);
#30=IFCCARTESIANPOINT((1.,2.,0.));
#31=IFCDIRECTION((0.,0.,1.));
#32=IFCDIRECTION((0.,1.,0.));
#33=IFCAXIS2PLACEMENT3D(#30,#31,#32);
#34=IFCLOCALPLACEMENT(#24,#33);
#35=IFCCARTESIANPOINT((0.,0.));
#36=IFCCARTESIANPOINT((5.,0.));
#37=IFCPOLYLINE((#35,#36));
#38=IFCSHAPEREPRESENTATION(#5,'Axis','Curve2D',(#37));
#39=IFCCARTESIANPOINT((0.,0.2));
#40=IFCCARTESIANPOINT((5.,0.2));
#41=IFCPOLYLINE((#35,#36,#40,#39,#35));
#42=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#41);
#43=IFCEXTRUDEDAREASOLID(#42,#4,#31,2.7);
#44=IFCSHAPEREPRESENTATION(#5,'Body','SweptSolid',(#43));
#45=IFCPRODUCTDEFINITIONSHAPE($,$,(#38,#44));
#46=IFCWALLSTANDARDCASE('0000000000000000000006',$,'Basic Wall:Exterior:101',$,'Basic Wall:Exterior',#34,#45,'101');
#50=IFCCARTESIANPOINT((5.,4.,0.));
#51=IFCDIRECTION((-1.,0.,0.));
#52=IFCAXIS2PLACEMENT3D(#50,#31,#51);
#53=IFCLOCALPLACEMENT(#24,#52);
#54=IFCCARTESIANPOINT((2.,0.));
#55=IFCAXIS2PLACEMENT2D(#54,$);
#56=IFCRECTANGLEPROFILEDEF(.AREA.,$,#55,4.,0.1);
#57=IFCEXTRUDEDAREASOLID(#56,#4,#31,2.7);
#58=IFCSHAPEREPRESENTATION(#5,'Body','SweptSolid',(#57));
#59=IFCPRODUCTDEFINITIONSHAPE($,$,(#58));
#60=IFCWALL('0000000000000000000007',$,'Partition',$,$,#53,#59,$);
#70=IFCCARTESIANPOINT((3.,0.3,0.9));
#71=IFCDIRECTION((0.,-1.,0.));
#72=IFCDIRECTION((1.,0.,0.));
#73=IFCAXIS2PLACEMENT3D(#70,#71,#72);
#74=IFCLOCALPLACEMENT(#34,#4);
#75=IFCCARTESIANPOINT((0.5,0.6));
#76=IFCAXIS2PLACEMENT2D(#75,$);
#77=IFCRECTANGLEPROFILEDEF(.AREA.,$,#76,1.,1.2);
#78=IFCEXTRUDEDAREASOLID(#77,#73,#31,0.4);
#79=IFCSHAPEREPRESENTATION(#5,'Body','SweptSolid',(#78));
#80=IFCPRODUCTDEFINITIONSHAPE($,$,(#79));
#81=IFCOPENINGELEMENT('0000000000000000000008',$,'Opening',$,$,#74,#80,$);
#82=IFCRELVOIDSELEMENT('0000000000000000000009',$,$,$,#46,#81);
#83=IFCLOCALPLACEMENT(#74,#4);
#84=IFCWINDOW('000000000000000000000A',$,'Fixed:1000 x 1200:201',$,'Fixed:1000 x 1200',#83,$,'201',1.2,1.);
#85=IFCRELFILLSELEMENT('000000000000000000000B',$,$,$,#81,#84);
#90=IFCCARTESIANPOINT((1.,2.));
#91=IFCCARTESIANPOINT((5.,2.));
#92=IFCCARTESIANPOINT((5.,3.9));
#93=IFCCARTESIANPOINT((1.,3.9));
#94=IFCPOLYLINE((#90,#91,#92,#93,#90));
#95=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#94);
#96=IFCEXTRUDEDAREASOLID(#95,#4,#31,2.5);
#97=IFCSHAPEREPRESENTATION(#5,'Body','SweptSolid',(#96));
#98=IFCPRODUCTDEFINITIONSHAPE($,$,(#97));
#99=IFCLOCALPLACEMENT(#24,#4);
#100=IFCSPACE('000000000000000000000C',$,'101',$,$,#99,#98,'Office',.ELEMENT.,.INTERNAL.,$);
#101=IFCRELAGGREGATES('000000000000000000000D',$,$,$,#25,(#100));
#110=IFCCARTESIANPOINT((0.,0.,-0.25));
#111=IFCAXIS2PLACEMENT3D(#110,$,$);
#112=IFCCARTESIANPOINT((0.8,1.9));
#113=IFCCARTESIANPOINT((5.2,1.9));
#114=IFCCARTESIANPOINT((5.2,4.1));
#115=IFCCARTESIANPOINT((2.,4.1));
#116=IFCCARTESIANPOINT((2.,7.1));
#117=IFCCARTESIANPOINT((0.8,7.1));
#118=IFCPOLYLINE((#112,#113,#114,#115,#116,#117,#112));
#119=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#118);
#120=IFCEXTRUDEDAREASOLID(#119,#111,#31,0.25);
#121=IFCSHAPEREPRESENTATION(#5,'Body','SweptSolid',(#120));
#122=IFCPRODUCTDEFINITIONSHAPE($,$,(#121));
#123=IFCLOCALPLACEMENT(#24,#4);
#124=IFCSLAB('000000000000000000000E',$,'Floor:Generic 250mm',$,'Floor:Generic 250mm',#123,#122,$,.FLOOR.);
#130=IFCCARTESIANPOINT((0.,-1.));
#131=IFCCARTESIANPOINT((0.,8.));
#132=IFCPOLYLINE((#130,#131));
#133=IFCGRIDAXIS('1',#132,.T.);
#134=IFCCARTESIANPOINT((-1.,0.));
#135=IFCCARTESIANPOINT((6.,0.));
#136=IFCPOLYLINE((#134,#135));
#137=IFCGRIDAXIS('A',#136,.T.);
#138=IFCLOCALPLACEMENT(#12,#4);
#139=IFCGRID('000000000000000000000F',$,'Grid',$,$,#138,$,(#133),(#137),$);
#140=IFCFURNISHINGELEMENT('000000000000000000000G',$,'Desk',$,$,#99,$,$);
#150=IFCRELCONTAINEDINSPATIALSTRUCTURE('000000000000000000000H',$,$,$,(#46,#60,#84,#124,#140),#25);
#151=IFCRELCONTAINEDINSPATIALSTRUCTURE('000000000000000000000I',$,$,$,(#139),#21);
#152=IFCRELAGGREGATES('000000000000000000000J',$,$,$,#13,(#21,#25));
ENDSEC;
END-ISO-10303-21;
)";

    void TestOffice()
    {
        const std::string path = Temp("office.ifc");
        WriteText(path, kOffice);
        Session s;
        const Json r = Run(s, "open", Json{ { "path", path } });
        std::error_code ec;
        std::filesystem::remove(PathOf(path), ec);
        const Model& m = s.GetModel();
        Check(r["format"] == "IFC2X3" && r["name"] == "Small \xC3\x96" "ffice", "别的软件的 IFC：IFC2x3、米制，项目名称的转义还原\n" + r.dump());
        std::vector<std::pair<std::string, double>> levels;
        for (const auto& [_, l] : m.Levels())
            levels.emplace_back(l->name, l->elevation);
        Check(levels == std::vector<std::pair<std::string, double>>{ { "Ground", 0.0 }, { "First", 3000.0 } }, "别的软件的 IFC：楼层成为标高，换成毫米");
        // 毫米；墙的中心线在它画的那个面往里 100 mm；隔墙从那个面接到墙的中心线，所以两面墙相接
        const std::vector<std::string> walls = {
            "start=[5000,4000] end=[900,4000] thickness=100 base_offset=0 height=2700 level=\"First\" top_level=-",
            "start=[900,2000] end=[900,7000] thickness=200 base_offset=0 height=2700 level=\"First\" top_level=-",
        };
        const auto unquote = [](std::vector<std::string> v)
        {
            for (std::string& s : v)
                s = Replace(s, "\"First\"", "First");
            return v;
        };
        Check(Building(m, "wall") == unquote(walls), "别的软件的 IFC：墙按几何量出，差一点相接的墙相接\n" + Join(Building(m, "wall")));
        Check(Building(m, "window") ==
                  std::vector<std::string>{ "host=(start=[900,2000] end=[900,7000] thickness=200 base_offset=0 height=2700 level=First top_level=-) "
                                            "offset=3500 width=1000 height=1200 sill=900" },
              "别的软件的 IFC：窗在它的洞口切开墙的地方\n" + Join(Building(m, "window")));
        Check(Building(m, "floor") == std::vector<std::string>{ "level=First area_m2=13.28 top=[3000,3000]" },
              "别的软件的 IFC：L 形的板\n" + Join(Building(m, "floor")));
        Check(Building(m, "grid") == std::vector<std::string>{ "name=\"1\" start=[0,-1000] end=[0,8000]", "name=\"A\" start=[-1000,0] end=[6000,0]" },
              "别的软件的 IFC：轴网\n" + Join(Building(m, "grid")));
        const auto rooms = Only(m, "room");
        const Room* room = rooms.empty() ? nullptr : rooms.front()->As<Room>();
        const Level* rl = room ? m.GetLevel(room->level) : nullptr;
        Check(room && room->name == "Office" && room->number == "101" && rl && rl->name == "First" && room->point.x >= 1000 && room->point.x < 5000 &&
                  room->point.y >= 2000 && room->point.y < 3900,
              "别的软件的 IFC：房间在它的轮廓里");
        // 只是元素 ID 的标记（Revit 的写法）不算标记：窗重新编号
        std::vector<std::string> marks;
        for (const Element* e : Only(m, "window"))
            marks.push_back(e->AsOpening()->mark);
        Check(marks == std::vector<std::string>{ "1" } && Contains(r["skipped"].dump(), "IFCFURNISHINGELEMENT"),
              "别的软件的 IFC：只是 ID 的标记重新编号；读不出的东西列出来\n" + r["skipped"].dump());
    }

    void TestMarks()
    {
        Session s;
        House(s);
        Script(s, R"(door {"at": [1000, 0], "mark": "P1"}
door {"at": [3000, 0], "mark": "P2"}
window {"at": [8000, 2000], "mark": "J1"}
window {"at": [8000, 4000], "mark": "J2"}
door {"at": [5000, 0]}
window {"at": [4000, 6000]}
window {"at": [2000, 6000]})");
        const auto marks = [](const Model& m)
        {
            std::vector<std::string> all;
            for (const auto& [_, e] : m.elements.Items())
                if (const Opening* o = e->AsOpening())
                    all.push_back(o->mark);
            std::sort(all.begin(), all.end());
            return all;
        };
        const std::string file = Ifc::Export(s.GetModel());
        Ifc::Imported back;
        Import(file, back);
        Check(marks(back.model) == marks(s.GetModel()) && marks(s.GetModel()) == std::vector<std::string>{ "J1", "J2", "J3", "J4", "P1", "P2", "P3" },
              "门窗编号：经过 IFC 保留");
        const std::string untagged = Replace(file, ",'P2',", ",$,", true);
        Ifc::Imported again;
        Import(untagged, again);
        Check(untagged != file && marks(again.model) == std::vector<std::string>{ "J1", "J2", "J3", "J4", "P1", "P3", "P4" },
              "门窗编号：没有标记的接着序列往下编");
    }

    void TestBroken()
    {
        Session s;
        Script(s, SampleScript);
        std::string before, error;
        s.GetModel().ToFile(before, error);
        const std::string path = Temp("broken.ifc");
        bool ok = true;
        for (const char* text : { "this is not STEP at all",
                                  "ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('AUTOMOTIVE_DESIGN'));\nENDSEC;\nDATA;\nENDSEC;\nEND-ISO-10303-21;\n",
                                  "ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n#1=IFCWALL('x',$,(((((;\n",
                                  "ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n#1=IFCPROJECT('x',$,'unterminated);\nENDSEC;\n" })
        {
            WriteText(path, text);
            Json out;
            std::string why, after;
            const bool opened = s.Execute("open", Json{ { "path", path } }, out, why);
            s.GetModel().ToFile(after, error);
            ok &= !opened && !why.empty() && after == before;
        }
        Check(ok, "坏的 IFC 文件：清楚地失败，打开的项目不变");
        // 套一千层的列表、互相指着的放置：报错或读出模型，不会卡死或崩溃
        const std::string deep = "ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n#1=IFCX(" + std::string(1000, '(') + "1" +
                                 std::string(1000, ')') + ");\nENDSEC;\n";
        const std::string cycle = Replace(kOffice, "#24=IFCLOCALPLACEMENT(#12,#23);", "#24=IFCLOCALPLACEMENT(#34,#23);");
        for (const std::string& text : { deep, cycle })
        {
            WriteText(path, text);
            Json out;
            std::string why;
            s.Execute("open", Json{ { "path", path } }, out, why);
        }
        Check(true, "坏的 IFC 文件：很深的嵌套与循环的放置不会卡死或崩溃");
        std::error_code ec;
        std::filesystem::remove(PathOf(path), ec);
    }

    void TestMaterials()
    {
        Session s;
        House(s);
        Run(s, "material", R"({"name": "Oak", "color": "#a0703c", "cut": "diagonal"})");
        Run(s, "type", R"({"category": "floor", "name": "Oak 200", "layers": [{"function": "finish", "material": "Oak", "thickness": 20}, {"function": "structure", "material": "Concrete", "thickness": 180}]})");
        Run(s, "floor", R"({"type": "Oak 200"})");
        const std::string path = Temp("materials.ifc");
        Run(s, "save", Json{ { "path", path } });
        const std::string ifc = ReadText(path);
        Check(Contains(ifc, "IFCMATERIAL('Oak'") && Contains(ifc, "IFCMATERIALLAYERSET(") && Contains(ifc, "'Oak 200'") &&
                  Contains(ifc, "IFCCOLOURRGB($,0.627") && Contains(ifc, "IFCMATERIALDEFINITIONREPRESENTATION("),
              "材质与层：写出层组与材质的颜色");
        Session t;
        Run(t, "open", Json{ { "path", path } });
        std::error_code ec;
        std::filesystem::remove(PathOf(path), ec);
        const Model& m = t.GetModel();
        const FloorType* two = nullptr;
        for (const auto& [_, ty] : m.types.floor)
            if (ty.layers.size() == 2)
                two = &ty;
        Check(two && two->layers[0].material == "Oak" && two->layers[0].thickness == 20.0 && two->layers[1].function == Function::Structure &&
                  m.materials.count("Oak") && m.materials.at("Oak").color == std::array<uint8_t, 3>{ 160, 112, 60 },
              "材质与层：从 IFC 回来时层、材质与颜色不变");
    }

    void TestSliding()
    {
        Session s;
        Script(s, R"(wall {"points": [[0,0],[8000,0],[8000,6000],[0,6000]], "closed": true}
door {"at": [2000, 0], "family": "Sliding Door", "type": "2000 x 2100"}
door {"at": [5500, 0], "family": "Double Door"}
window {"at": [8000, 3000], "family": "Sliding Window"}
window {"at": [4000, 6000], "family": "Casement Window"})");
        std::vector<Id> doors;
        for (const auto& [id, e] : s.GetModel().elements.Items())
            if (e->IsDoor())
                doors.push_back(id);
        std::string file = Ifc::Export(s.GetModel());
        Check(Contains(file, ".SLIDING_TO_LEFT.") && Contains(file, ".DOUBLE_DOOR_SINGLE_SWING.") && Occurrences(file, ".DOUBLE_PANEL_VERTICAL.") == 2,
              "推拉门窗：IFC 里写出怎么开");
        Run(s, "modify", Json{ { "id", doors.front() }, { "flip_hand", true } });
        file = Ifc::Export(s.GetModel());
        Ifc::Imported back;
        Import(file, back);
        std::vector<std::pair<std::optional<Operation>, bool>> ops;
        for (const auto& [_, e] : back.model.elements.Items())
            if (e->IsDoor())
            {
                const auto t = back.model.OpeningTypeOf(*e);
                ops.emplace_back(t ? t->operation : std::nullopt, e->AsOpening()->flipHand);
            }
        const auto has = [&](std::optional<Operation> op, bool right) { return std::find(ops.begin(), ops.end(), std::pair{ op, right }) != ops.end(); };
        Check(Contains(file, ".SLIDING_TO_RIGHT.") && has(Operation::Sliding, true) && has(Operation::DoubleSwing, false),
              "推拉门窗：回来时门保留怎么开、开向哪边");
        // 门自己没说时按它的类型：IfcDoorType（IFC4）的开启方式，同 Revit 的写法
        const size_t at = file.find(".SLIDING_TO_RIGHT.");
        const size_t lineStart = file.rfind('\n', at) + 1;
        const std::string doorRef = file.substr(lineStart, file.find('=', lineStart) - lineStart);
        std::string typed = Replace(file, ".SLIDING_TO_RIGHT.,$)", "$,$)", true);
        typed = Replace(typed, "ENDSEC;\nEND-ISO",
                        "#999990=IFCDOORTYPE('0000000000000000000T01',$,'Patio',$,$,$,$,$,$,.DOOR.,.SLIDING_TO_LEFT.,.F.,$);\n"
                        "#999991=IFCRELDEFINESBYTYPE('0000000000000000000T02',$,$,$,(" + doorRef + "),#999990);\nENDSEC;\nEND-ISO",
                        true);
        Ifc::Imported patio;
        Import(typed, patio);
        const auto it = patio.model.types.door.find("Patio");
        Check(!Contains(typed, ".SLIDING_TO_RIGHT.") && it != patio.model.types.door.end() && it->second.operation == Operation::Sliding,
              "推拉门窗：门没说时按它的 IfcDoorType");
    }

    void TestRoomSeparators()
    {
        Session s;
        House(s);
        Run(s, "room", R"({"point": [2000, 3000], "name": "Living"})");
        Run(s, "room_separator", R"({"points": [[5000, 0], [5000, 6000]]})");
        Run(s, "room", R"({"point": [7000, 3000], "name": "Kitchen"})");
        const std::string file = Ifc::Export(s.GetModel());
        Ifc::Imported back;
        Import(file, back);
        std::vector<std::pair<P2, P2>> lines;
        for (const auto& [_, e] : back.model.elements.Items())
            if (const RoomSeparator* rs = e->As<RoomSeparator>())
                lines.emplace_back(rs->start, rs->end);
        Check(Contains(file, "IFCVIRTUALELEMENT(") && Contains(file, "'RoomSeparation'") && back.skipped.empty() &&
                  lines == std::vector<std::pair<P2, P2>>{ { P2(5000, 0), P2(5000, 6000) } },
              "房间分隔线：写成 IfcVirtualElement，读回同一条线\n" + Skipped(back));
        std::vector<double> areas;
        for (const auto& [id, e] : back.model.elements.Items())
            if (e->As<Room>())
                areas.push_back(ElementJson(back.model, id)["area_m2"].get<double>());
        std::sort(areas.begin(), areas.end());
        Check(areas == std::vector<double>{ 18.0, 30.0 }, "房间分隔线：回来后照样分开两个房间");
    }
}

int main()
{
    // 不缓冲：崩溃时也看得到崩在哪一项之后
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestStep();
    TestGlobalIds();
    TestExportStrings();
    TestExportSample();
    TestExportComponents();
    TestExportFamilyOpenings();
    TestExportLayers();
    TestSaveIfc();
    TestImportMeshes();
    TestImportHoles();
    TestImportLayers();
    TestImportComponents();
    TestImportProxies();
    TestImportSame();
    TestRoundTrip();
    TestOffice();
    TestMarks();
    TestBroken();
    TestMaterials();
    TestSliding();
    TestRoomSeparators();
    std::printf("\n%s：%d 项失败\n", g_failed == 0 ? "全部通过" : "有失败", g_failed);
    return g_failed == 0 ? 0 : 1;
}
