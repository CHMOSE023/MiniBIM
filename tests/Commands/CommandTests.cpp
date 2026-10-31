// 命令与会话（src/Commands）：像前端一样经命令驱动模型——撤销、脚本、事务、建模、编辑、项目文件，以及示例房屋。
// 移植自 BIMCraft crates/core/src/tests.rs、sample.rs 的测试（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md。
// 用到还没移植的命令（剖面、平面图、IFC、阶段……）与 view3d::tops_at 的断言先略去或改用模型的函数，移植那些时补回
#include "Commands/Autosave.h"
#include "Commands/Commands.h"
#include "Commands/Sample.h"
#include "Commands/Session.h"
#include "Model/Align.h"
#include "Model/ModelJson.h"
#include "Model/Render.h"
#include "Model/Walls.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
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

    constexpr double kPi = 3.14159265358979323846;

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

    // 执行命令，应当失败：返回原因（成功了返回空串）
    std::string Refused(Session& s, const char* id, const Json& p)
    {
        Json out;
        std::string error;
        return s.Execute(id, p, out, error) ? std::string() : error;
    }

    std::string Refused(Session& s, const char* id, const char* p) { return Refused(s, id, Json::parse(p)); }

    // 元素 JSON 里的引用（GUID）指的元素的 ID
    std::optional<Id> IdOf(const Session& s, const Json& v)
    {
        Guid g;
        std::string error;
        if (!FromJson(v, g, error))
            return std::nullopt;
        return s.GetModel().elements.IdOf(g);
    }

    uint64_t Count(Session& s, const char* category)
    {
        const Json r = Run(s, "elements", Json{ { "category", category } });
        return r.is_null() ? 0 : r["count"].get<uint64_t>();
    }

    // Level 1 上 8 × 6 米的一圈墙；各墙的 ID，底边的在前
    std::vector<Id> House(Session& s)
    {
        const Json r = Run(s, "wall", R"({"points": [[0, 0], [8000, 0], [8000, 6000], [0, 6000]], "closed": true})");
        std::vector<Id> ids;
        for (const Json& v : r["ids"])
            ids.push_back(v.get<Id>());
        return ids;
    }

    Json ElementOf(Session& s, Id id)
    {
        const Json all = Run(s, "elements", Json::object());
        for (const Json& e : all["elements"])
            if (e["id"] == id)
                return e;
        return nullptr;
    }

    std::pair<Json, Json> Ends(Session& s, Id id)
    {
        const Json w = ElementOf(s, id);
        return { w["start"], w["end"] };
    }

    Json Pt(double x, double y) { return Json::array({ x, y }); }

    std::string Temp(const std::string& name)
    {
        const std::filesystem::path p = std::filesystem::temp_directory_path() / ("minibim-test-" + std::to_string(MINIBIM_GETPID()) + "-" + name);
        const std::u8string u8 = p.u8string();
        return std::string(u8.begin(), u8.end());
    }

    // 300 毫米的空腔墙类型：砖、矿棉、砌块核心层、抹灰
    void Cavity(Session& s)
    {
        Run(s, "material", R"({"name": "Mineral wool", "color": "#e6d68c", "cut": "insulation"})");
        Run(s, "type", R"({"category": "wall", "name": "Cavity 300", "layers": [
            {"function": "finish", "material": "Brick", "thickness": 100},
            {"function": "thermal", "material": "Mineral wool", "thickness": 50},
            {"function": "structure", "material": "Concrete block", "thickness": 140},
            {"function": "finish", "material": "Plaster", "thickness": 10}]})");
    }

    // ring 上或里面（边上 1 微米以内也算）
    bool Over(const Ring& ring, P2 q)
    {
        if (Geom::Contains(ring, q))
            return true;
        for (const auto& [a, b] : Geom::RingEdges(ring))
            if (Geom::DistToSegment(q, a, b) < 1e-6)
                return true;
        return false;
    }

    // 屋顶在 q 处顶面的高度；不在它上方时为空（BIMCraft 的测试用 view3d::tops_at，那部分还没移植，这里由屋顶的板块算）
    std::optional<double> RoofTop(const Session& s, Id id, P2 q)
    {
        const Model& m = s.GetModel();
        const Element* e = m.elements.Get(id);
        const Roof* r = e ? e->As<Roof>() : nullptr;
        std::vector<std::pair<Ring, Plane>> pieces;
        std::string error;
        if (!r || !m.RoofPieces(*r, pieces, error))
            return std::nullopt;
        std::optional<double> top;
        for (const auto& [ring, p] : pieces)
            if (Over(ring, q))
                top = std::max(top.value_or(-INFINITY), p.At(q) + m.RoofRise(*r, p));
        return top;
    }

    // 天花板在 q 处顶面的高度；不在它上方时为空
    std::optional<double> CeilingTop(const Session& s, Id id, P2 q)
    {
        const Model& m = s.GetModel();
        const Element* e = m.elements.Get(id);
        const Ceiling* c = e ? e->As<Ceiling>() : nullptr;
        if (!c)
            return std::nullopt;
        std::optional<double> top;
        for (const auto& [face, t] : m.CeilingSlabs(*c))
        {
            Ring ring;
            for (const P3& p : face)
                ring.emplace_back(p[0], p[1]);
            if (Over(ring, q))
                top = std::max(top.value_or(-INFINITY), face.front()[2] + t);
        }
        return top;
    }

    // 墙在沿定位线 along 处的顶
    double WallTop(const Session& s, Id id, double along)
    {
        const Model& m = s.GetModel();
        return Walls::TopAt(m, *m.GetWall(id), along);
    }

    bool NearOpt(const std::optional<double>& a, double b, double tol) { return a && std::abs(*a - b) < tol; }
    bool NearNum(const Json& v, double b, double tol) { return v.is_number() && std::abs(v.get<double>() - b) < tol; }

    void TestWallsAndOpenings()
    {
        {
            Session s;
            const bool four = House(s).size() == 4;
            const Json w = Run(s, "elements", R"({"category": "walls"})");
            const Json& first = w["elements"][0];
            Check(four && first["length"] == 8000.0 && first["height"] == 3000.0 && IdOf(s, first["top_level"]) == Id(100001),
                  "命令：闭合的墙链每边一面墙，高到上一个标高");
        }
        {
            Session s;
            const auto ids = House(s);
            const Json d = Run(s, "door", R"({"at": [2000, 30]})");
            const Json w = Run(s, "WN", R"({"at": [8000, 3000]})");
            const std::string e = Refused(s, "door", R"({"at": [4000, 3000]})");
            Check(IdOf(s, d["host"]) == ids[0] && d["offset"] == 2000.0 && d["mark"] == "1" && IdOf(s, w["host"]) == ids[1] && Contains(e, "no wall"),
                  "命令：门窗找到点下面的墙");
        }
        {
            Session s;
            Cavity(s);
            const Json t = Run(s, "inspect", Json::object())["types"]["wall"]["Cavity 300"];
            Json first = t["layers"][0];
            const bool parts = first["id"].is_number_unsigned() && first["id"].get<Id>() >= FirstId && first["guid"].is_string() &&
                               Guid::Parse(first["guid"].get<std::string>()).has_value();
            first.erase("id");
            first.erase("guid");
            const bool layer = first == Json::parse(R"({"function": "finish", "material": "Brick", "thickness": 100.0})");
            const Json thicker = Run(s, "type", R"({"category": "wall", "name": "Cavity 300", "thickness": 320})")["type"];
            bool refused = true;
            const std::pair<const char*, const char*> bad[] = {
                { R"({"category": "wall", "name": "Cavity 300", "thickness": 100})", "structure" },
                { R"({"category": "wall", "name": "Thin", "layers": [{"thickness": -5}]})", "thickness" },
                { R"({"category": "wall", "name": "Odd", "layers": [{"function": "rubber", "thickness": 5}]})", "rubber" },
                { R"({"category": "wall", "name": "None", "layers": []})", "thickness" },
                { R"({"category": "wall", "name": "Both", "thickness": 200, "layers": [{"thickness": 100}]})", "200" },
            };
            for (const auto& [p, says] : bad)
                refused = refused && Contains(Refused(s, "type", p), says);
            Check(t["thickness"] == 300.0 && parts && layer && thicker["thickness"] == 320.0 && thicker["layers"][2]["thickness"] == 160.0 && refused,
                  "命令：复合墙类型和它的各层一样厚，新厚度加到结构层");

        }
        {
            // 从西往东画的墙外侧在北（左边）；换到核心层内侧面时墙不动，定位线横移；翻转后外侧到另一边，门留在原处
            Session s;
            Cavity(s);
            const Id w = Run(s, "wall", R"({"points": [[0, 0], [5000, 0]], "type": "Cavity 300", "location": "exterior-finish"})")["ids"][0].get<Id>();
            const auto [ext, inner] = Walls::Faces(s.GetModel(), *s.GetModel().GetWall(w));
            const bool faces = ext == 0.0 && inner == -300.0;
            Run(s, "modify", Json{ { "id", w }, { "location", "interior-core" } });
            const Json moved = ElementOf(s, w);
            const Id door = Run(s, "door", Json{ { "wall", w }, { "offset", 1000 } })["id"].get<Id>();
            const Json at = ElementOf(s, door)["at"];
            Run(s, "flip", Json{ { "ids", { w } } });
            const Json flipped = ElementOf(s, w);
            const Json d = ElementOf(s, door);
            Check(faces && moved["start"] == Pt(0, -290) && moved["end"] == Pt(5000, -290) && flipped["start"] == Pt(5000, -290) &&
                      flipped["end"] == Pt(0, -290) && d["at"] == at && d["flip_facing"] == true && d["flip_hand"] == true &&
                      Contains(Refused(s, "wall", R"({"points": [[0, 0], [1, 1000]], "location": "middle"})"), "middle"),
                  "命令：墙立在定位线上，外侧在左；换定位线不动墙，翻转后门留在原处");
        }
    }

    void TestHistory()
    {
        {
            Session s;
            House(s);
            Run(s, "door", R"({"at": [2000, 0]})");
            const Json undone = Run(s, "undo", Json::object());
            const bool back = Count(s, "door") == 0 && Count(s, "wall") == 4;
            const Json redone = Run(s, "redo", Json::object());
            Check(undone["undone"] == "Door" && back && redone["redone"] == "Door" && Count(s, "door") == 1 && !Refused(s, "redo", "{}").empty(),
                  "命令：撤销、重做一步步走");
        }
        {
            Session s;
            const auto ids = House(s);
            const Json r = Run(s, "modify", Json{ { "ids", { ids[0], ids[2] } }, { "type", "Exterior - 300mm" } });
            auto thick = [&] { return std::vector<double>{ ElementOf(s, ids[0])["thickness"], ElementOf(s, ids[1])["thickness"], ElementOf(s, ids[2])["thickness"] }; };
            const bool changed = thick() == std::vector<double>{ 300, 200, 300 } && s.History().back() == "Modify";
            Run(s, "undo", Json::object());
            Check(r["modified"] == Json{ ids[0], ids[2] } && changed && thick() == std::vector<double>{ 200, 200, 200 }, "命令：一次修改多个元素是一个撤销步骤");
        }
        {
            Session s;
            std::vector<Json> out;
            std::string error;
            const bool ok = s.Script("# a box\nwall {\"points\": [[0,0],[4000,0],[4000,3000],[0,3000]], \"closed\": true}\n\nfloor {}\n", out, error) && out.size() == 2;
            const bool failed = !s.Script("level {\"elevation\": 9000}\nnope\n", out, error) && Contains(error, "line 2");
            Check(ok && failed && Count(s, "level") == 3, "命令：脚本逐行执行，说出失败的那一行，之前的保留");
        }
        {
            Session s;
            Run(s, "level", R"({"elevation": 3000})");
            const auto history = s.History();
            const uint64_t levels = Count(s, "level");
            std::vector<Json> out;
            std::string error;
            const bool failed = !s.ScriptAtomic("level {\"elevation\": 6000}\ngrid {\"start\": [0, 0], \"end\": [0, 5000]}\nnope\n", out, error) && Contains(error, "line 3");
            const bool untouched = Count(s, "level") == levels && s.History() == history;
            out.clear();
            const bool ok = s.ScriptAtomic("# two levels\nlevel {\"elevation\": 6000}\nlevel {\"elevation\": 9000}\n", out, error) && out.size() == 2 &&
                            Count(s, "level") == levels + 2 && s.History().size() == history.size() + 1 && s.History().back() == "Script";
            Run(s, "undo", Json::object());
            const bool undone = Count(s, "level") == levels;
            Run(s, "redo", Json::object());
            const bool redone = Count(s, "level") == levels + 2;
            const size_t steps = s.History().size();
            s.ScriptAtomic("inspect\n", out, error);
            Check(failed && untouched && ok && undone && redone && s.History().size() == steps, "命令：全做或全不做的脚本是一个撤销步骤，失败时什么都不留");
        }
        {
            Session s;
            Run(s, "level", R"({"elevation": 3000})");
            Run(s, "undo", Json::object());
            const auto model = s.Snapshot();
            const auto history = s.History();
            s.SetPath("kept.minibim");
            std::string error;
            const bool failed = !s.Transaction("Grouped", [](Session& t, std::string& why)
            {
                Json out;
                // 里面的撤销退不到事务之前；新建的项目也随其余的一起撤回
                if (t.Execute("undo", Json::object(), out, why))
                    return false;
                return t.Execute("redo", Json::object(), out, why) && t.Execute("new", Json::object(), out, why) &&
                       t.Execute("level", Json{ { "elevation", NAN } }, out, why);
            }, error);
            const bool restored = failed && !error.empty() && s.Snapshot() == model && s.History() == history && s.Path() == "kept.minibim";
            Run(s, "redo", Json::object());
            const bool redo = Count(s, "level") == 3;
            std::vector<Json> out;
            s.ScriptAtomic("new\nlevel {\"elevation\": 3000}\n", out, error);
            Check(restored && redo && s.History() == std::vector<std::string>{ "Level" } && Count(s, "level") == 3,
                  "命令：失败的事务把模型、历史和路径都放回去；换了项目的事务丢下旧项目的历史");
        }
    }

    void TestFloors()
    {
        {
            Session s;
            House(s);
            Check(Run(s, "floor", Json::object())["area_m2"] == 48.0 && Contains(Refused(s, "floor", R"({"level": "Level 2"})"), "boundary"),
                  "命令：楼板默认沿墙的轮廓");
        }
        {
            Session s;
            House(s);
            // 沿南墙的一段坡道，6 米长，从箭头尾部高 100 毫米处向东升高 5°
            const Id ramp = Run(s, "floor", R"({"boundary": [[1000, 500], [7000, 500], [7000, 2000], [1000, 2000]], "offset": 100, "slope": 5, "arrow": [[1000, 1250], [7000, 1250]]})")["id"].get<Id>();
            const double tan5 = std::tan(5.0 * kPi / 180.0);
            auto top = [&](double x) { return s.GetModel().FloorTop(*s.GetModel().elements.Get(ramp)->As<Floor>()).At(P2(x, 1000)); };
            const Json e = ElementOf(s, ramp);
            const bool rises = std::abs(top(1500) - (100 + 500 * tan5)) < 1e-6 && std::abs(top(4000) - (100 + 3000 * tan5)) < 1e-6 && e["slope"] == 5.0 &&
                               e["offset"] == 100.0 && e["top"] == Json::array({ 100.0, 624.93 });
            Run(s, "wall", Json{ { "points", { Pt(4000, 500), Pt(4000, 2000) } }, { "type", "Interior - 100mm" }, { "attach_top", ramp } });
            const bool arrow = Contains(Refused(s, "modify", Json{ { "id", ramp }, { "arrow", nullptr } }), "arrow");
            const bool steep = Contains(Refused(s, "modify", Json{ { "id", ramp }, { "slope", 80 } }), "75°");
            // 平放在 offset：贴着它的墙要它比 200 毫米高
            const bool low = Contains(Refused(s, "modify", Json{ { "id", ramp }, { "slope", 0 } }), "above its base");
            Run(s, "modify", Json{ { "id", ramp }, { "slope", 0 }, { "offset", 500 } });
            Check(rises && arrow && steep && low && top(1500) == 500.0 && top(6500) == 500.0, "命令：坡楼板沿坡度箭头升高");
        }
        {
            Session s;
            // 两层、16 米见方、中间 4 米天井的房子：每层天井一圈的墙都不与外墙相连
            for (const char* level : { "Level 1", "Level 2" })
            {
                Run(s, "wall", Json{ { "level", level }, { "points", { Pt(0, 0), Pt(16000, 0), Pt(16000, 16000), Pt(0, 16000) } }, { "closed", true } });
                Run(s, "wall", Json{ { "level", level }, { "points", { Pt(6000, 6000), Pt(10000, 6000), Pt(10000, 10000), Pt(6000, 10000) } }, { "closed", true } });
            }
            const Json made = Run(s, "floor", R"({"level": "Level 2"})");
            const Id floor = made["id"].get<Id>();
            const Json e = ElementOf(s, floor);
            const Json court = Json::array({ Json::array({ Pt(6000, 6000), Pt(6000, 10000), Pt(10000, 10000), Pt(10000, 6000) }) });
            const bool open = made["area_m2"] == 240.0 && e["area_m2"] == 240.0 && e["openings"] == court;
            const Id given = Run(s, "floor", R"({"level": "Level 2", "openings": []})")["id"].get<Id>();
            const bool own = ElementOf(s, given).count("openings") == 0;
            // 里面有房间的环是室内：铺满
            const Id room = Run(s, "room", R"({"level": "Level 2", "point": [8000, 8000], "name": "Hall"})")["id"].get<Id>();
            const Json hall = Run(s, "floor", R"({"level": "Level 2"})");
            const bool indoors = ElementOf(s, hall["id"].get<Id>()).count("openings") == 0 && hall["area_m2"] == 256.0;
            Run(s, "delete", Json{ { "ids", { room } } });
            Run(s, "wall", R"({"level": "Level 2", "points": [[8000, 0], [8000, 6000]]})");
            const Json joined = Run(s, "floor", R"({"level": "Level 2"})");
            Check(open && own && indoors && ElementOf(s, joined["id"].get<Id>()).count("openings") == 0 && joined["area_m2"] == 256.0,
                  "命令：楼板沿墙铺，独立的一圈墙围出天井；里面有房间的、与外墙相连的不算");
        }
    }

    void TestEditing()
    {
        {
            Session s;
            const auto ids = House(s);
            Run(s, "modify", Json{ { "id", ids[0] }, { "type", "Exterior - 300mm" } });
            const bool thick = Run(s, "elements", R"({"category": "wall"})")["elements"][0]["thickness"] == 300.0;
            const bool typo = Contains(Refused(s, "modify", Json{ { "id", ids[0] }, { "hieght", 2000 } }), "hieght");
            const bool category = Contains(Refused(s, "modify", Json{ { "id", ids[0] }, { "category", "floor" } }), "category");
            Run(s, "modify", Json{ { "id", ids[0] }, { "top_level", nullptr }, { "height", 2500 }, { "level", "Level 1" } });
            Check(thick && typo && category && Run(s, "elements", R"({"category": "wall"})")["elements"][0]["height"] == 2500.0, "命令：修改字段，拒绝拼错的");
        }
        {
            Session s;
            House(s);
            const bool shown = ElementOf(s, 100000)["view_range"] == Json::parse(R"({"top": null, "cut": 1200.0, "bottom": 0.0, "depth": 0.0})");
            Run(s, "modify", R"({"id": 100000, "view_range": {"cut": 1500, "top": 2800}})");
            Run(s, "modify", R"({"id": 100000, "view_range": {"depth": -500}})");
            Check(shown && ElementOf(s, 100000)["view_range"] == Json::parse(R"({"top": 2800.0, "cut": 1500.0, "bottom": 0.0, "depth": -500.0})"),
                  "命令：平面的视图范围由修改合并进去");
        }
        {
            Session s;
            const auto ids = House(s);
            const Id part = Run(s, "wall", R"({"points": [[0, 3000], [8000, 3000]], "type": "Interior - 100mm"})")["ids"][0].get<Id>();
            const Id room = Run(s, "room", R"({"point": [4000, 1500]})")["id"].get<Id>();
            const Id door = Run(s, "door", R"({"at": [2000, 0]})")["id"].get<Id>();
            const Id window = Run(s, "window", R"({"at": [8000, 1500]})")["id"].get<Id>();
            const bool before = ElementOf(s, room)["area_m2"] == 24.0;
            Run(s, "MV", Json{ { "ids", { ids[1] } }, { "by", { 1000, 0 } } });
            const bool ok = Ends(s, ids[1]) == std::pair{ Pt(9000, 0), Pt(9000, 6000) } && Ends(s, ids[0]).second == Pt(9000, 0) &&
                            Ends(s, ids[2]).first == Pt(9000, 6000) && Ends(s, part).second == Pt(9000, 3000) && ElementOf(s, room)["area_m2"] == 27.0 &&
                            ElementOf(s, door)["at"] == Pt(2000, 0) && ElementOf(s, window)["at"] == Pt(9000, 1500);
            // 上一层正上方的墙不与这些相接
            const Id up = Run(s, "wall", R"({"points": [[9000, 6000], [9000, 9000]], "level": "Level 2", "height": 3000})")["ids"][0].get<Id>();
            Run(s, "move", Json{ { "ids", { ids[1] } }, { "by", { -1000, 0 } } });
            Check(before && ok && Ends(s, up).first == Pt(9000, 6000), "命令：移动一面墙，相接的墙跟着伸缩，房间跟着变，墙上的门窗留在原处或随墙走");
        }
        {
            Session s;
            const auto ids = House(s);
            const Id door = Run(s, "door", R"({"at": [6000, 0]})")["id"].get<Id>();
            Run(s, "move", Json{ { "ids", { ids[0] } }, { "by", { -1000, 0 } }, { "end", "start" } });
            Check(Ends(s, ids[0]) == std::pair{ Pt(-1000, 0), Pt(8000, 0) } && Ends(s, ids[3]).second == Pt(-1000, 0) && ElementOf(s, door)["at"] == Pt(6000, 0) &&
                      Contains(Refused(s, "move", Json{ { "ids", { ids[0], ids[1] } }, { "by", { 10, 0 } }, { "end", "end" } }), "one wall"),
                  "命令：只移动墙的一端，墙与那端相接的墙伸缩");
        }
        {
            Session s;
            const auto ids = House(s);
            const Id door = Run(s, "door", R"({"at": [2000, 0]})")["id"].get<Id>();
            Run(s, "move", Json{ { "ids", { door } }, { "by", { 1500, 700 } } });
            const bool slid = ElementOf(s, door)["at"] == Pt(3500, 0);
            const Json made = Run(s, "CO", Json{ { "ids", { ids[0] } }, { "by", { 0, -2000 } } })["ids"];
            const Id wall = made[0].get<Id>(), copy = made[1].get<Id>();
            const Json c = ElementOf(s, copy);
            Check(slid && Ends(s, wall) == std::pair{ Pt(0, -2000), Pt(8000, -2000) } && IdOf(s, c["host"]) == wall && c["mark"] == "2" && c["at"] == Pt(3500, -2000),
                  "命令：门只沿墙移动，复制的墙带上它的门");
        }
        {
            Session s;
            const auto ids = House(s);
            const bool level = Contains(Refused(s, "move", R"({"ids": [100000], "by": [10, 0]})"), "elevation") &&
                               Contains(Refused(s, "copy", R"({"ids": [100000], "by": [10, 0]})"), "level");
            const Id door = Run(s, "door", R"({"at": [2000, 0]})")["id"].get<Id>();
            Check(level && Contains(Refused(s, "copy", Json{ { "ids", { door } }, { "by", { 300, 0 } } }), "overlap") &&
                      !Refused(s, "move", Json{ { "ids", { ids[0] } }, { "by", { 1e300, 0 } } }).empty() &&
                      Contains(Refused(s, "move", Json{ { "ids", { ids[0] } } }), "by"),
                  "命令：移动、复制拒绝标高和会弄坏模型的");
        }
        {
            Session s;
            const auto ids = House(s);
            Run(s, "door", R"({"at": [2000, 0]})");
            const Json d = Run(s, "DE", Json{ { "ids", { ids[0] } } });
            const std::string e = Refused(s, "delete", R"({"ids": [100000]})");
            Check(d["deleted"].size() == 2 && Count(s, "door") == 0 && Contains(e, "level") && Contains(e, "doesn't exist"),
                  "命令：删墙带走它的门窗；还有墙用着的标高删不掉");
        }
        {
            Session s;
            const auto ids = House(s);
            Run(s, "type", R"({"category": "wall", "name": "Brick - 250mm", "thickness": 250})");
            Run(s, "modify", Json{ { "id", ids[0] }, { "type", "Brick - 250mm" } });
            Run(s, "type", R"({"category": "wall", "name": "Brick - 250mm", "thickness": 280})");
            Check(Run(s, "elements", R"({"category": "wall"})")["elements"][0]["thickness"] == 280.0 &&
                      Contains(Refused(s, "type", R"({"category": "door", "name": "Wide"})"), "width"),
                  "命令：类型新建或修改，所有用它的地方都变");
        }
        {
            Session s;
            Check(Run(s, "level", R"({"elevation": 6000})")["name"] == "Level 3" && Run(s, "grid", R"({"start": [0, -1000], "end": [0, 7000]})")["name"] == "1" &&
                      Run(s, "GR", R"({"start": [8000, -1000], "end": [8000, 7000]})")["name"] == "2" &&
                      Run(s, "grid", R"({"start": [-1000, 0], "end": [9000, 0], "name": "A"})")["name"] == "A" &&
                      Run(s, "grid", R"({"start": [-1000, 6000], "end": [9000, 6000]})")["name"] == "B",
                  "命令：标高、轴网自己起名");
        }
        {
            Session s;
            House(s);
            Run(s, "delete", R"({"ids": [100002]})");
            const Json i = Run(s, "inspect", Json::object());
            Check(i["counts"]["wall"] == 3 && i["levels"][0]["name"] == "Level 1" && i["history"] == Json{ "Wall", "Delete" }, "命令：inspect 汇总标高、数目与历史");
        }
    }

    void TestFiles()
    {
        {
            Session s;
            House(s);
            Run(s, "door", R"({"at": [2000, 0], "flip_hand": true})");
            const std::string path = Temp("house.minibim");
            const bool saved = !Run(s, "save", Json{ { "path", path } }).is_null();
            const Json before = s.GetModel().ToJson();
            Session t;
            Run(t, "open", Json{ { "path", path } });
            const bool same = t.GetModel().ToJson() == before && t.Path() == path;
            {
                std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary | std::ios::trunc);
                out << "{\"name\": 3}";
            }
            const bool refused = Contains(Refused(t, "open", Json{ { "path", path } }), "not a project file") && t.GetModel().ToJson() == before;
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
            Check(saved && same && refused, "命令：项目保存后原样打开；打不开时保留已打开的项目");
        }
        {
            // BIMCraft 的项目文件：还没移植的东西（阶段、柱类型、楼梯、墙的项目参数）略去并列出，其余照常打开，
            // 贴着屋顶的墙仍贴着，带族的门照常
            Session s;
            House(s);
            Run(s, "door", R"({"at": [2000, 0]})");
            std::string text, error;
            s.GetModel().ToFile(text, error);
            Json file = Json::parse(text);
            file["phases"] = Json::array({ "Existing", "New Construction" });
            file["types"]["column"] = Json::parse(R"({"Rectangular 400 x 400": {"width": 400, "depth": 400}})");
            file["materials"]["Brick"]["texture"] = Json::parse(R"({"kind": "brick"})");
            Json roof = Json::parse(R"({"guid": "3f2a9c1e-1111-4000-8000-000000000001", "category": "roof", "type": "Generic - 300mm",
                "boundary": [[-500, -500], [8500, -500], [8500, 6500], [-500, 6500]], "slope": 30, "overhang": 0, "offset": 0})");
            roof["level"] = file["elements"]["100001"]["guid"];
            file["elements"]["100100"] = roof;
            file["elements"]["100101"] = Json::parse(R"({"category": "door", "type": "900 x 2100", "family": "Single Door", "offset": 6000, "sill": 0, "mark": "9", "flip_facing": false, "flip_hand": false})");
            file["elements"]["100101"]["host"] = file["elements"]["100002"]["guid"];
            file["elements"]["100102"] = Json::parse(R"({"category": "stair", "level": 100000, "start": [1000, 1000], "angle": 0})");
            file["elements"]["100003"]["attach_top"] = "3f2a9c1e-1111-4000-8000-000000000001";
            file["elements"]["100003"]["params"] = Json::parse(R"({"Fire Rating": "EI 60"})");
            file["library"]["900000"] = Json::parse(R"({"kind": "phase", "name": "Existing", "guid": "3f2a9c1e-2222-4000-8000-000000000002"})");
            const std::string path = Temp("from-bimcraft.bimcraft");
            {
                std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary | std::ios::trunc);
                out << file.dump(2);
            }
            Session t;
            const Json r = Run(t, "open", Json{ { "path", path } });
            std::vector<std::string> left;
            if (!r.is_null())
                for (const Json& v : r["left_out"])
                    left.push_back(v.get<std::string>());
            auto has = [&](const char* p) { return std::find(left.begin(), left.end(), p) != left.end(); };
            const bool listed = !has("/families") && has("/phases") && has("/types/column") && has("/materials/Brick/texture") && !has("/elements/100100") &&
                                !has("/elements/100101") && has("/elements/100102") && !has("/elements/100003/attach_top") && has("/elements/100003/params");
            std::string why;
            const bool opened = !r.is_null() && t.GetModel().Check(why) && Count(t, "wall") == 4 && Count(t, "door") == 2 && Count(t, "roof") == 1 &&
                                t.GetModel().GetWall(Id(100003))->attachTop && !t.GetModel().library.IdOf(Kind::Of(Kind::Tag::Phase), "Existing");
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
            Check(listed && opened, "命令：打开 BIMCraft 的文件时略去还没移植的内容并列出，其余照常");
        }
        {
            Session s;
            Json file = s.GetModel().ToJson();
            file["format"] = FileFormat + 1;
            const std::string path = Temp("newer.minibim");
            {
                std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary | std::ios::trunc);
                out << file.dump();
            }
            const std::string e = Refused(s, "open", Json{ { "path", path } });
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
            Check(Contains(e, "newer"), "命令：更新版本的文件拒绝打开，说明原因");
        }
    }

    // ── 屋顶、房间、天花板（第 2b 步）────────────────────────────
    void TestRoofsRoomsCeilings()
    {
        const double tan30 = std::tan(30.0 * kPi / 180.0), rise30 = 300.0 / std::cos(30.0 * kPi / 180.0);
        {
            Session s;
            House(s);
            const bool enclosed = Contains(Refused(s, "room", R"({"point": [9000, 1000]})"), "enclosed");
            const Json r = Run(s, "RM", R"({"point": [4000, 3000], "name": "Living"})");
            const bool first = r["number"] == "1" && r["area_m2"] == 48.0;
            // 隔墙把房子分开；房间留下它的点所在的那部分
            Run(s, "wall", R"({"points": [[5000, 0], [5000, 6000]], "type": "Interior - 100mm"})");
            Check(enclosed && first && Run(s, "elements", R"({"category": "room"})")["elements"][0]["area_m2"] == 30.0,
                  "房间：要有墙围着，跟着墙变");
        }
        {
            Session s;
            House(s);
            Run(s, "wall", R"({"points": [[4000, 0], [4000, 6000]]})");
            // 在 Level 2 上盖住 Level 1 的墙：30° 四坡，伸出墙面 500；四个角从西南起，隔墙与外墙相接处没有角点
            const Id roof = Run(s, "roof", Json::object())["id"].get<Id>();
            const Json e = ElementOf(s, roof);
            bool ok = IdOf(s, e["level"]) == Id(100001) && e["slope"] == 30.0 && e["overhang"] == 500.0 && NearNum(e["boundary"][0][0], -600, 1e-6) &&
                      NearNum(e["boundary"][0][1], -600, 1e-6) && e["boundary"].size() == 4;
            // 屋脊：离墙面 3100、30°，在 Level 2 之上
            ok = ok && NearNum(e["ridge"], 3000.0 + 3100.0 * tan30, 0.01);
            const Id flat = Run(s, "roof", R"({"slope": 0, "boundary": [[0, 0], [4000, 0], [2000, 3000]]})")["id"].get<Id>();
            ok = ok && ElementOf(s, flat)["ridge"] == 3000.0;
            // 三角形也能坡：三个坡面
            ok = ok && !Run(s, "roof", R"({"boundary": [[0, 0], [4000, 0], [2000, 3000]]})").is_null();
            Check(ok, "屋顶：盖住下一层的墙，平的或坡的");
        }
        {
            Session s;
            const auto ids = House(s);
            // 8 × 6 米上的双坡：坡面朝南北，屋脊东西走向
            const Id roof = Run(s, "roof", R"({"gable": true})")["id"].get<Id>();
            // 高过墙顶的窗放不进东墙，直到墙升进山墙
            bool ok = Contains(Refused(s, "window", R"({"at": [8000, 3000], "sill": 2000})"), "fit");
            Run(s, "modify", Json{ { "ids", ids }, { "attach_top", roof } });
            ok = ok && std::abs(WallTop(s, ids[1], 3000) - (3000.0 + 3100.0 * tan30)) < 1.0 && std::abs(WallTop(s, ids[0], 4000) - (3000.0 + 100.0 * tan30)) < 1.0;
            ok = ok && !Run(s, "window", R"({"at": [8000, 3000], "sill": 2000})").is_null();
            ok = ok && NearNum(ElementOf(s, ids[1])["height"], 3000.0 + 3100.0 * tan30, 1.0);
            // 松开的话墙放不下这扇窗
            ok = ok && Contains(Refused(s, "modify", Json{ { "id", ids[1] }, { "attach_top", nullptr } }), "fit");
            // 贴着楼板的墙停在楼板下；底部贴着的立在楼板上
            const Id floor = Run(s, "floor", R"({"level": 100001, "boundary": [[0, 0], [8000, 0], [8000, 6000], [0, 6000]]})")["id"].get<Id>();
            const Id part = Run(s, "wall", Json{ { "points", { Pt(4000, 0), Pt(4000, 6000) } }, { "base_offset", 500 }, { "attach_top", floor } })["ids"][0].get<Id>();
            ok = ok && std::abs(WallTop(s, part, 3000) - 2800.0) < 1e-6;
            const Id low = Run(s, "floor", Json::object())["id"].get<Id>();
            ok = ok && ElementOf(s, part)["height"] == 2300.0;
            Run(s, "modify", Json{ { "id", part }, { "attach_base", low } });
            ok = ok && ElementOf(s, part)["height"] == 2800.0;
            // 屋顶或楼板删了不带走墙：墙松开
            Run(s, "delete", Json{ { "ids", { low } } });
            ok = ok && ElementOf(s, part).count("attach_base") == 0 &&
                 Contains(Refused(s, "modify", Json{ { "id", part }, { "attach_top", ids[0] } }), "roof or a floor");
            Check(ok, "屋顶：贴着屋顶的墙跟着它的底面；贴着楼板的停在板下、立在板上；删掉就松开");
        }
        {
            Session s;
            House(s);
            Run(s, "wall", R"({"points": [[5000, 0], [5000, 6000]], "type": "Interior - 100mm"})");
            // 点所在的房间，到墙面：4850 × 5800；离地 2600，20 厚
            const Json c = Run(s, "ceiling", R"({"point": [2000, 3000]})");
            const Id id = c["id"].get<Id>();
            const Json e = ElementOf(s, id);
            bool ok = c["area_m2"] == 28.13 && IdOf(s, e["level"]) == Id(100000) && e["offset"] == 2600.0 && e["thickness"] == 20.0 &&
                      NearOpt(CeilingTop(s, id, P2(2000, 3000)), 2620.0, 1e-9);
            // 给出轮廓、低一些、方格；没有墙围着的点要给轮廓
            const Json tiles = Run(s, "ceiling", R"({"boundary": [[5050, 100], [7900, 100], [7900, 5900], [5050, 5900]], "offset": 2400, "type": "Tile 600 x 600"})");
            ok = ok && ElementOf(s, tiles["id"].get<Id>())["grid"] == 600.0 && Contains(Refused(s, "ceiling", R"({"point": [9000, 3000]})"), "boundary");
            const Json t = Run(s, "type", R"({"category": "ceiling", "name": "Tile 1200", "thickness": 15, "grid": 1200})");
            ok = ok && t["type"] == Json::parse(R"({"thickness": 15.0, "grid": 1200.0})") &&
                 Contains(Refused(s, "type", R"({"category": "ceiling", "name": "Fine", "thickness": 15, "grid": 10})"), "grid");
            Check(ok, "天花板：铺满房间到墙面；没有墙围着时要给轮廓；格子不能太细");
        }
        {
            Session s;
            // 两个 6 米宽的翼组成 L 形，在 Level 2 上盖屋顶：伸出墙面 500，30°
            Run(s, "wall", R"({"points": [[0, 0], [12000, 0], [12000, 6000], [6000, 6000], [6000, 12000], [0, 12000]], "closed": true})");
            const Id roof = Run(s, "roof", Json::object())["id"].get<Id>();
            const double eave = 3000.0 - 500.0 * tan30;
            const double ridge = eave + 3600.0 * tan30 + rise30;
            auto top = [&](double x, double y) { return RoofTop(s, roof, P2(x, y)); };
            // 两翼的屋脊，以及两翼相交处的上方
            bool ok = NearOpt(top(9000, 3000), ridge, 1e-6) && NearOpt(top(3000, 9000), ridge, 1e-6) && NearOpt(top(3000, 3000), ridge, 1e-6);
            // 内角里是天沟：每个坡从自己的檐口升起
            ok = ok && NearOpt(top(9000, 5000), eave + 1600.0 * tan30 + rise30, 1e-6) && NearOpt(top(4800, 4800), eave + 1800.0 * tan30 + rise30, 1e-6);
            // 两翼的端部竖直：山墙，屋脊通到那里
            Run(s, "modify", Json{ { "id", roof }, { "gables", { 1, 4 } } });
            ok = ok && NearOpt(top(12000, 3000), ridge, 1e-6);
            ok = ok && Contains(Refused(s, "modify", Json{ { "id", roof }, { "gable", true } }), "rectangle") &&
                 Contains(Refused(s, "modify", Json{ { "id", roof }, { "gables", { 6 } } }), "edge");
            // 三角形也能坡：三个坡面在中间相交
            const Id tri = Run(s, "roof", R"({"boundary": [[20000, 0], [26000, 0], [23000, 5196.152]]})")["id"].get<Id>();
            ok = ok && NearNum(ElementOf(s, tri)["ridge"], 3000.0 + 6000.0 / (2.0 * std::sqrt(3.0)) * tan30, 0.01);
            Check(ok, "屋顶：四坡盖住任何迹线，两翼相交处是天沟；山墙让屋脊通到端部");
        }
        {
            Session s;
            const auto ids = House(s);
            // Level 2 上盖住 8 × 6 的房子：底面轮廓沿南边，两端各伸出 500，在 3 米处升到 2 米高的屋脊，向北扫过墙
            const Json profile = Json::parse(R"({"line": [[0, 0], [8000, 0]], "profile": [[-500, 0], [3000, 2000], [8500, 0]], "from": -600, "to": 6600})");
            const Id roof = Run(s, "roof", Json{ { "extrusion", profile } })["id"].get<Id>();
            const Json e = ElementOf(s, roof);
            bool ok = e["ridge"] == 5000.0 && e["area_m2"] == 64.8;
            // 西坡的一半：高 1 米，垂直于坡面 300 厚
            ok = ok && NearOpt(RoofTop(s, roof, P2(1250, 3000)), 4000.0 + 300.0 * std::hypot(3500.0, 2000.0) / 3500.0, 1e-6);
            // 墙贴着它：南墙的顶跟着东坡
            Run(s, "modify", Json{ { "ids", ids }, { "attach_top", roof } });
            ok = ok && std::abs(WallTop(s, ids[0], 4000) - (3000.0 + 2000.0 * 4500.0 / 5500.0)) < 1e-6;
            // 轮廓沿线单向前进，扫过一定的进深
            ok = ok && Contains(Refused(s, "modify", Json{ { "id", roof }, { "extrusion", Json::parse(R"({"profile": [[0, 0], [0, 1000]]})") } }), "profile") &&
                 Contains(Refused(s, "modify", Json{ { "id", roof }, { "extrusion", Json::parse(R"({"to": -600})") } }), "across");
            Check(ok, "屋顶：拉伸屋顶把轮廓扫过去，墙可以贴着它");
        }
        {
            Session s;
            // 16 米见方围着 4 米的天井：四个 6 米宽的翼，Level 2 上 30°
            const Json outline = Json::parse("[[0, 0], [16000, 0], [16000, 16000], [0, 16000]]");
            const Json court = Json::parse("[[6000, 6000], [10000, 6000], [10000, 10000], [6000, 10000]]");
            const Id roof = Run(s, "roof", Json{ { "boundary", outline }, { "courtyards", { court } } })["id"].get<Id>();
            auto top = [&](double x, double y) { return RoofTop(s, roof, P2(x, y)); };
            // 每翼的屋脊离两边檐口各 3 米：围着天井的一圈屋脊，天井上方敞开
            const double ridge = 3000.0 + 3000.0 * tan30;
            bool ok = true;
            for (const auto& [x, y] : { std::pair{ 8000.0, 3000.0 }, std::pair{ 13000.0, 8000.0 }, std::pair{ 8000.0, 13000.0 }, std::pair{ 3000.0, 8000.0 },
                                        std::pair{ 3000.0, 3000.0 }, std::pair{ 13000.0, 13000.0 } })
                ok = ok && NearOpt(top(x, y), ridge + rise30, 1e-6);
            ok = ok && !top(8000, 8000);
            // 坡面从天井的檐口升起，同外轮廓的
            for (const double y : { 5000.0, 1000.0 })
                ok = ok && NearOpt(top(8000, y), 3000.0 + 1000.0 * tan30 + rise30, 1e-6);
            const Json e = ElementOf(s, roof);
            ok = ok && e["ridge"] == 4732.05 && e["area_m2"] == 240.0;
            // 天井的边接着外轮廓编号：它的南边（4）是山墙，南翼的坡升到它，6 米进深
            Run(s, "modify", Json{ { "id", roof }, { "gables", { 4 } } });
            ok = ok && NearOpt(top(8000, 5900), 3000.0 + 5900.0 * tan30 + rise30, 1e-6);
            ok = ok && Contains(Refused(s, "modify", Json{ { "id", roof }, { "gables", { 8 } } }), "edge 8");
            // 天井在外轮廓里面、互不相交；拉伸屋顶没有天井
            const std::pair<const char*, const char*> bad[] = {
                { R"([[[20000, 0], [22000, 0], [22000, 2000]]])", "inside" },
                { R"([[[-1000, 6000], [2000, 6000], [2000, 8000], [-1000, 8000]]])", "inside" },
            };
            for (const auto& [rings, why] : bad)
                ok = ok && Contains(Refused(s, "modify", Json{ { "id", roof }, { "courtyards", Json::parse(rings) } }), why);
            ok = ok && Contains(Refused(s, "modify", Json{ { "id", roof }, { "courtyards", { court, Json::parse("[[7000, 7000], [12000, 7000], [12000, 12000]]") } } }), "overlap");
            const Json extruded = Json::parse(R"({"line": [[0, 0], [16000, 0]], "profile": [[0, 0], [8000, 3000], [16000, 0]], "to": 16000})");
            ok = ok && Contains(Refused(s, "roof", Json{ { "extrusion", extruded }, { "courtyards", { court } } }), "footprint");
            // 平的时候是穿过它的洞
            Run(s, "modify", Json{ { "id", roof }, { "slope", 0 }, { "gables", Json::array() } });
            ok = ok && !top(8000, 8000) && ElementOf(s, roof)["area_m2"] == 240.0 && top(8000, 5000);
            Check(ok, "屋顶：围着天井从檐口升到一圈屋脊；天井的边可以是山墙");
        }
        {
            Session s;
            // 16 米见方的房子围着 4 米的天井，墙 200 厚：没有墙把天井那圈与外墙连起来
            Run(s, "wall", R"({"points": [[0, 0], [16000, 0], [16000, 16000], [0, 16000]], "closed": true})");
            Run(s, "wall", R"({"points": [[6000, 6000], [10000, 6000], [10000, 10000], [6000, 10000]], "closed": true})");
            const Id roof = Run(s, "roof", Json::object())["id"].get<Id>();
            const Json e = ElementOf(s, roof);
            // 伸出墙面 500：外面绕着房子，里面绕着天井，从西南角起顺时针
            bool ok = e["boundary"][0] == Pt(-600, -600) &&
                      e["courtyards"] == Json::parse("[[[6600.0, 6600.0], [6600.0, 9400.0], [9400.0, 9400.0], [9400.0, 6600.0]]]");
            // 每翼檐口到檐口 7.2 米，屋脊离两边 3.6 米：3000 + 3100 tan 30°。面积：17.2² 减 2.8²
            ok = ok && e["ridge"] == 4789.79 && e["area_m2"] == 288.0 && !RoofTop(s, roof, P2(8000, 8000));
            // 给出的天井是屋顶自己的；小得伸出后合上的被盖住
            const Id given = Run(s, "roof", R"({"courtyards": []})")["id"].get<Id>();
            ok = ok && RoofTop(s, given, P2(8000, 8000));
            const Id wide = Run(s, "roof", R"({"overhang": 2100})")["id"].get<Id>();
            ok = ok && ElementOf(s, wide).count("courtyards") == 0 && RoofTop(s, wide, P2(8000, 8000));
            // 里面有房间的环是室内，与外墙相连的也是：盖住
            const Id room = Run(s, "room", R"({"point": [8000, 8000], "name": "Hall"})")["id"].get<Id>();
            const Id hall = Run(s, "roof", Json::object())["id"].get<Id>();
            ok = ok && ElementOf(s, hall).count("courtyards") == 0 && RoofTop(s, hall, P2(8000, 8000));
            Run(s, "delete", Json{ { "ids", { room } } });
            Run(s, "wall", R"({"points": [[8000, 0], [8000, 6000]]})");
            const Id joined = Run(s, "roof", Json::object())["id"].get<Id>();
            ok = ok && ElementOf(s, joined).count("courtyards") == 0 && RoofTop(s, joined, P2(8000, 8000));
            Check(ok, "屋顶：沿墙盖时，独立的一圈墙围出天井；里面有房间的、与外墙相连的不算");
        }
        {
            Session s;
            // 16 米见方的房子围着 4 米的天井：天井四周的房间是一个环
            Run(s, "wall", R"({"points": [[0, 0], [16000, 0], [16000, 16000], [0, 16000]], "closed": true})");
            Run(s, "wall", R"({"points": [[6000, 6000], [10000, 6000], [10000, 10000], [6000, 10000]], "closed": true})");
            // 到墙面，也绕着那圈墙的墙面，从西南角起顺时针：15.8² 减 4.2²
            const Json made = Run(s, "ceiling", R"({"point": [3000, 3000], "type": "Tile 600 x 600"})");
            const Id id = made["id"].get<Id>();
            const Json e = ElementOf(s, id);
            bool ok = made["area_m2"] == 232.0 && e["area_m2"] == 232.0 &&
                      e["openings"] == Json::parse("[[[5900.0, 5900.0], [5900.0, 10100.0], [10100.0, 10100.0], [10100.0, 5900.0]]]");
            ok = ok && !CeilingTop(s, id, P2(8000, 8000)) && NearOpt(CeilingTop(s, id, P2(8000, 3000)), 2620.0, 1e-9);
            // 移动时洞口跟着走
            Run(s, "move", Json{ { "ids", { id } }, { "by", { 0, 1000 } } });
            ok = ok && ElementOf(s, id)["openings"][0][0] == Pt(5900, 6900);
            // 给出轮廓时洞口是给出的，各在它里面
            const Json square = Json::parse("[[20000, 0], [24800, 0], [24800, 4800], [20000, 4800]]");
            const Id plain = Run(s, "ceiling", Json{ { "boundary", square } })["id"].get<Id>();
            ok = ok && ElementOf(s, plain).count("openings") == 0;
            Run(s, "delete", Json{ { "ids", { plain } } });
            const Json out = Json::parse("[[[25000, 1000], [26000, 1000], [26000, 2000], [25000, 2000]]]");
            ok = ok && Contains(Refused(s, "ceiling", Json{ { "boundary", square }, { "openings", out } }), "isn't inside its outline");
            const Json hole = Json::parse("[[[21800, 1800], [23000, 1800], [23000, 3000], [21800, 3000]]]");
            ok = ok && Run(s, "ceiling", Json{ { "boundary", square }, { "openings", hole }, { "type", "Tile 600 x 600" } })["area_m2"] == 21.6;
            Check(ok, "天花板：房间围着一圈墙时停在那圈墙的墙面，洞口随它移动");
        }
        {
            Session s;
            House(s);
            const Json roof = Json::parse(R"([
                {"function": "finish", "material": "Roof tiles", "thickness": 50},
                {"function": "membrane", "material": "Membrane", "thickness": 0},
                {"function": "thermal", "material": "Insulation", "thickness": 150},
                {"function": "structure", "material": "Timber", "thickness": 100}])");
            bool ok = Run(s, "type", Json{ { "category", "roof" }, { "name", "Tiled roof" }, { "layers", roof } })["type"]["thickness"] == 300.0;
            const Json board = Json::parse(R"([{"function": "substrate", "material": "Timber", "thickness": 25}, {"function": "finish", "material": "Gypsum board", "thickness": 13}])");
            const Json t = Run(s, "type", Json{ { "category", "ceiling" }, { "name", "Board" }, { "layers", board }, { "grid", 600 } })["type"];
            ok = ok && t["thickness"] == 38.0 && t["grid"] == 600.0;
            ok = ok && Contains(Refused(s, "type", R"({"category": "roof", "name": "Odd", "thickness": 10, "layers": [{"thickness": 50}]})"), "50");
            // 各层有自己的 ID 和 GUID，在库里
            const Model& m = s.GetModel();
            ok = ok && m.LibraryInStep() && m.types.roof.at("Tiled roof").layers[0].id != 0 && m.library.GuidOf(Kind::Type("ceiling"), "Board");
            Check(ok, "类型：屋顶与天花板由从上往下的各层组成");
        }
        {
            Session s;
            House(s);
            bool ok = Run(s, "room", R"({"point": [2000, 3000], "name": "Living"})")["area_m2"] == 48.0;
            // 开敞的平面由一条线（不是墙）分成起居室和厨房
            const Json made = Run(s, "room_separator", R"({"points": [[5000, 0], [5000, 6000]]})");
            ok = ok && made["ids"].size() == 1 && Run(s, "elements", R"({"category": "room"})")["elements"][0]["area_m2"] == 30.0;
            ok = ok && Run(s, "room", R"({"point": [7000, 3000], "name": "Kitchen"})")["area_m2"] == 18.0;
            const Json lines = Run(s, "elements", R"({"category": "roomseparator"})");
            ok = ok && lines["count"] == 1 && lines["elements"][0]["length"] == 6000.0;
            // 楼板仍沿墙：分隔线不是板的边
            ok = ok && Run(s, "floor", Json::object())["area_m2"] == 48.0;
            // 一串点每段一条；重复的点、点太少是错误
            ok = ok && Run(s, "room_separator", R"({"points": [[0, 3000], [2000, 3000], [2000, 6000]]})")["ids"].size() == 2;
            Run(s, "undo", Json::object());
            for (const char* p : { R"({"points": [[0, 0]]})", R"({"points": [[0, 0], [0, 0]]})", R"({"points": [[0, 0], [1000, 0]], "level": "Nowhere"})" })
                ok = ok && !Refused(s, "room_separator", p).empty();
            Check(ok, "房间：分隔线不用墙就把房间分开");
        }
        {
            Session s;
            House(s);
            const double t = Run(s, "elements", R"({"category": "wall"})")["elements"][0]["thickness"].get<double>();
            Run(s, "room", R"({"point": [4000, 3000], "name": "Living"})");
            auto area = [&] { return Run(s, "elements", R"({"category": "room"})")["elements"][0]["area_m2"].get<double>(); };
            bool ok = Run(s, "room_area", Json::object())["at"] == "centre" && area() == 48.0;
            // 在墙面量：墙以内的地面，每边从中心线往里半个墙厚
            ok = ok && Run(s, "room_area", R"({"at": "finish"})")["at"] == "finish" && std::abs(area() - (8000.0 - t) * (6000.0 - t) / 1e6) < 1e-6;
            // 隔墙也取它的墙面
            Run(s, "wall", R"({"points": [[5000, 0], [5000, 6000]], "type": "Interior - 100mm"})");
            ok = ok && std::abs(area() - (5000.0 - t / 2.0 - 50.0) * (6000.0 - t) / 1e6) < 1e-6;
            // 随项目保存，也像别的改动一样撤销
            ok = ok && s.GetModel().ToJson()["room_area"] == "finish" && Contains(Refused(s, "room_area", R"({"at": "core"})"), "centre or finish") &&
                 !Refused(s, "room_area", R"({"to": "finish"})").empty();
            Run(s, "undo", Json::object());
            Run(s, "undo", Json::object());
            ok = ok && area() == 48.0 && s.GetModel().ToJson().count("room_area") == 0;
            Check(ok, "房间：在墙中心线或墙面量面积");
        }
        {
            Session s;
            House(s);
            const double t = Run(s, "elements", R"({"category": "wall"})")["elements"][0]["thickness"].get<double>();
            Run(s, "room_area", R"({"at": "finish"})");
            Run(s, "room_separator", R"({"points": [[5000, 0], [5000, 6000]]})");
            const double living = Run(s, "room", R"({"point": [2000, 3000], "name": "Living"})")["area_m2"].get<double>();
            const double kitchen = Run(s, "room", R"({"point": [7000, 3000], "name": "Kitchen"})")["area_m2"].get<double>();
            // 线没有厚度：两个房间平分墙之间的地面
            Check(std::abs(living - (5000.0 - t / 2.0) * (6000.0 - t) / 1e6) < 1e-6 && std::abs(kitchen - (3000.0 - t / 2.0) * (6000.0 - t) / 1e6) < 1e-6,
                  "房间：在墙面量时也停在分隔线");
        }
        {
            Session s;
            // 东边是 3 米宽的半圆：从南往北左转，向东凸出
            Run(s, "wall", R"({"points": [[4000, 3000], [0, 3000], [0, 0], [4000, 0]]})");
            Run(s, "wall", R"({"points": [[4000, 0], [4000, 3000]], "arc": 180})");
            Check(Run(s, "room", R"({"point": [4500, 1500]})")["area_m2"] == 15.53, "房间：圆弧墙也围房间（12 m² 加半个圆）");
        }
    }

    // ── 族与构件（第 2c 步）──────────────────────────────────────
    // 写字台族：顶面高 Height，宽和深按类型
    void Desk(Session& s)
    {
        Run(s, "family", R"({"name": "Work Desk",
            "parameters": [{"name": "Width", "value": 1200}, {"name": "Depth", "value": 600}, {"name": "Height", "instance": true, "value": 740},
                           {"name": "Top", "formula": "Height - 30"}],
            "types": {"1200 x 600": {}, "1600 x 800": {"Width": 1600, "Depth": 800}},
            "forms": [{"profile": [[0, 0], ["Width", 0], ["Width", "Depth"], [0, "Depth"]], "from": "Top", "to": "Height", "material": "Timber"}]})");
    }

    Fam::Piece FirstPiece(const Session& s, Id id)
    {
        const Element* e = s.GetModel().elements.Get(id);
        const Component* c = e ? e->As<Component>() : nullptr;
        const auto pieces = c ? s.GetModel().ComponentPieces(*c) : std::vector<Fam::Piece>{};
        return pieces.empty() ? Fam::Piece{} : pieces.front();
    }

    size_t FirstPieces(const Session& s, Id id)
    {
        const Element* e = s.GetModel().elements.Get(id);
        const Component* c = e ? e->As<Component>() : nullptr;
        return c ? s.GetModel().ComponentPieces(*c).size() : 0;
    }

    // 带族的门窗的房子：墙 100002–100005，门 100006，窗 100007
    Session FamilyHouse()
    {
        Session s;
        std::vector<Json> out;
        std::string error;
        if (!s.Script(R"(wall {"points": [[0,0],[8000,0],[8000,6000],[0,6000]], "closed": true}
door {"at": [2000, 0], "family": "Single Door", "type": "800 x 2100"}
window {"at": [8000, 3000], "family": "Fixed Window", "type": "600 x 900"})", out, error))
            std::printf("  带族门窗的房子：%s\n", error.c_str());
        return s;
    }

    void TestFamiliesAndComponents()
    {
        {
            Session s;
            Desk(s);
            const Id id = Run(s, "component", R"({"family": "Work Desk", "type": "1600 x 800", "at": [1000, 2000], "angle": 90})")["id"].get<Id>();
            Json e = ElementJson(s.GetModel(), id);
            bool ok = e["family"] == "Work Desk" && e["type"] == "1600 x 800" && e["values"]["Width"] == 1600.0 && e["values"]["Top"] == 710.0;
            // 转了四分之一：它的 x 朝北
            Fam::Piece p = FirstPiece(s, id);
            Ring corners;
            for (const P2& q : p.outline)
                corners.emplace_back(std::round(q.x * 1e6) / 1e6 + 0.0, std::round(q.y * 1e6) / 1e6 + 0.0);
            ok = ok && p.bottom == 710.0 && p.top == 740.0 && corners == Ring{ P2(1000, 2000), P2(1000, 3600), P2(200, 3600), P2(200, 2000) };
            // 实例参数是构件自己的，类型参数是类型的
            Run(s, "modify", Json{ { "id", id }, { "params", Json{ { "Height", 720 } } }, { "offset", 100 } });
            p = FirstPiece(s, id);
            ok = ok && p.bottom == 790.0 && p.top == 820.0;
            std::string why = Refused(s, "modify", Json{ { "id", id }, { "params", Json{ { "Width", 5 } } } });
            ok = ok && Contains(why, "Width") && Contains(why, "type parameter");
            why = Refused(s, "modify", Json{ { "id", id }, { "params", Json{ { "Height", "tall" } } } });
            ok = ok && Contains(why, "Height") && Contains(why, "number");
            ok = ok && Contains(Refused(s, "family", R"({"name": "Work Desk", "types": {"1600 x 800": {"Width": 0}}})"), "no area");
            // 族的类型改了，它的构件都跟着；改名跟着；有构件用着就删不掉
            Run(s, "family", R"({"name": "Work Desk", "types": {"1600 x 800": {"Width": 1700}}})");
            ok = ok && std::round(FirstPiece(s, id).outline[1].y) == 3700.0 && s.GetModel().families.at("Work Desk").types.at("1600 x 800").at("Depth") == 800;
            Run(s, "family", R"({"name": "Work Desk", "rename": "Writing desk"})");
            ok = ok && ElementJson(s.GetModel(), id)["family"] == "Writing desk";
            ok = ok && Contains(Refused(s, "family", R"({"name": "Writing desk", "delete": true})"), std::to_string(id));
            ok = ok && Contains(Refused(s, "component", R"({"family": "Work Desk", "at": [0, 0]})"), "Writing desk");
            // 不给类型：族的第一个
            const Id other = Run(s, "component", R"({"family": "Writing desk", "at": [0, 0]})")["id"].get<Id>();
            ok = ok && ElementJson(s.GetModel(), other)["type"] == "1200 x 600";
            Run(s, "delete", Json{ { "ids", { id, other } } });
            Run(s, "family", R"({"name": "Writing desk", "delete": true})");
            Check(ok && !s.GetModel().families.count("Writing desk"), "构件：放置族的类型，尺寸由参数定；族改了构件跟着");
        }
        {
            Session s;
            Desk(s);
            const Json saved = Run(s, "inspect", Json::object());
            bool ok = saved["families"]["Work Desk"]["types"] == Json::parse(R"(["1200 x 600", "1600 x 800"])");
            // family 返回的族带上名称存成文件，另一个项目读进来
            Json f = Run(s, "family", R"({"name": "Work Desk"})")["family"];
            f["name"] = "Work Desk";
            const std::string path = Temp("desk.json");
            {
                std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary | std::ios::trunc);
                out << f.dump();
            }
            Session other;
            ok = ok && Run(other, "family", Json{ { "path", path } })["name"] == "Work Desk" &&
                 other.GetModel().families.at("Work Desk").parameters == s.GetModel().families.at("Work Desk").parameters &&
                 other.GetModel().families.at("Work Desk").types == s.GetModel().families.at("Work Desk").types;
            Run(other, "family", Json{ { "path", path }, { "name", "Writing Table" } });
            ok = ok && other.GetModel().families.count("Writing Table");
            {
                std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary | std::ios::trunc);
                out << "[1, 2]";
            }
            ok = ok && Contains(Refused(other, "family", Json{ { "path", path } }), "isn't an object");
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
            Check(ok, "族：从文件读进来");
        }
        {
            // 新项目带一套族：家具 10、门窗各三种、各专业的器具；每个类型都放得下、建得出来
            bool ok = true;
            for (const Language l : { Language::English, Language::Portuguese, Language::Spanish })
            {
                const Model m = Model::New("x", l);
                // 家具 10、门窗 6、各专业的器具 18
                std::string why;
                ok = ok && m.Check(why) && m.families.size() == 10 + 6 + 18;
                Session s;
                s.Reset(m, std::nullopt);
                Run(s, "wall", R"({"points": [[-30000, 5000], [30000, 5000]]})");
                double along = -27500.0;
                for (const auto& [family, f] : m.families)
                    for (const auto& [t, _] : f.types)
                    {
                        if (Fam::CategoryOf::Hosted(f.category))
                        {
                            along += 2500.0;
                            const bool door = f.category == Fam::Category::Door;
                            const Json r = Run(s, door ? "door" : "window", Json{ { "at", Pt(along, 5000) }, { "family", family }, { "type", t } });
                            const auto local = r.is_null() ? std::nullopt : s.GetModel().OpeningLocal(*s.GetModel().elements.Get(r["id"].get<Id>()));
                            if (!local || local->first.empty() || (door && local->second.empty()))
                            {
                                std::printf("  %s：%s\n", family.c_str(), t.c_str());
                                ok = false;
                            }
                        }
                        else
                        {
                            const Json r = Run(s, "component", Json{ { "family", family }, { "type", t }, { "at", Pt(0, 0) } });
                            if (r.is_null() || FirstPiece(s, r["id"].get<Id>()).tris.empty())
                            {
                                std::printf("  %s：%s\n", family.c_str(), t.c_str());
                                ok = false;
                            }
                        }
                    }
            }
            const Model pt = Model::New("x", Language::Portuguese);
            ok = ok && pt.families.count("Cama") && std::any_of(pt.families.at("Cama").parameters.begin(), pt.families.at("Cama").parameters.end(),
                                                                [](const Fam::Parameter& p) { return p.name == "Largura"; });
            // 桌腿、椅腿和靠背都在桌面或座面以内
            const Model m = Model::New("x");
            for (const char* family : { "Table", "Chair" })
            {
                const Family& f = m.families.at(family);
                for (const auto& [t, _] : f.types)
                {
                    Fam::Values v;
                    std::vector<Fam::Piece> pieces;
                    std::string why;
                    if (!f.Values(t, {}, v, why) || !f.Pieces(v, pieces, why))
                    {
                        ok = false;
                        continue;
                    }
                    P2 lo(1e300, 1e300), hi(-1e300, -1e300);
                    for (const P2& q : pieces[0].outline)
                    {
                        lo = P2(std::min(lo.x, q.x), std::min(lo.y, q.y));
                        hi = P2(std::max(hi.x, q.x), std::max(hi.y, q.y));
                    }
                    for (const Fam::Piece& p : pieces)
                        for (const P2& q : p.outline)
                            ok = ok && q.x >= lo.x - 1e-6 && q.x <= hi.x + 1e-6 && q.y >= lo.y - 1e-6 && q.y <= hi.y + 1e-6;
                }
            }
            // 双人床两个枕头，单人床一个
            Session s;
            const auto bed = [&](const char* t)
            {
                // 先执行命令再取模型：命令换掉了会话的模型
                const Id id = Run(s, "component", Json{ { "family", "Bed" }, { "type", t }, { "at", Pt(0, 0) } })["id"].get<Id>();
                return FirstPieces(s, id);
            };
            const size_t twin = bed("1600 x 2000"), single = bed("900 x 2000");
            Check(ok && twin - single == 1, "族：新项目带一套族，每个类型都放得下、建得出来");
        }
        {
            Session s;
            // 文件说要删除或改名：只读进它带的族
            const std::string path = Temp("chair.json");
            {
                std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary | std::ios::trunc);
                out << R"({"name": "Chair", "delete": true, "rename": "Gone", "types": {"Tall": {"Height": 1100}}})";
            }
            Run(s, "family", Json{ { "path", path } });
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
            bool ok = s.GetModel().families.count("Chair") && !s.GetModel().families.count("Gone") && s.GetModel().families.at("Chair").types.at("Tall").at("Height") == 1100;
            // 新类型里、构件自己的值里、modify 里的 null：没有值，用族的
            Run(s, "family", R"({"name": "Chair", "types": {"Low": {"Height": null, "Width": 500}}})");
            ok = ok && s.GetModel().families.at("Chair").types.at("Low") == std::map<std::string, Json>{ { "Width", Json(500) } };
            Run(s, "family", R"({"name": "Stool", "parameters": [{"name": "Seat", "instance": true, "value": 450}], "types": {"A": {}},
                "forms": [{"profile": [[0, 0], [400, 0], [400, 400], [0, 400]], "to": "Seat"}]})");
            const Id id = Run(s, "component", R"({"family": "Stool", "at": [0, 0], "params": {"Seat": null}})")["id"].get<Id>();
            Run(s, "modify", Json{ { "id", id }, { "params", Json{ { "Seat", nullptr } } } });
            Check(ok && ElementJson(s.GetModel(), id)["values"]["Seat"] == 450.0, "族：文件只带来一个族，null 去掉值");
        }
        {
            // 撤销步骤与模型共用族的三角形
            Session s;
            Run(s, "family", R"({"name": "Mesh", "types": {"A": {}}, "forms": [{"mesh": [[[0, 0, 0], [100, 0, 0], [0, 100, 0]], [[0, 0, 0], [100, 0, 0], [0, 100, 0]]]}]})");
            const Model copy = s.GetModel();
            Check(copy.families.at("Mesh").forms[0].mesh == s.GetModel().families.at("Mesh").forms[0].mesh, "族：复制模型时共用族的三角形");
        }
        {
            Session s = FamilyHouse();
            const Json door = ElementJson(s.GetModel(), 100006);
            bool ok = door["family"] == "Single Door" && door["type"] == "800 x 2100" && door["width"] == 800.0 && door["height"] == 2100.0 && door["sill"] == 0.0 &&
                      door["values"]["Wall Thickness"] == 200.0 && door["values"]["Frame Width"] == 50.0;
            const Json window = ElementJson(s.GetModel(), 100007);
            ok = ok && window["width"] == 600.0 && window["height"] == 900.0 && window["sill"] == 1100.0;
            // 墙上的洞是族的尺寸：换成 1000 宽的类型
            Run(s, "modify", R"({"id": 100006, "type": "1000 x 2100"})");
            ok = ok && ElementJson(s.GetModel(), 100006)["width"] == 1000.0;
            // 不是门的族、没有的族、族没有的类型：拒绝
            const std::pair<const char*, const char*> bad[] = { { R"({"at": [4000, 0], "family": "Fixed Window"})", "no door family" },
                                                                { R"({"at": [4000, 0], "family": "Nowhere"})", "no door family" },
                                                                { R"({"at": [4000, 0], "family": "Single Door", "type": "9 x 9"})", "9 x 9" } };
            for (const auto& [p, says] : bad)
                ok = ok && Contains(Refused(s, "door", p), says);
            // 门的族不能当家具放；有门用着就删不掉
            ok = ok && Contains(Refused(s, "component", R"({"family": "Single Door", "at": [1000, 1000]})"), "door") &&
                 Contains(Refused(s, "family", R"({"name": "Single Door", "delete": true})"), "100006");
            Check(ok, "门窗：族定洞口的尺寸，窗的族给它窗台高度");
        }
        {
            Session s = FamilyHouse();
            Run(s, "family", R"({"name": "Hatch", "category": "door", "parameters": [{"name": "Width", "instance": true, "value": 600}, {"name": "Height", "value": 700}],
                "types": {"A": {}}, "forms": [{"profile": [["-Width / 2", "-Wall Thickness / 2"], ["Width / 2", "-Wall Thickness / 2"], ["Width / 2", "Wall Thickness / 2"]], "to": "Height"}]})");
            const Id id = Run(s, "door", R"({"at": [4000, 0], "family": "Hatch"})")["id"].get<Id>();
            bool ok = ElementJson(s.GetModel(), id)["width"] == 600.0;
            Run(s, "modify", Json{ { "id", id }, { "params", Json{ { "Width", 900 } } } });
            ok = ok && ElementJson(s.GetModel(), id)["width"] == 900.0;
            ok = ok && Contains(Refused(s, "modify", Json{ { "id", id }, { "params", Json{ { "Height", 5 } } } }), "type parameter");
            const std::pair<const char*, const char*> bad[] = {
                { R"({"name": "Bad", "category": "door", "types": {"A": {}}, "parameters": [{"name": "Width", "value": 1}]})", "Height" },
                { R"({"name": "Bad", "category": "window", "types": {"A": {}}, "parameters": [{"name": "Width", "value": 1}, {"name": "Height", "value": 1}, {"name": "Wall Thickness", "value": 1}]})", "Wall Thickness" },
                { R"({"name": "Bad", "category": "furniture", "types": {"A": {}}, "symbols": [{"line": [[0, 0], [1, 1]]}]})", "symbols" },
                { R"({"name": "Bad", "category": "door", "types": {"A": {}}, "parameters": [{"name": "Width", "value": 1}, {"name": "Height", "value": 1}], "symbols": [{"arc": {"centre": [0, 0], "radius": 0, "to": 90}}]})", "radius" },
            };
            for (const auto& [p, says] : bad)
                ok = ok && Contains(Refused(s, "family", p), says);
            Check(ok, "门窗：门族的实例参数是门自己的，类型参数是类型的；不健全的族拒绝");
        }
        {
            // 门族的开启线在平面上，随门翻转（BIMCraft 的测试看平面图，这里直接看门画的线）
            Session s = FamilyHouse();
            const auto arc = [&](P2 hinge, P2 sign)
            {
                std::vector<Ring> lines;
                for (const Ring& l : s.GetModel().OpeningStrokes(*s.GetModel().elements.Get(100006)))
                    if (l.size() > 10)
                        lines.push_back(l);
                bool ok = lines.size() == 1;
                // 离铰链 700（800 的洞口减两边 50 的门框），在门开向的那侧
                for (const P2& p : ok ? lines[0] : Ring{})
                    ok = ok && std::abs(p.Dist(hinge) - 700.0) < 1.5 && (p.x - hinge.x) * sign.x >= -1.5 && (p.y - hinge.y) * sign.y >= -1.5;
                return ok;
            };
            bool ok = arc(P2(1650, 100), P2(1, 1));
            Run(s, "modify", R"({"id": 100006, "flip_hand": true})");
            ok = ok && arc(P2(2350, 100), P2(-1, 1));
            Run(s, "modify", R"({"id": 100006, "flip_facing": true})");
            ok = ok && arc(P2(2350, -100), P2(-1, -1));
            Run(s, "modify", R"({"id": 100006, "flip_hand": false})");
            ok = ok && arc(P2(1650, -100), P2(1, -1));
            Check(ok, "门窗：门族的开启线随门翻转");
        }
        {
            // 翻转的门族形体仍朝外、大小不变
            Session s = FamilyHouse();
            const auto volumes = [&]
            {
                std::vector<double> out;
                const auto local = s.GetModel().OpeningLocal(*s.GetModel().elements.Get(100006));
                for (const Fam::Piece& p : local ? local->first : std::vector<Fam::Piece>{})
                {
                    double v = 0.0;
                    for (const Tri& t : p.tris)
                        v += V3::Dot(t[0], V3::Cross(t[1], t[2]));
                    out.push_back(v / 6.0);
                }
                return out;
            };
            const auto straight = volumes();
            bool ok = straight.size() > 3 && std::all_of(straight.begin(), straight.end(), [](double v) { return v > 0.0; });
            for (const auto& [facing, hand] : { std::pair{ true, false }, std::pair{ false, true }, std::pair{ true, true } })
            {
                Run(s, "modify", Json{ { "id", 100006 }, { "flip_facing", facing }, { "flip_hand", hand } });
                const auto now = volumes();
                ok = ok && now.size() == straight.size();
                for (size_t i = 0; ok && i < now.size(); ++i)
                    ok = std::abs(straight[i] - now[i]) < 1e-6 * straight[i];
            }
            Check(ok, "门窗：翻转的门族形体仍朝外、大小不变");
        }
    }

    // ── 平面图（第 2d 步，render.rs 的测试）──────────────────────
    Session PlanHouse()
    {
        Session s;
        std::vector<Json> out;
        std::string error;
        if (!s.Script(R"(wall {"points": [[0,0],[8000,0],[8000,6000],[0,6000]], "closed": true}
door {"at": [2000, 0]}
window {"at": [8000, 3000]}
room {"point": [4000, 3000], "name": "Living"}
grid {"start": [0, -2000], "end": [0, 8000]})", out, error))
            std::printf("  平面图的房子：%s\n", error.c_str());
        return s;
    }

    void TestPlan()
    {
        const Guid level1 = Session().GetModel().GuidOf(Id(100000));
        {
            Session s = PlanHouse();
            const Model& m = s.GetModel();
            const Render::Drawing d = Render::Plan(m, m.GuidOf(Id(100000)), 800, 600);
            bool ok = d.owners.size() == d.prims.size();
            // 墙 100002 到 100005，然后是门、窗、房间、轴网
            for (Id id = 100002; id <= 100009; ++id)
                ok = ok && std::find(d.owners.begin(), d.owners.end(), std::optional<Id>(id)) != d.owners.end();
            Check(ok, "平面图：每一项都写着它画的元素");
            (void)level1;
        }
        {
            Session s = PlanHouse();
            const Model& m = s.GetModel();
            const Render::Drawing d = Render::Plan(m, m.GuidOf(Id(100000)), 800, 600);
            const auto at = [&](double x, double y) { return d.view.ToPx(P2(x, y)); };
            bool ok = d.Pick(at(2000, 0), 4.0f) == Id(100006) && d.Pick(at(6000, 0), 4.0f) == Id(100002) && d.Pick(at(0, 3000), 4.0f) == Id(100005);
            const auto [x, y] = at(6000, -100);
            ok = ok && d.Pick({ x, y + 3.0f }, 4.0f) == Id(100002) && d.Pick(at(8000, 3000), 4.0f) == Id(100007) && d.Pick(at(4000, 3000), 10.0f) == Id(100008) &&
                 d.Pick(at(0, -1500), 4.0f) == Id(100009) && !d.Pick(at(4000, 1200), 4.0f);
            const auto all = d.PickAll(at(2000, 0), 4.0f);
            ok = ok && all == std::vector<Id>{ 100006, 100002 };
            Check(ok, "平面图：拾取画得离点最近的元素，门在墙上面");
        }
        {
            Session s = PlanHouse();
            const Model& m = s.GetModel();
            const Render::Drawing d = Render::Plan(m, m.GuidOf(Id(100001)), 800, 600);
            const bool poche = std::any_of(d.prims.begin(), d.prims.end(), [](const Render::Prim& p) { return p.kind == Render::Prim::Kind::Fill && p.color == Render::Poche; });
            Check(!d.prims.empty() && !poche, "平面图：别的标高的平面不画这些墙");
        }
        {
            Session s = PlanHouse();
            const Model& m = s.GetModel();
            const Render::Drawing fitted = Render::Plan(m, m.GuidOf(Id(100000)), 800, 600);
            const Render::Drawing again = Render::Draw(m, m.GuidOf(Id(100000)), fitted.view);
            Render::View view = fitted.view;
            view.scale *= 2.0;
            view.cx = 8000.0;
            view.cy = 6000.0;
            const P2 back = view.ToMm(view.ToPx(P2(1234, -567)));
            Check(again.owners == fitted.owners && back.Dist(P2(1234, -567)) < 0.01, "平面图：按给的视图画同一张显示列表，像素与毫米互换");
        }
        {
            // 门族的开启线画在平面上，随门翻转（tests.rs 的原样：看平面图里门画的、不闭合的长折线）
            Session s = FamilyHouse();
            const auto swing = [&]
            {
                const Model& m = s.GetModel();
                const Render::Drawing d = Render::Plan(m, m.GuidOf(Id(100000)), 1600, 1000);
                std::vector<std::vector<P2>> lines;
                for (size_t i = 0; i < d.prims.size(); ++i)
                    if (d.owners[i] == Id(100006) && d.prims[i].kind == Render::Prim::Kind::Line && !d.prims[i].closed && d.prims[i].pts.size() > 10)
                    {
                        std::vector<P2> pts;
                        for (const auto& q : d.prims[i].pts)
                            pts.push_back(d.view.ToMm(q));
                        lines.push_back(std::move(pts));
                    }
                return lines;
            };
            const auto arc = [&](P2 hinge, P2 sign)
            {
                const auto lines = swing();
                bool ok = lines.size() == 1;
                for (const P2& p : ok ? lines[0] : std::vector<P2>{})
                    ok = ok && std::abs(p.Dist(hinge) - 700.0) < 1.5 && (p.x - hinge.x) * sign.x >= -1.5 && (p.y - hinge.y) * sign.y >= -1.5;
                return ok;
            };
            bool ok = arc(P2(1650, 100), P2(1, 1));
            Run(s, "modify", R"({"id": 100006, "flip_hand": true})");
            ok = ok && arc(P2(2350, 100), P2(-1, 1));
            Run(s, "modify", R"({"id": 100006, "flip_facing": true})");
            ok = ok && arc(P2(2350, -100), P2(-1, -1));
            Run(s, "modify", R"({"id": 100006, "flip_hand": false})");
            ok = ok && arc(P2(1650, -100), P2(1, -1));
            // 项目自己类型的门照旧画开启线：24 段弦
            Run(s, "modify", R"({"id": 100006, "family": null, "type": "Single 800 x 2100"})");
            Check(ok && swing().size() == 1, "平面图：门族的开启线画在平面上，随门翻转");
        }
        {
            // 示例房屋的平面：每面墙、门、窗、房间、轴网、家具都画出来，屋顶在上一层的平面上画屋脊
            Session s;
            std::vector<Json> out;
            std::string error;
            s.Script(SampleScript, out, error);
            const Model& m = s.GetModel();
            const Render::Drawing d = Render::Plan(m, m.GuidOf(Id(100000)), 1200, 900);
            bool ok = true;
            for (const auto& [id, e] : m.elements.Items())
            {
                const bool drawn = std::find(d.owners.begin(), d.owners.end(), std::optional<Id>(id)) != d.owners.end();
                if ((e->As<Wall>() || e->AsOpening() || e->As<Room>() || e->As<Grid>() || e->As<Component>() || e->As<Floor>()) && !drawn)
                {
                    std::printf("  %s %llu 没画出来\n", e->Category(), static_cast<unsigned long long>(id));
                    ok = false;
                }
            }
            const Render::Drawing up = Render::Plan(m, m.GuidOf(Id(100001)), 1200, 900);
            ok = ok && std::find(up.owners.begin(), up.owners.end(), std::optional<Id>(100024)) != up.owners.end();
            Check(ok, "平面图：示例房屋的墙、门窗、房间、轴网、家具都画出来，屋顶在它的标高上");
        }
    }

    void TestSample()
    {
        Session s;
        std::vector<Json> out;
        std::string error;
        const bool ran = s.Script(SampleScript, out, error);
        if (!ran)
            std::printf("  示例：%s\n", error.c_str());
        const Model& m = s.GetModel();
        const Json i = Run(s, "inspect", Json::object());
        const Json floor = Run(s, "elements", R"({"category": "floor"})")["elements"][0];
        Check(ran && m.name == "Sample House" && i["counts"]["wall"] == 7 && i["counts"]["door"] == 4 && i["counts"]["window"] == 5 &&
                  i["counts"]["grid"] == 5 && i["counts"]["floor"] == 1 && floor["area_m2"] == 70.0 && m.Check(error) && s.History().empty() == false,
              "示例房屋：用命令搭起来，检查通过");
        // 四个房间，每个一块天花板，一个四坡屋顶，外墙贴着它
        // 先存下来：range-for 里临时对象的成员在循环开始前就析构了（C++20）
        const Json rooms = Run(s, "elements", R"({"category": "room"})");
        std::vector<double> areas;
        for (const Json& r : rooms["elements"])
            areas.push_back(r["area_m2"].get<double>());
        const Model& after = s.GetModel();
        Check(i["counts"]["room"] == 4 && i["counts"]["ceiling"] == 4 && i["counts"]["roof"] == 1 && i["counts"]["component"] == 11 &&
                  areas == std::vector<double>{ 24, 18, 14, 14 } &&
                  after.GetWall(Id(100007))->attachTop == after.GuidOf(Id(100024)),
              "示例房屋：四个房间（24、18、14、14 m²）各有天花板，外墙贴着屋顶，十一件家具");
    }

    // 一组不怀好意的参数逐条命令扫一遍：命令可以拒绝，但不能抛出内部错误，也不能弄坏模型
    void TestHostileParams()
    {
        const Json payloads[] = {
            Json::object(),
            Json::array(),
            Json(nullptr),
            Json::parse(R"({"ids": [1e300], "by": [1e300, 0]})"),
            Json::parse(R"({"id": "nope", "points": "x", "elevation": "x", "at": [1, 2, 3]})"),
            Json::parse(R"({"points": [[0, 0], [1e9, 1e9]], "height": -1, "arc": 359.9})"),
            Json::parse(R"({"points": [[0, 0], [0, 0]], "type": "\u0000"})"),
            Json::parse(R"({"boundary": [[0, 0], [1, 0]], "openings": [[[5, 5]]], "slope": 89, "arrow": [[0, 0], [0, 0]]})"),
            Json::parse(R"({"category": "wall", "name": "X", "layers": [{"thickness": 1e300}], "thickness": "x"})"),
            Json::parse(R"({"name": "", "color": "#zzzzzz", "rename": "{00000000-0000-4000-8000-000000000000}"})"),
            Json::parse(R"({"id": 100002, "start": [1e301, 0], "type": null, "level": "nowhere"})"),
            Json::parse(R"({"id": 100002, "level": 100006})"),
            Json::parse(R"({"count": 1e9, "limit": -1, "offset": "x", "length": "furlong"})"),
            Json::parse(R"({"wall": 100006, "offset": -1e9, "sill": 1e9, "mark": 5})"),
            Json::parse(R"({"ids": [100002, 100002, 100003], "by": [0.0001, 0], "end": "middle"})"),
            Json::parse(R"({"gables": [1e20, -1], "slope": 75, "overhang": 1e9, "extrusion": {"line": [[0, 0], [0, 0]], "profile": [], "to": 1e300}})"),
            Json::parse(R"({"courtyards": [[[0, 0]]], "point": [4000, 3000], "join": 100002, "at": "finish", "grid": 1})"),
            Json::parse(R"({"id": 100009, "boundary": [], "extrusion": {"line": [[0, 0], [1, 0]], "profile": [[0, 0], [1, 1]], "to": 1}})"),
        };
        bool ok = true;
        for (const Command& c : Commands())
        {
            if (std::string(c.id) == "open" || std::string(c.id) == "save")
                continue;
            for (const Json& p : payloads)
            {
                Session s;
                const auto ids = House(s);
                Run(s, "door", R"({"at": [2000, 0]})");
                Run(s, "floor", Json::object());
                Run(s, "roof", Json::object());
                Run(s, "room", R"({"point": [4000, 3000]})");
                (void)ids;
                Json out;
                std::string error;
                s.Execute(c.id, p, out, error);
                std::string why;
                if (Contains(error, "internal error") || !s.GetModel().Check(why))
                {
                    std::printf("  %s %s：%s %s\n", c.id, p.dump().c_str(), error.c_str(), why.c_str());
                    ok = false;
                }
            }
        }
        Check(ok, "命令：恶意参数不会让命令出内部错误，也不会弄坏模型");
    }

    bool Near(const Json& a, double x, double y)
    {
        return a.is_array() && std::abs(a[0].get<double>() - x) < 0.5 && std::abs(a[1].get<double>() - y) < 0.5;
    }

    Id FirstId(const Json& r) { return r["ids"][0].get<Id>(); }

    // 旋转、镜像、修剪到角、拆分、对齐（移植自 BIMCraft tests.rs 与 align.rs 的测试）
    void TestModifyTools()
    {
        {
            Session s;
            const auto ids = House(s);
            const Id part = FirstId(Run(s, "wall", R"({"points": [[4000, 0], [4000, 6000]], "type": "Interior - 100mm"})"));
            // 隔墙绕中点转 10°：两端滑回它原来连着的墙上
            Run(s, "RO", Json{ { "ids", { part } }, { "about", Pt(4000, 3000) }, { "angle", 10 } });
            auto [a, b] = Ends(s, part);
            bool ok = Near(a, 4529, 0) && Near(b, 3471, 6000);
            // 整圈转 90°：整数还是整数，接头也还在
            Run(s, "rotate", Json{ { "ids", { ids[0], ids[1], ids[2], ids[3] } }, { "about", Pt(4000, 3000) }, { "angle", 90 } });
            ok = ok && Ends(s, ids[0]) == std::pair{ Pt(7000, -1000), Pt(7000, 7000) };
            const Id door = Run(s, "door", R"({"at": [7000, 3000]})")["id"].get<Id>();
            ok = ok && Contains(Refused(s, "rotate", Json{ { "ids", { door } }, { "about", Pt(0, 0) }, { "angle", 5 } }), "with its wall");
            Check(ok, "旋转：绕一点转，连着的墙保持相接");
        }
        {
            Session s;
            const auto ids = House(s);
            const Id door = Run(s, "door", R"({"at": [2000, 0]})")["id"].get<Id>();
            const Json made = Run(s, "MM", Json{ { "ids", { ids[0] } }, { "axis", Json::array({ Pt(10000, -5000), Pt(10000, 5000) }) }, { "copy", true } });
            const Id wall = made["ids"][0].get<Id>(), twin = made["ids"][1].get<Id>();
            // 镜像反着走，外侧是原来那面的镜像
            bool ok = Ends(s, wall) == std::pair{ Pt(12000, 0), Pt(20000, 0) };
            const Json d = ElementOf(s, door), t = ElementOf(s, twin);
            ok = ok && IdOf(s, t["host"]) == wall && t["at"] == Pt(18000, 0);
            // 挂在镜像的门框上，朝同一边开
            ok = ok && t["flip_facing"] == d["flip_facing"] && d["flip_hand"] == false && t["flip_hand"] == true;
            const Id grid = Run(s, "grid", R"({"start": [0, -1000], "end": [0, 7000]})")["id"].get<Id>();
            Run(s, "mirror", Json{ { "ids", { grid } }, { "axis", Json::array({ Pt(4000, 0), Pt(4000, 1000) }) } });
            ok = ok && ElementOf(s, grid)["start"] == Pt(8000, -1000);
            ok = ok && Contains(Refused(s, "mirror", Json{ { "ids", { grid } }, { "axis", Json::array({ Pt(1, 1), Pt(1, 1) }) } }), "axis");
            Check(ok, "镜像：复制的门朝镜像的方向开，不复制时轴网自己过去");
        }
        {
            Session s;
            const Id a = FirstId(Run(s, "wall", R"({"points": [[0, 0], [4000, 0]]})"));
            const Id b = FirstId(Run(s, "wall", R"({"points": [[5000, 1000], [5000, 6000]]})"));
            Run(s, "TR", Json{ { "ids", { a, b } } });
            bool ok = Ends(s, a).second == Pt(5000, 0) && Ends(s, b).first == Pt(5000, 0);
            // 相交的墙：keep 说每面墙留哪部分
            const Id c = FirstId(Run(s, "wall", R"({"points": [[-2000, 3000], [3000, 3000]]})"));
            const Id d = FirstId(Run(s, "wall", R"({"points": [[1000, 1500], [1000, 5000]]})"));
            Run(s, "corner", Json{ { "ids", { c, d } }, { "keep", Json::array({ Pt(-1000, 3000), Pt(1000, 4500) }) } });
            ok = ok && Ends(s, c) == std::pair{ Pt(-2000, 3000), Pt(1000, 3000) } && Ends(s, d) == std::pair{ Pt(1000, 3000), Pt(1000, 5000) };
            const Id e = FirstId(Run(s, "wall", R"({"points": [[0, 8000], [4000, 8000]]})"));
            ok = ok && Contains(Refused(s, "corner", Json{ { "ids", { a, e } } }), "parallel");
            Check(ok, "修剪到角：两面墙伸缩到相交处");
        }
        {
            Session s;
            const auto ids = House(s);
            const Id room = Run(s, "room", R"({"point": [4000, 3000]})")["id"].get<Id>();
            const Id door = Run(s, "door", R"({"at": [2000, 0]})")["id"].get<Id>();
            const Id window = Run(s, "window", R"({"at": [6000, 0]})")["id"].get<Id>();
            const Json pieces = Run(s, "SL", Json{ { "id", ids[0] }, { "at", Pt(4000, 20) } });
            const Id made = pieces["ids"][1].get<Id>();
            bool ok = Ends(s, ids[0]) == std::pair{ Pt(0, 0), Pt(4000, 0) } && Ends(s, made) == std::pair{ Pt(4000, 0), Pt(8000, 0) };
            ok = ok && IdOf(s, ElementOf(s, door)["host"]) == ids[0] && IdOf(s, ElementOf(s, window)["host"]) == made;
            ok = ok && ElementOf(s, window)["at"] == Pt(6000, 0) && ElementOf(s, room)["area_m2"] == 48.0;
            ok = ok && Contains(Refused(s, "split", Json{ { "id", ids[0] }, { "at", Pt(2000, 0) } }), "in the way");
            ok = ok && Contains(Refused(s, "split", Json{ { "id", ids[0] }, { "at", Pt(2000, 3000) } }), "isn't on");
            Check(ok, "拆分：墙一分为二，门窗随它所在的一段");
        }
        {
            Session s;
            House(s);
            // 一面斜的隔墙对齐到 x = 7000 的线上：先转平，再移过去
            const Id part = FirstId(Run(s, "wall", R"({"points": [[3000, 1000], [3200, 5000]], "type": "Interior - 100mm"})"));
            const auto lines = Align::Lines(s.GetModel(), part);
            const auto near = Align::Nearest(s.GetModel(), part, { 3100, 3000 });
            bool ok = lines.size() == 5 && near && near->second < 1e-6;
            const Json r = Run(s, "AL", Json{ { "id", part }, { "reference", Json::array({ Pt(7000, 0), Pt(7000, 6000) }) },
                                               { "line", Json::array({ ToJson(near->first.first), ToJson(near->first.second) }) } });
            const auto [a, b] = Ends(s, part);
            ok = ok && !r.is_null() && std::abs(a[0].get<double>() - 7000.0) < 0.01 && std::abs(b[0].get<double>() - 7000.0) < 0.01;
            // 门窗只能沿墙滑
            const Id door = Run(s, "door", R"({"at": [2000, 0]})")["id"].get<Id>();
            ok = ok && Contains(Refused(s, "align", Json{ { "id", door }, { "reference", Json::array({ Pt(0, 0), Pt(1000, 1000) }) },
                                                         { "line", Json::array({ Pt(1550, -150), Pt(1550, 150) }) } }), "slide along");
            Run(s, "align", Json{ { "id", door }, { "reference", Json::array({ Pt(1000, -500), Pt(1000, 500) }) },
                                  { "line", Json::array({ Pt(1550, -150), Pt(1550, 150) }) } });
            ok = ok && ElementOf(s, door)["at"] == Pt(1450, 0);
            Check(ok, "对齐：转到与参照平行再移上去，门窗沿墙滑");
        }
    }

    std::vector<std::string> WallTypes(const Session& s)
    {
        std::vector<std::string> out;
        for (const auto& [n, _] : s.GetModel().types.wall)
            out.push_back(n);
        return out;
    }

    // 新项目自带的家具里也有 Chair：测试的族用另一个名字，免得类型并进那个族
    void Chair(Session& s)
    {
        Run(s, "family", R"({"name": "Test Chair", "category": "furniture", "types": {"Small": {}, "Large": {}},
            "forms": [{"plane": "xy", "profile": [[-200, -200], [200, -200], [200, 200], [-200, 200]], "to": 450}]})");
    }

    // 删除、改名类型（移植自 BIMCraft purge_tests.rs）
    void TestTypeEntries()
    {
        auto has = [](const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); };
        {
            Session s;
            const Id wall = FirstId(Run(s, "wall", R"({"points": [[0, 0], [4000, 0]]})"));
            Run(s, "type", R"({"category": "wall", "name": "Spare 100", "thickness": 100})");
            bool ok = has(WallTypes(s), "Spare 100");
            const Json r = Run(s, "delete_type", R"({"category": "wall", "type": "Spare 100"})");
            ok = ok && r["deleted"] == Json::parse(R"({"kind": "wall type", "name": "Spare 100"})") && !has(WallTypes(s), "Spare 100");
            const std::string used = s.GetModel().GetWall(wall)->typeName;
            ok = ok && Contains(Refused(s, "delete_type", Json{ { "category", "wall" }, { "type", used } }), "is used 1 time");
            const std::string e = Refused(s, "delete_type", R"({"category": "wall", "type": "No such"})");
            ok = ok && Contains(e, "no wall type") && Contains(e, "there are:");
            ok = ok && Contains(Refused(s, "delete_type", R"({"category": "wall"})"), "give `category` and `type`");
            Run(s, "undo", Json::object());
            ok = ok && has(WallTypes(s), "Spare 100");
            Check(ok, "删除类型：没用的删掉，用着的拒绝并说几处用着，可以撤销");
        }
        {
            Session s;
            const auto names = WallTypes(s);
            for (size_t i = 1; i < names.size(); ++i)
                Run(s, "delete_type", Json{ { "category", "wall" }, { "type", names[i] } });
            Check(WallTypes(s) == std::vector<std::string>{ names[0] } &&
                      Contains(Refused(s, "delete_type", Json{ { "category", "wall" }, { "type", names[0] } }), "last wall type"),
                  "删除类型：一个类别最后一个类型删不掉");
        }
        {
            Session s;
            Chair(s);
            Run(s, "delete_type", R"({"family": "Test Chair", "type": "Large"})");
            bool ok = Contains(Refused(s, "delete_type", R"({"family": "Test Chair", "type": "Small"})"), "last type: delete the family");
            Run(s, "delete_type", R"({"family": "Test Chair"})");
            ok = ok && !s.GetModel().families.count("Test Chair");
            Check(ok, "删除类型：族的类型没用时删掉，最后一个删不掉，族可以删");
        }
        {
            Session s;
            const Id wall = FirstId(Run(s, "wall", R"({"points": [[0, 0], [4000, 0]]})"));
            const std::string was = s.GetModel().GetWall(wall)->typeName;
            const auto guid = s.GetModel().library.GuidOf(Kind::Type("wall"), was);
            const Json r = Run(s, "rename_type", Json{ { "category", "wall" }, { "type", was }, { "name", " Brick 200 " } });
            bool ok = r["renamed"] == Json{ { "kind", "wall type" }, { "from", was }, { "to", "Brick 200" } };
            ok = ok && has(WallTypes(s), "Brick 200") && !has(WallTypes(s), was) && s.GetModel().GetWall(wall)->typeName == "Brick 200";
            ok = ok && s.GetModel().library.GuidOf(Kind::Type("wall"), "Brick 200") == guid;
            std::string taken;
            for (const std::string& n : WallTypes(s))
                if (n != "Brick 200")
                    taken = n;
            ok = ok && Contains(Refused(s, "rename_type", Json{ { "category", "wall" }, { "type", "Brick 200" }, { "name", taken } }), "already a wall type");
            ok = ok && Contains(Refused(s, "rename_type", R"({"category": "wall", "type": "Brick 200", "name": "  "})"), "`name`");
            // 族的类型，构件跟着
            Chair(s);
            const Id c = Run(s, "component", R"({"family": "Test Chair", "type": "Small", "at": [1000, 1000]})")["id"].get<Id>();
            Run(s, "rename_type", R"({"family": "Test Chair", "type": "Small", "name": "Child"})");
            const auto& types = s.GetModel().families.at("Test Chair").types;
            ok = ok && types.count("Child") && !types.count("Small") && s.GetModel().elements.Get(c)->As<Component>()->typeName == "Child";
            Run(s, "undo", Json::object());
            ok = ok && s.GetModel().families.at("Test Chair").types.count("Small");
            Check(ok, "类型改名：元素跟着，GUID 不变，重名、空名拒绝，族的类型也行");
        }
    }

    // 一个正方形的线，[x, y] 是西南角，顺时针画
    Json SquareLines(double x, double y, double size)
    {
        const Json c[4] = { Pt(x, y), Pt(x, y + size), Pt(x + size, y + size), Pt(x + size, y) };
        Json out = Json::array();
        for (int i = 0; i < 4; ++i)
            out.push_back(Json::array({ c[i], c[(i + 1) % 4] }));
        return out;
    }

    Json WithHole()
    {
        Json ls = SquareLines(0, 0, 6000);
        for (const Json& l : SquareLines(2000, 2000, 1000))
            ls.push_back(l);
        return ls;
    }

    Json SketchOf(Session& s, const Json& id)
    {
        const Json e = ElementOf(s, id.get<Id>());
        return e.contains("sketch") ? e["sketch"] : Json::array();
    }

    // 草图经命令（移植自 BIMCraft sketch.rs 的 command_tests）
    void TestSketches()
    {
        {
            Session s;
            const size_t steps = s.History().size();
            const Json r = Run(s, "sketch", Json{ { "lines", WithHole() } });
            bool ok = r["regions"].size() == 1 && r["regions"][0]["holes"].size() == 1 && r["area_m2"] == 35.0 &&
                      r["regions"][0]["outline"][1] == Pt(6000, 0) && s.History().size() == steps;
            Json ls = SquareLines(0, 0, 4000);
            ls.erase(ls.size() - 1);
            ls.push_back(Json::parse("[[-1000, 2000], [1000, 2000]]"));
            const std::string e = Refused(s, "sketch", Json{ { "lines", ls } });
            ok = ok && Contains(e, "line 0 and line 3 intersect at [0, 2000]") && Contains(e, "line 2 ends at [4000, 0] without meeting another");
            ok = ok && Contains(Refused(s, "sketch", R"({"lines": [[[0, 0], [1]]]})"), "`lines`");
            Check(ok, "草图：只解不改模型，一条条说出哪里不对");
        }
        {
            Session s;
            const Json f = Run(s, "floor", Json{ { "sketch", WithHole() } });
            const Floor* fl = s.GetModel().elements.Get(f["id"].get<Id>())->As<Floor>();
            bool ok = f["area_m2"] == 35.0 && fl->boundary.size() == 4 && fl->openings.size() == 1 && Geom::Area(fl->boundary) > 0.0;
            ok = ok && Run(s, "ceiling", Json{ { "sketch", WithHole() } })["area_m2"] == 35.0;
            const Json r = Run(s, "roof", Json{ { "sketch", WithHole() }, { "slope", 0 } });
            ok = ok && s.GetModel().elements.Get(r["id"].get<Id>())->As<Roof>()->courtyards.size() == 1;
            Json two = SquareLines(0, 0, 1000);
            for (const Json& l : SquareLines(5000, 0, 1000))
                two.push_back(l);
            ok = ok && Contains(Refused(s, "floor", Json{ { "sketch", two } }), "2 separate outlines");
            ok = ok && Contains(Refused(s, "floor", Json{ { "sketch", WithHole() }, { "boundary", Json::parse("[[0, 0], [1000, 0], [0, 1000]]") } }),
                                "`sketch` or `boundary`");
            Check(ok, "草图：楼板、天花板、屋顶的轮廓与洞可以画成草图");
        }
        {
            Session s;
            const Json f = Run(s, "floor", R"({"boundary": [[0, 0], [4000, 0], [4000, 4000], [0, 4000]]})")["id"];
            bool ok = Run(s, "edit_boundary", Json{ { "id", f } })["sketch"].size() == 4;
            ok = ok && Run(s, "edit_boundary", Json{ { "id", f }, { "sketch", WithHole() } })["area_m2"] == 35.0;
            ok = ok && s.GetModel().elements.Get(f.get<Id>())->As<Floor>()->openings.size() == 1 && s.History().back() == "Edit Boundary";
            Json open = SquareLines(0, 0, 4000);
            open.erase(open.size() - 1);
            const auto before = s.Snapshot();
            ok = ok && Contains(Refused(s, "edit_boundary", Json{ { "id", f }, { "sketch", open } }), "open") && s.Snapshot() == before;
            Run(s, "undo", Json::object());
            ok = ok && s.GetModel().elements.Get(f.get<Id>())->As<Floor>()->openings.empty();
            const Json w = Run(s, "wall", R"({"points": [[0, 0], [1000, 0]]})")["ids"][0];
            ok = ok && Contains(Refused(s, "edit_boundary", Json{ { "id", w }, { "sketch", WithHole() } }), "floor, a ceiling or a roof's footprint");
            Check(ok, "编辑边界：一步重画轮廓，不行时原样不动");
        }
        {
            Session s;
            const Json r = Run(s, "roof", R"({"boundary": [[0, 0], [8000, 0], [8000, 6000], [0, 6000]], "gables": [1, 3], "slope": 30})")["id"];
            Run(s, "edit_boundary", Json{ { "id", r }, { "sketch", Json::parse(R"([[[0, -1000], [8000, -1000]], [[8000, -1000], [8000, 6000]], [[8000, 6000], [0, 6000]], [[0, 6000], [0, -1000]]])") } });
            bool ok = s.GetModel().elements.Get(r.get<Id>())->As<Roof>()->gables.empty();
            Run(s, "undo", Json::object());
            Run(s, "edit_boundary", Json{ { "id", r }, { "sketch", Json::parse(R"([[[0, 0], [8000, 0]], [[8000, 0], [8000, 6000]], [[8000, 6000], [4000, 6000]], [[4000, 6000], [0, 6000]], [[0, 6000], [0, 0]]])") } });
            ok = ok && s.GetModel().elements.Get(r.get<Id>())->As<Roof>()->gables == std::vector<size_t>{ 1, 4 };
            const Json x = Run(s, "roof", R"({"extrusion": {"line": [[0, 0], [4000, 0]], "profile": [[0, 0], [2000, 1000], [4000, 0]], "to": 3000}})")["id"];
            ok = ok && Contains(Refused(s, "edit_boundary", Json{ { "id", x }, { "sketch", WithHole() } }), "extrusion");
            Check(ok, "编辑边界：屋顶的山墙留在迹线还有同一条边的地方");
        }
        {
            Session s;
            const Json f = Run(s, "floor", Json{ { "sketch", SquareLines(0, 0, 6000) } })["id"];
            const Json kept = SketchOf(s, f);
            std::vector<Id> ids;
            for (const Json& l : kept)
                if (l.contains("id"))
                    ids.push_back(l["id"].get<Id>());
            bool ok = ids.size() == 4 && std::all_of(ids.begin(), ids.end(), [](Id i) { return i >= 100000 && i <= 999999; }) &&
                      std::all_of(kept.begin(), kept.end(), [](const Json& l) { return l["guid"].is_string(); });
            const Json now = Run(s, "edit_boundary", Json{ { "id", f } })["sketch"];
            Json again = Json::array();
            for (size_t i = 0; i < 3; ++i)
                again.push_back(Json{ { "id", now[i]["id"] }, { "start", now[i]["start"] }, { "end", now[i]["end"] } });
            const Json mid = Pt(3000, 0);
            again.push_back(Json::array({ now[3]["start"], mid }));
            again.push_back(Json::array({ mid, now[3]["end"] }));
            Run(s, "edit_boundary", Json{ { "id", f }, { "sketch", again } });
            std::vector<Id> after;
            for (const Json& l : SketchOf(s, f))
                after.push_back(l["id"].get<Id>());
            ok = ok && after.size() == 5 && std::equal(ids.begin(), ids.begin() + 3, after.begin()) &&
                 std::none_of(after.begin() + 3, after.end(), [&](Id i) { return std::find(ids.begin(), ids.end(), i) != ids.end(); });
            ok = ok && Contains(Refused(s, "edit_boundary", Json{ { "id", f }, { "sketch", Json::parse(R"([{"id": 123456, "start": [0, 0], "end": [1, 1]}])") } }),
                                "123456 isn't a line of this sketch");
            // 移动：草图跟着走，保留 ID；复制：副本的线是新的
            const Json was = SketchOf(s, f);
            Run(s, "move", Json{ { "ids", { f } }, { "by", Pt(1000, 0) } });
            const Json moved = SketchOf(s, f);
            ok = ok && moved[0]["id"] == was[0]["id"] && moved[0]["start"][0].get<double>() == was[0]["start"][0].get<double>() + 1000.0;
            const Json copy = Run(s, "copy", Json{ { "ids", { f } }, { "by", Pt(0, 10000) } })["ids"][0];
            const Json copied = SketchOf(s, copy);
            ok = ok && copied.size() == 5 && std::none_of(copied.begin(), copied.end(), [&](const Json& l)
                                                         { return std::find(after.begin(), after.end(), l["id"].get<Id>()) != after.end(); });
            // 沿墙建的楼板没有草图，编辑边界时它的线是新的
            Run(s, "wall", R"({"points": [[20000, 0], [26000, 0], [26000, 6000], [20000, 6000]], "closed": true})");
            const Json w = Run(s, "floor", R"({"level": "Level 1"})")["id"];
            ok = ok && SketchOf(s, w).empty();
            const Json fresh = Run(s, "edit_boundary", Json{ { "id", w } })["sketch"];
            ok = ok && std::none_of(fresh.begin(), fresh.end(), [](const Json& l) { return l.contains("id"); });
            Check(ok, "草图线：各有 ID 与 GUID，编辑、移动时保留，复制的是新的");
        }
        {
            Session s;
            const Json f = Run(s, "floor", Json{ { "sketch", Json::parse("[[[0, 0], [4000, 0]], [[4000, 0], [0, 0], [2000, 2000]]]") } });
            const double a = f["area_m2"].get<double>();
            const double turn = SketchOf(s, f["id"])[1].value("arc", 0.0);
            Check(std::abs(a - 2.0 * 3.14159265358979323846) < 0.1 && std::abs(turn - 180.0) < 1e-6, "草图：画了圆弧的楼板是圆的");
            // 存成文件再读回来，草图还在
            const std::string path = Temp("sketch.minibim");
            Run(s, "save", Json{ { "path", path } });
            Session t;
            Run(t, "open", Json{ { "path", path } });
            Check(SketchOf(t, f["id"]) == SketchOf(s, f["id"]), "草图：存进项目文件再打开还在");
        }
    }
}

