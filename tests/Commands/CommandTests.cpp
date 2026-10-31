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
#include "Model/Snap.h"
#include "Model/Walls.h"
#include <algorithm>
#include <array>
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
            // BIMCraft 的项目文件：还没移植的东西（阶段、栏杆类型、楼梯、墙的项目参数）略去并列出，柱类型读进来，其余照常打开，
            // 贴着屋顶的墙仍贴着，带族的门照常
            Session s;
            House(s);
            Run(s, "door", R"({"at": [2000, 0]})");
            std::string text, error;
            s.GetModel().ToFile(text, error);
            Json file = Json::parse(text);
            file["phases"] = Json::array({ "Existing", "New Construction" });
            file["types"]["column"] = Json::parse(R"({"Square 500": {"width": 500, "depth": 500}})");
            file["types"]["railing"] = Json::parse(R"({"Handrail 900": {"height": 900}})");
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
            const bool listed = !has("/families") && has("/phases") && !has("/types/column") && has("/types/railing") && has("/materials/Brick/texture") && !has("/elements/100100") &&
                                !has("/elements/100101") && has("/elements/100102") && !has("/elements/100003/attach_top") &&
                                has("/elements/100003/params/Fire Rating");
            std::string why;
            const bool opened = !r.is_null() && t.GetModel().Check(why) && Count(t, "wall") == 4 && Count(t, "door") == 2 && Count(t, "roof") == 1 &&
                                t.GetModel().GetWall(Id(100003))->attachTop && !t.GetModel().library.IdOf(Kind::Of(Kind::Tag::Phase), "Existing") &&
                                t.GetModel().types.column.count("Square 500") == 1;
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
            ok = ok && Contains(Refused(s, "edit_boundary", Json{ { "id", w }, { "sketch", WithHole() } }), "floor, a ceiling, a roof's footprint or a filled region");
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
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
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

    // ── 同步 BIMCraft 0733fd4：文件格式 11、房间、材质透明度 ──────────────
    // 移植自 BIMCraft room_area_tests.rs 与 tests.rs 的
    // a_separator_drawn_to_the_wall_faces_splits_rooms_measured_at_the_centre_lines_too（0733fd4）
    void TestBimcraft0733()
    {
        // 所有房间的面积，从小到大
        auto areas = [](Session& s)
        {
            std::vector<double> out;
            const Json rooms = Run(s, "elements", R"({"category": "room"})");
            for (const Json& r : rooms["elements"])
                if (r.contains("area_m2") && r["area_m2"].is_number())
                    out.push_back(r["area_m2"].get<double>());
            std::sort(out.begin(), out.end());
            return out;
        };
        auto near = [](double got, double want) { return std::abs(got - want) <= 0.006; };
        auto wall = [](Session& s, const char* points, bool closed)
        {
            Run(s, "wall", Json{ { "points", Json::parse(points) }, { "closed", closed }, { "type", "Exterior - 300mm" } });
        };
        {
            // BIMCraft 第 11 版的文件：法语项目（还没移植）、尺寸类型、尺寸标注、透明的材质
            Session s;
            House(s);
            Json file = s.GetModel().ToJson();
            file["format"]          = 11;
            file["language"]        = "fr";
            file["dimension_types"] = Json::object({ { "Linear", Json::object() } });
            file["materials"]["Glass"]["transparency"] = 70;
            Model m;
            std::vector<std::string> left;
            std::string error;
            const bool read = Model::Read(file.dump(), m, left, error);
            auto has = [&](const char* p) { return std::find(left.begin(), left.end(), p) != left.end(); };
            Check(read && m.language == Language::English && has("/language") && has("/dimension_types") && m.materials.at("Glass").transparency == 70 &&
                      m.ToJson()["format"] == 11,
                  "文件：BIMCraft 第 11 版的文件能打开，还没移植的语言与尺寸类型略去并列出 " + error);
        }
        {
            Session s;
            House(s);
            const double half = Run(s, "elements", R"({"category": "wall"})")["elements"][0]["thickness"].get<double>() / 2.0;
            // 像在平面上那样从墙面画到墙面，再斜着从墙面画一条到第一条线
            Run(s, "room_separator", Json{ { "points", { { 5000, half }, { 5000, 6000.0 - half } } } });
            Run(s, "room_separator", Json{ { "points", { { 8000.0 - half, 4000.0 - half / 3.0 }, { 5000, 3000 } } } });
            Run(s, "room", R"({"point": [2000, 3000], "name": "Living"})");
            Run(s, "room", R"({"point": [7000, 1000], "name": "Kitchen"})");
            Run(s, "room", R"({"point": [7000, 5500], "name": "Pantry"})");
            // 在中心线：5 m × 6 m，以及剩下的 3 m × 6 m 被 (8000, 4000) 到 (5000, 3000) 的斜线切开
            const std::vector<double> centre = areas(s);
            bool ok = centre.size() == 3 && near(centre[0], 3.0 * 2.5) && near(centre[1], 3.0 * 3.5) && near(centre[2], 30.0);
            // 线本身照画的样子
            ok = ok && Run(s, "elements", R"({"category": "roomseparator"})")["elements"][0]["length"] == 6000.0 - 2.0 * half;
            // 在墙面量时一样分开，加起来是墙以内的地面
            Run(s, "room_area", R"({"at": "finish"})");
            const std::vector<double> inside = areas(s);
            double sum = 0.0;
            for (const double a : inside)
                sum += a;
            Check(ok && std::abs(sum - (8000.0 - 2.0 * half) * (6000.0 - 2.0 * half) / 1e6) < 0.02,
                  "房间：画到墙面的分隔线在墙中心线量时也把房间分开");
        }
        {
            // 4000 × 3000 的一圈 300 墙，里面一个房间
            Session s;
            wall(s, "[[0, 0], [4000, 0], [4000, 3000], [0, 3000]]", true);
            Run(s, "room", R"({"point": [2000, 1500], "name": "TEST"})");
            bool ok = Run(s, "inspect", Json::object())["room_area"] == "centre" && near(areas(s)[0], 12.0);
            Run(s, "room_area", R"({"at": "finish"})");
            ok = ok && Run(s, "inspect", Json::object())["room_area"] == "finish" && near(areas(s)[0], 9.99);
            // 贴着南墙内面 20 mm 的仍是房间；在墙体里 10 mm 的不是
            ok = ok && near(Run(s, "room", R"({"point": [2000, 170]})")["area_m2"].get<double>(), 9.99) &&
                 Contains(Refused(s, "room", R"({"point": [2000, 140]})"), "inside wall");
            for (const char* bad : { R"({"at": "core"})", R"({"at": 5})", R"({"at": [0, 0]})" })
            {
                const std::string e = Refused(s, "room_area", bad);
                ok = ok && Contains(e, "centre") && Contains(e, "finish");
            }
            Check(ok, "房间：在墙面量时房间不能在墙体里，inspect 报出量法");
        }
        {
            // 单独一面墙：两种量法都不出房间，在墙面量时说在墙里
            Session s;
            Run(s, "wall", R"({"points": [[0, 0], [4000, 0]], "type": "Exterior - 300mm"})");
            bool ok = true;
            for (const char* at : { "centre", "finish" })
            {
                Run(s, "room_area", Json{ { "at", at } });
                for (const char* p : { R"({"point": [2000, 0]})", R"({"point": [2000, 100]})" })
                {
                    const std::string e = Refused(s, "room", p);
                    ok = ok && !e.empty() && (std::string(at) == "centre" || Contains(e, "inside wall")) && areas(s).empty();
                }
            }
            Check(ok, "房间：墙体里的点不成房间");
        }
        {
            // L 形：中心线上 18 m²，墙以内 5700 × 1700 + 1700 × 3000
            Session s;
            wall(s, "[[0, 0], [6000, 0], [6000, 2000], [2000, 2000], [2000, 5000], [0, 5000]]", true);
            Run(s, "room", R"({"point": [1000, 1000]})");
            bool ok = near(areas(s)[0], 18.0);
            Run(s, "room_area", R"({"at": "finish"})");
            ok = ok && near(areas(s)[0], 14.79);
            // 一圈墙里的岛：在墙面量时扣去岛的外表面，岛自己里面的空也是一个房间
            Session t;
            wall(t, "[[0, 0], [6000, 0], [6000, 5000], [0, 5000]]", true);
            wall(t, "[[2500, 2000], [3500, 2000], [3500, 3000], [2500, 3000]]", true);
            Run(t, "room", R"({"point": [1000, 1000]})");
            ok = ok && near(areas(t)[0], 29.0);
            Run(t, "room_area", R"({"at": "finish"})");
            ok = ok && near(areas(t)[0], 25.1);
            const Json island = Run(t, "room", R"({"point": [3000, 2500]})");
            ok = ok && !island.is_null() && near(island["area_m2"].get<double>(), 0.49);
            Check(ok, "房间：L 形与墙岛在墙面量");
        }
        {
            // 十字交叉的墙围出四个房间；交叉处是墙
            Session s;
            wall(s, "[[0, 0], [6000, 0], [6000, 4000], [0, 4000]]", true);
            wall(s, "[[3000, 0], [3000, 4000]]", false);
            wall(s, "[[0, 2000], [6000, 2000]]", false);
            for (const char* p : { R"({"point": [1500, 1000]})", R"({"point": [4500, 1000]})", R"({"point": [1500, 3000]})", R"({"point": [4500, 3000]})" })
                Run(s, "room", p);
            bool ok = areas(s) == std::vector<double>(4, 6.0);
            Run(s, "room_area", R"({"at": "finish"})");
            const std::vector<double> got = areas(s);
            ok = ok && got.size() == 4;
            for (const double a : got)
                ok = ok && near(a, 4.59);
            ok = ok && Contains(Refused(s, "room", R"({"point": [3000, 2000]})"), "inside wall");
            // 墙的定位线在哪都一样：墙以内的面积相同
            auto finish = [&](const char* location, const char* points)
            {
                Session u;
                Run(u, "wall", Json{ { "points", Json::parse(points) }, { "closed", true }, { "type", "Exterior - 300mm" }, { "location", location } });
                Run(u, "room_area", R"({"at": "finish"})");
                Run(u, "room", R"({"point": [2000, 1500]})");
                const std::vector<double> a = areas(u);
                return a.empty() ? -1.0 : a[0];
            };
            const double centre = finish("centre", "[[0, 0], [4000, 0], [4000, 3000], [0, 3000]]");
            ok = ok && near(centre, 9.99) && near(finish("exterior-finish", "[[-150, -150], [-150, 3150], [4150, 3150], [4150, -150]]"), centre) &&
                 near(finish("interior-finish", "[[150, 150], [150, 2850], [3850, 2850], [3850, 150]]"), centre);
            Check(ok, "房间：十字墙的四个房间在墙面量，与墙的定位线无关");
        }
        {
            // 材质的透明度，百分数：留着、清掉、超过 100 拒绝
            Session s;
            bool ok = Run(s, "material", R"({"name": "Oak", "transparency": 40})")["material"]["transparency"] == 40 &&
                      s.GetModel().materials.at("Oak").transparency == 40;
            ok = ok && Run(s, "material", R"({"name": "Oak", "transparency": 0})")["material"].count("transparency") == 0;
            for (const char* bad : { "101", "-1", "2.5", "\"x\"", "1e300" })
                ok = ok && Contains(Refused(s, "material", Json{ { "name", "Oak" }, { "transparency", Json::parse(bad) } }), "transparency");
            ok = ok && s.GetModel().materials.at("Oak").transparency == 0 && s.GetModel().materials.at("Glass").transparency == GlassTransparency;
            // 随项目保存；第 8 版以前没有它的文件读成不透明
            Json file = s.GetModel().ToJson();
            ok = ok && file["materials"]["Glass"]["transparency"] == GlassTransparency && file["materials"]["Oak"].count("transparency") == 0;
            Model m;
            std::vector<std::string> left;
            std::string error;
            file["materials"]["Glass"]["transparency"] = 101;
            ok = ok && Model::Read(file.dump(), m, left, error) && !m.Check(error) && Contains(error, "transparency");
            Check(ok, "材质：透明度 0 到 100，新项目的玻璃是 70");
        }
    }
    // 移植自 BIMCraft tests.rs 的 columns_stand_from_their_level_to_the_next_and_turn_with_rotate 与 structure_tests.rs 的柱
    void TestColumns()
    {
        {
            Session s;
            House(s);
            const Id c = Run(s, "CL", R"({"at": [4000, 3000]})")["id"].get<Id>();
            Json e = ElementOf(s, c);
            Check(e["type"] == "Rectangular 400 x 400" && e["height"] == 3000.0 && e["width"] == 400.0 && IdOf(s, e["top_level"]).has_value(),
                  "柱：默认从所在标高立到上一个标高");
            const Id round = Run(s, "column", R"({"at": [2000, 3000], "type": "Round 300", "height": 2500})")["id"].get<Id>();
            e = ElementOf(s, round);
            Check(e["height"] == 2500.0 && e["top_level"].is_null() && e["round"] == true && e["depth"] == 300.0, "柱：给了 height 时不到上一个标高");
            Run(s, "rotate", Json{ { "ids", { c } }, { "about", Pt(4000, 3000) }, { "angle", 45 } });
            Check(NearNum(ElementOf(s, c)["angle"], 45.0, 1e-6), "柱：随旋转转过去");
            Run(s, "type", R"({"category": "column", "name": "Steel 200 x 300", "width": 200, "depth": 300})");
            Run(s, "modify", Json{ { "id", c }, { "type", "Steel 200 x 300" } });
            Check(ElementOf(s, c)["depth"] == 300.0, "柱：新建的类型、换类型");
            Check(Contains(Refused(s, "column", R"({"at": [0, 0], "type": "Marble"})"), "Marble"), "柱：没有的类型报出名称");
            Check(Contains(Refused(s, "column", R"({"at": [0, 0], "base_offset": 3000, "height": -1})"), "height"), "柱：高度要大于 0");
            Run(s, "type", R"({"category": "column", "name": "Drum 500", "width": 500, "depth": 200, "round": true})");
            Check(s.GetModel().types.column.at("Drum 500").depth == 500.0, "柱：圆柱的进深即直径");
        }
        {
            Session s;
            const Id c = Run(s, "structural_column", R"({"at": [1000, 2000]})")["id"].get<Id>();
            const Json e = ElementOf(s, c);
            Check(e["category"] == "structuralcolumn" && e["type"] == "Concrete 300 x 300" && e["height"] == 3000.0 && e["width"] == 300.0 &&
                      e["depth"] == 300.0,
                  "结构柱：自己的类别与类型");
            const std::string wrong = Refused(s, "structural_column", R"({"at": [0, 0], "type": "Rectangular 400 x 400"})");
            Check(Contains(wrong, "Concrete 300 x 300"), "结构柱：建筑柱的类型不是它的，报错列出它自己的：" + wrong);
            Check(Contains(Refused(s, "column", R"({"at": [0, 0], "type": "Concrete 300 x 300"})"), "Rectangular 400 x 400"), "柱：结构柱的类型也不是它的");
            Check(Count(s, "structural columns") == 1 && Count(s, "structural_column") == 1 && Count(s, "columns") == 0, "结构柱：按自己的类别数，怎么写都行");
            const Json r = Run(s, "CL", R"({"at": [9000, 0]})");
            Check(!r.is_null() && ElementOf(s, r["id"].get<Id>())["category"] == "column", "柱：CL 是建筑柱的（同 Revit）");
        }
        {
            Session s;
            const Id c = Run(s, "structural_column", R"({"at": [0, 0], "angle": 0})")["id"].get<Id>();
            Run(s, "move", Json{ { "ids", { c } }, { "by", Pt(1000, 500) } });
            Check(ElementOf(s, c)["at"] == Pt(1000, 500), "结构柱：移动");
            Run(s, "rotate", Json{ { "ids", { c } }, { "about", Pt(1000, 500) }, { "angle", 90 } });
            Check(ElementOf(s, c)["angle"] == 90.0, "结构柱：旋转");
            const Json m = Run(s, "mirror", Json{ { "ids", { c } }, { "axis", { Pt(0, 0), Pt(0, 1000) } }, { "copy", true } });
            const Json mirrored = m.is_null() ? Json() : ElementOf(s, m["ids"][0].get<Id>());
            Check(!mirrored.is_null() && NearNum(mirrored["at"][0], -1000.0, 1e-6) && NearNum(mirrored["at"][1], 500.0, 1e-6), "结构柱：镜像复制");
            Run(s, "copy", Json{ { "ids", { c } }, { "by", Pt(0, -2000) } });
            Check(Count(s, "structuralcolumn") == 3, "结构柱：复制");
            Run(s, "delete", Json{ { "ids", { c } } });
            Check(Count(s, "structuralcolumn") == 2, "结构柱：删除");
            Run(s, "undo", Json::object());
            Check(Count(s, "structuralcolumn") == 3, "结构柱：撤销删除");
        }
        {
            // 存了再打开一模一样；没有结构柱类型的旧文件也能打开
            Session s;
            Run(s, "column", R"({"at": [0, 0], "type": "Round 300"})");
            Run(s, "structural_column", R"({"at": [1000, 0], "height": 2000, "base_offset": -500})");
            const std::string path = Temp("columns.minibim");
            Run(s, "save", Json{ { "path", path } });
            Session t;
            Run(t, "open", Json{ { "path", path } });
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
            Check(t.GetModel().ToJson() == s.GetModel().ToJson(), "柱：存了再打开一模一样");
            Json old = Session().GetModel().ToJson();
            old["types"].erase("structural_column");
            Model m;
            std::vector<std::string> left;
            std::string error;
            Check(Model::Read(old.dump(), m, left, error) && m.types.structuralColumn.empty() && m.Check(error), "柱：没有结构柱类型的旧文件能打开");
        }
        {
            // 移植自 BIMCraft render.rs 的 plans_cut_columns_draw_stairs_and_show_roofs_from_both_sides 的柱：剖到的柱填色，拾取得到
            Session s;
            House(s);
            const Id c = Run(s, "column", R"({"at": [6000, 4000]})")["id"].get<Id>();
            const Id r = Run(s, "column", R"({"at": [2000, 4000], "type": "Round 300"})")["id"].get<Id>();
            const Model& m = s.GetModel();
            const Render::Drawing d = Render::Plan(m, m.GuidOf(Id(100000)), 1200, 900);
            const auto filled = [&](Id id)
            {
                for (size_t i = 0; i < d.prims.size(); ++i)
                    if (d.owners[i] == std::optional<Id>(id) && d.prims[i].kind == Render::Prim::Kind::Fill && d.prims[i].color == Render::Poche)
                        return true;
                return false;
            };
            Check(filled(c) && filled(r), "平面图：剖到的柱填色");
            Check(d.Pick(d.view.ToPx(P2(6200, 4000)), 4.0f) == c && d.Pick(d.view.ToPx(P2(2150, 4000)), 4.0f) == r, "平面图：拾取柱的边");
            // 上一层的平面里它在剖切面以下：只有轮廓
            const Render::Drawing up = Render::Plan(m, m.GuidOf(Id(100001)), 1200, 900);
            bool line = false, fill = false;
            for (size_t i = 0; i < up.prims.size(); ++i)
                if (up.owners[i] == std::optional<Id>(c))
                    (up.prims[i].kind == Render::Prim::Kind::Fill ? fill : line) = true;
            Check(!fill, "平面图：上一层不剖到柱");
            (void)line;
        }
    }

    // 移植自 BIMCraft structure_tests.rs（f3bbb33）：结构选项卡的梁、结构柱、独立基础、条形基础与结构楼板
    std::vector<Id> IdsOf(const Json& r)
    {
        std::vector<Id> ids;
        if (r.is_object() && r.contains("ids"))
            for (const Json& v : r["ids"])
                ids.push_back(v.get<Id>());
        return ids;
    }

    // 应当失败，且模型不变：返回原因
    std::string Fails(Session& s, const char* id, const Json& p)
    {
        const Json before = s.GetModel().ToJson();
        const std::string why = Refused(s, id, p);
        Check(!why.empty() && s.GetModel().ToJson() == before, std::string("结构：被拒的 ") + id + " 不改模型（" + p.dump().substr(0, 60) + "）");
        return why;
    }

    Json Axis(std::initializer_list<std::array<double, 3>> pts)
    {
        Json a = Json::array();
        for (const auto& p : pts)
            a.push_back(Json::array({ p[0], p[1], p[2] }));
        return a;
    }

    void TestStructure()
    {
        {
            Session s;
            const auto beams = IdsOf(Run(s, "beam", R"({"points": [[0, 0], [6000, 0], [6000, 4000]]})"));
            Check(beams.size() == 2, "梁：每段一根");
            const Json b = beams.empty() ? Json() : ElementOf(s, beams[0]);
            Check(b["category"] == "beam" && b["type"] == "Beam 200 x 500" && b["justify"] == "top", "梁：类别、默认类型、顶对齐");
            Check(b["length"] == 6000.0 && b["width"] == 200.0 && b["depth"] == 500.0, "梁：长度与截面");
            Check(b["axis"] == Axis({ { 0, 0, -250 }, { 6000, 0, -250 } }), "梁：顶在标高上，中心线低半个高度：" + b["axis"].dump());
            Check(beams.size() == 2 && ElementOf(s, beams[1])["length"] == 4000.0, "梁：第二段");
            Check(IdsOf(Run(s, "BM", R"({"points": [[0, 0], [1000, 0], [1000, 1000]], "closed": true})")).size() == 3, "梁：BM，闭合成一圈");
            Check(Count(s, "beams") == 5, "梁：按类别数");
        }
        {
            Session s;
            const auto r = IdsOf(Run(s, "beam",
                                     R"({"points": [[0, 0], [3000, 0]], "offset": 2800, "end_offset": 3400, "justify": "centre", "system": "Roof frame", "level": "Level 1"})"));
            const Json b = r.empty() ? Json() : ElementOf(s, r[0]);
            Check(b["axis"] == Axis({ { 0, 0, 2800 }, { 3000, 0, 3400 } }), "梁：两端的高度，坡起来");
            Check(b["system"] == "Roof frame" && b["justify"] == "centre", "梁：所属系统与对齐");
            const auto c = IdsOf(Run(s, "beam", R"({"points": [[0, 1000], [3000, 1000]], "justify": "center"})"));
            if (!c.empty())
            {
                Run(s, "modify", Json{ { "id", c[0] }, { "offset", 500 } });
                Check(ElementOf(s, c[0])["axis"] == Axis({ { 0, 1000, 500 }, { 3000, 1000, 500 } }), "梁：center 也认，modify 改高度");
            }
        }
        {
            Session s;
            const Json line = Json::array({ Pt(0, 0), Pt(3000, 0) });
            Check(Contains(Fails(s, "beam", Json{ { "points", { Pt(0, 0), Pt(0, 0) } } }), "too short"), "梁：太短");
            Check(Contains(Fails(s, "beam", Json{ { "points", { Pt(0, 0) } } }), "at least 2"), "梁：至少两点");
            Check(Contains(Fails(s, "beam", Json{ { "points", line }, { "type", "Nope" } }), "type `Nope` doesn't exist"), "梁：没有的类型");
            Check(Contains(Fails(s, "beam", Json{ { "points", line }, { "justify", "sideways" } }), "justify"), "梁：对齐写错");
            Check(Contains(Fails(s, "beam", Json{ { "points", line }, { "offset", 1e300 } }), "offset"), "梁：高度太大");
            Check(Contains(Fails(s, "beam", Json{ { "points", line }, { "offset", "high" } }), "offset"), "梁：高度不是数");
            Check(Contains(Fails(s, "beam", Json{ { "points", line }, { "system", std::string(300, 'x') } }), "system"), "梁：系统名太长");
            Check(Contains(Fails(s, "beam", Json{ { "points", line }, { "level", "Nowhere" } }), "Nowhere"), "梁：没有的标高");
            const std::string steep = Fails(s, "beam", Json{ { "points", { Pt(0, 0), Pt(100, 0) } }, { "offset", 0 }, { "end_offset", 5000 } });
            Check(Contains(steep, "slopes") && Contains(steep, "column"), "梁：比 75° 陡的是柱：" + steep);
            Check(Contains(Fails(s, "beam", Json{ { "points", { Pt(0, 0), Pt(1000, 0), Pt(2000, 0) } }, { "end_offset", 100 } }), "end_offset"),
                  "梁：end_offset 只给一段");
        }
        {
            Session s;
            const Json made = Run(s, "type", R"({"category": "beam", "name": "IPE 200", "width": 100, "depth": 200, "material": "Metal"})");
            Check(made["type"] == Json::parse(R"({"width": 100.0, "depth": 200.0, "round": false, "material": "Metal"})"), "run 类型：新建：" + made.dump());
            const Json tube = Run(s, "type", R"({"category": "beams", "name": "Tube 100", "width": 100, "round": true})");
            Check(tube["type"]["depth"] == 100.0 && tube["type"]["round"] == true, "run 类型：圆的进深即宽度");
            const auto id = IdsOf(Run(s, "beam", R"({"points": [[0, 0], [3000, 0]], "type": "IPE 200"})"));
            Run(s, "type", R"({"category": "beam", "name": "IPE 200", "width": 150})");
            const Json b = id.empty() ? Json() : ElementOf(s, id[0]);
            Check(b["width"] == 150.0 && b["depth"] == 200.0, "run 类型：改了处处跟着变，没说的不变");
            Check(Contains(Fails(s, "type", Json::parse(R"({"category": "beam", "name": "Odd", "width": 100, "depth": 200, "material": "Unobtainium"})")), "library"),
                  "run 类型：材质要在库里");
            Check(Contains(Fails(s, "type", Json::parse(R"({"category": "beam", "name": "Flat", "width": 100})")), "depth"), "run 类型：要 depth");
            Check(Contains(Fails(s, "type", Json::parse(R"({"category": "beam", "name": "Zero", "width": 0, "depth": 10})")), "above zero"), "run 类型：尺寸大于 0");
            const std::string e = Fails(s, "type", Json::parse(R"({"category": "duct", "name": "P", "width": 10})"));
            Check(Contains(e, "beam"), "run 类型：报错列出有类型的类别：" + e);
            Run(s, "type", R"({"category": "stripfooting", "name": "Strip 800 x 300", "width": 800, "depth": 300})");
            Run(s, "type", R"({"category": "structural column", "name": "C 250 x 600", "width": 250, "depth": 600})");
            Run(s, "type", R"({"category": "footing", "name": "F 2000", "width": 2000, "depth": 2000})");
            const Model& m = s.GetModel();
            Check(m.types.structuralColumn.count("C 250 x 600") && !m.types.column.count("C 250 x 600") && m.types.footing.count("F 2000") &&
                      m.types.runs.count("stripfooting") && m.types.runs.at("stripfooting").count("Strip 800 x 300"),
                  "类型：条形基础、结构柱、独立基础各有各的");
        }
        {
            Session s;
            const Id f = Run(s, "footing", R"({"at": [1000, 2000]})")["id"].get<Id>();
            Json e = ElementOf(s, f);
            Check(e["category"] == "footing" && e["type"] == "Footing 1200 x 1200" && e["height"] == 400.0 && e["base_offset"] == -400.0,
                  "独立基础：400 厚，顶在标高上：" + e.dump());
            const Id g = Run(s, "footing", R"({"at": [5000, 2000], "thickness": 600, "offset": -200, "angle": 30})")["id"].get<Id>();
            e = ElementOf(s, g);
            Check(e["height"] == 600.0 && e["base_offset"] == -800.0 && e["angle"] == 30.0, "独立基础：厚度、顶的高度、角度");
            Check(Contains(Fails(s, "footing", Json::parse(R"({"at": [0, 0], "thickness": 0})")), "thickness"), "独立基础：厚度为 0");
            Check(Contains(Fails(s, "footing", Json::parse(R"({"at": [0, 0], "thickness": -5})")), "thickness"), "独立基础：厚度为负");
            Check(Count(s, "footings") == 2 && Count(s, "columns") == 0, "独立基础：按自己的类别数");
        }
        {
            Session s;
            const auto r = IdsOf(Run(s, "strip_footing", R"({"points": [[0, 0], [5000, 0]]})"));
            const Json e = r.empty() ? Json() : ElementOf(s, r[0]);
            Check(e["category"] == "stripfooting" && e["type"] == "Strip 600 x 300" && e["justify"] == "top", "条形基础：类别、默认类型、顶对齐");
            Check(e["axis"] == Axis({ { 0, 0, -150 }, { 5000, 0, -150 } }), "条形基础：顶在标高上");
            Check(Contains(Fails(s, "strip_footing", Json::parse(R"({"points": [[0, 0], [5000, 0]], "type": "Beam 200 x 500"})")), "Strip 600 x 300"),
                  "条形基础：梁的类型不是它的");
        }
        {
            Session s;
            const Id beam = IdsOf(Run(s, "beam", R"({"points": [[0, 0], [4000, 0]], "offset": 2800, "end_offset": 3000})")).at(0);
            const Id column = Run(s, "structural_column", R"({"at": [0, 0], "angle": 0})")["id"].get<Id>();
            const Id footing = Run(s, "footing", R"({"at": [0, 0]})")["id"].get<Id>();
            const auto axis = [&] { return ElementOf(s, beam)["axis"]; };
            const Json z0 = axis()[0][2], z1 = axis()[1][2];
            Run(s, "move", Json{ { "ids", { beam, column, footing } }, { "by", Pt(1000, 500) } });
            Check(axis()[0] == Json::array({ 1000.0, 500.0, z0 }), "结构：移动，高度不变");
            Check(ElementOf(s, column)["at"] == Pt(1000, 500) && ElementOf(s, footing)["at"] == Pt(1000, 500), "结构：柱与基础随着移动");
            Run(s, "rotate", Json{ { "ids", { beam, column } }, { "about", Pt(1000, 500) }, { "angle", 90 } });
            const Json a = axis();
            Check(NearNum(a[1][0], 1000.0, 1e-6) && NearNum(a[1][1], 4500.0, 1e-6) && ElementOf(s, column)["angle"] == 90.0, "结构：旋转：" + a.dump());
            const auto copy = IdsOf(Run(s, "mirror", Json{ { "ids", { beam } }, { "axis", { Pt(0, 0), Pt(0, 1000) } }, { "copy", true } }));
            const Json mirrored = copy.empty() ? Json() : ElementOf(s, copy[0])["axis"];
            Check(!copy.empty() && mirrored[0][2] == z0 && mirrored[1][2] == z1 && NearNum(mirrored[0][0], -1000.0, 1e-6), "结构：镜像，各端的高度跟着各端");
            Run(s, "copy", Json{ { "ids", { beam, column, footing } }, { "by", Pt(0, -2000) } });
            Check(Count(s, "beam") == 3, "结构：复制");
            Run(s, "delete", Json{ { "ids", { beam, column, footing } } });
            Check(Count(s, "footing") == 1, "结构：删除");
            Run(s, "undo", Json::object());
            Check(Count(s, "footing") == 2, "结构：撤销删除");
        }
        {
            Session s;
            Run(s, "beam", R"({"points": [[0, 0], [4000, 0]], "system": "Level 2 frame"})");
            Run(s, "structural_column", R"({"at": [0, 0]})");
            Run(s, "footing", R"({"at": [0, 0]})");
            Run(s, "strip_footing", R"({"points": [[0, 0], [4000, 0]]})");
            Run(s, "pipe", R"({"points": [[0, 0], [4000, 0]], "offset": 2500, "end_offset": 2400, "justify": "bottom"})");
            Run(s, "floor", R"({"boundary": [[0, 0], [4000, 0], [4000, 3000], [0, 3000]], "structural": true})");
            const std::string path = Temp("structure.minibim");
            Run(s, "save", Json{ { "path", path } });
            Session t;
            Run(t, "open", Json{ { "path", path } });
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
            Check(t.GetModel().ToJson() == s.GetModel().ToJson(), "结构：存了再打开一模一样");
            Json old = Session().GetModel().ToJson();
            for (const char* gone : { "runs", "structural_column", "footing" })
                old["types"].erase(gone);
            Model m;
            std::vector<std::string> left;
            std::string error;
            Check(Model::Read(old.dump(), m, left, error) && m.types.runs.empty() && m.Check(error), "结构：没有结构类型的旧文件能打开：" + error);
            // 圆而不如宽那么深的 run 类型的文件被拒
            Json bad = Session().GetModel().ToJson();
            bad["types"]["runs"]["beam"]["Bad"] = Json::parse(R"({"width": 100, "depth": 50, "round": true})");
            Model n;
            Check(!Model::Read(bad.dump(), n, left, error) || !n.Check(error), "结构：圆的 run 类型进深不等于宽度的文件被拒");
        }
        {
            // 移植自 BIMCraft structure_tests.rs 的平面图：看到挂在标高下的梁、剖切面以上的梁与标高下的基础
            Session s;
            const Id below = IdsOf(Run(s, "beam", R"({"points": [[0, 0], [4000, 0]]})")).at(0);
            const Id over  = IdsOf(Run(s, "beam", R"({"points": [[0, 2000], [4000, 2000]], "offset": 1800})")).at(0);
            const Id f     = Run(s, "footing", R"({"at": [2000, 4000]})")["id"].get<Id>();
            const Model& m = s.GetModel();
            const Render::Drawing d = Render::Plan(m, m.GuidOf(Id(100000)), 1200, 900);
            const auto owns = [&](Id id)
            {
                for (size_t i = 0; i < d.prims.size(); ++i)
                    if (d.owners[i] == std::optional<Id>(id))
                        return true;
                return false;
            };
            Check(owns(below) && owns(over) && owns(f), "平面图：标高下的梁、剖切面以上的梁、标高下的基础都画");
            Check(d.Pick(d.view.ToPx(P2(2000, 0)), 4.0f) == below, "平面图：拾取梁");
        }
    }
    // ── 卫浴、电气、防雷、数据（移植自 BIMCraft mep_tests.rs，f3bbb33）──────────
    // 每类 run 的命令、快捷名、默认类型、是否圆的
    struct MepCase
    {
        const char* category;
        const char* command;
        const char* alias;
        const char* defaultType;
        bool        round;
    };
    constexpr MepCase MepCases[] = {
        { "pipe", "pipe", "PI", "PVC 50", true },
        { "conduit", "conduit", "CN", "PVC 25", true },
        { "cabletray", "cable_tray", "CT", "Perforated 100 x 50", false },
        { "wire", "wire", "EW", "Wire 2.5", true },
        { "downconductor", "down_conductor", "DC", "Steel 8", true },
        { "groundconductor", "ground_conductor", "GC", "Copper 9", true },
        { "cable", "cabling", "CB", "Cat6", true },
    };

    // 起步的设备族（英文名）、类别，以及它的形体在基点之上的范围（毫米：最低点可在的区间，最高点可在的区间）
    struct MepFixture
    {
        const char*   family;
        Fam::Category category;
        double        base[2];
        double        top[2];
    };
    const MepFixture MepFixtures[] = {
        { "WC", Fam::Category::Plumbing, { 0, 0 }, { 700, 900 } },
        { "Washbasin", Fam::Category::Plumbing, { 0, 0 }, { 800, 900 } },
        { "Kitchen Sink", Fam::Category::Plumbing, { 0, 0 }, { 850, 1300 } },
        { "Shower", Fam::Category::Plumbing, { 0, 0 }, { 2000, 2200 } },
        { "Floor Drain", Fam::Category::Drain, { -100, 0 }, { 0, 0 } },
        { "Elbow 90", Fam::Category::PipeFitting, { -100, -10 }, { 10, 100 } },
        { "Tee", Fam::Category::PipeFitting, { -100, -10 }, { 10, 100 } },
        { "Gate Valve", Fam::Category::PipeAccessory, { -100, -10 }, { 40, 300 } },
        { "Distribution Board", Fam::Category::Equipment, { 1200, 1400 }, { 1700, 2300 } },
        { "Ceiling Luminaire", Fam::Category::Lighting, { 2900, 3000 }, { 2950, 3050 } },
        { "Recessed Luminaire", Fam::Category::Lighting, { 2900, 3000 }, { 2950, 3050 } },
        { "Outlet", Fam::Category::Outlet, { 200, 2100 }, { 300, 2200 } },
        { "Switch", Fam::Category::Switch, { 200, 1200 }, { 300, 1300 } },
        { "Air Terminal", Fam::Category::AirTerminal, { -1, 1 }, { 400, 2100 } },
        { "Earthing Electrode", Fam::Category::Earthing, { -3100, -2300 }, { 0, 0 } },
        { "Inspection Box", Fam::Category::InspectionBox, { -350, -200 }, { 0, 0 } },
        { "Data Outlet", Fam::Category::DataOutlet, { 200, 1200 }, { 300, 1300 } },
        { "Voice Outlet", Fam::Category::VoiceOutlet, { 200, 1200 }, { 300, 1300 } },
        { "Patch Panel", Fam::Category::PatchPanel, { 1400, 1600 }, { 1500, 1700 } },
        { "Rack", Fam::Category::Rack, { -1, 1 }, { 1900, 2100 } },
    };

    // 平面上 id 的各项（没给 id 时所有的项）占的范围，模型毫米，到十分之一
    std::array<double, 4> PlanExtent(const Render::Drawing& d, std::optional<Id> id)
    {
        double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        const auto add = [&](Render::Px p)
        {
            const P2 q = d.view.ToMm(p);
            x0 = std::min(x0, q.x);
            y0 = std::min(y0, q.y);
            x1 = std::max(x1, q.x);
            y1 = std::max(y1, q.y);
        };
        for (size_t i = 0; i < d.prims.size(); ++i)
        {
            if (id && d.owners[i] != id)
                continue;
            for (const auto& ring : d.prims[i].rings)
                for (const Render::Px p : ring)
                    add(p);
            for (const Render::Px p : d.prims[i].pts)
                add(p);
        }
        const auto tenth = [](double v) { return std::round(v * 10.0) / 10.0 + 0.0; };
        return { tenth(x0), tenth(y0), tenth(x1), tenth(y1) };
    }

    void TestMep()
    {
        for (const MepCase& c : MepCases)
        {
            const std::string of = std::string(c.category) + "：";
            {
                // 点与点之间一段一根，中心线在标高上
                Session s;
                const auto made = IdsOf(Run(s, c.command, R"({"points": [[0, 0], [6000, 0], [6000, 4000]]})"));
                Check(made.size() == 2, of + "每段一根");
                const Json e = made.empty() ? Json() : ElementOf(s, made[0]);
                Check(e["category"] == c.category && e["type"] == c.defaultType && e["justify"] == "centre" && e["round"] == c.round,
                      of + "类别、默认类型、居中、圆不圆：" + e.dump().substr(0, 200));
                Check(e["length"] == 6000.0 && e["axis"] == Axis({ { 0, 0, 0 }, { 6000, 0, 0 } }), of + "长度与轴");
                Check(made.size() == 2 && ElementOf(s, made[1])["length"] == 4000.0, of + "第二段");
                const auto ring = IdsOf(Run(s, c.alias, R"({"points": [[0, 0], [1000, 0], [1000, 1000]], "closed": true})"));
                Check(ring.size() == 3, of + "快捷名，闭合成一圈");
                Check(Count(s, (std::string(c.category) + "s").c_str()) == 5, of + "按复数的类别数");
            }
            {
                // 坡下去的排水管、所属系统；立管；modify 改高度与系统
                Session s;
                const auto r = IdsOf(Run(s, c.command,
                                         R"({"points": [[0, 0], [6000, 0]], "offset": 2400, "end_offset": 2340, "system": "Esgoto", "level": "Level 1"})"));
                const Json e = r.empty() ? Json() : ElementOf(s, r[0]);
                Check(e["axis"] == Axis({ { 0, 0, 2400 }, { 6000, 0, 2340 } }) && e["system"] == "Esgoto", of + "两端的高度与系统");
                const auto up = IdsOf(Run(s, c.command, R"({"points": [[500, 500], [500, 500]], "offset": 0, "end_offset": 3000})"));
                const Json u = up.empty() ? Json() : ElementOf(s, up[0]);
                Check(u["length"] == 3000.0 && u["axis"] == Axis({ { 500, 500, 0 }, { 500, 500, 3000 } }), of + "立管：同一点两个高度");
                if (!up.empty())
                {
                    Run(s, "modify", Json{ { "id", up[0] }, { "offset", 100 }, { "system", "\xC3\x81gua fria" } });
                    const Json m = ElementOf(s, up[0]);
                    Check(m["axis"][0][2] == 100.0 && m["system"] == "\xC3\x81gua fria", of + "modify 改高度与系统");
                }
            }
            {
                // 做不成的被拒，模型不变
                Session s;
                const Json line = Json::array({ Pt(0, 0), Pt(3000, 0) });
                Check(Contains(Fails(s, c.command, Json{ { "points", { Pt(0, 0), Pt(0, 0) } } }), "too short"), of + "太短");
                Check(Contains(Fails(s, c.command, Json{ { "points", { Pt(0, 0) } } }), "at least 2"), of + "至少两点");
                Check(Contains(Fails(s, c.command, Json{ { "points", line }, { "type", "Nope" } }), "type `Nope` doesn't exist"), of + "没有的类型");
                Check(Contains(Fails(s, c.command, Json{ { "points", line }, { "justify", "sideways" } }), "justify"), of + "对齐写错");
                Check(Contains(Fails(s, c.command, Json{ { "points", line }, { "offset", 1e300 } }), "offset"), of + "高度太大");
                Check(Contains(Fails(s, c.command, Json{ { "points", line }, { "system", std::string(300, 'x') } }), "system"), of + "系统名太长");
                Check(Contains(Fails(s, c.command, Json{ { "points", { Pt(0, 0), Pt(1000, 0), Pt(2000, 0) } }, { "end_offset", 100 } }), "end_offset"),
                      of + "end_offset 只给一段");
            }
            {
                // 类型：圆不圆、材质；改了处处跟着变
                Session s;
                const Json made = Run(s, "type", Json{ { "category", c.category }, { "name", "Mine" }, { "width", 75 }, { "depth", 75 },
                                                       { "round", c.round }, { "material", "Metal" } });
                Check(made["type"] == Json{ { "width", 75.0 }, { "depth", 75.0 }, { "round", c.round }, { "material", "Metal" } }, of + "新建类型");
                const auto id = IdsOf(Run(s, c.command, R"({"points": [[0, 0], [3000, 0]], "type": "Mine"})"));
                Run(s, "type", Json{ { "category", c.category }, { "name", "Mine" }, { "width", 90 } });
                Check(!id.empty() && ElementOf(s, id[0])["width"] == 90.0, of + "改类型，用它的跟着变");
                Check(Contains(Fails(s, "type", Json{ { "category", c.category }, { "name", "Odd" }, { "width", 50 }, { "depth", 50 },
                                                     { "round", c.round }, { "material", "Unobtainium" } }),
                               "library"),
                      of + "材质要在库里");
            }
            {
                // 移动、旋转、镜像、复制、删除
                Session s;
                const auto made = IdsOf(Run(s, c.command, R"({"points": [[0, 0], [4000, 0]], "offset": 2800, "end_offset": 3000})"));
                if (made.empty())
                    continue;
                const Id a = made[0];
                const auto axis = [&](Id id) { return ElementOf(s, id)["axis"]; };
                const Json z0 = axis(a)[0][2], z1 = axis(a)[1][2];
                Run(s, "move", Json{ { "ids", { a } }, { "by", Pt(1000, 500) } });
                Check(axis(a)[0] == Json::array({ 1000.0, 500.0, z0 }), of + "移动，高度不变");
                Run(s, "rotate", Json{ { "ids", { a } }, { "about", Pt(1000, 500) }, { "angle", 90 } });
                const Json end = axis(a)[1];
                Check(NearNum(end[0], 1000.0, 1e-6) && NearNum(end[1], 4500.0, 1e-6), of + "旋转：" + end.dump());
                const auto m = IdsOf(Run(s, "mirror", Json{ { "ids", { a } }, { "axis", { Pt(0, 0), Pt(0, 1000) } }, { "copy", true } }));
                const Json mirrored = m.empty() ? Json() : axis(m[0]);
                Check(!m.empty() && mirrored[0][2] == z0 && mirrored[1][2] == z1, of + "镜像，各端的高度跟着各端");
                Run(s, "copy", Json{ { "ids", { a } }, { "by", Pt(0, -2000) } });
                Check(Count(s, c.category) == 3, of + "复制");
                Run(s, "delete", Json{ { "ids", { a } } });
                Run(s, "undo", Json::object());
                Check(Count(s, c.category) == 3, of + "删除再撤销");
            }
            {
                // 平面：剖切面穿过的剖开，上面的虚线，视图范围以上的不画；预览一样画
                Session s;
                const Id cut  = IdsOf(Run(s, c.command, R"({"points": [[0, 0], [6000, 0]], "offset": 1200})")).at(0);
                const Id over = IdsOf(Run(s, c.command, R"({"points": [[0, 1000], [6000, 1000]], "offset": 2800})")).at(0);
                const Id high = IdsOf(Run(s, c.command, R"({"points": [[0, 4000], [6000, 4000]], "offset": 6000})")).at(0);
                const Model& m   = s.GetModel();
                const Guid level = m.GuidOf(Id(100000));
                const Render::View view{ 0.1, 3000.0, 0.0, 1000, 800 };
                const Render::Drawing d = Render::Draw(m, level, view);
                const double half = ElementOf(s, cut)["width"].get<double>() / 2.0;
                const std::array<double, 4> want{ 0.0, -half, 6000.0, half };
                Check(PlanExtent(d, cut) == want, of + "平面：剖开的范围");
                bool poche = false, dashed = true, overSeen = false, highSeen = false;
                for (size_t i = 0; i < d.prims.size(); ++i)
                {
                    const Render::Prim& p = d.prims[i];
                    if (d.owners[i] == std::optional<Id>(cut))
                        poche |= p.kind == Render::Prim::Kind::Fill && p.color == Render::Poche;
                    if (d.owners[i] == std::optional<Id>(over))
                    {
                        overSeen = true;
                        dashed &= p.kind == Render::Prim::Kind::Line && p.dash.has_value();
                    }
                    highSeen |= d.owners[i] == std::optional<Id>(high);
                }
                Check(poche, of + "平面：剖开的涂实");
                Check(overSeen && dashed, of + "平面：剖切面以上的画虚线");
                Check(!highSeen, of + "平面：视图范围以上的不画");
                const Element* e = m.elements.Get(cut);
                const Render::Drawing sketch = e ? Render::Sketch(m, level, view, *e) : Render::Drawing(view);
                Check(!sketch.prims.empty() && PlanExtent(sketch, std::nullopt) == want, of + "预览一样画");
            }
            {
                // 捕捉 run 的端点与中点
                Session s;
                Run(s, c.command, R"({"points": [[0, 0], [6000, 0]], "offset": 2400})");
                const Model& m = s.GetModel();
                const auto at  = [&](P2 p) { return Snap::At(m, m.GuidOf(Id(100000)), p, Snap::Reach{ 100.0, 60.0 }, {}, false, Snap::Snaps{}); };
                const Snap::Found end = at(P2(6040, 30)), mid = at(P2(3040, 30));
                Check(end.at == P2(6000, 0) && end.kind == Snap::Kind::Endpoint, of + "捕捉端点");
                Check(mid.at == P2(3000, 0) && mid.kind == Snap::Kind::Midpoint, of + "捕捉中点");
            }
        }
        {
            // 七类 run 存了再打开一模一样
            Session s;
            for (const MepCase& c : MepCases)
                Run(s, c.command, R"({"points": [[0, 0], [4000, 0]], "system": "S"})");
            const std::string path = Temp("mep.minibim");
            Run(s, "save", Json{ { "path", path } });
            Session t;
            Run(t, "open", Json{ { "path", path } });
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
            Check(t.GetModel().ToJson() == s.GetModel().ToJson(), "MEP：存了再打开一模一样");
        }
        // 新项目各种语言都有各选项卡的起步设备，每个族都建得出
        for (const Language l : LanguageOf::All)
        {
            const Model m = Model::New("x", l);
            std::string error;
            bool ok = m.Check(error);
            for (const MepFixture& f : MepFixtures)
            {
                const auto it = m.families.find(LanguageOf::Word(l, f.family));
                ok = ok && it != m.families.end() && it->second.category == f.category && !it->second.types.empty();
            }
            Check(ok, std::string("设备：") + LanguageOf::Code(l) + " 的项目有起步的设备族：" + error);
        }
        {
            // 每个设备族的每个类型都有形体，立在它该在的高度
            Session s;
            for (const MepFixture& f : MepFixtures)
            {
                std::vector<std::string> types;
                if (const auto it = s.GetModel().families.find(f.family); it != s.GetModel().families.end())
                    for (const auto& [t, _] : it->second.types)
                        types.push_back(t);
                bool ok = !types.empty();
                for (const std::string& t : types)
                {
                    const Json r = Run(s, "component", Json{ { "family", f.family }, { "type", t }, { "at", Pt(1000, 1000) } });
                    const Element* e = r.is_null() ? nullptr : s.GetModel().elements.Get(r["id"].get<Id>());
                    const Component* c = e ? e->As<Component>() : nullptr;
                    const auto pieces = c ? s.GetModel().ComponentPieces(*c) : std::vector<Fam::Piece>{};
                    double top = -INFINITY, bottom = INFINITY;
                    for (const Fam::Piece& p : pieces)
                    {
                        top    = std::max(top, p.top);
                        bottom = std::min(bottom, p.bottom);
                    }
                    const bool fits = !pieces.empty() && top >= f.top[0] - 1e-6 && top <= f.top[1] + 1e-6 && bottom >= f.base[0] - 1e-6 &&
                                      bottom <= f.base[1] + 1e-6;
                    if (!fits)
                        std::printf("  %s %s：%g 到 %g\n", f.family, t.c_str(), bottom, top);
                    ok = ok && fits;
                }
                Check(ok, std::string("设备：") + f.family + " 的每个类型都有形体，高度对");
            }
        }
    }

    // 移植自 BIMCraft tests.rs（f3bbb33）的体量测试：体量、mass_floors、按面做墙楼板屋顶、update_by_face、放样
    Json Block() { return Json::parse("[[0, 0], [10000, 0], [10000, 6000], [0, 6000]]"); }
    Json BlockCcw() { return Json::parse("[[0.0, 0.0], [10000.0, 0.0], [10000.0, 6000.0], [0.0, 6000.0]]"); }

    // 标高 0、3000、6000、9000（Level 1 到 4），Level 1 上 10 × 6 米、到 Level 4 的体量
    std::pair<Id, std::array<Id, 4>> Tower(Session& s)
    {
        const Id l3 = Run(s, "level", R"({"elevation": 6000, "name": "Level 3"})")["id"].get<Id>();
        const Id l4 = Run(s, "level", R"({"elevation": 9000, "name": "Level 4"})")["id"].get<Id>();
        const Id id = Run(s, "mass", Json{ { "boundary", Block() }, { "top_level", l4 }, { "name", "Tower" } })["id"].get<Id>();
        return { id, { 100000, 100001, l3, l4 } };
    }

    Json Mark(Session& s, Id mass, const Json& face) { return Json{ { "mass", ToJson(s.GetModel().GuidOf(mass)) }, { "face", face } }; }

    void TestMassing()
    {
        {
            // 顺时针画的变成逆时针；没给顶时到上一个标高
            Session s;
            const Json r = Run(s, "mass", R"({"boundary": [[0, 0], [0, 6000], [10000, 6000], [10000, 0]], "name": "Block"})");
            const Id id  = r["id"].get<Id>();
            Check(r["area_m2"] == 60.0 && r["volume_m3"] == 180.0 && r["height"] == 3000.0, "体量：面积、体积、高：" + r.dump());
            Json e = ElementOf(s, id);
            Check(IdOf(s, e["level"]) == Id(100000) && IdOf(s, e["top_level"]) == Id(100001) && e["name"] == "Block", "体量：标高到上一个标高");
            Check(e["boundary"] == Json::parse("[[10000.0, 0.0], [10000.0, 6000.0], [0.0, 6000.0], [0.0, 0.0]]"), "体量：逆时针");
            const Json tall = Run(s, "MS", Json{ { "boundary", Block() }, { "height", 9000 }, { "base_offset", 500 } });
            Check(tall["volume_m3"] == 540.0 && ElementOf(s, tall["id"].get<Id>())["name"] == "Mass", "体量：MS、height、默认名称");
            Check(Count(s, "masses") == 2, "体量：按复数的类别数");
            Run(s, "modify", Json{ { "id", id }, { "top_level", nullptr }, { "height", 4500 } });
            e = ElementOf(s, id);
            Check(e["volume_m3"] == 270.0 && e["gross_floor_area_m2"] == 120.0 && e["levels"].size() == 2, "体量：跨两个标高的总楼面面积：" + e.dump());
            const std::pair<Json, const char*> bad[] = {
                { Json{ { "id", id }, { "height", -5 } }, "height" },
                { Json{ { "id", id }, { "top_level", 100000 }, { "height", 0 } }, "above its base" },
                { Json{ { "id", id }, { "boundary", Json::parse("[[0, 0], [1000, 0]]") } }, "points" },
                { Json{ { "id", id }, { "level", 100098 } }, "level" },
                { Json{ { "id", id }, { "base_offset", 1e12 } }, "base_offset" },
            };
            for (const auto& [p, says] : bad)
            {
                const std::string why = Refused(s, "modify", p);
                Check(Contains(why, says), std::string("体量：modify 拒绝 ") + p.dump() + "：" + why);
            }
            Check(ElementOf(s, id)["volume_m3"] == 270.0, "体量：被拒后不变");
            // 移动、复制、镜像、旋转带着轮廓走
            Run(s, "move", Json{ { "ids", { id } }, { "by", Pt(1000, 500) } });
            Check(ElementOf(s, id)["boundary"][0] == Pt(11000, 500), "体量：移动");
            const Id made = FirstId(Run(s, "copy", Json{ { "ids", { id } }, { "by", Pt(0, 8000) } }));
            Check(ElementOf(s, made)["boundary"][0] == Pt(11000, 8500), "体量：复制");
            const Id mirrored = FirstId(Run(s, "MM", Json{ { "ids", { id } }, { "axis", { Pt(0, 0), Pt(0, 1000) } }, { "copy", true } }));
            Check(ElementOf(s, mirrored)["area_m2"] == 60.0, "体量：镜像后仍逆时针");
            Run(s, "rotate", Json{ { "ids", { id } }, { "about", Pt(0, 0) }, { "angle", 90 } });
            Check(ElementOf(s, id)["area_m2"] == 60.0, "体量：旋转");
            // 项目文件原样存下体量
            const std::string file = Temp("mass.minibim");
            Run(s, "save", Json{ { "path", file } });
            Session t;
            Run(t, "open", Json{ { "path", file } });
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(std::u8string(file.begin(), file.end())), ec);
            Check(t.GetModel().ToJson()["elements"] == s.GetModel().ToJson()["elements"], "体量：存盘再打开不变");
            for (int i = 0; i < 3; ++i)
                Run(s, "undo", Json::object());
            Check(Count(s, "mass") == 2 && ElementOf(s, id)["boundary"][0] == Pt(11000, 500), "体量：各一步撤销");
            Check(Contains(Refused(s, "delete", R"({"ids": [100000]})"), "level"), "体量：它立在上面的标高删不掉");
        }
        {
            // mass_floors：它跨过的每个标高一块楼板
            Session s;
            const auto [id, l] = Tower(s);
            const auto floors = IdsOf(Run(s, "MF", Json{ { "mass", id } }));
            Check(floors.size() == 3 && Count(s, "floor") == 3, "体量楼板：三块");
            for (size_t i = 0; i < floors.size() && i < 3; ++i)
            {
                const Json e = ElementOf(s, floors[i]);
                Check(IdOf(s, e["level"]) == l[i] && e["area_m2"] == 60.0 && e["boundary"] == BlockCcw(), "体量楼板：标高与轮廓 " + std::to_string(i));
                Check(e["params"]["Mass Floor"] == Json{ { "mass", ToJson(s.GetModel().GuidOf(id)) }, { "level", ToJson(s.GetModel().GuidOf(l[i])) } },
                      "体量楼板：记着体量与标高");
            }
            const Id high = Run(s, "mass", Json{ { "boundary", Block() }, { "base_offset", 500 }, { "height", 7000 } })["id"].get<Id>();
            const auto hf = IdsOf(Run(s, "mass_floors", Json{ { "mass", high } }));
            Check(hf.size() == 2 && IdOf(s, ElementOf(s, hf[0])["level"]) == l[1], "体量楼板：高出标高的从它够到的第一个标高起");
            Check(Run(s, "mass_floors", Json::object())["ids"].size() == 5, "体量楼板：不给 mass 时所有体量");
            Session empty;
            Check(Contains(Refused(empty, "mass_floors", Json::object()), "no mass"), "体量楼板：没有体量");
            const Id wall = FirstId(Run(s, "wall", R"({"points": [[0, 0], [1000, 0]]})"));
            Check(Contains(Fails(s, "mass_floors", Json{ { "mass", id }, { "type", "Nothing" } }), "type"), "体量楼板：类型");
            Check(Contains(Fails(s, "mass_floors", Json{ { "mass", wall } }), "isn't a mass"), "体量楼板：墙不是体量");
        }
        {
            // 再跑一次：更新它做的，过时的只在 prune 时删
            Session s;
            const auto [id, l] = Tower(s);
            const Json first   = Run(s, "mass_floors", Json{ { "mass", id } });
            const auto floors  = IdsOf(first);
            Check(floors.size() == 3 && first["made"].size() == 3 && first["updated"].empty(), "体量楼板：第一次都是新做的");
            const Json again = Run(s, "MF", Json{ { "mass", id } });
            Check(IdsOf(again) == floors && again["made"].empty() && again["updated"].empty() && Count(s, "floor") == 3, "体量楼板：再跑一次什么也不做");
            const Id own = Run(s, "floor", R"({"boundary": [[0, 0], [1000, 0], [0, 1000]]})")["id"].get<Id>();
            Run(s, "modify", Json{ { "id", id }, { "boundary", Json::parse("[[0, 0], [12000, 0], [12000, 6000], [0, 6000]]") } });
            const Json grown = Run(s, "mass_floors", Json{ { "mass", id } });
            Check(grown["updated"].size() == 3 && ElementOf(s, floors[1])["area_m2"] == 72.0 && ElementOf(s, own)["area_m2"] == 0.5,
                  "体量楼板：体量变大，楼板跟着，别的不动");
            Run(s, "type", R"({"category": "floor", "name": "Thin", "thickness": 100})");
            Check(Run(s, "mass_floors", Json{ { "mass", id }, { "type", "Thin" } })["updated"].size() == 3 && ElementOf(s, floors[0])["type"] == "Thin",
                  "体量楼板：换类型");
            Check(Run(s, "mass_floors", Json{ { "mass", id } })["updated"].empty() && ElementOf(s, floors[0])["type"] == "Thin", "体量楼板：不给类型时保留");
            Run(s, "modify", Json{ { "id", id }, { "top_level", l[2] } });
            Json r = Run(s, "mass_floors", Json{ { "mass", id } });
            Check(r["ids"].size() == 2 && r["stale"] == Json::array({ floors[2] }) && r["removed"].empty() && Count(s, "floor") == 4, "体量楼板：过时的报出来");
            r = Run(s, "mass_floors", Json{ { "mass", id }, { "prune", true } });
            Check(r["removed"] == Json::array({ floors[2] }) && r["stale"].empty() && Count(s, "floor") == 3, "体量楼板：prune 删掉过时的");
            Check(Run(s, "mass_floors", R"({"prune": true})")["removed"].empty() && Count(s, "floor") == 3, "体量楼板：没记号的楼板不动");
            const std::pair<Json, const char*> marks[] = {
                { Json{ { "mass", 100000 }, { "level", l[0] } }, "isn't a mass" },
                { Json{ { "mass", id }, { "level", id } }, "isn't a level" },
                { Json{ { "face", 1 } }, "{mass, level}" },
                { Json("x"), "{mass, level}" },
            };
            for (const auto& [value, says] : marks)
            {
                const std::string why = Refused(s, "modify", Json{ { "id", floors[0] }, { "params", { { "Mass Floor", value } } } });
                Check(Contains(why, says), "体量楼板：记号 " + value.dump() + "：" + why);
            }
            const Id wall = FirstId(Run(s, "wall", R"({"points": [[0, 0], [1000, 0]]})"));
            Check(!Refused(s, "modify", Json{ { "id", wall }, { "params", { { "Mass Floor", { { "mass", id }, { "level", l[0] } } } } } }).empty(),
                  "体量楼板：墙不能记体量楼板");
            const Id copy = FirstId(Run(s, "copy", Json{ { "ids", { floors[0] } }, { "by", Pt(20000, 0) } }));
            Check(!ElementOf(s, copy)["params"].contains("Mass Floor"), "体量楼板：副本是普通楼板");
            Run(s, "delete", Json{ { "ids", { id } } });
            Check(!ElementOf(s, floors[0])["params"].contains("Mass Floor"), "体量楼板：体量删了，楼板是普通的");
            Check(Contains(Refused(s, "mass_floors", R"({"prune": true})"), "no mass"), "体量楼板：删了体量后没有体量");
        }
        {
            // 按面做墙、楼板、屋顶
            Session s;
            const Id id      = Run(s, "mass", Json{ { "boundary", Block() } })["id"].get<Id>();
            const auto walls = IdsOf(Run(s, "WB", Json{ { "mass", id } }));
            Check(walls.size() == 4, "按面：四面墙");
            Json w = walls.empty() ? Json() : ElementOf(s, walls[0]);
            Check(w["start"] == Pt(10000, 0) && w["end"] == Pt(0, 0) && IdOf(s, w["level"]) == Id(100000) && IdOf(s, w["top_level"]) == Id(100001),
                  "按面：墙反着走、到体量顶：" + w.dump().substr(0, 200));
            double lo = 1e300, hi = -1e300, south = 1e300, north = -1e300;
            for (const Id wid : walls)
                for (const P2& q : Walls::Outline(s.GetModel(), wid))
                {
                    lo    = std::min(lo, q.x);
                    hi    = std::max(hi, q.x);
                    south = std::min(south, q.y);
                    north = std::max(north, q.y);
                }
            Check(std::abs(lo) + std::abs(hi - 10000) + std::abs(south) + std::abs(north - 6000) < 1e-6, "按面：墙在轮廓里、外饰面在面上");
            const Id tall  = Run(s, "mass", Json{ { "boundary", Block() }, { "height", 4000 }, { "base_offset", 200 } })["id"].get<Id>();
            const auto two = IdsOf(Run(s, "wall_by_face", Json{ { "mass", tall }, { "faces", { 1, 3 } }, { "location", "centre" } }));
            w = two.empty() ? Json() : ElementOf(s, two[0]);
            Check(two.size() == 2 && w["height"] == 4000.0 && w["base_offset"] == 200.0 && w["start"] == Pt(10000, 6000), "按面：选的面、高度");
            const std::pair<Json, const char*> bad[] = {
                { Json{ { "mass", tall }, { "faces", { 4 } } }, "face" },
                { Json{ { "mass", tall }, { "faces", { -1 } } }, "face" },
                { Json{ { "mass", tall }, { "faces", { "top" } } }, "face" },
                { Json{ { "mass", tall }, { "type", "Nothing" } }, "type" },
                { Json{ { "mass", 100000 } }, "isn't a mass" },
                { Json::object(), "mass" },
            };
            for (const auto& [p, says] : bad)
                Check(Contains(Fails(s, "wall_by_face", p), says), std::string("按面：拒绝 ") + p.dump());
            Json e = ElementOf(s, Run(s, "FB", Json{ { "mass", id } })["id"].get<Id>());
            Check(IdOf(s, e["level"]) == Id(100001) && e.value("offset", 0.0) == 0.0 && e["area_m2"] == 60.0, "按面：楼板在顶标高");
            e = ElementOf(s, Run(s, "floor_by_face", Json{ { "mass", tall } })["id"].get<Id>());
            Check(IdOf(s, e["level"]) == Id(100000) && e["offset"] == 4200.0, "按面：楼板在 height 处");
            e = ElementOf(s, Run(s, "RB", Json{ { "mass", id } })["id"].get<Id>());
            Check(IdOf(s, e["level"]) == Id(100001) && e.value("slope", 0.0) == 0.0 && e["boundary"] == BlockCcw(), "按面：平屋顶");
            e = ElementOf(s, Run(s, "roof_by_face", Json{ { "mass", tall }, { "slope", 20 } })["id"].get<Id>());
            Check(IdOf(s, e["level"]) == Id(100000) && e["offset"] == 4200.0 && e["slope"] == 20.0, "按面：坡屋顶");
            Run(s, "delete", Json{ { "ids", { id, tall } } });
            Check(Count(s, "wall") == 6 && Count(s, "roof") == 2, "按面：体量删了它们还在");
        }
        {
            // update_by_face 照体量现在的样子重建
            Session s;
            const auto [id, l] = Tower(s);
            const auto walls   = IdsOf(Run(s, "WB", Json{ { "mass", id } }));
            const Id floor     = Run(s, "FB", Json{ { "mass", id } })["id"].get<Id>();
            const Id roof      = Run(s, "RB", Json{ { "mass", id } })["id"].get<Id>();
            bool marked = walls.size() == 4;
            for (size_t i = 0; i < walls.size(); ++i)
                marked = marked && ElementOf(s, walls[i])["params"]["Mass Face"] == Mark(s, id, i);
            for (const Id e : { floor, roof })
                marked = marked && ElementOf(s, e)["params"]["Mass Face"] == Mark(s, id, "top");
            Check(marked, "按面：记着体量与面");
            const auto two = IdsOf(Run(s, "wall_by_face", Json{ { "mass", id }, { "faces", { 1, 3 } } }));
            Check(two.size() == 2 && ElementOf(s, two[1])["params"]["Mass Face"] == Mark(s, id, 3), "按面：选的面记自己的编号");
            Run(s, "delete", Json{ { "ids", two } });
            Run(s, "modify", Json{ { "id", id }, { "boundary", Json::parse("[[0, 0], [12000, 0], [12000, 6000], [0, 6000]]") }, { "top_level", l[2] } });
            Check(ElementOf(s, walls[0])["start"] == Pt(10000, 0), "按面：不会自己跟着体量");
            Run(s, "wall", R"({"points": [[0, 0], [100, 0]]})");
            std::vector<Id> every = walls;
            every.push_back(floor);
            every.push_back(roof);
            Json done = Run(s, "UF", Json{ { "mass", id } });
            Check(done["updated"] == Json(every), "按面：更新到面：" + done.dump());
            Json w = ElementOf(s, walls[0]);
            Check(w["start"] == Pt(12000, 0) && w["end"] == Pt(0, 0) && IdOf(s, w["top_level"]) == l[2], "按面：墙到新边、新顶");
            Check(ElementOf(s, walls[1])["start"] == Pt(12000, 6000), "按面：第二面墙");
            for (const Id x : { floor, roof })
            {
                const Json e = ElementOf(s, x);
                Check(IdOf(s, e["level"]) == l[2] && e["area_m2"] == 72.0, "按面：楼板、屋顶到新顶");
            }
            const size_t steps = s.History().size();
            done = Run(s, "update_by_face", Json::object());
            Check(done["updated"].empty() && done["unchanged"] == Json(every) && s.History().size() == steps, "按面：没有要做的，不算一步");
            Run(s, "modify", Json{ { "id", roof }, { "slope", 25 } });
            Run(s, "modify", Json{ { "id", id }, { "top_level", l[3] } });
            Run(s, "update_by_face", Json{ { "ids", { roof, walls[2] } } });
            const Json r = ElementOf(s, roof);
            Check(IdOf(s, r["level"]) == l[3] && r["slope"] == 25.0, "按面：之后改的坡度保留");
            Check(IdOf(s, ElementOf(s, walls[2])["top_level"]) == l[3] && IdOf(s, ElementOf(s, walls[3])["top_level"]) == l[2], "按面：只更新给的");
            const auto halves = IdsOf(Run(s, "split", Json{ { "id", walls[2] }, { "at", Pt(6000, 6000) } }));
            bool plain = halves.size() == 2;
            for (const Id h : halves)
                plain = plain && !ElementOf(s, h)["params"].contains("Mass Face");
            Check(plain, "按面：拆开的两段是普通的墙");
            const Id low = Run(s, "mass", Json{ { "boundary", Block() }, { "height", 4000 }, { "base_offset", 200 } })["id"].get<Id>();
            const Id f   = Run(s, "floor_by_face", Json{ { "mass", low } })["id"].get<Id>();
            const Id lw  = FirstId(Run(s, "wall_by_face", Json{ { "mass", low }, { "faces", { 0 } } }));
            Run(s, "modify", Json{ { "id", low }, { "height", 5000 } });
            Run(s, "update_by_face", Json{ { "mass", low } });
            Check(ElementOf(s, f)["offset"] == 5200.0 && ElementOf(s, lw)["height"] == 5000.0, "按面：给了高度的体量");
            const Id flip = Run(s, "mass", Json{ { "boundary", Block() }, { "height", 3000 }, { "name", "Flip" } })["id"].get<Id>();
            const auto fw = IdsOf(Run(s, "wall_by_face", Json{ { "mass", flip }, { "faces", { 0, 1 } } }));
            Run(s, "mirror", Json{ { "ids", { flip } }, { "axis", { Pt(-1000, 0), Pt(-1000, 1000) } } });
            Check(fw.size() == 2 && ElementOf(s, fw[0])["params"]["Mass Face"] == Mark(s, flip, 2) &&
                      ElementOf(s, fw[1])["params"]["Mass Face"] == Mark(s, flip, 1),
                  "按面：原地镜像后面重新编号");
            Run(s, "update_by_face", Json{ { "mass", flip } });
            w = fw.empty() ? Json() : ElementOf(s, fw[0]);
            Check(w["start"] == Pt(-2000, 0) && w["end"] == Pt(-12000, 0), "按面：镜像后跟着自己的边");
            Run(s, "delete", Json{ { "ids", { flip } } });
            Run(s, "modify", Json{ { "id", id }, { "boundary", Json::parse("[[0, 0], [12000, 0], [0, 6000]]") } });
            const std::string why = Fails(s, "update_by_face", Json{ { "mass", id } });
            Check(Contains(why, "wall " + std::to_string(walls[3])) && Contains(why, "face 3") && Contains(why, "3 faces"), "按面：面没了，整个拒绝：" + why);
            Run(s, "update_by_face", Json{ { "ids", { walls[0], walls[1], floor } } });
            Check(ElementOf(s, floor)["area_m2"] == 36.0, "按面：按 ID 更新面还在的");
            const Id plainWall = FirstId(Run(s, "wall", R"({"points": [[0, 0], [100, 0]]})"));
            const std::pair<Json, std::string> bad[] = {
                { Json{ { "ids", { plainWall } } }, "isn't made by face" },
                { Json{ { "mass", plainWall } }, "isn't a mass" },
                { Json{ { "mass", id }, { "ids", { walls[0] } } }, "ids" },
                { Json{ { "ids", { 9999 } } }, "9999" },
                { Json{ { "ids", "x" } }, "ids" },
            };
            for (const auto& [p, says] : bad)
                Check(Contains(Refused(s, "update_by_face", p), says), "按面：拒绝 " + p.dump());
            Run(s, "delete", Json{ { "ids", { id, low } } });
            Check(!ElementOf(s, walls[0])["params"].contains("Mass Face"), "按面：体量删了，记号也去掉");
            Check(Contains(Refused(s, "update_by_face", Json::object()), "by face"), "按面：没有按面做的");
            const Id m2 = Run(s, "mass", Json{ { "boundary", Block() }, { "height", 3000 } })["id"].get<Id>();
            const std::pair<Id, Json> marks[] = {
                { walls[0], Json{ { "mass", m2 }, { "face", "top" } } },
                { floor, Json{ { "mass", m2 }, { "face", 0 } } },
                { walls[0], Json{ { "mass", plainWall }, { "face", 0 } } },
                { walls[0], Json{ { "mass", m2 } } },
                { plainWall, Json{ { "mass", m2 }, { "face", -1 } } },
            };
            for (const auto& [e, mark] : marks)
                Check(!Refused(s, "modify", Json{ { "id", e }, { "params", { { "Mass Face", mark } } } }).empty(), "按面：记号不对 " + mark.dump());
            Run(s, "modify", Json{ { "id", plainWall }, { "params", { { "Mass Face", { { "mass", m2 }, { "face", 7 } } } } } });
            Check(Contains(Refused(s, "update_by_face", Json::object()), "face 7"), "按面：超出轮廓的面是没了的面");
        }
        {
            // 有顶部轮廓的体量在两个轮廓之间直直放样
            Session s;
            const Id l3       = Run(s, "level", R"({"elevation": 6000, "name": "Level 3"})")["id"].get<Id>();
            const Json top    = Json::parse("[[2000, 2000], [8000, 2000], [8000, 4000], [2000, 4000]]");
            const Json topCcw = Json::parse("[[2000.0, 2000.0], [8000.0, 2000.0], [8000.0, 4000.0], [2000.0, 4000.0]]");
            const Json r = Run(s, "mass", Json{ { "boundary", Block() }, { "top_level", l3 }, { "top", top }, { "name", "Frustum" } });
            const Id id  = r["id"].get<Id>();
            Check(r["area_m2"] == 60.0 && r["height"] == 6000.0 && r["volume_m3"] == 200.0, "放样：体积按拟柱体：" + r.dump());
            Json e = ElementOf(s, id);
            Check(e["top"] == topCcw && e["volume_m3"] == 200.0 && e["gross_floor_area_m2"] == 92.0, "放样：顶部轮廓、总楼面面积");
            auto floors = IdsOf(Run(s, "MF", Json{ { "mass", id } }));
            Check(floors.size() == 2 &&
                      ElementOf(s, floors[1])["boundary"] == Json::parse("[[1000.0, 1000.0], [9000.0, 1000.0], [9000.0, 5000.0], [1000.0, 5000.0]]") &&
                      ElementOf(s, floors[1])["area_m2"] == 32.0 && ElementOf(s, floors[0])["boundary"] == BlockCcw(),
                  "放样：半高处的楼板轮廓");
            Run(s, "modify", Json{ { "id", id }, { "top", Block() } });
            Check(Run(s, "mass_floors", Json{ { "mass", id } })["updated"] == Json::array({ floors[1] }), "放样：顶部轮廓变了楼板跟着");
            Run(s, "modify", Json{ { "id", id }, { "top", top } });
            const Id f = Run(s, "floor_by_face", Json{ { "mass", id } })["id"].get<Id>();
            Check(ElementOf(s, f)["boundary"] == topCcw && IdOf(s, ElementOf(s, f)["level"]) == l3, "放样：顶面楼板在顶部轮廓");
            const Id rf = Run(s, "roof_by_face", Json{ { "mass", id } })["id"].get<Id>();
            Check(ElementOf(s, rf)["boundary"] == topCcw, "放样：顶面屋顶");
            std::string why = Refused(s, "wall_by_face", Json{ { "mass", id } });
            Check(Contains(why, "plumb") && Contains(why, "lean"), "放样：斜面上立不了墙：" + why);
            why = Refused(s, "wall_by_face", Json{ { "mass", id }, { "faces", { 1 } } });
            Check(Contains(why, "face 1") && Contains(why, "plumb"), "放样：选的面斜：" + why);
            Run(s, "modify", Json{ { "id", id }, { "top", Json::parse("[[0, 0], [10000, 0], [10000, 3000], [0, 3000]]") } });
            Run(s, "update_by_face", Json{ { "mass", id } });
            Check(ElementOf(s, f)["area_m2"] == 30.0, "放样：顶面楼板跟着顶部轮廓");
            const Json lean = Run(s, "mass", Json{ { "boundary", Block() }, { "top_level", l3 }, { "lean", Pt(3000, 0) }, { "name", "Lean" } });
            const Id leanId = lean["id"].get<Id>();
            Check(lean["volume_m3"] == 360.0 && ElementOf(s, leanId)["top"][0] == Pt(3000, 0), "放样：lean");
            floors = IdsOf(Run(s, "mass_floors", Json{ { "mass", leanId } }));
            Check(floors.size() == 2 && ElementOf(s, floors[1])["boundary"][0] == Pt(1500, 0), "放样：lean 的楼板");
            Check(Contains(Refused(s, "wall_by_face", Json{ { "mass", leanId } }), "leans"), "放样：lean 没有竖直的面");
            const Id part =
                Run(s, "mass", Json{ { "boundary", Block() }, { "height", 3000 }, { "top", Json::parse("[[0, 0], [10000, 0], [8000, 6000], [0, 6000]]") } })["id"]
                    .get<Id>();
            const auto pw = IdsOf(Run(s, "wall_by_face", Json{ { "mass", part } }));
            Check(pw.size() == 2 && ElementOf(s, pw[0])["params"]["Mass Face"]["face"] == 0 && ElementOf(s, pw[1])["params"]["Mass Face"]["face"] == 3,
                  "放样：只有竖直的面立墙");
            Run(s, "modify", Json{ { "id", part }, { "top", Json::parse("[[0, 0], [10000, 0], [10000, 5000], [0, 5000]]") } });
            Check(Contains(Refused(s, "update_by_face", Json{ { "ids", pw } }), "leans now"), "放样：面斜了");
            const Id copy = FirstId(Run(s, "copy", Json{ { "ids", { id } }, { "by", Pt(20000, 0) } }));
            Check(ElementOf(s, copy)["top"][0] == Pt(20000, 0), "放样：复制带着顶部轮廓");
            Run(s, "mirror", Json{ { "ids", { id } }, { "axis", { Pt(-1000, 0), Pt(-1000, 1000) } } });
            e = ElementOf(s, id);
            Check(e["volume_m3"] == 270.0 && e["gross_floor_area_m2"] == 105.0, "放样：镜像：" + e.dump().substr(0, 300));
            Run(s, "rotate", Json{ { "ids", { id } }, { "about", Pt(0, 0) }, { "angle", 90 } });
            Check(ElementOf(s, id)["volume_m3"] == 270.0, "放样：旋转");
            Run(s, "modify", Json{ { "id", id }, { "top", nullptr } });
            e = ElementOf(s, id);
            Check(!e.contains("top") && e["volume_m3"] == 360.0, "放样：top 为 null 时不再放样");
        }
        {
            // 顶部轮廓与底面轮廓点点对应，从不缩没
            Session s;
            const std::pair<const char*, const char*> bad[] = {
                { R"({"height": 3000, "top": [[0, 0], [1, 0], [1, 1]]})", "points" },
                { R"({"height": 3000, "top": "x"})", "top" },
                { R"({"height": 3000, "top": [[0, 0], [1e300, 0], [1, 1], [0, 1]]})", "top" },
                { R"({"height": 3000, "top": [[0, 6000], [10000, 6000], [10000, 0], [0, 0]]})", "other way" },
                { R"({"height": 3000, "top": [[10000, 6000], [0, 6000], [0, 0], [10000, 0]]})", "collapses" },
                { R"({"height": 3000, "top": [[0, 0], [10000, 0], [10000, 6000], [0, 6000]], "lean": [1, 1]})", "lean" },
                { R"({"height": 3000, "lean": 5})", "lean" },
                { R"({"top_level": 100001, "lean": [1e300, 0]})", "lean" },
            };
            for (const auto& [text, says] : bad)
            {
                Json p        = Json::parse(text);
                p["boundary"] = Block();
                Check(Contains(Fails(s, "mass", p), says), std::string("放样：拒绝 ") + text);
            }
            const Id id = Run(s, "mass", Json{ { "boundary", Json::parse("[[0, 0], [0, 6000], [10000, 6000], [10000, 0]]") }, { "height", 6000 },
                                               { "top", Json::parse("[[2000, 2000], [2000, 4000], [8000, 4000], [8000, 2000]]") } })["id"]
                              .get<Id>();
            const Json e = ElementOf(s, id);
            Check(e["volume_m3"] == 200.0 && e["boundary"][1] == Pt(10000, 6000) && e["top"][1] == Pt(8000, 4000), "放样：顺时针的两个轮廓一起反过来");
            Check(Contains(Refused(s, "modify", Json{ { "id", id }, { "boundary", Json::parse("[[0, 0], [100, 0], [0, 100]]") } }), "points"),
                  "放样：只改底面轮廓的点数");
            const Id flat = Run(s, "mass", Json{ { "boundary", Block() }, { "height", 3000 }, { "top", Block() } })["id"].get<Id>();
            Check(!ElementOf(s, flat).contains("top"), "放样：和底面一样的顶部轮廓不放样");
        }
    }

    // ── 注释（第 5d 步）────────────────────────────────────────
    void TestAnnotations()
    {
        {
            // 尺寸：点在墙上取参照，跟着墙走；量的墙删了它也删
            Session s;
            const std::vector<Id> walls = House(s);
            const Json r = Run(s, "DI", Json{ { "refs", { Pt(4000, 0), Pt(4000, 6000) } }, { "at", Pt(9000, 3000) } });
            const Id dim = r["id"].get<Id>();
            Check(r["values"] == Json::array({ 6000.0 }), "尺寸：两面平行的墙中心线之间：" + r.dump());
            Run(s, "move", Json{ { "ids", { walls[2] } }, { "by", Pt(0, 500) } });
            Check(ElementOf(s, dim)["values"] == Json::array({ 6500.0 }), "尺寸：跟着它量的墙");
            const Json faces = Run(s, "dimension", Json{ { "refs", { { { "id", walls[0] }, { "face", "left" } }, { { "id", walls[0] }, { "face", "right" } } } },
                                                         { "at", Pt(2000, -1000) } });
            Check(!faces.is_null() && NearNum(faces["values"][0], 200.0, 1e-6), "尺寸：墙的两面：" + faces.dump());
            Check(Contains(Refused(s, "dimension", Json{ { "refs", { Pt(4000, 0), Pt(0, 3000) } }, { "at", Pt(9000, 3000) } }), "no wall face"),
                  "尺寸：对齐尺寸的第二条参照要平行");
            const Json angle = Run(s, "dimension", Json{ { "kind", "angular" }, { "refs", { Pt(4000, 0), Pt(0, 3000) } }, { "at", Pt(1000, 1000) } });
            Check(!angle.is_null() && NearNum(angle["values"][0], 90.0, 1e-6), "尺寸：角度：" + angle.dump());
            Run(s, "delete", Json{ { "ids", { walls[0] } } });
            Check(ElementOf(s, dim).is_null() && ElementOf(s, faces["id"].get<Id>()).is_null(), "尺寸：量的墙删了，剩不到两条参照的尺寸也删");
        }
        {
            // 全部标记：门窗的标记写编号，跟着门走；门删了标记也删
            Session s;
            const std::vector<Id> walls = House(s);
            const Id door = Run(s, "door", Json{ { "wall", walls[0] }, { "offset", 2000 } })["id"].get<Id>();
            Run(s, "window", Json{ { "wall", walls[1] }, { "offset", 3000 } });
            const Json tags = Run(s, "TG", Json::object());
            Check(tags["ids"].size() == 2, "标记：全部标记标上门和窗：" + tags.dump());
            const Id tag = tags["ids"][0].get<Id>();
            const Json e = ElementOf(s, tag);
            Check(e["text"] == ElementOf(s, door)["mark"] && e["leader"] == false, "标记：写门的编号：" + e.dump());
            Check(Contains(Refused(s, "tag", Json::object()), "nothing to tag"), "标记：都标过了");
            const Json before = e["at"];
            Run(s, "move", Json{ { "ids", { door } }, { "by", Pt(500, 0) } });
            Check(ElementOf(s, tag)["at"][0] == before[0].get<double>() + 500.0, "标记：跟着门走");
            const Json room = Run(s, "tag", Json{ { "ids", { walls[0] } }, { "label", "Length" } });
            Check(ElementOf(s, room["ids"][0].get<Id>())["text"] != "?", "标记：墙的长度");
            Run(s, "delete", Json{ { "ids", { walls[0] } } });
            Check(ElementOf(s, tag).is_null() && ElementOf(s, room["ids"][0].get<Id>()).is_null(), "标记：宿主删了标记也删");
        }
        {
            // 文字与文字类型
            Session s;
            const Id note = Run(s, "TX", Json{ { "text", "Hello\nworld" }, { "at", Pt(1000, 1000) } })["id"].get<Id>();
            Json e = ElementOf(s, note);
            Check(e["type"] == "2.5mm" && e["size"] == 2.5 && IdOf(s, e["view"]) == Id(100000), "文字：默认 2.5mm，在最低标高的平面：" + e.dump());
            Check(Contains(Refused(s, "text", Json{ { "text", "x" }, { "at", Pt(0, 0) }, { "size", 3 } }), "text type"), "文字：size 是类型的");
            Check(Contains(Refused(s, "text", Json{ { "text", "x" }, { "at", Pt(0, 0) }, { "type", "9mm" } }), "9mm"), "文字：没有的类型");
            const Json made = Run(s, "text_type", Json{ { "name", "Title" }, { "like", "5mm" }, { "bold", true }, { "font", "Courier New" } });
            Check(made["type"]["size"] == 5.0 && made["type"]["bold"] == true && made["type"]["font"] == "Courier New", "文字类型：照另一个做：" + made.dump());
            Run(s, "modify", Json{ { "id", note }, { "type", "Title" } });
            Run(s, "text_type", Json{ { "name", "Title" }, { "rename", "Heading" } });
            Check(ElementOf(s, note)["type"] == "Heading", "文字类型：改名后注释跟着");
            Check(Contains(Refused(s, "text_type", Json{ { "name", "Heading" }, { "delete", true } }), "give it another type"), "文字类型：有注释用时不删");
            Check(Contains(Refused(s, "text_type", Json{ { "name", "Big" }, { "size", 0 } }), "size"), "文字类型：字高要大于 0");
            Check(Run(s, "text_type", Json::object())["types"].size() == 4, "文字类型：列出");
            Run(s, "mirror", Json{ { "ids", { note } }, { "axis", { Pt(0, 0), Pt(0, 1000) } } });
            e = ElementOf(s, note);
            Check(e["at"] == Pt(-1000, 1000) && e["angle"] == 0.0, "文字：镜像后仍然正着读");
            Check(Contains(Refused(s, "text", Json{ { "text", "x" }, { "at", Pt(0, 0) }, { "override_format", true },
                                                       { "runs", Json::parse(R"([{"from": 0, "to": 5}])") } }), "run"),
                  "文字：格式段超出文字");
        }
        {
            // 高程点读楼板顶；详图线；填充区域
            Session s;
            House(s);
            const Id floor = Run(s, "floor", Json{ { "offset", 150 } })["id"].get<Id>();
            const Json sp = Run(s, "EL", Json{ { "at", Pt(4000, 3000) } });
            Check(sp["elevation"] == 150.0 && sp["host"] == floor, "高程点：读楼板的顶：" + sp.dump());
            Check(Contains(Refused(s, "spot", Json{ { "at", Pt(50000, 50000) } }), "nothing to read"), "高程点：那里没有东西");
            const Id line = Run(s, "DL", Json{ { "start", Pt(0, -2000) }, { "end", Pt(4000, -2000) } })["id"].get<Id>();
            Json e = ElementOf(s, line);
            Check(e["style"] == "Thin Lines" && e["shape"] == "line" && e["length"] == 4000.0, "详图线：默认细线：" + e.dump());
            const Json circle = Run(s, "detail_line", Json{ { "centre", Pt(0, 0) }, { "radius", 500 }, { "style", "Hidden Lines" }, { "centre_mark", true } });
            Check(ElementOf(s, circle["id"].get<Id>())["shape"] == "circle", "详图线：圆");
            Check(Contains(Refused(s, "detail_line", Json{ { "centre", Pt(0, 0) }, { "radii", Pt(500, 300) } }), "isn't ported"), "详图线：椭圆还没移植");
            Check(Contains(Refused(s, "detail_line", Json{ { "start", Pt(0, 0) }, { "end", Pt(1000, 0) }, { "style", "Nope" } }), "Nope"), "详图线：没有的线样式");
            const Id copy = FirstId(Run(s, "copy", Json{ { "ids", { line } }, { "by", Pt(0, -1000) } }));
            Check(ElementOf(s, copy)["start"] == Pt(0, -3000), "详图线：复制");
            const Json fr = Run(s, "FR", Json{ { "sketch", SquareLines(10000, 0, 2000) }, { "line_style", nullptr } });
            e = ElementOf(s, fr["id"].get<Id>());
            Check(fr["area_m2"] == 4.0 && e["type"] == "Solid Black" && !e.contains("line_style"), "填充区域：默认实心黑色，边界不画：" + e.dump());
            const Json styled = Run(s, "filled_region", Json{ { "sketch", SquareLines(13000, 0, 1000) }, { "type", "Crosshatch" } });
            Check(ElementOf(s, styled["id"].get<Id>())["line_style"] == "Thin Lines", "填充区域：边界默认细线");
            Check(Run(s, "edit_boundary", Json{ { "id", fr["id"] }, { "sketch", SquareLines(10000, 0, 3000) } })["area_m2"] == 9.0, "填充区域：编辑边界");
            Check(Contains(Refused(s, "filled_region", Json{ { "sketch", Json::parse("[[[0, 0], [1000, 0]]]") } }), "loops"), "填充区域：不闭合");
            // 标高删了，在它平面里的注释都删
            const Id roofLevel = Run(s, "level", Json{ { "name", "Roof" }, { "elevation", 6000 } })["id"].get<Id>();
            Run(s, "DL", Json{ { "start", Pt(0, 0) }, { "end", Pt(1000, 0) }, { "level", "Roof" } });
            Run(s, "TX", Json{ { "text", "Up" }, { "at", Pt(0, 0) }, { "view", "Roof" } });
            const uint64_t lines = Count(s, "line");
            Run(s, "delete", Json{ { "ids", { roofLevel } } });
            Check(Count(s, "line") == lines - 1 && Count(s, "text") == 0, "注释：标高删了它平面里的注释也删");
        }
        {
            // 存进文件再读回来一样；注释的库条目按 GUID 存
            Session s;
            const std::vector<Id> walls = House(s);
            Run(s, "DI", Json{ { "refs", { Pt(4000, 0), Pt(4000, 6000) } }, { "at", Pt(9000, 3000) } });
            Run(s, "TX", Json{ { "text", "Note" }, { "at", Pt(1000, 1000) }, { "leader", Pt(0, 0) } });
            Run(s, "door", Json{ { "wall", walls[0] }, { "offset", 2000 } });
            Run(s, "TG", Json::object());
            Run(s, "EL", Json{ { "at", Pt(100, 0) } });
            Run(s, "DL", Json{ { "points", { Pt(0, 0), Pt(1000, 0), Pt(1000, 1000) } }, { "arcs", { 0, 90 } } });
            Run(s, "FR", Json{ { "sketch", SquareLines(10000, 0, 2000) } });
            const Model& m = s.GetModel();
            // 平面上每个注释都画了东西
            const Render::Drawing d = Render::Plan(m, m.GuidOf(Id(100000)), 800, 600);
            bool drawn = true;
            for (const auto& [id, e] : m.elements.Items())
                if (e->IsAnnotation())
                    drawn = drawn && std::find(d.owners.begin(), d.owners.end(), std::optional<Id>(id)) != d.owners.end();
            Check(drawn, "注释：平面上画尺寸、文字、标记、高程点、详图线、填充区域");
            std::string text, error;
            Check(m.ToFile(text, error), "注释：写进文件");
            const Json file = Json::parse(text);
            const auto thin = m.library.GuidOf(Kind::Of(Kind::Tag::LineStyle), "Thin Lines");
            bool byGuid = false;
            for (const auto& [_, e] : file["elements"].items())
                if (e["category"] == "line")
                    byGuid = thin && e["style"] == thin->ToString();
            Check(byGuid && file["line_styles"].contains("Thin Lines") && file["text_types"].contains("2.5mm"), "注释：线样式按 GUID 存");
            Model back;
            std::vector<std::string> leftOut;
            Check(Model::Read(text, back, leftOut, error) && leftOut.empty() && back.ToJson() == m.ToJson() && back.Check(error), "注释：读回来一样：" + error);
        }
        {
            // BIMCraft 的文件：第 7 版的文字按 size 记字高；在平面视图、剖面里的注释略去
            Session s;
            House(s);
            Run(s, "TX", Json{ { "text", "Note" }, { "at", Pt(1000, 1000) } });
            std::string text, error;
            s.GetModel().ToFile(text, error);
            Json file = Json::parse(text);
            Json note;
            std::string noteKey;
            for (auto& [k, e] : file["elements"].items())
                if (e["category"] == "text")
                    noteKey = k;
            file["elements"][noteKey].erase("type");
            file["elements"][noteKey]["size"] = 4;
            Json other = file["elements"][noteKey];
            other["view"] = "3f2a9c1e-0000-4000-8000-0000000000aa";
            other["guid"] = "3f2a9c1e-0000-4000-8000-0000000000ab";
            file["elements"]["200000"] = other;
            Model m;
            std::vector<std::string> left;
            const bool read = Model::Read(file.dump(), m, left, error);
            const Note* n = read ? m.elements.Get(std::stoull(noteKey))->As<Note>() : nullptr;
            Check(n && n->typeName == "4mm" && m.textTypes.at("4mm").size == 4.0 && !m.elements.Get(200000) &&
                      std::find(left.begin(), left.end(), "/elements/200000") != left.end() && m.Check(error),
                  "注释：旧文件的字高变成文字类型，别的视图里的略去：" + error);
        }
        {
            // 测量：不改模型
            Session s;
            const Json r = Run(s, "ME", Json{ { "from", Pt(0, 0) }, { "to", Pt(3000, 4000) } });
            Check(r["length"] == 5000.0 && r["dx"] == 3000.0 && r["dy"] == 4000.0 && NearNum(r["angle"], 53.1301, 1e-4) && !s.CanUndo(),
                  "测量：长度、分量、角度：" + r.dump());
        }
    }
}

