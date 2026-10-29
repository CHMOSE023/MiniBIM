// 命令与会话（src/Commands）：像前端一样经命令驱动模型——撤销、脚本、事务、建模、编辑、项目文件，以及示例房屋。
// 移植自 BIMCraft crates/core/src/tests.rs、sample.rs 的测试（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md。
// 用到还没移植的命令（房间、屋顶、剖面、阶段……）的断言先略去，移植那些命令时补回
#include "Commands/Commands.h"
#include "Commands/Sample.h"
#include "Commands/Session.h"
#include "Model/ModelJson.h"
#include "Model/Walls.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
            Run(s, "wall", R"({"level": "Level 2", "points": [[8000, 0], [8000, 6000]]})");
            const Json joined = Run(s, "floor", R"({"level": "Level 2"})");
            Check(open && own && ElementOf(s, joined["id"].get<Id>()).count("openings") == 0 && joined["area_m2"] == 256.0,
                  "命令：楼板沿墙铺，独立的一圈墙围出天井；与外墙相连的不算");
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
            const Id door = Run(s, "door", R"({"at": [2000, 0]})")["id"].get<Id>();
            const Id window = Run(s, "window", R"({"at": [8000, 1500]})")["id"].get<Id>();
            Run(s, "MV", Json{ { "ids", { ids[1] } }, { "by", { 1000, 0 } } });
            const bool ok = Ends(s, ids[1]) == std::pair{ Pt(9000, 0), Pt(9000, 6000) } && Ends(s, ids[0]).second == Pt(9000, 0) &&
                            Ends(s, ids[2]).first == Pt(9000, 6000) && Ends(s, part).second == Pt(9000, 3000) && ElementOf(s, door)["at"] == Pt(2000, 0) &&
                            ElementOf(s, window)["at"] == Pt(9000, 1500);
            // 上一层正上方的墙不与这些相接
            const Id up = Run(s, "wall", R"({"points": [[9000, 6000], [9000, 9000]], "level": "Level 2", "height": 3000})")["ids"][0].get<Id>();
            Run(s, "move", Json{ { "ids", { ids[1] } }, { "by", { -1000, 0 } } });
            Check(ok && Ends(s, up).first == Pt(9000, 6000), "命令：移动一面墙，相接的墙跟着伸缩，墙上的门窗留在原处或随墙走");
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
            // BIMCraft 的项目文件：还没移植的东西（族、阶段、屋顶、带族的门、贴着屋顶）略去并列出，其余照常打开
            Session s;
            House(s);
            Run(s, "door", R"({"at": [2000, 0]})");
            std::string text, error;
            s.GetModel().ToFile(text, error);
            Json file = Json::parse(text);
            file["families"] = Json::parse(R"({"Single Door": {"category": "door"}})");
            file["phases"] = Json::array({ "Existing", "New Construction" });
            file["types"]["roof"] = Json::parse(R"({"Generic - 300mm": {"thickness": 300}})");
            file["materials"]["Brick"]["texture"] = Json::parse(R"({"kind": "brick"})");
            file["elements"]["100100"] = Json::parse(R"({"guid": "3f2a9c1e-1111-4000-8000-000000000001", "category": "roof", "type": "Generic - 300mm"})");
            file["elements"]["100101"] = Json::parse(R"({"category": "door", "type": "900 x 2100", "family": "Single Door", "host": 100002, "offset": 6000, "sill": 0, "mark": "9", "flip_facing": false, "flip_hand": false})");
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
            for (const Json& v : r["left_out"])
                left.push_back(v.get<std::string>());
            auto has = [&](const char* p) { return std::find(left.begin(), left.end(), p) != left.end(); };
            const bool listed = has("/families") && has("/phases") && has("/types/roof") && has("/materials/Brick/texture") && has("/elements/100100") &&
                                has("/elements/100101") && has("/elements/100003/attach_top") && has("/elements/100003/params");
            std::string why;
            const bool opened = !r.is_null() && t.GetModel().Check(why) && Count(t, "wall") == 4 && Count(t, "door") == 1 &&
                                !t.GetModel().GetWall(Id(100003))->attachTop && !t.GetModel().library.IdOf(Kind::Of(Kind::Tag::Phase), "Existing");
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
    TestSample();
    TestHostileParams();
    std::printf("\n%s：%d 项失败\n", g_failed == 0 ? "全部通过" : "有失败", g_failed);
    return g_failed == 0 ? 0 : 1;
}