namespace
{
    // 自动保存：autosave、recover 两条命令，工作线程按顺序写、删、找
    std::optional<Autosave::Notice> WaitNotice(Autosave::Worker& w)
    {
        for (int i = 0; i < 1000; ++i)
        {
            if (auto n = w.Poll())
                return n;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::nullopt;
    }

    bool Exists(const std::string& path)
    {
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
    }

    void TestAutosave()
    {
        using namespace Autosave;
        const std::string project = Temp("saved.minibim");
        const std::string backup  = BackupPath(project);
        Check(backup == project + ".autosave.minibim", "自动保存：副本在项目名后面接一段");
        {
            Session s;
            House(s);
            Run(s, "save", Json{ { "path", project } });
            Check(Contains(Refused(s, "autosave", Json{ { "path", project } }), "must differ"), "自动保存：副本不能写在项目文件上");
            const bool wrote = !Run(s, "autosave", Json{ { "path", backup } }).is_null();
            const Json snap = s.GetModel().ToJson();
            Check(wrote && s.Path() == project && s.CanUndo(), "自动保存：写副本不改项目的路径与撤销历史");
            Run(s, "wall", R"({"start": [0, 8000], "end": [4000, 8000]})");

            Session t;
            const Json r = Run(t, "recover", Json{ { "path", backup }, { "project", project } });
            Check(!r.is_null() && t.Path() == project && t.GetModel().ToJson() == snap, "自动保存：恢复出副本那一刻的模型，之后保存存回项目");
            Session u;
            Run(u, "recover", Json{ { "path", backup } });
            Check(!u.Path().has_value(), "自动保存：没给项目时恢复的是没保存过的项目");
            const Json before = t.GetModel().ToJson();
            Check(!Refused(t, "recover", Json{ { "path", backup }, { "project", 5 } }).empty() && t.GetModel().ToJson() == before &&
                      t.Path() == project, "自动保存：参数不对时什么都不恢复");
        }

        const std::string dir = Temp("autosave-dir");
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(std::u8string(dir.begin(), dir.end())), ec);
        {
            std::atomic<int> woke = 0;
            Worker w(dir, [&] { ++woke; });
            Session s;
            House(s);
            // 和找到的副本一样用 path 拼（Windows 上是反斜杠）
            const std::filesystem::path up = std::filesystem::path(std::u8string(dir.begin(), dir.end())) / ("untitled-1" + std::string(Suffix));
            const std::u8string u8 = up.u8string();
            const std::string untitled(u8.begin(), u8.end());
            const bool took = w.Save(untitled, s.Snapshot());
            const bool refused = !w.Save(untitled, s.Snapshot());
            const auto n = WaitNotice(w);
            const auto* saved = n ? std::get_if<Saved>(&*n) : nullptr;
            Check(took && refused && saved && saved->ok && saved->snapshot == s.Snapshot() && Exists(untitled) && !w.Busy() && woke >= 1,
                  "自动保存：工作线程写快照（建临时文件夹），上一份没写完时不接下一份");

            w.Discover(std::nullopt, 7);
            const auto d = WaitNotice(w);
            const auto* found = d ? std::get_if<Discovered>(&*d) : nullptr;
            Check(found && found->generation == 7 && found->found == std::vector<Backup>{ { untitled, std::nullopt } },
                  "自动保存：在临时文件夹里找到没保存过的项目的副本");
            w.Discover(project, 8);
            const auto e = WaitNotice(w);
            const auto* near = e ? std::get_if<Discovered>(&*e) : nullptr;
            Check(near && near->found == std::vector<Backup>{ { backup, project } }, "自动保存：项目旁边的副本");

            w.Remove(untitled);
            w.Remove(dir + "/missing" + Suffix);
            const auto r1 = WaitNotice(w), r2 = WaitNotice(w);
            const auto* a = r1 ? std::get_if<Removed>(&*r1) : nullptr;
            const auto* b = r2 ? std::get_if<Removed>(&*r2) : nullptr;
            w.Discover(std::nullopt, 9);
            const auto f = WaitNotice(w);
            const auto* none = f ? std::get_if<Discovered>(&*f) : nullptr;
            Check(a && a->ok && b && b->ok && !Exists(untitled) && none && none->found.empty(), "自动保存：删掉副本，不在的也算删了");
            // 析构时做完排着的事：最后排的删除不丢
            w.Save(untitled, s.Snapshot());
            w.Remove(untitled);
        }
        Check(!Exists(dir + "/untitled-1" + Suffix), "自动保存：关掉时做完排着的写与删");
        std::filesystem::remove_all(std::filesystem::path(std::u8string(dir.begin(), dir.end())), ec);
        std::filesystem::remove(std::filesystem::path(std::u8string(project.begin(), project.end())), ec);
        std::filesystem::remove(std::filesystem::path(std::u8string(backup.begin(), backup.end())), ec);
    }
}

int main()
{
    // 不缓冲：崩溃时也看得到崩在哪一项之后
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestWallsAndOpenings();
    TestHistory();
    TestFloors();
    TestEditing();
    TestFiles();
    TestRoofsRoomsCeilings();
    TestFamiliesAndComponents();
    TestPlan();
    TestSample();
    TestModifyTools();
    TestTypeEntries();
    TestSketches();
    TestAutosave();
    TestHostileParams();
    std::printf("\n%s：%d 项失败\n", g_failed == 0 ? "全部通过" : "有失败", g_failed);
    return g_failed == 0 ? 0 : 1;
}