namespace
{
    // ── 视图（5e：可见性 / 图形、平面视图）──────────────────────────
    // 平面 plan 按 Level 1 自己的平面的取景画出的
    Render::Drawing Drawn(const Session& s, Id plan)
    {
        const Model& m = s.GetModel();
        const Render::View v = Render::Plan(m, m.GuidOf(Id(100000)), 800, 600).view;
        return Render::Draw(m, m.GuidOf(plan), v);
    }

    bool Has(const Render::Drawing& d, Id id) { return std::find(d.owners.begin(), d.owners.end(), std::optional<Id>(id)) != d.owners.end(); }

    // 模型点 p 处最上面的填色（没有时为纸色）
    Render::Rgb FillAt(const Render::Drawing& d, P2 p)
    {
        const Render::Px at = d.view.ToPx(p);
        Render::Rgb out = Render::Paper;
        for (const Render::Prim& pr : d.prims)
        {
            if (pr.kind != Render::Prim::Kind::Fill)
                continue;
            bool inside = false;
            for (const auto& ring : pr.rings)
                for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
                    if ((ring[i].second > at.second) != (ring[j].second > at.second) &&
                        at.first < (ring[j].first - ring[i].first) * (at.second - ring[i].second) / (ring[j].second - ring[i].second) + ring[i].first)
                        inside = !inside;
            if (inside)
                out = pr.color;
        }
        return out;
    }

    // 元素 id 的线：笔、颜色、有没有虚线
    struct LineLook
    {
        uint8_t     pen;
        Render::Rgb color;
        bool        dashed;
        bool operator==(const LineLook&) const = default;
    };
    std::vector<LineLook> Lines(const Render::Drawing& d, Id id)
    {
        std::vector<LineLook> out;
        for (size_t i = 0; i < d.prims.size(); ++i)
            if (d.owners[i] == id && d.prims[i].kind == Render::Prim::Kind::Line && d.parts[i] != Render::Part::Text)
                out.push_back(LineLook{ d.pens[i].value_or(255), d.prims[i].color, d.prims[i].dash.has_value() });
        return out;
    }
    bool Any(const std::vector<LineLook>& v, const std::function<bool(const LineLook&)>& f) { return std::any_of(v.begin(), v.end(), f); }

    // 8 × 6 米的一圈墙，中间一道内墙，两个房间（Kitchen 编号 1，Living 编号 10），三扇门（标记 D2、1、2）
    void Flat(Session& s)
    {
        House(s);
        Run(s, "wall", R"({"points": [[5000, 0], [5000, 6000]], "type": "Interior - 100mm"})");
        Run(s, "room", R"({"point": [6500, 3000], "name": "Kitchen"})");
        Run(s, "room", R"({"point": [2000, 3000], "name": "Living", "number": "10"})");
        Run(s, "door", R"({"at": [6000, 0], "mark": "D2"})");
        Run(s, "door", R"({"at": [2000, 0]})");
        Run(s, "door", R"({"at": [5000, 3000], "type": "Single 800 x 2100"})");
    }

    void TestSchedules()
    {
        const Id level1 = 100000;
        {
            // 选的字段、过滤器留下的、排序、合计
            Session s;
            Flat(s);
            Json r = Run(s, "schedule", R"({"name": "Rooms by Area", "category": "rooms", "fields": ["Name", {"name": "Area", "total": true}, "Number"],
                                            "sort": [{"field": "Area", "descending": true}]})");
            Check(r["columns"] == Json::parse(R"j(["Name", "Area (m²)", "Number"])j"), "明细表：列标题：" + r["columns"].dump());
            Check(r["rows"] == Json::parse(R"([["Living", 30.0, "10"], ["Kitchen", 18.0, "1"]])"), "明细表：按面积从大到小：" + r["rows"].dump());
            Check(r["totals"] == Json::parse("[null, 48.0, null]"), "明细表：合计：" + r["totals"].dump());
            Check(std::find(r["available"].begin(), r["available"].end(), Json("Perimeter")) != r["available"].end(), "明细表：可选的字段有周长");
            r = Run(s, "schedule", R"({"name": "Rooms by Area", "filters": [{"field": "Name", "op": "begins-with", "value": "kit"}]})");
            Check(r["rows"] == Json::parse(R"([["Kitchen", 18.0, "1"]])") && r["totals"] == Json::parse("[null, 18.0, null]"), "明细表：过滤器不分大小写");
            r = Run(s, "schedule", R"({"name": "Rooms by Area", "filters": [{"field": "Area", "op": "greater", "value": 20}]})");
            Check(r["rows"] == Json::parse(R"([["Living", 30.0, "10"]])"), "明细表：面积大于 20");
            Check(Count(s, "schedule") == 1, "明细表：是项目的一个视图");
            const Id id = Run(s, "schedule", R"({"name": "Rooms by Area"})")["id"].get<Id>();
            const std::string e = Refused(s, "modify", Json{ { "id", id }, { "name", "Window Schedule" } });
            Check(Contains(e, "Window Schedule") && Contains(e, "window"), "明细表：不能用别的类别内置明细表的名称：" + e);
            Check(Run(s, "schedule", R"({"name": "Rooms by Area"})")["rows"] == Json::parse(R"([["Living", 30.0, "10"]])"), "明细表：留着上次的样子");
            // 内置的改过后成为项目的：按类型成组，门计数，不同的标记空着
            r = Run(s, "schedule", R"({"name": "Door Schedule", "fields": ["Type", "Width", {"name": "Count", "total": true}, "Mark"], "sort": ["Type"], "grouped": true})");
            Check(r["rows"] == Json::parse(R"([["Single 800 x 2100", 800.0, 1.0, "2"], ["Single 900 x 2100", 900.0, 2.0, null]])"), "明细表：成组：" + r["rows"].dump());
            Check(r["totals"] == Json::parse("[null, null, 3.0, null]"), "明细表：成组的合计");
            Check(r["elements"].size() == 2 && r["elements"][1].size() == 2, "明细表：一组列出它的两扇门");
            Check(Count(s, "schedule") == 2 && Run(s, "schedule", R"({"category": "doors"})")["rows"].size() == 2, "明细表：按类别读到门明细表");
            // 回到类别的字段：每个元素一行，按第一列排
            r = Run(s, "schedule", R"({"name": "Door Schedule", "fields": [], "sort": [], "grouped": false})");
            Check(r["columns"] == Json::parse(R"(["Mark", "Type", "Width", "Height", "Level", "Wall"])"), "明细表：门的默认字段：" + r["columns"].dump());
            std::vector<std::string> marks;
            for (const Json& row : r["rows"])
                marks.push_back(row[0].get<std::string>());
            Check(marks == std::vector<std::string>{ "1", "2", "D2" }, "明细表：按标记的自然顺序");
            Check(Run(s, "schedule", R"({"category": "rooms"})")["rows"][0][4] == Json(18000.0), "明细表：房间的周长是长度（毫米）");
            // 撤销两步回到成组的门明细表之前一步
            Run(s, "undo", Json::object());
            Check(Run(s, "schedule", R"({"name": "Door Schedule"})")["columns"] == Json::parse(R"(["Type", "Width", "Count", "Mark"])"), "明细表：撤销");
            // 内置的不管哪种语言、大小写都是同一个
            Check(Run(s, "schedule", R"({"name": "tabla de puertas"})")["id"] == Run(s, "schedule", R"({"name": "DOOR SCHEDULE"})")["id"], "明细表：内置的名称三种语言都认");
            // 写成 CSV
            const std::string path = Temp("rooms.csv");
            Run(s, "schedule", Json{ { "name", "Rooms by Area" }, { "path", path } });
            std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
            const std::string csv((std::istreambuf_iterator<char>(in)), {});
            Check(csv == "Name,Area (m²),Number\nLiving,30,10\n,30,\n" || Contains(csv, "Living"), "明细表：CSV：" + csv);
            in.close();
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())));
            Check(Contains(Refused(s, "schedule", Json{ { "name", "Rooms by Area" }, { "path", Temp("x.xlsx") } }), "xlsx"), "明细表：还不写 xlsx");
        }
        {
            // 算出的字段
            Session s;
            Flat(s);
            const Json fields = Json::parse(R"([
                "Name", "Area",
                {"name": "Cost", "formula": "Area * 120", "total": true},
                {"name": "Half Perimeter", "formula": "Perimeter / 2", "kind": "length"},
                {"name": "Large", "formula": "Area > 20", "kind": "yesno"},
                {"name": "Cost With Tax", "formula": "Cost * 1.1", "heading": "Cost + 10%"}])");
            Json r = Run(s, "schedule", Json{ { "name", "Costs" }, { "category", "rooms" }, { "fields", fields } });
            Check(r["columns"] == Json::parse(R"j(["Name", "Area (m²)", "Cost", "Half Perimeter", "Large", "Cost + 10%"])j"), "算出的字段：列标题：" + r["columns"].dump());
            Check(r["rows"] == Json::parse(R"([["Kitchen", 18.0, 2160.0, 9000.0, false, 2376.0], ["Living", 30.0, 3600.0, 11000.0, true, 3960.0]])"),
                  "算出的字段：值：" + r["rows"].dump());
            Check(r["totals"] == Json::parse("[null, null, 5760.0, null, null, null]"), "算出的字段：合计");
            r = Run(s, "schedule", R"({"name": "Costs", "filters": [{"field": "Large", "op": "equals", "value": true}]})");
            Check(r["rows"].size() == 1, "算出的字段：能过滤");
            for (const auto& [bad, says] : std::vector<std::pair<const char*, const char*>>{
                     { R"([{"name": "X", "formula": "Nope * 2"}])", "Nope" },
                     { R"([{"name": "A", "formula": "B + 1"}, {"name": "B", "formula": "A * 2"}])", "comes back to itself" },
                     { R"([{"name": "X", "formula": "Name * 2"}])", "text" },
                     { R"([{"name": "Area", "formula": "1"}])", "call it something else" },
                     { R"([{"name": "X", "formula": "Area", "kind": "text"}])", "number" },
                     { R"(["Colour"])", "Colour" },
                     { R"(["Name", "Name"])", "twice" },
                     { R"([{"name": "Name", "total": true}])", "total" },
                     { R"([{"name": "Area", "kind": "length"}])", "formula" } })
            {
                const std::string e = Refused(s, "schedule", Json{ { "name", "Costs" }, { "fields", Json::parse(bad) } });
                Check(Contains(e, says), std::string("算出的字段：拒绝 ") + bad + "：" + e);
            }
            for (const auto& [bad, says] : std::vector<std::pair<const char*, const char*>>{
                     { R"({"filters": [{"field": "Area", "op": "contains", "value": "1"}]})", "text" },
                     { R"({"filters": [{"field": "Area", "op": "greater", "value": "big"}]})", "area" },
                     { R"({"filters": [{"field": "Area", "op": "near", "value": 1}]})", "near" },
                     { R"({"filters": [{"field": "Colour", "op": "has-value"}]})", "Colour" },
                     { R"({"sort": ["Colour"]})", "Colour" },
                     { R"({"grouped": "yes"})", "grouped" },
                     { R"({"category": "doors"})", "rooms" } })
            {
                Json p = Json::parse(bad);
                p["name"] = "Costs";
                const std::string e = Refused(s, "schedule", p);
                Check(Contains(e, says), std::string("明细表：拒绝 ") + bad + "：" + e);
            }
            Check(Contains(Refused(s, "schedule", R"({"name": "Nowhere"})"), "category"), "明细表：新的要类别");
            Check(Contains(Refused(s, "schedule", R"({"name": "S", "category": "clouds"})"), "clouds"), "明细表：没有的类别");
        }
        {
            // 视图过滤器：挑出标记以 D 开头的门，平面里隐藏它们；改成覆盖为红色
            Session s;
            Flat(s);
            Json r = Run(s, "filter", R"({"name": "Marked Doors", "categories": ["doors"], "rules": [{"field": "Mark", "op": "begins-with", "value": "d"}]})");
            Check(r["matches"].size() == 1, "过滤器：挑出一扇门：" + r.dump());
            const Id d2 = r["matches"][0].get<Id>();
            Check(Has(Drawn(s, level1), d2), "过滤器：套用前画");
            Run(s, "graphics", R"({"view": "Level 1", "filters": [{"filter": "Marked Doors", "hidden": true}]})");
            Check(!Has(Drawn(s, level1), d2), "过滤器：套用后不画它");
            r = Run(s, "graphics", R"({"view": "Level 1", "filters": [{"filter": "Marked Doors", "projection": {"color": [255, 0, 0]}}]})");
            Check(r["filters"].size() == 1 && Has(Drawn(s, level1), d2), "过滤器：改成覆盖后又画");
            Check(Contains(Refused(s, "graphics", R"({"view": "Level 1", "filters": [{"filter": "Nope"}]})"), "Nope"), "过滤器：没有的过滤器");
            Check(Contains(Refused(s, "filter", R"({"name": "X", "categories": ["clouds"]})"), "clouds"), "过滤器：没有的类别");
            Check(Contains(Refused(s, "filter", R"({"name": "X", "categories": ["rooms"], "rules": [{"field": "Mark", "op": "equals", "value": "1"}]})"), "Mark"),
                  "过滤器：规则要是类别的字段");
            Check(Contains(Refused(s, "filter", R"({"name": "X"})"), "categories"), "过滤器：新的要类别");
            // 项目文件往返
            const std::string path = Temp("filters.bim");
            Run(s, "save", Json{ { "path", path } });
            Session t;
            Run(t, "open", Json{ { "path", path } });
            Check(t.GetModel().viewFilters == s.GetModel().viewFilters, "过滤器：项目文件往返");
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())));
            // 删掉后视图不再套用
            Run(s, "filter", R"({"name": "Marked Doors", "delete": true})");
            r = Run(s, "graphics", R"({"view": "Level 1"})");
            Check(r["filters"].empty(), "过滤器：删掉后视图不再套用");
        }
        {
            // 视图样板：由 Level 1 做出，Level 2 跟随它；控制的设置不能自己改
            Session s;
            Flat(s);
            Run(s, "graphics", R"({"view": "Level 1", "hide": ["rooms"]})");
            Run(s, "scale", R"({"view": "Level 1", "scale": 50})");
            Json r = Run(s, "template", R"({"name": "Working", "from": "Level 1"})");
            Check(r["hidden"] == Json::array({ "room" }) && r["scale"] == Json(50.0) && r["controls"].size() == 8, "样板：由视图做出：" + r.dump());
            Run(s, "graphics", R"({"view": "Level 2", "template": "Working"})");
            const Model& m = s.GetModel();
            const Guid l2 = m.GuidOf(100001);
            Check(m.ScaleOf(l2) == 50.0, "样板：跟随的比例");
            Check(Run(s, "detail", R"({"view": "Level 2"})")["detail"] == "fine", "样板：详细程度");
            std::string e = Refused(s, "scale", R"({"view": "Level 2", "scale": 20})");
            Check(Contains(e, "Working"), "样板：控制的比例不能自己改：" + e);
            e = Refused(s, "graphics", R"({"view": "Level 2", "hide": ["walls"]})");
            Check(Contains(e, "Working"), "样板：控制的类别不能自己改：" + e);
            e = Refused(s, "modify", R"({"id": 100001, "view_range": {"cut": 1500}})");
            Check(Contains(e, "Working"), "样板：控制的视图范围不能自己改：" + e);
            // 样板改了，跟随的都跟着
            Run(s, "template", R"({"name": "Working", "detail": "coarse", "controls": ["categories", "detail"]})");
            Check(Run(s, "detail", R"({"view": "Level 2"})")["detail"] == "coarse", "样板：改了样板，视图跟着");
            Check(s.GetModel().ScaleOf(l2) == 100.0, "样板：不再控制比例时用自己的");
            Run(s, "scale", R"({"view": "Level 2", "scale": 20})");
            Check(s.GetModel().ScaleOf(l2) == 20.0, "样板：不控制的可以自己改");
            r = Run(s, "template", R"({"name": "Working"})");
            Check(r["views"] == Json::array({ 100001 }), "样板：列出跟随它的视图：" + r.dump());
            Check(Contains(Refused(s, "template", R"({"name": "Working", "hide_elements": [100002]})"), "view's own"), "样板：不收视图自己的");
            Check(Contains(Refused(s, "graphics", R"({"view": "Level 2", "template": "Nope"})"), "Nope"), "样板：没有的样板");
            Check(Contains(Refused(s, "template", R"({"name": "Nope"})"), "from"), "样板：没有时要 from 或 controls");
            // 不再跟随并同时改动可以
            Run(s, "graphics", R"({"view": "Level 2", "template": null, "hide": ["walls"]})");
            Check(Run(s, "detail", R"({"view": "Level 2"})")["detail"] == "fine", "样板：不再跟随");
            // 删掉：跟随它的留着自己的设置
            Run(s, "graphics", R"({"view": "Level 2", "template": "Working"})");
            Run(s, "template", R"({"name": "Working", "delete": true})");
            Check(Run(s, "graphics", R"({"view": "Level 2"})")["template"].is_null(), "样板：删掉后不再跟随");
        }
        {
            // 样板控制视图范围
            Session s;
            Flat(s);
            Run(s, "template", R"({"name": "Low Cut", "controls": ["range"], "view_range": {"cut": 900}})");
            Run(s, "graphics", R"({"view": "Level 1", "template": "Low Cut"})");
            Check(s.GetModel().ViewRangeOf(s.GetModel().GuidOf(level1)).cut == 900.0, "样板：控制视图范围");
            const std::string path = Temp("templates.bim");
            Run(s, "save", Json{ { "path", path } });
            Session t;
            Run(t, "open", Json{ { "path", path } });
            Check(t.GetModel().viewTemplates == s.GetModel().viewTemplates && t.GetModel().ViewRangeOf(t.GetModel().GuidOf(level1)).cut == 900.0,
                  "样板：项目文件往返");
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())));
        }
    }

    void TestViews()
    {
        const Id level1 = 100000, level2 = 100001;
        {
            // 隐藏类别只在那个视图里：房间、轴网不画，墙照旧；Level 2 的平面还画轴网
            Session s;
            const std::vector<Id> walls = House(s);
            const Id room = Run(s, "room", R"({"point": [4000, 3000], "name": "Living"})")["id"].get<Id>();
            const Id grid = Run(s, "grid", R"({"start": [-1000, 3000], "end": [9000, 3000]})")["id"].get<Id>();
            Check(Has(Drawn(s, level1), room) && Has(Drawn(s, level1), grid), "图形：起初都画");
            Json r = Run(s, "graphics", R"({"view": "Level 1", "hide": ["rooms", "Grids"]})");
            Check(r["hidden"] == Json::array({ "grid", "room" }), "图形：隐藏的类别：" + r.dump());
            Render::Drawing d = Drawn(s, level1);
            Check(!Has(d, room) && !Has(d, grid) && Has(d, walls[0]), "图形：房间与轴网不画，墙照旧");
            Check(Has(Drawn(s, level2), grid), "图形：Level 2 的平面是它自己的视图");
            r = Run(s, "graphics", Json{ { "view", level1 }, { "show", { "room" } } });
            Check(r["hidden"] == Json::array({ "grid" }) && Has(Drawn(s, level1), room), "图形：再显示房间");
            Check(FillAt(Drawn(s, level1), P2{ 4000, 0 }) == Render::Poche, "图形：剖到的墙灰色填充");
            Run(s, "graphics", Json{ { "view", level1 }, { "hide", { "walls" } } });
            Check(FillAt(Drawn(s, level1), P2{ 4000, 0 }) == Render::Paper, "图形：墙隐藏了不留填充");
            // 什么也不改时不留撤销步：撤销一步就回到隐藏墙之前
            Run(s, "graphics", Json{ { "view", level1 }, { "hide", { "walls" } } });
            Run(s, "graphics", Json{ { "view", level1 } });
            Run(s, "undo", Json::object());
            Check(FillAt(Drawn(s, level1), P2{ 4000, 0 }) == Render::Poche, "图形：什么也不改时不留撤销步");
        }
        {
            // 隐藏元素：只少它一个
            Session s;
            const std::vector<Id> walls = House(s);
            const Json r = Run(s, "graphics", Json{ { "view", "Level 1" }, { "hide_elements", { walls[0] } } });
            Check(r["hidden_elements"] == Json::array({ walls[0] }), "图形：隐藏的元素：" + r.dump());
            const Render::Drawing d = Drawn(s, level1);
            Check(!Has(d, walls[0]) && FillAt(d, P2{ 4000, 0 }) == Render::Paper && FillAt(d, P2{ 8000, 3000 }) == Render::Poche, "图形：只少这面墙");
            Run(s, "graphics", Json{ { "view", "Level 1" }, { "show_elements", { walls[0] } } });
            Check(FillAt(Drawn(s, level1), P2{ 4000, 0 }) == Render::Poche, "图形：再显示它");
        }
        {
            // 剖到的与看到的线各按自己的覆盖画；覆盖合并进上一次的，null 去掉一项
            Session s;
            const std::vector<Id> walls = House(s);
            const Id low = Run(s, "wall", R"({"points": [[1000, 1000], [3000, 1000]], "height": 1000})")["ids"][0].get<Id>();
            Render::Drawing d = Drawn(s, level1);
            const auto cutPen = Lines(d, walls[0]).empty() ? 0 : Lines(d, walls[0]).front().pen;
            Check(Any(Lines(d, walls[0]), [](const LineLook& l) { return l.color == Render::Ink && !l.dashed; }), "图形：剖到的墙线起初黑色实线");
            Run(s, "graphics", Json::parse(R"({"view": "Level 1", "categories": {"walls": {
                "cut": {"color": [200, 0, 0], "weight": 6},
                "projection": {"color": "#0000c8", "weight": 3, "dash": "dashed"}}}})"));
            d = Drawn(s, level1);
            const Render::Rgb red{ 200, 0, 0 }, blue{ 0, 0, 200 };
            Check(Any(Lines(d, walls[0]), [&](const LineLook& l) { return l.pen == 6 && l.color == red; }), "图形：剖到的线按 cut 画");
            Check(Any(Lines(d, low), [&](const LineLook& l) { return l.pen == 3 && l.color == blue && l.dashed; }), "图形：看到的线按 projection 画");
            Run(s, "graphics", Json::parse(R"({"view": 100000, "categories": {"walls": {"cut": {"weight": 2}}}})"));
            Check(Any(Lines(Drawn(s, level1), walls[0]), [&](const LineLook& l) { return l.pen == 2 && l.color == red; }), "图形：合并时留着颜色");
            const Json r = Run(s, "graphics", Json::parse(R"({"view": 100000, "categories": {"walls": {"cut": {"color": null}}}})"));
            Check(r["categories"]["wall"]["cut"] == Json{ { "weight", 2 } }, "图形：null 去掉颜色：" + r.dump());
            Run(s, "graphics", Json::parse(R"({"view": 100000, "categories": {"walls": null}})"));
            Check(Any(Lines(Drawn(s, level1), walls[0]), [&](const LineLook& l) { return l.pen == cutPen && l.color == Render::Ink; }), "图形：去掉覆盖回到原样");
        }
        {
            // 元素的覆盖胜过类别的；剖到的填充按自己的颜色一起填；半色调向纸色走一半
            Session s;
            const std::vector<Id> walls = House(s);
            Run(s, "graphics", Json::parse(R"({"view": 100000, "categories": {"walls": {"cut": {"fill": [220, 160, 40]}}}})"));
            Render::Drawing d = Drawn(s, level1);
            const Render::Rgb amber{ 220, 160, 40 }, blue{ 10, 120, 200 };
            Check(FillAt(d, P2{ 4000, 0 }) == amber && FillAt(d, P2{ 8000, 3000 }) == amber, "图形：类别的填充色");
            Run(s, "graphics", Json{ { "view", level1 }, { "elements", { { std::to_string(walls[0]), Json::parse(R"({"cut": {"fill": [10, 120, 200], "color": [0, 200, 0]}})") } } } });
            d = Drawn(s, level1);
            Check(FillAt(d, P2{ 4000, 0 }) == blue && FillAt(d, P2{ 8000, 3000 }) == amber, "图形：元素自己的胜过类别的");
            Check(Any(Lines(d, walls[0]), [](const LineLook& l) { return l.color == Render::Rgb{ 0, 200, 0 }; }) &&
                      !Any(Lines(d, walls[1]), [](const LineLook& l) { return l.color == Render::Rgb{ 0, 200, 0 }; }),
                  "图形：元素的线色只改它自己");
            size_t poche = 0;
            for (size_t i = 0; i < d.prims.size(); ++i)
                poche += !d.owners[i] && d.prims[i].kind == Render::Prim::Kind::Fill && d.prims[i].color == Render::Poche;
            Check(poche == 0, "图形：合在一起的填充也是覆盖的颜色");
            Session h;
            const std::vector<Id> hw = House(h);
            Run(h, "graphics", Json::parse(R"({"view": 100000, "categories": {"walls": {"halftone": true}}})"));
            d = Drawn(h, level1);
            Check(FillAt(d, P2{ 4000, 0 }) == Render::Halftone(Render::Poche) &&
                      Any(Lines(d, hw[0]), [](const LineLook& l) { return l.color == Render::Halftone(Render::Ink); }),
                  "图形：半色调");
        }
        {
            // 默认三维视图：第一次改它的图形时建出来；它隐藏的三维里不画；平面是平面自己的
            Session s;
            const std::vector<Id> walls = House(s);
            Json r = Run(s, "graphics", R"({"view": "{3D}"})");
            Check(r["view"].is_null() && !s.GetModel().Default3D(), "三维：只问不建");
            r = Run(s, "graphics", R"({"view": "{3D}", "hide": ["walls"]})");
            Check(r["name"] == "{3D}" && s.GetModel().Default3D().has_value(), "三维：改了才建：" + r.dump());
            Check(!s.GetModel().LookOf3D().Shows(walls[0]) && Has(Drawn(s, level1), walls[0]), "三维：隐藏墙，平面照旧");
            r = Run(s, "visual_style", R"({"style": "Hidden Line"})");
            Check(r["style"] == "hidden_line", "三维：视觉样式：" + r.dump());
            Check(Contains(Refused(s, "visual_style", R"({"view": "Level 1", "style": "shaded"})"), "isn't a 3D view"), "三维：平面没有视觉样式");
        }
        {
            // 比例与详细程度：每个平面都有比例，null 不行；问时只回答
            Session s;
            House(s);
            Json r = Run(s, "scale", R"({"view": "Level 1"})");
            Check(r["scale"] == 100.0, "比例：默认 1 : 100：" + r.dump());
            r = Run(s, "scale", R"({"view": "Level 1", "scale": 50})");
            Check(r["scale"] == 50.0 && s.GetModel().ScaleOf(s.GetModel().GuidOf(level1)) == 50.0, "比例：改成 1 : 50");
            Check(Contains(Refused(s, "scale", R"({"view": "Level 1", "scale": null})"), "always has a scale"), "比例：null 不行");
            Check(Contains(Refused(s, "scale", R"({"view": "Level 1", "scale": 0})"), "from 1 to 100000"), "比例：0 不行");
            r = Run(s, "detail", R"({"view": "Level 1", "detail": "Coarse"})");
            Check(r["detail"] == "coarse", "详细程度：粗略：" + r.dump());
            Check(Contains(Refused(s, "detail", R"({"view": "Level 1", "detail": "rough"})"), "coarse, medium or fine"), "详细程度：拼错");
        }
        {
            // 说不通的图形被拒，说明要改什么
            Session s;
            House(s);
            Check(Contains(Refused(s, "graphics", R"({"hide": ["ducts"]})"), "isn't a category views draw"), "图形：不画的类别");
            Check(Contains(Refused(s, "graphics", R"({"categories": {"wall": {"cut": {"weight": 17}}}})"), "a pen, 0 (a hairline) to 16"), "图形：笔号超出");
            Check(Contains(Refused(s, "graphics", R"({"categories": {"wall": {"transparency": 101}}})"), "a percent"), "图形：透明度超出");
            Check(Contains(Refused(s, "graphics", R"({"categories": {"wall": {"cut": {"color": "red"}}}})"), "isn't a colour"), "图形：颜色写错");
            Check(Contains(Refused(s, "graphics", R"({"categories": {"wall": {"cut": {"foreground": {"pattern": "Nope"}}}}})"), "no fill pattern"), "图形：没有的图案");
            Check(Contains(Refused(s, "graphics", R"({"phase": "New Construction"})"), "aren't in MiniBIM yet"), "图形：阶段还没有");
            const Id pv = Run(s, "duplicate_view", R"({"view": "Level 1"})")["id"].get<Id>();
            Check(Contains(Refused(s, "graphics", Json{ { "hide_elements", { pv } } }), "views don't draw those"), "图形：平面视图不是视图画的元素");
        }
        {
            // 复制视图：同一标高的平面视图，按源视图起名并编号；名称不能与标高、别的视图重
            Session s;
            House(s);
            const Json a = Run(s, "duplicate_view", R"({"view": "Level 1"})");
            const Json b = Run(s, "DP", R"({"view": "Level 1"})");
            Check(a["name"] == "Level 1 Copy 1" && b["name"] == "Level 1 Copy 2", "复制视图：名称：" + a.dump() + b.dump());
            const Model& m = s.GetModel();
            Check(m.PlanLevel(m.GuidOf(a["id"].get<Id>())) == m.GuidOf(level1) && m.PlanViews(m.GuidOf(level1)).size() == 2, "复制视图：同一标高");
            Run(s, "duplicate_view", R"({"view": "Level 1", "name": "Plumbing"})");
            for (const char* taken : { "Level 2", "Plumbing" })
                Check(Contains(Refused(s, "duplicate_view", Json{ { "view", "Level 1" }, { "name", taken } }), "already"), std::string("复制视图：名称已有：") + taken);
            const Json again = Run(s, "duplicate_view", R"({"view": "Plumbing"})");
            Check(again["name"] == "Plumbing Copy 1", "复制视图：平面视图的复制");
            Check(Contains(Refused(s, "duplicate_view", R"({"view": "Level 1", "mode": "twin"})"), "duplicate, detailing or dependent"), "复制视图：方式三选一");
            Check(Contains(Refused(s, "duplicate_view", R"({"view": "No such view"})"), "no view named"), "复制视图：没有这个视图");
        }
        {
            // 复制的视图起初有源视图的视图范围、裁剪与图形，之后各管各的
            Session s;
            const std::vector<Id> walls = House(s);
            const Id door = Run(s, "door", Json{ { "wall", walls[0] }, { "offset", 2000 } })["id"].get<Id>();
            Run(s, "graphics", R"({"view": "Level 1", "hide": ["door"]})");
            Run(s, "crop", R"({"view": "Level 1", "region": [[-500, -500], [8500, 6500]]})");
            Run(s, "modify", Json{ { "id", level1 }, { "view_range", { { "cut", 1500 } } } });
            const Id copy = Run(s, "duplicate_view", R"({"view": "Level 1"})")["id"].get<Id>();
            const Model* m = &s.GetModel();
            const Guid cg = m->GuidOf(copy), lg = m->GuidOf(level1);
            Check(m->ViewRangeOf(cg).cut == 1500.0 && m->CropOf(cg) == m->CropOf(lg) && !m->LookOf(cg).Shows(door), "复制视图：起初同源视图");
            Run(s, "graphics", R"({"view": "Level 1 Copy 1", "hide": ["wall"]})");
            Run(s, "modify", Json{ { "id", copy }, { "view_range", { { "cut", 900 } } } });
            m = &s.GetModel();
            Check(!m->LookOf(cg).Shows(walls[0]) && m->LookOf(lg).Shows(walls[0]) && m->ViewRangeOf(cg).cut == 900.0 && m->ViewRangeOf(lg).cut == 1500.0,
                  "复制视图：之后各管各的");
            Run(s, "crop", R"({"view": "Level 1 Copy 1", "region": null})");
            m = &s.GetModel();
            Check(!m->CropOf(cg) && m->CropOf(lg), "复制视图：裁剪也各自的");
            // 裁剪了的平面：框外的墙线裁掉，房间、注释整个留着
            const Render::Drawing cropped = Drawn(s, level1);
            const Render::Px lo = cropped.view.ToPx(P2{ -500, 6500 }), hi = cropped.view.ToPx(P2{ 8500, -500 });
            bool inside = true;
            for (size_t i = 0; i < cropped.prims.size(); ++i)
                for (const Render::Px& q : cropped.prims[i].pts)
                    inside = inside && q.first >= lo.first - 0.5f && q.first <= hi.first + 0.5f && q.second >= lo.second - 0.5f && q.second <= hi.second + 0.5f;
            Check(inside, "裁剪：画的都在框里");
            Run(s, "crop", R"({"view": "Level 1", "region": null})");
            const Json c = Run(s, "crop", R"({"view": "Level 1"})");
            Check(c["crop"].is_array() && c["crop"][0][0].get<double>() < -1000.0, "裁剪：不给区域时裁到显示的四周留 1 米：" + c.dump());
        }
        {
            // 注释在它画在的视图里，只在那里；全部标记按视图
            Session s;
            const std::vector<Id> walls = House(s);
            const Id door = Run(s, "door", Json{ { "wall", walls[0] }, { "offset", 2000 } })["id"].get<Id>();
            const Id copy = Run(s, "duplicate_view", R"({"view": "Level 1"})")["id"].get<Id>();
            std::vector<std::pair<Id, std::vector<Id>>> made;
            for (const auto& [view, y] : { std::pair{ "Level 1", -2000.0 }, std::pair{ "Level 1 Copy 1", -3000.0 } })
            {
                const Id dim = Run(s, "dimension", Json{ { "level", view }, { "refs", { Pt(4000, 0), Pt(4000, 6000) } }, { "at", Pt(9000, y) } })["id"].get<Id>();
                const Id tag = Run(s, "tag", Json{ { "ids", { door } }, { "level", view } })["ids"][0].get<Id>();
                const Id note = Run(s, "text", Json{ { "text", view }, { "at", Pt(1000, y) }, { "view", view } })["id"].get<Id>();
                const Id spot = Run(s, "spot", Json{ { "at", Pt(2000, 2000) }, { "view", view }, { "host", walls[0] } })["id"].get<Id>();
                made.push_back({ std::string(view) == "Level 1" ? level1 : copy, { dim, tag, note, spot } });
            }
            const Render::Drawing inLevel = Drawn(s, level1), inCopy = Drawn(s, copy);
            bool right = true;
            for (const auto& [view, ids] : made)
                for (const Id i : ids)
                    right = right && Has(inLevel, i) == (view == level1) && Has(inCopy, i) == (view == copy);
            Check(right, "平面视图：注释只在它的视图里");
            Check(ElementOf(s, made[0].second[1])["view"].is_null() && !ElementOf(s, made[1].second[1])["view"].is_null(), "平面视图：标记记着它的视图");
            Session t;
            const std::vector<Id> tw = House(t);
            Run(t, "door", Json{ { "wall", tw[0] }, { "offset", 2000 } });
            Run(t, "duplicate_view", R"({"view": "Level 1"})");
            Run(t, "tag", R"({"level": "Level 1"})");
            const Json r = Run(t, "tag", R"({"level": "Level 1 Copy 1"})");
            Check(r["ids"].size() == 1 && Contains(Refused(t, "tag", R"({"level": "Level 1 Copy 1"})"), "nothing to tag"), "平面视图：全部标记按视图");
        }
        {
            // 带细节复制的复制注释，普通的不复制
            Session s;
            const std::vector<Id> walls = House(s);
            Run(s, "door", Json{ { "wall", walls[0] }, { "offset", 2000 } });
            Run(s, "text", R"({"text": "note", "at": [1000, 1000], "view": "Level 1"})");
            Run(s, "tag", R"({"level": "Level 1"})");
            const auto notes = [&] { return Count(s, "text") + Count(s, "tag"); };
            const uint64_t before = notes();
            Check(Run(s, "duplicate_view", R"({"view": "Level 1"})")["annotations"] == 0 && notes() == before, "带细节复制：普通复制不复制注释");
            const Json det = Run(s, "duplicate_view", R"({"view": "Level 1", "mode": "detailing"})");
            Check(det["annotations"] == 2 && notes() == before + 2, "带细节复制：复制文字与标记：" + det.dump());
        }
        {
            // 相关视图：跟着父视图的视图范围与图形，自己不能改；有自己的裁剪；显示父视图的注释
            Session s;
            const std::vector<Id> walls = House(s);
            const Id door = Run(s, "door", Json{ { "wall", walls[0] }, { "offset", 2000 } })["id"].get<Id>();
            const Id parentNote = Run(s, "text", R"({"text": "parent's", "at": [1000, 1000], "view": "Level 1"})")["id"].get<Id>();
            const Json dep = Run(s, "duplicate_view", R"({"view": "Level 1", "mode": "dependent"})");
            Check(dep["name"] == "Level 1 Dependent 1", "相关视图：名称：" + dep.dump());
            const Id depId = dep["id"].get<Id>();
            Run(s, "graphics", R"({"view": "Level 1", "hide": ["door"]})");
            Run(s, "modify", Json{ { "id", level1 }, { "view_range", { { "cut", 1700 } } } });
            const Model* m = &s.GetModel();
            const Guid dg = m->GuidOf(depId);
            Check(!m->LookOf(dg).Shows(door) && m->ViewRangeOf(dg).cut == 1700.0, "相关视图：跟着父视图");
            for (const auto& [cmd, p] : { std::pair{ "graphics", R"({"view": "Level 1 Dependent 1", "hide": ["wall"]})" },
                                          std::pair{ "scale", R"({"view": "Level 1 Dependent 1", "scale": 50})" },
                                          std::pair{ "detail", R"({"view": "Level 1 Dependent 1", "detail": "coarse"})" } })
            {
                const std::string e = Refused(s, cmd, p);
                Check(Contains(e, "dependent view") && Contains(e, "Level 1"), std::string("相关视图：不能自己改：") + cmd + "：" + e);
            }
            Run(s, "crop", R"({"view": "Level 1 Dependent 1", "region": [[0, 0], [4000, 3000]]})");
            m = &s.GetModel();
            Check(m->CropOf(dg) && !m->CropOf(m->GuidOf(level1)), "相关视图：自己的裁剪");
            const Id mine = Run(s, "text", R"({"text": "dependent's", "at": [1500, 1500], "view": "Level 1 Dependent 1"})")["id"].get<Id>();
            const Render::Drawing inDep = Drawn(s, depId), inParent = Drawn(s, level1);
            Check(Has(inDep, parentNote) && Has(inDep, mine) && Has(inParent, parentNote) && !Has(inParent, mine), "相关视图：显示父视图的注释");
            Check(Contains(Refused(s, "duplicate_view", R"({"view": "Level 1 Dependent 1", "mode": "dependent"})"), "dependent view"),
                  "相关视图：没有相关视图的相关视图");
        }
        {
            // 删除：视图带走它的注释与相关视图，标高带走它的平面视图
            Session s;
            const std::vector<Id> walls = House(s);
            Run(s, "door", Json{ { "wall", walls[0] }, { "offset", 2000 } });
            const Id copy = Run(s, "duplicate_view", R"({"view": "Level 1"})")["id"].get<Id>();
            Run(s, "text", R"({"text": "in the copy", "at": [1000, 1000], "view": "Level 1 Copy 1"})");
            Run(s, "tag", R"({"level": "Level 1 Copy 1"})");
            const Id dep = Run(s, "duplicate_view", R"({"view": "Level 1 Copy 1", "mode": "dependent"})")["id"].get<Id>();
            Run(s, "level", R"({"elevation": 9000})");
            const Id upper = Run(s, "duplicate_view", R"({"view": "Level 3"})")["id"].get<Id>();
            Run(s, "delete", Json{ { "ids", { copy } } });
            Check(ElementOf(s, dep).is_null() && Count(s, "text") + Count(s, "tag") == 0 && !ElementOf(s, upper).is_null(), "删除：视图带走相关视图与注释");
            const Id third = s.GetModel().Levels()[2].first;
            Run(s, "delete", Json{ { "ids", { third } } });
            Check(ElementOf(s, upper).is_null() && Count(s, "planview") == 0, "删除：标高带走它的平面视图");
        }
        {
            // 存了再打开一模一样：视图的图形、平面视图、三维视图
            Session s;
            const std::vector<Id> walls = House(s);
            Run(s, "graphics", Json::parse(R"({"view": "Level 1", "hide": ["room"], "categories": {"wall": {"cut": {"color": "#c80000", "weight": 6, "dash": "dotted"}, "halftone": true}}})"));
            Run(s, "graphics", Json{ { "view", "Level 1" }, { "hide_elements", { walls[1] } } });
            Run(s, "scale", R"({"view": "Level 1", "scale": 50})");
            Run(s, "detail", R"({"view": "Level 1", "detail": "medium"})");
            Run(s, "duplicate_view", R"({"view": "Level 1", "mode": "dependent"})");
            Run(s, "crop", R"({"view": "Level 1 Dependent 1", "region": [[0, 0], [4000, 3000]]})");
            Run(s, "graphics", R"({"view": "{3D}", "hide": ["walls"]})");
            Run(s, "visual_style", R"({"style": "wireframe"})");
            const std::string path = Temp("views.minibim");
            Run(s, "save", Json{ { "path", path } });
            Session t;
            Run(t, "open", Json{ { "path", path } });
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
            Check(t.GetModel().ToJson() == s.GetModel().ToJson(), "视图：存了再打开一模一样");
            const Json lj = ElementOf(t, level1);
            Check(lj["graphics"]["scale"] == 50.0 && lj["graphics"]["detail"] == "medium" && lj["graphics"]["categories"]["wall"]["cut"]["dash"] == "dotted",
                  "视图：图形写进文件：" + lj["graphics"].dump());
        }
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
    TestBimcraft0733();
    TestColumns();
    TestStructure();
    TestMep();
    TestMassing();
    TestAnnotations();
    TestViews();
    TestSchedules();
    std::printf("\n%s：%d 项失败\n", g_failed == 0 ? "全部通过" : "有失败", g_failed);
    return g_failed == 0 ? 0 : 1;
}
