// 模型核心（src/Model）：平面几何、单位、ID 与 GUID、项目语言、元素与 Model::Check、墙的几何、库、屋顶的直骨架、族与 CSG。
// 移植自 BIMCraft crates/core/src/{geom,units,ids,language,model,walls,walls/index,library,skeleton,family,csg}.rs 的测试（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Commands/Commands.h"
#include "Commands/Sample.h"
#include "Commands/Session.h"
#include "Model/Csg.h"
#include "Model/Family.h"
#include "Model/Font.h"
#include "Model/Geom.h"
#include "Model/Language.h"
#include "Model/Model.h"
#include "Model/ModelJson.h"
#include "Model/Render.h"
#include "Model/Skeleton.h"
#include "Model/Snap.h"
#include "Model/Sketch.h"
#include "Model/Units.h"
#include "Model/Walls.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

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

    bool Near(double a, double b, double tol) { return std::abs(a - b) <= tol; }
    bool NearP(P2 a, P2 b) { return a.Dist(b) < 1e-6; }
    bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // 跑一段脚本的会话；失败时打印原因
    Session Scripted(const std::string& script)
    {
        Session s;
        std::vector<Json> out;
        std::string error;
        if (!s.Script(script, out, error))
            std::printf("  脚本失败：%s\n", error.c_str());
        return s;
    }

    // ── geom.rs ──────────────────────────────────────────────────
    std::vector<Segment> Rect(double x0, double y0, double x1, double y1)
    {
        const P2 a(x0, y0), b(x1, y0), c(x1, y1), d(x0, y1);
        return { { a, b }, { b, c }, { c, d }, { d, a } };
    }

    std::vector<double> Areas(const std::vector<Segment>& segs)
    {
        std::vector<double> a;
        for (const Ring& f : Geom::Faces(segs, 1.0))
            a.push_back(Geom::Area(f));
        std::sort(a.begin(), a.end());
        return a;
    }

    void TestGeom()
    {
        const Ring l = { P2(0, 0), P2(4000, 0), P2(4000, 2000), P2(2000, 2000), P2(2000, 5000), P2(0, 5000) };
        {
            const Ring g = Geom::Grow(l, 100.0);
            Check(g[0] == P2(-100, -100) && g[3] == P2(2100, 2100), "几何：环向外扩，内角也外移");
            // d 乘周长，再加外角数减内角数个 d²：5 减 1
            Check(Near(Geom::Area(g) - Geom::Area(l) - 100.0 * Geom::Perimeter(l) - 4.0 * 100.0 * 100.0, 0.0, 1e-6), "几何：外扩后的面积");
        }
        {
            const Ring sq = { P2(0, 0), P2(4, 0), P2(4, 4), P2(0, 4) };
            Check(Geom::Area(Geom::Clip(sq, [](P2 p) { return 1.0 - p.x; })) == 4.0, "几何：按线性函数裁剪");
            Check(Geom::Clip(sq, [](P2 p) { return -1.0 - p.x; }).empty() && Geom::Clip(sq, [](P2 p) { return 9.0 - p.x; }) == sq, "几何：全裁掉 / 全保留");
        }
        {
            const auto tris = Geom::Triangulate(l);
            double sum = 0.0;
            bool positive = true;
            for (const auto& t : tris)
            {
                const double a = Geom::Area({ l[t[0]], l[t[1]], l[t[2]] });
                positive = positive && a > 0.0;
                sum += a;
            }
            Check(tris.size() == 4 && positive && Near(sum, Geom::Area(l), 1e-6), "几何：凹多边形剖分成三角形，面积不变");
            Check(Geom::Triangulate(Ring(l.begin(), l.begin() + 2)).empty(), "几何：两个点围不出三角形");
            const Ring twice = { l[0], l[1], l[2], l[2], l[3], l[4], l[5], l[0] };
            double s2 = 0.0;
            for (const auto& t : Geom::Triangulate(twice))
                s2 += Geom::Area({ twice[t[0]], twice[t[1]], twice[t[2]] });
            Check(Near(s2, Geom::Area(l), 1e-6), "几何：重复的角点算一个");
        }
        {
            const Ring ccw = { P2(0, 0), P2(4, 0), P2(4, 3), P2(0, 3) };
            const Ring cw(ccw.rbegin(), ccw.rend());
            Check(Geom::Area(ccw) == 12.0 && Geom::Area(cw) == -12.0 && Geom::Perimeter(ccw) == 14.0, "几何：逆时针面积为正");
            const Ring sq = { P2(0, 0), P2(4, 0), P2(4, 4), P2(0, 4) };
            Check(Geom::Contains(sq, P2(2, 2)) && !Geom::Contains(sq, P2(5, 2)) && !Geom::Contains(sq, P2(-1, 2)), "几何：奇偶规则判断点在环内");
            Check(Geom::DistToSegment(P2(5, 3), P2(0, 0), P2(10, 0)) == 3.0 && Geom::DistToSegment(P2(13, 4), P2(0, 0), P2(10, 0)) == 5.0,
                  "几何：到线段的距离夹在两端之间");
            const auto hit = Geom::LineIntersection(P2(0, 3000), P2(8000, 3000), P2(9000, 0), P2(9000, 6000));
            Check(hit && *hit == P2(9000, 3000) && !Geom::LineIntersection(P2(0, 0), P2(1, 0), P2(0, 1), P2(5, 1)), "几何：直线交点，平行的没有");
        }
        Check(Areas(Rect(0, 0, 4000, 3000)) == std::vector<double>{ -12e6, 12e6 }, "几何：闭合的环有内外两个面");
        {
            auto segs = Rect(0, 0, 6000, 4000);
            segs.push_back({ P2(2000, 0), P2(2000, 4000) });
            Check(Areas(segs) == std::vector<double>{ -24e6, 8e6, 16e6 }, "几何：隔墙在 T 形相接处分开房间");
            segs.push_back({ P2(-500, 2000), P2(6500, 2000) });
            std::vector<double> rooms;
            for (double a : Areas(segs))
                if (a > 0.0)
                    rooms.push_back(a);
            Check(rooms == std::vector<double>{ 4e6, 4e6, 8e6, 8e6 }, "几何：十字相交也分开房间");
        }
        {
            auto segs = Rect(0, 0, 4000, 3000);
            segs.push_back({ P2(4000, 1500), P2(3000, 1500) });
            const auto r = Geom::RegionAt(segs, P2(1000, 1000), 1.0);
            Check(r && r->Area() == 12e6, "几何：房间里的墙垛不改变面积");
        }
        {
            auto segs = Rect(0, 0, 10000, 10000);
            for (const auto& s : Rect(4000, 4000, 6000, 6000))
                segs.push_back(s);
            for (const auto& s : Rect(4500, 4500, 5500, 5500))
                segs.push_back(s);
            const auto hall = Geom::RegionAt(segs, P2(1000, 1000), 1.0);
            Check(hall && hall->holes.size() == 1 && hall->Area() == 96e6 && hall->Perimeter() == 48000.0, "几何：区域减去最外层的独立岛");
            const auto shaft = Geom::RegionAt(segs, P2(4200, 4200), 1.0);
            Check(shaft && shaft->Area() == 3e6 && !Geom::RegionAt(segs, P2(-10, 5), 1.0), "几何：管井里的区域；外面没有区域");
        }
        {
            auto segs = Rect(0, 0, 10000, 10000);
            for (const auto& s : Rect(4000, 4000, 6000, 6000))
                segs.push_back(s);
            segs.push_back({ P2(0, 5000), P2(3000, 5000) });
            const auto all = Geom::Faces(segs, 1.0);
            bool same = true;
            for (const P2 p : { P2(1000, 1000), P2(7000, 2000), P2(4200, 4200), P2(-10, 5) })
                same = same && Geom::RegionIn(all, p, 1.0) == Geom::RegionAt(segs, p, 1.0);
            Check(same, "几何：算好的面回答每个点都与重算的一样");
        }
        {
            const std::vector<Segment> segs = { { P2(0, 0), P2(4000, 0) }, { P2(4000.4, 0.3), P2(4000, 3000) }, { P2(4000, 3000), P2(0, 3000) },
                                                { P2(0, 3000), P2(0.2, -0.2) } };
            const auto r = Geom::RegionAt(segs, P2(2000, 1500), 1.0);
            Check(r && std::abs(r->Area() - 12e6) < 3000.0, "几何：容差内的墙端照样围合房间");
        }
    }

    // ── units.rs ─────────────────────────────────────────────────
    void TestUnits()
    {
        using namespace UnitsOf;
        const Units m = Units::Millimetres, ft = Units::FeetInches;
        bool ok = Scale(m, 100.0) == "1 : 100" && Scale(m, 96.0) == "1 : 96";
        const std::pair<double, const char*> named[] = {
            { 1.0, "12\" = 1'-0\"" },   { 8.0, "1 1/2\" = 1'-0\"" }, { 12.0, "1\" = 1'-0\"" },   { 16.0, "3/4\" = 1'-0\"" },
            { 48.0, "1/4\" = 1'-0\"" }, { 64.0, "3/16\" = 1'-0\"" }, { 96.0, "1/8\" = 1'-0\"" }, { 128.0, "3/32\" = 1'-0\"" },
            { 384.0, "1/32\" = 1'-0\"" }, { 120.0, "1\" = 10'-0\"" }, { 240.0, "1\" = 20'-0\"" }, { 1200.0, "1\" = 100'-0\"" },
            { 100.0, "1 : 100" },        { 50.0, "1 : 50" },
        };
        for (const auto& [n, name] : named)
            ok = ok && Scale(ft, n) == name && ParseScale(ft, name) == n;
        for (const double n : Scales(Units::Inches))
            ok = ok && Scale(Units::Inches, n).rfind("1 :", 0) != 0;
        ok = ok && DefaultScale(m) == 100.0 && DefaultScale(ft) == 96.0;
        ok = ok && ParseScale(m, "75") == 75.0 && ParseScale(m, " 1 : 250 ") == 250.0 && ParseScale(m, "1 mm = 2 m") == 2000.0;
        ok = ok && ParseScale(ft, "1/4\" = 1'") == 48.0 && ParseScale(ft, "1\" = 30'") == 360.0;
        for (const char* bad : { "", "big", "2 : 100", "0", "-5", "1/8\" = 0'", "1 : 0" })
            ok = ok && !ParseScale(m, bad);
        Check(ok, "单位：比例按单位的写法命名，也能读回");

        Check(Length(Units::Millimetres, 3000.4) == "3000" && Length(Units::Centimetres, 3005.0) == "300.5" && Length(Units::Metres, 2950.0) == "2.95" &&
                  Length(Units::Inches, 3000.0) == "118 1/8\"" && Length(Units::Inches, 12.7) == "1/2\"" && Length(Units::Feet, 3000.0) == "9.84'" &&
                  Length(ft, 3000.0) == "9'-10 1/8\"" && Length(ft, 304.8) == "1'-0\"" && Length(ft, -457.2) == "-1'-6\"" && Length(ft, 0.1) == "0'-0\"",
              "单位：长度按项目单位显示，舍入成 0 的不带负号");
        const auto area = Area(ft, 18e6);
        Check(WithUnit(Units::Metres, 3000.0) == "3.00 m" && WithUnit(ft, 3000.0) == "9'-10 1/8\"" && Near(area.first, 193.75, 0.005) &&
                  std::string(area.second) == "ft²", "单位：带单位的长度与面积");

        ok = Parse(m, "3000") == 3000.0 && Parse(m, " 3 m") == 3000.0 && Parse(Units::Metres, "3.2") == 3200.0 && Parse(Units::Metres, "250mm") == 250.0 &&
             Parse(Units::Centimetres, "12.5") == 125.0 && Parse(ft, "10' 6\"") == 3200.4 && Parse(ft, "10'-6 1/2\"") == 3213.1 && Parse(ft, "10") == 3048.0 &&
             Parse(ft, "-1'-6\"") == -457.2 && Parse(ft, "2.5ft") == 762.0 && Parse(m, "6\"") == 152.4 && Parse(Units::Inches, "6 1/2") == 165.1 &&
             Parse(Units::Inches, "30in") == 762.0;
        for (const char* bad : { "", "x", "1/0\"", "3 m m", "1e999" })
            ok = ok && !Parse(ft, bad);
        for (Units u : All)
        {
            const auto back = Parse(u, Length(u, 3213.1));
            ok = ok && back && std::abs(*back - 3213.1) <= 5.0;
        }
        Check(ok, "单位：输入的长度按项目单位或自带的单位读，显示的能读回");
    }

    // ── ids.rs ───────────────────────────────────────────────────
    void TestIds()
    {
        const Guid g = Guid::New();
        const std::string s = g.ToString();
        std::string upper = s;
        for (char& ch : upper)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        Guid back;
        std::string why;
        bool ok = s.size() == 36 && s[14] == '4' && std::string("89ab").find(s[19]) != std::string::npos && Guid::Parse(s) == g &&
                  Guid::Parse(upper) == g && Guid::Parse("{" + s + "}") == g && FromJson(ToJson(g), back, why) && back == g &&
                  Guid::New() != g && !g.Pending();
        for (const std::string bad : { std::string(), std::string("x"), std::string("00000000-0000-0000-0000-000000000005"),
                                       std::string("zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz"), s.substr(1) })
            ok = ok && !Guid::Parse(bad);
        Check(ok && Guid::Parse("1234") == Guid::OfId(1234), "ID：GUID 是第 4 版 UUID，写出的能读回；一串数字是 ID");

        Guid read;
        std::string error;
        Check(FromJson(Json(100004), read, error) && read.Pending() == 100004u && ToJson(read) == Json(100004) && !FromJson(Json(-3), read, error) &&
                  !FromJson(Json(2.5), read, error) && !FromJson(Json::array({ 1 }), read, error),
              "ID：要 GUID 的地方写数，就是那个 ID 的元素，直到解析");

        Elements es;
        const Element grid = Grid{ "A", P2(0, 0), P2(0, 1000) };
        es.Insert(FirstId, grid);
        const Guid first = *es.GuidOf(FirstId);
        ok = es.Get(first) && es.Get(FirstId) && es.Get(Guid::OfId(FirstId));
        es.Insert(FirstId, grid);
        ok = ok && es.GuidOf(FirstId) == first;
        es.Insert(FirstId + 1, grid);
        ok = ok && es.GuidOf(FirstId + 1) != first;
        ok = ok && !es.InsertAs(FirstId + 2, first, grid, error) && Contains(error, "same GUID") && !es.InsertAs(FirstId + 2, Guid::OfId(5), grid, error);
        es.Remove(first);
        Check(ok && !es.Get(first) && !es.Get(FirstId) && es.Size() == 1 && !es.IdOf(first), "ID：元素按 ID 或 GUID 找，修改时保留 GUID");
    }

    // ── language.rs ──────────────────────────────────────────────
    void TestLanguage()
    {
        using namespace LanguageOf;
        Check(Word(Language::Portuguese, "Level") == "Nível" && Word(Language::English, "Level") == "Level" && Word(Language::Portuguese, "Id") == "Id" &&
                  Word(Language::Spanish, "Level") == "Nivel",
              "项目语言：核心的词按项目语言写，没有译文的照原样");
        bool ok = true;
        const std::pair<const char*, std::optional<Language>> codes[] = {
            { "pt-BR", Language::Portuguese }, { "pt_BR.UTF-8", Language::Portuguese }, { "es", Language::Spanish }, { "es-419", Language::Spanish },
            { "Español", Language::Spanish },  { "en-US", Language::English },          { "en", Language::English },  { "de", std::nullopt },
            { "C", std::nullopt },             { "", std::nullopt },
        };
        for (const auto& [code, want] : codes)
            ok = ok && FromCode(code) == want;
        Check(ok, "项目语言：命令写的代码与系统给的区域设置都认");
    }

    // ── model.rs ─────────────────────────────────────────────────
    Element MakeWall(Guid level, P2 a, P2 b)
    {
        Wall w;
        w.typeName = "Generic - 200mm";
        w.level    = level;
        w.start    = a;
        w.end      = b;
        w.height   = 3000.0;
        return w;
    }

    // Level 1（100000）上 6 × 4 米的一圈墙，返回底边那面墙的 ID
    Id Boxed(Model& m)
    {
        const P2 a(0, 0), b(6000, 0), c(6000, 4000), d(0, 4000);
        const Id bottom = m.Insert(MakeWall(m.GuidOf(Id(100000)), a, b));
        m.Insert(MakeWall(m.GuidOf(Id(100000)), b, c));
        m.Insert(MakeWall(m.GuidOf(Id(100000)), c, d));
        m.Insert(MakeWall(m.GuidOf(Id(100000)), d, a));
        return bottom;
    }

    Element MakeDoor(Guid host, double offset)
    {
        Door d;
        d.typeName = "Single 900 x 2100";
        d.host     = host;
        d.offset   = offset;
        return d;
    }

    void TestModel()
    {
        std::string error;
        {
            const Model m = Model::New("House");
            const auto levels = m.Levels();
            Check(levels.size() == 2 && levels[0].second->name == "Level 1" && levels[1].second->name == "Level 2" && m.Check(error),
                  "模型：新模型有两个标高，检查通过");
        }
        {
            Model m = Model::New("t");
            const Id id = m.Insert(MakeWall(m.GuidOf(Id(100000)), P2(0, 0), P2(5000, 0)));
            Wall* w = m.elements.GetMut(id)->As<Wall>();
            w->topLevel   = m.GuidOf(Id(100001));
            w->baseOffset = 100.0;
            Check(m.BaseZ(*m.GetWall(id)) == 100.0 && m.TopZ(*m.GetWall(id)) == 3000.0, "模型：约束到标高的墙取标高的高度");
        }
        {
            Model m = Model::New("t");
            const Id one = m.Insert(MakeWall(m.GuidOf(Id(100000)), P2(0, 0), P2(5000, 0)));
            const Id tall = m.Insert(MakeWall(m.GuidOf(Id(100000)), P2(0, 1000), P2(5000, 1000)));
            m.elements.GetMut(tall)->As<Wall>()->height = 6000.0;
            const Wall& a = *m.GetWall(one);
            const Wall& b = *m.GetWall(tall);
            const Guid l1 = m.GuidOf(Id(100000)), l2 = m.GuidOf(Id(100001));
            Check(m.CutBy(a, l1) && !m.CutBy(a, l2) && m.CutBy(b, l1) && m.CutBy(b, l2), "模型：平面剖切到跨过剖切面的墙");
        }
        {
            Model m = Model::New("t");
            const Id bottom = Boxed(m);
            const auto cap = Walls::Cap(m, bottom, false);
            bool corner = std::find(cap.begin(), cap.end(), P2(-100, -100)) != cap.end();
            const Id t = m.Insert(MakeWall(m.GuidOf(Id(100000)), P2(3000, 0), P2(3000, 4000)));
            const bool tee = Walls::Cap(m, t, false) == std::vector<P2>{ P2(3100, 100), P2(2900, 100) };
            const Id up = m.Insert(MakeWall(m.GuidOf(Id(100001)), P2(0, 0), P2(0, -3000)));
            const bool apart = Walls::Cap(m, up, false) == std::vector<P2>{ P2(-100, 0), P2(100, 0) };
            Check(corner && tee && apart, "模型：墙在相交处连接，不跨标高连接");
        }
        {
            Model m = Model::New("t");
            const Id id = m.Insert(MakeWall(m.GuidOf(Id(100000)), P2(0, 0), P2(5000, 0)));
            m.elements.GetMut(id)->As<Wall>()->typeName = "Brick";
            const bool refused = !m.Check(error);
            Check(refused && Contains(error, "wall " + std::to_string(id)) && Contains(error, "Brick") && Contains(error, "Generic - 200mm"),
                  "模型：检查说出不存在的类型并列出已有的");
        }
        {
            Model m = Model::New("t");
            const Id host = m.Insert(MakeWall(m.GuidOf(Id(100000)), P2(0, 0), P2(5000, 0)));
            m.Insert(MakeDoor(m.GuidOf(host), 300.0));
            Check(!m.Check(error) && Contains(error, "fit"), "模型：检查拒绝放不进墙的门");
        }
        {
            Model m = Model::New("t");
            const Id host = m.Insert(MakeWall(m.GuidOf(Id(100000)), P2(0, 0), P2(5000, 0)));
            m.Insert(MakeDoor(m.GuidOf(host), 1000.0));
            m.Insert(MakeDoor(m.GuidOf(host), 2000.0));
            const bool apart = m.Check(error);
            m.Insert(MakeDoor(m.GuidOf(host), 2600.0));
            Check(apart && !m.Check(error) && Contains(error, "overlap"), "模型：检查拒绝重叠的门窗");
        }
        {
            Model m = Model::New("t");
            const Id host = m.Insert(MakeWall(m.GuidOf(Id(100000)), P2(0, 0), P2(5000, 0)));
            m.elements.GetMut(host)->As<Wall>()->height = 6000.0;
            m.Insert(MakeDoor(m.GuidOf(host), 1000.0));
            Element above = MakeDoor(m.GuidOf(host), 1000.0);
            above.AsOpening()->sill = 2500.0;
            m.Insert(above);
            const bool stacked = m.Check(error);
            m.Insert(MakeDoor(m.GuidOf(host), 1000.0));
            Check(stacked && !m.Check(error) && Contains(error, "overlap"), "模型：上下相叠的门窗不算重叠，并排的仍然拒绝");
        }
        {
            Model m = Model::New("t");
            m.Insert(MakeWall(Guid::OfId(99), P2(0, 0), P2(5000, 0)));
            bool ok = !m.Check(error) && Contains(error, "level 99 doesn't exist");
            Model n = Model::New("t");
            const Guid nowhere = Guid::New();
            n.Insert(MakeWall(nowhere, P2(0, 0), P2(5000, 0)));
            ok = ok && !n.Check(error) && Contains(error, "level " + nowhere.ToString() + " doesn't exist");
            Model k = Model::New("t");
            k.Insert(MakeWall(Guid::OfId(100000), P2(0, 0), P2(5000, 0)));
            ok = ok && !k.Check(error) && Contains(error, "points at element 100000 by its id, not its GUID");
            k.Resolve();
            ok = ok && k.Check(error);
            Model nan = Model::New("t");
            nan.Insert(MakeWall(nan.GuidOf(Id(100000)), P2(0, 0), P2(NAN, 0)));
            ok = ok && !nan.Check(error);
            Model shortWall = Model::New("t");
            shortWall.Insert(MakeWall(shortWall.GuidOf(Id(100000)), P2(0, 0), P2(0.5, 0)));
            Check(ok && !shortWall.Check(error) && Contains(error, "short"), "模型：检查拒绝悬空的引用与不能用的数");
        }
        {
            Element e;
            const bool ok = FromJson(Json::parse(R"({"category":"level","name":"Roof","elevation":6000})"), e, error);
            const bool typo = !FromJson(Json::parse(R"({"category":"level","name":"Roof","elevaton":6000})"), e, error) && Contains(error, "elevaton");
            Check(ok && typo, "模型：元素的 JSON 拒绝不认识的字段");
        }
        {
            Model m = Model::New("t");
            const bool ok = m.DefaultType("wall") == "Generic - 200mm" && m.DefaultType("floor") == "Generic - 200mm" &&
                            m.DefaultType("door") == "Single 900 x 2100" && m.DefaultType("window") == "Fixed 1200 x 1500" &&
                            m.TypeNames("doors") == std::vector<std::string>{ "Single 800 x 2100", "Single 900 x 2100" };
            m.types.wall.erase("Generic - 200mm");
            Check(ok && m.DefaultType("wall") == "Exterior - 300mm" && !m.DefaultType("room"), "模型：新元素默认用新模型起步的类型");
        }
        {
            Model m = Model::New("t");
            m.nextId = 1;
            Check(m.Insert(MakeWall(m.GuidOf(Id(100000)), P2(0, 0), P2(5000, 0))) == 100002, "模型：过时的计数器也不会重用 ID");
        }
    }

    // ── walls.rs ─────────────────────────────────────────────────
    // 两面 200 毫米的墙从原点出发，一面向东，一面转 degrees
    Session Corner(double degrees)
    {
        const double x = 4000.0 * std::cos(degrees * kPi / 180.0), y = 4000.0 * std::sin(degrees * kPi / 180.0);
        Json pts = Json::array({ Json::array({ 4000, 0 }), Json::array({ 0, 0 }), Json::array({ x, y }) });
        return Scripted("wall {\"points\": " + pts.dump() + "}");
    }

    bool Has(const std::vector<P2>& pts, P2 p)
    {
        return std::any_of(pts.begin(), pts.end(), [&](const P2& q) { return NearP(q, p); });
    }

    void TestWalls()
    {
        {
            const Walls::Axis a(P2(100, 50), P2(2100, -900), -75.0);
            Check(Near(Walls::SweepThrough(P2(0, 0), P2(2000, -2000), P2(4000, 0)), 180.0, 1e-9) &&
                      Near(Walls::SweepThrough(P2(0, 0), P2(2000, 2000), P2(4000, 0)), -180.0, 1e-9) &&
                      Near(Walls::SweepThrough(a.start, a.At(700.0, 0.0), a.end), -75.0, 1e-9) &&
                      Walls::SweepThrough(P2(0, 0), P2(1000, 0), P2(4000, 0)) == 0.0,
                  "墙：经过三点的圆弧朝中间那点转");
        }
        {
            const Walls::Axis a(P2(0, 0), P2(4000, 0), 180.0);
            bool ok = NearP(*a.Centre(), P2(2000, 0)) && Near(*a.Radius(), 2000.0, 1e-9) && NearP(a.At(a.Length() / 2.0, 0.0), P2(2000, -2000)) &&
                      NearP(a.Tangent(0.0), P2(0, -1)) && NearP(a.At(a.Length() / 2.0, 100.0), P2(2000, -1900));
            const auto [s, left] = a.Project(P2(2000, -2100));
            ok = ok && Near(s, a.Length() / 2.0, 1e-6) && Near(left, -100.0, 1e-9) && a.Project(P2(-500, 300)).first == 0.0;
            const Walls::Axis big(P2(0, 0), P2(2000, 0), 270.0);
            ok = ok && big.Centre()->y < 0.0 && Near(big.Length(), 1.5 * kPi * *big.Radius(), 1e-6);
            const auto points = a.Points(0.0, a.Length(), 0.0);
            ok = ok && points.size() > 30 && std::all_of(points.begin(), points.end(), [](P2 p) { return Near(p.Dist(P2(2000, 0)), 2000.0, 1e-6); });
            Check(ok, "墙：正的转角向左转，沿圆弧量长度");
        }
        {
            bool ok = true;
            for (const double degrees : { 90.0, 60.0, 120.0, 150.0 })
            {
                const Session s = Corner(degrees);
                const double half = degrees / 2.0 * kPi / 180.0;
                const P2 inner = P2(std::cos(half), std::sin(half)) * (100.0 / std::sin(half));
                for (const Id id : { Id(100002), Id(100003) })
                {
                    const auto o = Walls::Outline(s.GetModel(), id);
                    ok = ok && Has(o, inner) && Has(o, inner * -1.0) && Near(std::abs(Geom::Area(o)), 800000.0, 1e-3);
                }
            }
            Check(ok, "墙：任意角度相接时斜接，不重叠也不缺");
        }
        {
            const Session s = Corner(20.0);
            const auto o = Walls::Outline(s.GetModel(), 100003);
            const bool square = std::all_of(o.begin(), o.end(), [](P2 p) { return p.Length() < 700.0 || p.Length() > 3000.0; });
            Check(square && Has(o, P2(-std::sin(20.0 * kPi / 180.0), std::cos(20.0 * kPi / 180.0)) * 100.0), "墙：锐角处平头，不伸出尖角");
        }
        {
            const Session s = Corner(7.5);
            bool ok = true;
            for (const Id id : { Id(100002), Id(100003) })
            {
                const auto o = Walls::Outline(s.GetModel(), id);
                ok = ok && std::all_of(o.begin(), o.end(), [](P2 p) { return p.Length() < 300.0 || p.Length() > 3000.0; });
            }
            Check(ok, "墙：以很小的角度相接时平头，不延伸几米");
        }
        {
            const Session s = Scripted(R"(wall {"points": [[-2000, 0], [2000, 0]], "type": "Exterior - 300mm"}
wall {"points": [[0, 0], [1000, 3000]], "type": "Interior - 100mm"})");
            const auto t = Walls::Outline(s.GetModel(), 100003);
            bool stops = true;
            for (const P2& p : t)
                if (p.y < 1000.0)
                    stops = stops && Near(p.y, 150.0, 1e-6);
            Check(stops && Walls::Cap(s.GetModel(), 100003, false).size() == 2 && Walls::Outline(s.GetModel(), 100002).size() == 4,
                  "墙：端头落在另一面墙上的停在它的墙面，那面墙照常");
        }
        {
            const Session s = Scripted(R"(wall {"points": [[0, 0], [3000, 0]]}
wall {"points": [[0, 0], [-1500, 2598.0762]]}
wall {"points": [[0, 0], [-1500, -2598.0762]]})");
            double total = 0.0;
            bool node = true;
            for (const Id id : { Id(100002), Id(100003), Id(100004) })
            {
                total += std::abs(Geom::Area(Walls::Outline(s.GetModel(), id)));
                node = node && Has(Walls::Cap(s.GetModel(), id, false), P2(0, 0));
            }
            const double hub = std::sqrt(3.0) / 4.0 * 200.0 * 200.0;
            Check(Near(total, 3.0 * 600000.0 - hub, 1.0) && node, "墙：三面墙相接时共用节点");
        }
        {
            const Session s = Scripted(R"(wall {"points": [[0, 0], [4000, 0]], "arc": 180}
wall {"points": [[4000, 0], [4000, 3000]]})");
            const auto o = Walls::Outline(s.GetModel(), 100002);
            const double ring = kPi * 2000.0 * 200.0;
            const auto end = Walls::Cap(s.GetModel(), 100002, true);
            Check(std::abs(std::abs(Geom::Area(o)) - ring) < 0.005 * ring && Has(end, P2(3900, 0)) && Has(end, P2(4100, 0)), "墙：曲墙的轮廓沿圆弧，端头与直墙相接");
        }
        {
            const Session s = Scripted(
                R"(type {"category": "wall", "name": "C", "layers": [{"function": "finish", "thickness": 100}, {"function": "thermal", "thickness": 50}, {"function": "structure", "thickness": 140}, {"function": "finish", "thickness": 10}]}
wall {"points": [[0, 0], [5000, 0]], "type": "C"}
wall {"points": [[0, 2000], [5000, 2000]], "type": "C", "location": "core-centre"})");
            const Model& m = s.GetModel();
            const Wall& a = *m.GetWall(Id(100002));
            const Wall& b = *m.GetWall(Id(100003));
            const auto lines = Walls::LayerLines(m, 100003);
            Check(Walls::Boundaries(m, a) == std::vector<double>{ 150, 50, 0, -140, -150 } && Walls::Boundaries(m, b) == std::vector<double>{ 220, 120, 70, -70, -80 } &&
                      Walls::Faces(m, b) == std::pair{ 220.0, -80.0 } && Walls::Centre(m, b) == 70.0 && lines.size() == 5 && NearP(lines[2][0], P2(0, 2070)) &&
                      NearP(lines[2][1], P2(5000, 2070)),
                  "墙：墙面与各层在定位线决定的位置");
        }
        {
            const Session s = Scripted(R"(wall {"points": [[0, 0], [8000, 0], [8000, 6000], [0, 6000]], "closed": true}
window {"at": [8000, 3000]}
roof {"gable": true})");
            Check(Walls::Prisms(s.GetModel(), 100003, true).size() == 4 && Walls::Prisms(s.GetModel(), 100003, false).size() == 1,
                  "墙：在门窗处分成棱柱（窗下、窗上、两侧）");
            // 贴着双坡屋顶：屋脊两侧的两部分，各在一个坡面下
            Model m = s.GetModel();
            const Guid roof = m.GuidOf(Id(100007));
            m.elements.GetMut(100003)->As<Wall>()->attachTop = roof;
            const auto whole = Walls::Prisms(m, 100003, false);
            const double tan = std::tan(30.0 * kPi / 180.0);
            bool under = whole.size() == 2;
            for (const auto& p : whole)
                under = under && std::abs(p.top.At(P2(8000, 3000)) - (3000.0 + 3100.0 * tan)) < 1e-6;
            Check(under, "墙：贴着双坡屋顶时分成屋脊两侧的两部分，各在坡面下");
        }
        {
            // 贴着坡楼板的墙顶跟着楼板的底面
            Session s = Scripted(R"(wall {"points": [[0, 0], [8000, 0], [8000, 6000], [0, 6000]], "closed": true}
floor {"boundary": [[1000, 500], [7000, 500], [7000, 2000], [1000, 2000]], "offset": 100, "slope": 5, "arrow": [[1000, 1250], [7000, 1250]]}
wall {"points": [[4000, 500], [4000, 2000]], "type": "Interior - 100mm", "attach_top": 100006})");
            const Model& m = s.GetModel();
            const double tan5 = std::tan(5.0 * kPi / 180.0), cos5 = std::cos(5.0 * kPi / 180.0);
            const Wall* under = m.GetWall(Id(100007));
            Check(under && Near(Walls::TopAt(m, *under, 500.0), 100.0 + 3000.0 * tan5 - 200.0 / cos5, 1e-6), "墙：贴着坡楼板的墙停在楼板底面下");
        }
        {
            // 加速索引与逐个比较的结果相同：示例房屋、圆弧、很高的墙、两个标高
            Session s = Scripted(SampleScript);
            std::vector<Json> out;
            std::string error;
            s.Script(R"(wall {"points":[[14000,0],[18000,0]],"arc":90,"height":9000}
wall {"points":[[0,0],[0,-5000]],"base_offset":-10000,"height":1000000}
wall {"points":[[0,0],[6000,0],[6000,4000]],"level":"Level 1"}
wall {"points":[[0,0],[6000,0],[6000,4000]],"level":"Level 2"})", out, error);
            const Model& m = s.GetModel();
            const Walls::Index index(m);
            bool same = error.empty();
            for (const auto& [id, e] : m.elements.Items())
            {
                if (!e->As<Wall>())
                    continue;
                for (const bool end : { false, true })
                    same = same && Walls::Cap(m, id, end) == Walls::Cap(m, id, end, &index);
                same = same && Walls::Outline(m, id) == Walls::Outline(m, id, &index) && Walls::LayerLines(m, id) == Walls::LayerLines(m, id, &index);
                for (const bool openings : { false, true })
                    same = same && Walls::Prisms(m, id, openings) == Walls::Prisms(m, id, openings, &index);
            }
            Check(same, "墙：加速索引得到的接头、轮廓、棱柱与逐个比较的相同");
        }
    }

    // ── library.rs ───────────────────────────────────────────────
    void TestLibrary()
    {
        std::string error;
        {
            const Session s;
            const Model& m = s.GetModel();
            // 类型：墙 3、楼板 1、门 2、窗 2、屋顶 1、天花板 2；材质 17；族，以及每个族的类型和参数
            size_t familyEntries = 0;
            for (const auto& [_, f] : m.families)
                familyEntries += 1 + f.types.size() + f.parameters.size();
            bool ok = m.LibraryInStep() && !m.families.empty() && m.library.Size() == 3 + 1 + 2 + 2 + 1 + 2 + 17 + familyEntries;
            ok = ok && !m.library.Entries().empty() && m.library.Entries().rbegin()->first == LastId;
            for (const auto& [id, _] : m.library.Entries())
                ok = ok && id >= FirstId && id <= LastId && !m.elements.Contains(id);
            ok = ok && m.elements.Size() == 2 && m.elements.Items().begin()->first == 100000 && m.elements.Items().rbegin()->first == 100001;
            const auto brick = m.library.IdOf(Kind::Of(Kind::Tag::Material), "Brick");
            ok = ok && brick && m.library.Get(*brick)->name == "Brick" && m.library.GuidOf(Kind::Type("wall"), "Generic - 200mm") && m.Check(error);
            Check(ok, "库：每个条目有从上往下数的 ID 和 GUID，元素仍从下往上数");
        }
        {
            Session s;
            Json out;
            s.Execute("wall", Json::parse(R"({"points": [[0, 0], [5000, 0]], "type": "Exterior - 300mm"})"), out, error);
            const Id w = out["ids"][0].get<Id>();
            const Model& m = s.GetModel();
            std::string text;
            m.ToFile(text, error);
            const Json file = Json::parse(text);
            const Json typeGuid = m.library.GuidOf(Kind::Type("wall"), "Exterior - 300mm")->ToString();
            const Json brickGuid = m.library.GuidOf(Kind::Of(Kind::Tag::Material), "Brick")->ToString();
            bool ok = file["format"] == FileFormat && file["elements"][std::to_string(w)]["type"] == typeGuid &&
                      file["types"]["wall"]["Exterior - 300mm"]["layers"][0]["material"] == brickGuid && file["library"].size() == m.library.Size();
            Model back;
            std::vector<std::string> leftOut;
            ok = ok && Model::Read(text, back, leftOut, error) && leftOut.empty() && back.ToJson() == m.ToJson();
            Check(ok, "库：文件里存名称对应的 GUID，读回来与原来一样");
        }
        {
            Session s;
            Json out;
            const Guid exterior = *s.GetModel().library.GuidOf(Kind::Type("wall"), "Exterior - 300mm");
            const bool ran = s.Execute("wall", Json{ { "points", Json::array({ Json::array({ 0, 0 }), Json::array({ 4000, 0 }) }) }, { "type", exterior.ToString() } }, out, error);
            const Id w = out["ids"][0].get<Id>();
            const Json e = ElementJson(s.GetModel(), w);
            Check(ran && s.GetModel().GetWall(w)->typeName == "Exterior - 300mm" && e["type"] == "Exterior - 300mm" && e["type_guid"] == exterior.ToString(),
                  "库：命令收名称的地方也收条目的 GUID，元素显示类型的 GUID");
            const bool refused = !s.Execute("material", Json::parse(R"({"name": "3f2a9c1e-0000-4000-8000-000000000001", "color": [1, 2, 3]})"), out, error);
            Check(refused && Contains(error, "a name isn't a GUID"), "库：是 GUID 的名称拒绝");
        }
        {
            // 改名保留条目的 ID 和 GUID，文件里只有名称本身变了
            Session s;
            Json out;
            s.Execute("wall", Json::parse(R"({"points": [[0, 0], [5000, 0]], "type": "Exterior - 300mm"})"), out, error);
            const Kind material = Kind::Of(Kind::Tag::Material);
            const auto id = s.GetModel().library.IdOf(material, "Brick");
            const auto guid = s.GetModel().library.GuidOf(material, "Brick");
            std::string before, after;
            s.GetModel().ToFile(before, error);
            const bool renamed = s.Execute("material", Json::parse(R"({"name": "Brick", "rename": "Clay brick"})"), out, error);
            s.GetModel().ToFile(after, error);
            const Json a = Json::parse(before), b = Json::parse(after);
            Check(renamed && s.GetModel().library.IdOf(material, "Clay brick") == id && s.GetModel().library.GuidOf(material, "Clay brick") == guid &&
                      a["elements"] == b["elements"] && a["types"] == b["types"] &&
                      s.GetModel().types.wall.at("Exterior - 300mm").layers[0].material == "Clay brick",
                  "库：改名保留 ID 和 GUID，文件里元素与类型不变");
        }
        {
            // 层是部件：有自己的 ID 和 GUID，位置不变就保留
            Session s;
            Json out;
            const auto& before = s.GetModel().types.wall.at("Exterior - 300mm").layers;
            const Id first = before[0].id;
            const Guid firstGuid = before[0].guid;
            const bool ok = first >= FirstId && !firstGuid.IsNil() && s.GetModel().library.GetPart(first) &&
                            s.GetModel().library.GetPart(first)->Name() == "layer 1 of wall type Exterior - 300mm";
            s.Execute("type", Json::parse(R"({"category": "wall", "name": "Exterior - 300mm", "thickness": 320})"), out, error);
            const auto& after = s.GetModel().types.wall.at("Exterior - 300mm").layers;
            Check(ok && after[0].id == first && after[0].guid == firstGuid && after[2].thickness == 160.0, "库：类型的层有自己的 ID 和 GUID，改厚度时保留");
        }
    }

    // ── skeleton.rs ──────────────────────────────────────────────
    using Faces = std::vector<std::vector<Skeleton::Face>>;

    double FaceArea(const Skeleton::Face& face)
    {
        Ring ring;
        for (const auto& [p, t] : face)
            ring.push_back(p);
        return Geom::Area(ring);
    }

    double Highest(const Faces& f)
    {
        double h = 0.0;
        for (const auto& loops : f)
            for (const auto& face : loops)
                for (const auto& [p, t] : face)
                    h = std::max(h, t);
        return h;
    }

    double Total(const Faces& f)
    {
        double a = 0.0;
        for (const auto& loops : f)
            for (const auto& face : loops)
                a += FaceArea(face);
        return a;
    }

    double TotalOf(const std::vector<Skeleton::Face>& loops)
    {
        double a = 0.0;
        for (const auto& face : loops)
            a += FaceArea(face);
        return a;
    }

    std::optional<Faces> SkeletonOf(const std::vector<Ring>& rings, const std::vector<double>& speeds, std::string* why = nullptr)
    {
        Faces f;
        std::string error;
        if (!Skeleton::Faces(rings, speeds, f, error))
        {
            if (why)
                *why = error;
            return std::nullopt;
        }
        return f;
    }

    // 每个面都是平的：每个角点离它的边的距离等于边移动的距离
    bool Flat(const std::vector<Ring>& rings, const std::vector<double>& speeds, const Faces& f)
    {
        std::vector<Segment> edges;
        for (const Ring& r : rings)
            for (const Segment& e : Geom::RingEdges(r))
                edges.push_back(e);
        for (size_t i = 0; i < f.size() && i < edges.size(); ++i)
        {
            const P2 n = (edges[i].second - edges[i].first).Unit().Perp();
            for (const auto& face : f[i])
                for (const auto& [p, t] : face)
                    if (std::abs((p - edges[i].first).Dot(n) - t * speeds[i]) >= 1e-3)
                        return false;
        }
        return true;
    }

    Ring Turned(const Ring& r, double c, double s)
    {
        Ring out;
        for (const P2& p : r)
            out.emplace_back(p.x * c - p.y * s, p.x * s + p.y * c);
        return out;
    }

    void TestSkeleton()
    {
        {
            const Ring ring{ P2(0, 0), P2(10, 0), P2(10, 4), P2(0, 4) };
            const auto f = SkeletonOf({ ring }, { 1, 1, 1, 1 });
            // 长边的面是升到屋脊（2 进深）的梯形，短边的是三角形
            bool ok = f && f->size() == 4;
            const size_t sizes[] = { 4, 3, 4, 3 };
            for (size_t i = 0; ok && i < 4; ++i)
                ok = (*f)[i].size() == 1 && (*f)[i][0].size() == sizes[i];
            Check(ok && Near(Highest(*f), 2.0, 1e-9) && Near(Total(*f), 40.0, 1e-9), "骨架：矩形两端是斜脊，中间是屋脊");
            // 两端竖直，即山墙：屋脊一直到它们
            const auto g = SkeletonOf({ ring }, { 1, 0, 1, 0 });
            Check(g && (*g)[1].empty() && (*g)[3].empty() && Near(TotalOf((*g)[0]), 20.0, 1e-9), "骨架：两端是山墙时屋脊通到两端");
            // 一边倾斜、其余竖直：单坡，升到对边
            const auto shed = SkeletonOf({ ring }, { 1, 0, 0, 0 });
            Check(shed && Near(TotalOf((*shed)[0]), 40.0, 1e-9) && Near(Highest(*shed), 4.0, 1e-9) && !SkeletonOf({ ring }, { 0, 0, 0, 0 }),
                  "骨架：一边倾斜是单坡；都不倾斜时拒绝");
        }
        {
            const Ring ring{ P2(0, 0), P2(10, 0), P2(10, 4), P2(4, 4), P2(4, 10), P2(0, 10) };
            const auto f = SkeletonOf({ ring }, std::vector<double>(6, 1.0));
            // 内角两侧的边各升到从内角到 (2, 2) 的天沟
            const auto valley = [](const Skeleton::Face& x)
            {
                return std::any_of(x.begin(), x.end(), [](const auto& q) { return q.first.Dist(P2(2, 2)) < 1e-9 && Near(q.second, 2.0, 1e-9); });
            };
            Check(f && Near(Total(*f), 64.0, 1e-9) && Near(Highest(*f), 2.0, 1e-9) && std::any_of((*f)[2].begin(), (*f)[2].end(), valley) &&
                      std::any_of((*f)[3].begin(), (*f)[3].end(), valley),
                  "骨架：L 形在内角处有天沟");
        }
        {
            const Ring tri{ P2(0, 0), P2(6, 0), P2(3, 5.196152422706632) };
            const auto f = SkeletonOf({ tri }, { 1, 1, 1 });
            bool ok = f && Near(Highest(*f), std::sqrt(3.0), 1e-9);
            for (const Ring& ring : { Ring{ P2(0, 0), P2(9, 1), P2(8, 6), P2(1, 4) }, Ring{ P2(0, 0), P2(6, 0), P2(8, 4), P2(3, 7), P2(-2, 4) } })
            {
                const auto g = SkeletonOf({ ring }, std::vector<double>(ring.size(), 1.0));
                ok = ok && g && Near(Total(*g), Geom::Area(ring), 1e-6);
            }
            Check(ok, "骨架：凸多边形升到一点或一条屋脊");
        }
        {
            // 12 见方围着 4 见方的天井（顺时针）：天井的边向外移进屋顶
            const std::vector<Ring> rings{ { P2(0, 0), P2(12, 0), P2(12, 12), P2(0, 12) }, { P2(4, 4), P2(4, 8), P2(8, 8), P2(8, 4) } };
            const std::vector<double> ones(8, 1.0);
            const auto f = SkeletonOf(rings, ones);
            bool ok = f && Near(Total(*f), 128.0, 1e-9) && Flat(rings, ones, *f);
            for (size_t i = 0; ok && i < 8; ++i)
                ok = (*f)[i].size() == 1 && (*f)[i][0].size() == 4;
            // 屋脊：离天井 2、离外轮廓 2 的一圈
            if (ok)
                for (const auto& loops : *f)
                    for (const auto& face : loops)
                        for (const auto& [p, t] : face)
                        {
                            const double ring = std::max(std::abs(p.x - 6.0), std::abs(p.y - 6.0));
                            ok = ok && (t == 0.0 || (Near(t, 2.0, 1e-9) && Near(ring, 4.0, 1e-9)));
                        }
            Check(ok, "骨架：天井四周从檐口升到一圈屋脊");
            // 天井北边竖直：外轮廓的北坡升到它，4 进深
            const std::vector<double> speeds{ 1, 1, 1, 1, 1, 0, 1, 1 };
            const auto g = SkeletonOf(rings, speeds);
            const auto gable = [](const Skeleton::Face& x)
            {
                for (const P2 q : { P2(4, 8), P2(8, 8) })
                    if (!std::any_of(x.begin(), x.end(), [&](const auto& v) { return v.first.Dist(q) < 1e-9 && Near(v.second, 4.0, 1e-9); }))
                        return false;
                return true;
            };
            Check(g && (*g)[5].empty() && Near(Total(*g), 128.0, 1e-9) && Flat(rings, speeds, *g) && Near(Highest(*g), 4.0, 1e-9) &&
                      std::any_of((*g)[2].begin(), (*g)[2].end(), gable),
                  "骨架：天井一边是山墙时外轮廓的坡升到它");
            // 不在正中：每翼的屋脊在它的一半处，最宽的（5）最高
            const std::vector<Ring> off{ { P2(0, 0), P2(12, 0), P2(12, 10), P2(0, 10) }, { P2(3, 3), P2(3, 7), P2(7, 7), P2(7, 3) } };
            const auto h = SkeletonOf(off, ones);
            Check(h && Near(Total(*h), 104.0, 1e-9) && Flat(off, ones, *h) && Near(Highest(*h), 2.5, 1e-9) && !SkeletonOf({ off[0], off[0] }, ones),
                  "骨架：天井不在正中时各翼屋脊在一半处；洞要顺时针");
        }
        {
            // 天井离南檐 200，北边是山墙：南坡从两侧越过它，面是绕着天井和它的坡的一圈
            const std::vector<Ring> rings{ { P2(90, 0), P2(90, 10), P2(70, 10), P2(70, 30), P2(-30, 30), P2(-30, 10), P2(-80, 10), P2(-80, 0) },
                                           { P2(-5, 2), P2(-5, 8), P2(5, 8), P2(5, 2) } };
            const std::vector<double> speeds{ 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1 };
            const double c = std::sin(0.3468), s = std::cos(0.3468);  // 照 Rust 的 sin_cos：先正弦
            bool ok = true;
            for (const auto& rs : { rings, std::vector<Ring>{ Turned(rings[0], c, s), Turned(rings[1], c, s) } })
            {
                const auto f = SkeletonOf(rs, speeds);
                ok = ok && f && Near(Total(*f), 3640.0, 1e-6) && Flat(rs, speeds, *f);
                if (!ok)
                    break;
                const double island = 60.0 + TotalOf((*f)[8]) + TotalOf((*f)[10]) + TotalOf((*f)[11]);
                ok = (*f)[7].size() == 2 && Near(FaceArea((*f)[7][1]) + island, 0.0, 1e-6);
            }
            Check(ok, "骨架：坡面绕着另一边是山墙的天井合拢");
        }
        {
            // 南檐附近两个天井，东边那个的西、北两边是山墙：它们的角不动，南坡绕着天井合拢时在那里碰到自己
            const std::vector<Ring> rings{ { P2(-30, 0), P2(80, 0), P2(80, 70), P2(-30, 70) }, { P2(-9, 2), P2(-9, 8), P2(-4, 8), P2(-4, 2) },
                                           { P2(4, 2), P2(4, 8), P2(9, 8), P2(9, 2) } };
            std::vector<double> speeds(12, 1.0);
            speeds[8] = 0.0;
            speeds[9] = 0.0;
            const double c = std::sin(0.3468), s = std::cos(0.3468);  // 照 Rust 的 sin_cos：先正弦
            bool ok = true;
            for (const auto& rs : { rings, std::vector<Ring>{ Turned(rings[0], c, s), Turned(rings[1], c, s), Turned(rings[2], c, s) } })
            {
                const auto f = SkeletonOf(rs, speeds);
                ok = ok && f && Near(Total(*f), 7640.0, 1e-6) && Flat(rs, speeds, *f) && (*f)[0].size() == 2 && FaceArea((*f)[0][1]) < 0.0;
            }
            Check(ok, "骨架：绕天井合拢的坡面可以在两面山墙相交处碰到自己");
        }
        {
            // 房子可能有的迹线：随机的星形（很多凹角，到处分裂）和随机的台阶形（直角，同时发生的事件），有些边竖直，
            // 一半围着天井，有些台阶形围着两个
            uint64_t seed = 12345;
            auto rand = [&]
            {
                seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
                return static_cast<double>(seed >> 33) / static_cast<double>(1ULL << 31);
            };
            std::string failure;
            for (int round = 0; round < 2000 && failure.empty(); ++round)
            {
                const size_t n = 5 + static_cast<size_t>(rand() * 12.0);
                const bool court = round % 4 >= 2;
                std::vector<Ring> rings;
                if (round % 2 == 0)
                {
                    Ring star;
                    for (size_t i = 0; i < n; ++i)
                    {
                        const double a = (static_cast<double>(i) + rand() * 0.8) / static_cast<double>(n) * 2.0 * kPi;
                        const double r = 3000.0 + rand() * 7000.0;
                        star.emplace_back(r * std::cos(a), r * std::sin(a));
                    }
                    // 中间的天井，顺时针，在每条边都让开的 1277 以内
                    const size_t k = 3 + static_cast<size_t>(rand() * 6.0);
                    Ring hole;
                    for (size_t j = k; j-- > 0;)
                    {
                        const double a = (static_cast<double>(j) + rand() * 0.8) / static_cast<double>(k) * 2.0 * kPi;
                        const double r = 300.0 + rand() * 900.0;
                        hole.emplace_back(r * std::cos(a), r * std::sin(a));
                    }
                    // 三角形的角可能跨过半圈以上，绕成另一个方向
                    if (Geom::Area(hole) > 0.0)
                        std::reverse(hole.begin(), hole.end());
                    rings.push_back(std::move(star));
                    if (court)
                        rings.push_back(std::move(hole));
                }
                else
                {
                    // 右边随机的台阶上去，左边下来
                    const size_t steps = 1 + static_cast<size_t>(rand() * 4.0);
                    Ring right, left;
                    double y = 0.0;
                    for (size_t i = 0; i < steps; ++i)
                    {
                        const double xr = 2000.0 + std::floor(rand() * 8.0) * 1000.0;
                        const double xl = -2000.0 - std::floor(rand() * 8.0) * 1000.0;
                        const double h = 1000.0 + std::floor(rand() * 5.0) * 1000.0;
                        right.emplace_back(xr, y);
                        right.emplace_back(xr, y + h);
                        left.insert(left.begin(), { P2(xl, y + h), P2(xl, y) });
                        y += h;
                    }
                    const double first = right.size() > 1 ? right[1].y : 1000.0;
                    Ring ring;
                    for (const P2& q : right)
                        if (ring.empty() || ring.back().Dist(q) >= 1e-9)
                            ring.push_back(q);
                    for (const P2& q : left)
                        if (ring.empty() || ring.back().Dist(q) >= 1e-9)
                            ring.push_back(q);
                    // 第一级里的一个天井，顺时针；或两个，并排
                    const double w = 500.0 + std::floor(rand() * 3.0) * 500.0;
                    const double y0 = 200.0 + std::floor(rand() * 3.0) * 100.0;
                    const double g = 200.0 + std::floor(rand() * 3.0) * 100.0;
                    auto hole = [&](double a, double b) { return Ring{ P2(a, y0), P2(a, first - y0), P2(b, first - y0), P2(b, y0) }; };
                    const std::vector<Ring> holes = round % 8 == 7 ? std::vector<Ring>{ hole(-w - g, -g), hole(g, w + g) } : std::vector<Ring>{ hole(-w, w) };
                    // 转任意角度，本应平行的边只是接近平行
                    const double turn = rand() * 7.0;
                    const double c = std::sin(turn), s = std::cos(turn);
                    rings.push_back(Turned(ring, c, s));
                    if (court)
                        for (const Ring& h : holes)
                            rings.push_back(Turned(h, c, s));
                }
                size_t count = 0;
                for (const Ring& r : rings)
                    count += r.size();
                std::vector<double> some;
                for (size_t i = 0; i < count; ++i)
                    some.push_back(rand() < 0.15 ? 0.0 : 1.0);
                if (!some.empty())
                    some[0] = 1.0;
                double whole = 0.0;
                for (const Ring& r : rings)
                    whole += Geom::Area(r);
                for (const auto& speeds : { std::vector<double>(count, 1.0), some })
                {
                    std::string why;
                    const auto f = SkeletonOf(rings, speeds, &why);
                    if (!f)
                    {
                        if (Contains(why, "fence") && std::find(speeds.begin(), speeds.end(), 0.0) != speeds.end())
                            continue;
                        failure = "第 " + std::to_string(round) + " 轮：" + why;
                        break;
                    }
                    if (std::abs(Total(*f) - whole) >= 1e-3 * whole || !Flat(rings, speeds, *f))
                    {
                        failure = "第 " + std::to_string(round) + " 轮：面积 " + std::to_string(Total(*f)) + "，应为 " + std::to_string(whole);
                        break;
                    }
                }
            }
            if (!failure.empty())
                std::printf("  %s\n", failure.c_str());
            Check(failure.empty(), "骨架：随机的迹线都完整合拢");
        }
        {
            const Ring t{ P2(0, 0), P2(4, 0), P2(4, 6), P2(9, 6), P2(9, 10), P2(-5, 10), P2(-5, 6), P2(0, 6) };
            const Ring u{ P2(0, 0), P2(12, 0), P2(12, 9), P2(8, 9), P2(8, 4), P2(4, 4), P2(4, 9), P2(0, 9) };
            bool ok = true;
            for (const Ring& ring : { t, u })
            {
                const auto f = SkeletonOf({ ring }, std::vector<double>(ring.size(), 1.0));
                ok = ok && f && Near(Total(*f), Geom::Area(ring), 1e-6);
                if (f)
                    for (const auto& loops : *f)
                        for (const auto& face : loops)
                            ok = ok && FaceArea(face) > 0.0;
            }
            Check(ok, "骨架：T 形和 U 形完整合拢");
        }
    }

    // ── family.rs、csg.rs ────────────────────────────────────────
    // 封闭曲面的体积：从原点出发的有向四面体之和，三角形朝外时为正
    double Volume(const std::vector<Tri>& tris)
    {
        double v = 0.0;
        for (const Tri& t : tris)
            v += V3::Dot(t[0], V3::Cross(t[1], t[2])) / 6.0;
        return v;
    }

    double Surface(const std::vector<Tri>& tris)
    {
        double a = 0.0;
        for (const Tri& t : tris)
        {
            const P3 n = V3::Cross(V3::Sub(t[1], t[0]), V3::Sub(t[2], t[0]));
            a += std::sqrt(V3::Dot(n, n)) / 2.0;
        }
        return a;
    }

    Family FamilyFrom(const Json& v)
    {
        Family f;
        std::string error;
        if (!FromJson(v, f, error))
            std::printf("  族读不进来：%s\n", error.c_str());
        return f;
    }

    std::optional<double> EvalOf(const std::string& formula, const std::vector<std::pair<std::string, double>>& values, std::string* why = nullptr)
    {
        double out = 0.0;
        std::string error;
        const bool ok = Fam::Eval(
            formula,
            [&](const std::string& n, double& v, std::string& e)
            {
                for (const auto& [k, x] : values)
                    if (k == n)
                    {
                        v = x;
                        return true;
                    }
                e = "no `" + n + "`";
                return false;
            },
            out, error);
        if (!ok)
        {
            if (why)
                *why = error;
            return std::nullopt;
        }
        return out;
    }

    // 一个类型的族建出的实体；失败时 why 是原因
    std::optional<std::vector<Fam::Piece>> BuiltOf(const Json& forms, std::string* why = nullptr)
    {
        const Family f = FamilyFrom(Json{ { "types", Json{ { "A", Json::object() } } }, { "parameters", Json::parse(R"([{"name": "R", "value": 300}])") }, { "forms", forms } });
        std::string error;
        Fam::Values v;
        std::vector<Fam::Piece> pieces;
        if (!f.Check([](const std::string&) { return true; }, error) || !f.Values("A", {}, v, error) || !f.Pieces(v, pieces, error))
        {
            if (why)
                *why = error;
            return std::nullopt;
        }
        return pieces;
    }

    std::vector<Tri> Cuboid(P3 lo, P3 hi)
    {
        const auto v = [&](int x, int y, int z) { return P3{ x ? hi[0] : lo[0], y ? hi[1] : lo[1], z ? hi[2] : lo[2] }; };
        const std::array<std::array<P3, 4>, 6> faces = { { { v(0, 0, 1), v(1, 0, 1), v(1, 1, 1), v(0, 1, 1) },
                                                            { v(0, 0, 0), v(0, 1, 0), v(1, 1, 0), v(1, 0, 0) },
                                                            { v(0, 0, 0), v(1, 0, 0), v(1, 0, 1), v(0, 0, 1) },
                                                            { v(1, 1, 0), v(0, 1, 0), v(0, 1, 1), v(1, 1, 1) },
                                                            { v(0, 1, 0), v(0, 0, 0), v(0, 0, 1), v(0, 1, 1) },
                                                            { v(1, 0, 0), v(1, 1, 0), v(1, 1, 1), v(1, 0, 1) } } };
        std::vector<Tri> out;
        for (const auto& [a, b, c, d] : faces)
        {
            out.push_back(Tri{ a, b, c });
            out.push_back(Tri{ a, c, d });
        }
        return out;
    }

    void TestFamily()
    {
        const std::vector<std::pair<std::string, double>> values{ { "Width", 1000.0 }, { "Leg Height", 700.0 } };
        {
            bool ok = true;
            const std::pair<const char*, double> good[] = {
                { "Width / 2 + 10", 510.0 }, { "2 + 3 * 4", 14.0 }, { "(2 + 3) * 4", 20.0 }, { "-Width + 1", -999.0 }, { "2 ^ 3 ^ 2", 512.0 },
                { "Leg Height - 2*3", 694.0 }, { "if(Width > 1000, 4, 2)", 2.0 }, { "if(Width >= 1000, 4, 2)", 4.0 }, { "if (Width = 1000, 1, 0)", 1.0 },
                { "not(Width < 1000)", 1.0 }, { "and(1, 0) + or(1, 0)", 1.0 }, { "sqrt(16) + abs(-2) + min(3, 5) + max(3, 5)", 14.0 },
                { "round(2.5) + roundup(2.1) + rounddown(2.9)", 8.0 }, { "sin(30) + cos(60)", 1.0 }, { "pi() * 0", 0.0 },
                // 没走的分支不算
                { "if(Width > 0, 1, 1 / 0)", 1.0 },
            };
            for (const auto& [formula, want] : good)
            {
                const auto got = EvalOf(formula, values);
                if (!got || std::abs(*got - want) >= 1e-9)
                {
                    std::printf("  %s：%s\n", formula, got ? std::to_string(*got).c_str() : "失败");
                    ok = false;
                }
            }
            const std::pair<const char*, const char*> bad[] = { { "Width +", "`Width +`" }, { "Nope + 1", "Nope" }, { "1 / 0", "isn't a number" },
                                                                { "sqrt(-1)", "isn't a number" }, { "if(1, 2)", "if" }, { "frobnicate(1)", "frobnicate" },
                                                                { "2 3", "`2 3`" }, { "", "empty" } };
            for (const auto& [formula, says] : bad)
            {
                std::string why;
                ok = ok && !EvalOf(formula, values, &why) && Contains(why, says);
            }
            // 嵌套得过深：拒绝，不让栈溢出
            std::string why;
            const std::string deep = std::string(10000, '(') + "1" + std::string(10000, ')');
            Check(ok && !EvalOf(deep, values, &why) && Contains(why, "deep"), "族：公式的写法同 Revit，错的说出哪里错");
        }
        const Json table = Json::parse(R"({
            "parameters": [
                {"name": "Width", "value": 1000},
                {"name": "Depth", "value": 600},
                {"name": "Height", "instance": true, "value": 750},
                {"name": "Top", "formula": "Height - 30"},
                {"name": "Area", "kind": "number", "formula": "Width * Depth / 1e6"},
                {"name": "Legs", "kind": "yesno", "value": true},
                {"name": "Finish", "kind": "material", "value": "Timber"}
            ],
            "types": {"1000 x 600": {}, "Small": {"Width": 800}},
            "forms": [
                {"profile": [[0, 0], ["Width", 0], ["Width", "Depth"], [0, "Depth"]], "from": "Top", "to": "Height", "material": "Finish"},
                {"profile": [[0, 0], [50, 0], [50, 50], [0, 50]], "from": 0, "to": "Top", "visible": "Legs"}
            ]
        })");
        {
            const Family f = FamilyFrom(table);
            Fam::Values v;
            std::string error;
            const bool got = f.Values("Small", { { "Height", Json(700) } }, v, error);
            const auto n = [&](const char* k) { return std::get<double>(v.at(k)); };
            bool ok = got && n("Width") == 800.0 && n("Depth") == 600.0 && n("Height") == 700.0 && n("Top") == 670.0 && std::abs(n("Area") - 0.48) < 1e-12 &&
                      n("Legs") == 1.0 && std::get<std::string>(v.at("Finish")) == "Timber";
            Family cyclic = f;
            cyclic.parameters[0].formula = "Depth + Top";
            cyclic.parameters[1].formula = "Width - 1";
            ok = ok && !cyclic.Values("Small", {}, v, error) && Contains(error, "Width") && Contains(error, "itself");
            ok = ok && !f.Values("Huge", {}, v, error) && Contains(error, "Huge");
            Check(ok, "族：类型与构件设定参数，公式跟着算；绕回自己的公式、没有的类型报错");
        }
        {
            // 很长的平铺的和读得出；一个接一个取值的参数链太深就拒绝
            std::string flat = "1";
            for (int i = 0; i < 600; ++i)
                flat += " + 1";
            std::string terms;
            for (int i = 0; i < 500; ++i)
                terms += " + 1";
            Json params = Json::array();
            for (int i = 0; i < 64; ++i)
                params.push_back(i == 63 ? Json{ { "name", "P63" }, { "value", 1 } } : Json{ { "name", "P" + std::to_string(i) }, { "formula", "P" + std::to_string(i + 1) + terms } });
            const Family f = FamilyFrom(Json{ { "parameters", params }, { "types", Json{ { "A", Json::object() } } } });
            Fam::Values v;
            std::string error;
            const auto sum = EvalOf(flat, {});
            Check(sum == 601.0 && !f.Values("A", {}, v, error) && Contains(error, "deep"), "族：长公式、长的公式链不会耗尽栈");
        }
        {
            const auto ring = [](size_t n)
            {
                Json out = Json::array();
                for (size_t k = 0; k < n; ++k)
                {
                    const double a = static_cast<double>(k) * 2.0 * kPi / static_cast<double>(n);
                    out.push_back(Json::array({ 100.0 * std::cos(a), 100.0 * std::sin(a) }));
                }
                return out;
            };
            std::string error;
            const Family f = FamilyFrom(Json{ { "types", Json{ { "A", Json::object() } } }, { "forms", Json::array({ Json{ { "profile", ring(Fam::MaxProfile + 1) }, { "to", 10 } } }) } });
            bool ok = !f.Check([](const std::string&) { return true; }, error) && Contains(error, "corners");
            // 很多类型，各建一点：加起来超过一个族能建的，拒绝
            Json types = Json::object();
            for (size_t k = 0; k < Fam::MaxTypes; ++k)
                types["T" + std::to_string(k)] = Json::object();
            const Json form = Json{ { "profile", ring(Fam::MaxProfile) }, { "to", 10 } };
            const Family g = FamilyFrom(Json{ { "types", types }, { "forms", Json::array({ form, form }) } });
            ok = ok && !g.Check([](const std::string&) { return true; }, error) && Contains(error, "triangles");
            Check(ok, "族：太大建不出来的拒绝");
        }
        {
            const Family f = FamilyFrom(table);
            Fam::Values v;
            std::vector<Fam::Piece> pieces;
            std::string error;
            bool ok = f.Values("1000 x 600", {}, v, error) && f.Pieces(v, pieces, error) && pieces.size() == 2;
            if (ok)
            {
                const Fam::Piece& top = pieces[0];
                ok = top.bottom == 720.0 && top.top == 750.0 && top.material == "Timber" && !top.round && std::abs(Volume(top.tris) - 1000.0 * 600.0 * 30.0) < 1e-3 &&
                     top.outline == Ring{ P2(0, 0), P2(1000, 0), P2(1000, 600), P2(0, 600) };
            }
            // 藏起来：没有腿
            Fam::Values noLegs = v;
            noLegs["Legs"] = 0.0;
            ok = ok && f.Pieces(noLegs, pieces, error) && pieces.size() == 1;
            // 竖着的轮廓：x、z 上的靠背沿 y 扫；y、z 上的横杆沿 x 扫；圆柱的侧面算一个曲面
            const Family g = FamilyFrom(Json::parse(R"({"types": {"A": {}}, "forms": [
                {"plane": "xz", "profile": [[0, 400], [500, 400], [500, 900], [0, 900]], "from": 0, "to": 40},
                {"plane": "yz", "profile": [[0, 0], [300, 0], [300, 100]], "from": 10, "to": 210},
                {"circle": {"centre": [100, 100], "radius": 20}, "from": 0, "to": 1000}]})"));
            ok = ok && g.Values("A", {}, v, error) && g.Pieces(v, pieces, error) && pieces.size() == 3;
            if (ok)
            {
                const Fam::Piece &back = pieces[0], &rail = pieces[1], &post = pieces[2];
                const double ring = Geom::Area(post.outline);
                ok = std::abs(Volume(back.tris) - 500.0 * 500.0 * 40.0) < 1e-3 && back.bottom == 400.0 && back.top == 900.0 &&
                     back.outline == Ring{ P2(0, 0), P2(500, 0), P2(500, 40), P2(0, 40) } && std::abs(Volume(rail.tris) - 300.0 * 100.0 / 2.0 * 200.0) < 1e-3 &&
                     rail.outline == Ring{ P2(10, 0), P2(210, 0), P2(210, 300), P2(10, 300) } && post.round &&
                     std::abs(Volume(post.tris) - ring * 1000.0) < 1e-3 && std::abs(ring / (kPi * 400.0) - 1.0) < 0.01;
            }
            // 没有扫过的距离：说出是哪个形体
            Family flat = FamilyFrom(table);
            flat.forms[0].to = std::string("Top");
            ok = ok && flat.Values("Small", {}, v, error) && !flat.Pieces(v, pieces, error) && Contains(error, "form 1");
            Check(ok, "族：形体的轮廓扫过它的平面");
        }
        {
            // 管：x、z 上的矩形，离 z 轴 100 到 R，绕它转一整圈
            const Json ring = Json::parse(R"([[100, 0], ["R", 0], ["R", 500], [100, 500]])");
            const double k = 16.0 * std::sin(2.0 * kPi / 32.0) / kPi;
            const double want = kPi * (300.0 * 300.0 - 100.0 * 100.0) * 500.0 * k;
            const auto tube = BuiltOf(Json::array({ Json{ { "plane", "xz" }, { "profile", ring }, { "axis", Json::parse("[[0, 0], [0, 1]]") }, { "to", 360 } } }));
            bool ok = tube && tube->size() == 1 && std::abs(Volume((*tube)[0].tris) / want - 1.0) < 1e-6 && (*tube)[0].bottom == 0.0 && (*tube)[0].top == 500.0 &&
                      std::abs(Geom::Area((*tube)[0].outline) / (kPi * 300.0 * 300.0 * k) - 1.0) < 1e-6;
            // 四分之一，两端封上；轴反着给，或在 x、y 上
            const auto quarter =
                BuiltOf(Json::array({ Json{ { "plane", "xz" }, { "profile", ring }, { "axis", Json::parse("[[0, 1], [0, 0]]") }, { "from", 0 }, { "to", 90 } } }));
            ok = ok && quarter && std::abs(Volume((*quarter)[0].tris) / (want / 4.0) - 1.0) < 1e-6;
            const auto half = BuiltOf(Json::parse(R"([{"profile": [[100, 0], [300, 0], [300, 50], [100, 50]], "axis": [[0, 0], [0, 1]], "to": 180}])"));
            const double ringOf = kPi * (300.0 * 300.0 - 100.0 * 100.0) * 50.0 * k;
            ok = ok && half && std::abs(Volume((*half)[0].tris) / (ringOf / 2.0) - 1.0) < 1e-6 && std::abs((*half)[0].top - (*half)[0].bottom - 300.0) < 1e-6;
            // 轮廓横过轴、轴只有一点、转了零度或超过一圈：说出来
            const std::pair<Json, const char*> bad[] = {
                { Json::parse(R"({"plane": "xz", "profile": [[-100, 0], [300, 0], [300, 500]], "axis": [[0, 0], [0, 1]], "to": 360})"), "crosses its axis" },
                { Json{ { "plane", "xz" }, { "profile", ring }, { "axis", Json::parse("[[0, 0], [0, 0]]") }, { "to", 360 } }, "axis" },
                { Json{ { "plane", "xz" }, { "profile", ring }, { "axis", Json::parse("[[0, 0], [0, 1]]") }, { "from", 90 }, { "to", 90 } }, "nothing" },
                { Json{ { "plane", "xz" }, { "profile", ring }, { "axis", Json::parse("[[0, 0], [0, 1]]") }, { "to", 400 } }, "360" },
            };
            for (const auto& [form, says] : bad)
            {
                std::string why;
                ok = ok && !BuiltOf(Json::array({ form }), &why) && Contains(why, says) && Contains(why, "form 1");
            }
            Check(ok, "族：旋转体绕轴转它的轮廓");
        }
        {
            // 同一根管，100 到 300、长 500：画在哪个平面、在轴的哪一侧、轴怎么给、从哪个角度起，体积都一样，朝外
            const double k = 16.0 * std::sin(2.0 * kPi / 32.0);
            const double want = k * (300.0 * 300.0 - 100.0 * 100.0) * 500.0;
            const auto across = [](double a0, double a1) { return Json::array({ Json::array({ a0, 0 }), Json::array({ a1, 0 }), Json::array({ a1, 500 }), Json::array({ a0, 500 }) }); };
            const std::tuple<const char*, Json, Json> cases[] = {
                { "xz", across(100, 300), Json::parse("[[0, 0], [0, 1]]") },   { "xz", across(100, 300), Json::parse("[[0, 1], [0, 0]]") },
                { "xz", across(-300, -100), Json::parse("[[0, 0], [0, 1]]") }, { "xz", across(-300, -100), Json::parse("[[0, 1], [0, 0]]") },
                { "yz", across(100, 300), Json::parse("[[0, 0], [0, 1]]") },   { "yz", across(-300, -100), Json::parse("[[0, 1], [0, 0]]") },
                { "xy", Json::parse("[[0, 100], [500, 100], [500, 300], [0, 300]]"), Json::parse("[[0, 0], [1, 0]]") },
                { "xy", Json::parse("[[0, -300], [500, -300], [500, -100], [0, -100]]"), Json::parse("[[1, 0], [0, 0]]") },
            };
            bool ok = true;
            for (const auto& [plane, profile, axis] : cases)
                for (const auto& [from, to] : { std::pair{ 0, 360 }, std::pair{ -90, 270 }, std::pair{ 30, 390 } })
                {
                    const auto tube = BuiltOf(Json::array({ Json{ { "plane", plane }, { "profile", profile }, { "axis", axis }, { "from", from }, { "to", to } } }));
                    ok = ok && tube && std::abs(Volume((*tube)[0].tris) / want - 1.0) < 1e-6;
                }
            // 不到一圈的由两个端面封上，朝外
            const double sector = k / 4.0 * (300.0 * 300.0 - 100.0 * 100.0) * 500.0;
            for (const auto& [from, to] : { std::pair{ 30, 120 }, std::pair{ -120, -30 } })
                for (const Json& axis : { Json::parse("[[1000, 0], [1000, 1]]"), Json::parse("[[1000, 1], [1000, 0]]") })
                {
                    const auto part = BuiltOf(Json::array({ Json{ { "plane", "xz" }, { "profile", across(1100, 1300) }, { "axis", axis }, { "from", from }, { "to", to } } }));
                    ok = ok && part && std::abs(Volume((*part)[0].tris) / sector - 1.0) < 1e-6;
                }
            Check(ok, "族：旋转体不论画在哪、往哪边转都朝外");
        }
        {
            const double k = 16.0 * std::sin(2.0 * kPi / 32.0);
            const Json ring = Json::parse("[[100, 0], [300, 0], [300, 500], [100, 500]]");
            const auto quarter = BuiltOf(Json::array({ Json{ { "plane", "xz" }, { "profile", ring }, { "axis", Json::parse("[[0, 0], [0, 1]]") }, { "to", 90 } } }));
            bool ok = quarter && std::abs(Geom::Area((*quarter)[0].outline) / (k / 4.0 * (300.0 * 300.0 - 100.0 * 100.0)) - 1.0) < 1e-6;
            if (ok)
                for (const P2& p : (*quarter)[0].outline)
                    ok = ok && p.x > -1e-6 && p.y > -1e-6;
            const auto down = BuiltOf(Json::array({ Json{ { "plane", "xz" }, { "profile", ring }, { "axis", Json::parse("[[0, 1], [0, 0]]") }, { "to", 90 } } }));
            ok = ok && down;
            if (ok)
                for (const P2& p : (*down)[0].outline)
                    ok = ok && p.x > -1e-6 && p.y < 1e-6;
            const auto wedge = BuiltOf(Json::parse(R"([{"plane": "xz", "profile": [[0, 0], [300, 0], [300, 500], [0, 500]], "axis": [[0, 0], [0, 1]], "to": 90}])"));
            ok = ok && wedge && std::abs(Geom::Area((*wedge)[0].outline) / (k / 4.0 * 300.0 * 300.0) - 1.0) < 1e-6;
            // 绕旁边的轴转的圆：圆环，体积是圆面积乘圆心走过的路
            const auto donut = BuiltOf(Json::parse(R"([{"plane": "xz", "circle": {"centre": [500, 100], "radius": 100}, "axis": [[0, 0], [0, 1]], "to": 360}])"));
            const double exact = 2.0 * kPi * kPi * 500.0 * 100.0 * 100.0;
            ok = ok && donut && std::abs(Volume((*donut)[0].tris) / exact - 1.0) < 0.03 && (*donut)[0].bottom == 0.0 && (*donut)[0].top == 200.0;
            // 网格没有轴可转
            const Family f = FamilyFrom(Json::parse(R"({"types": {"A": {}}, "forms": [{"mesh": [[[0, 0, 0], [1, 0, 0], [0, 1, 0]]], "axis": [[0, 0], [0, 1]]}]})"));
            std::string error;
            ok = ok && !f.Check([](const std::string&) { return true; }, error) && Contains(error, "axis");
            Check(ok, "族：绕竖直轴转不到一圈在平面上是一段环；圆转出圆环");
        }
        {
            const Json slab = Json::parse(R"({"profile": [[0, 0], [1000, 0], [1000, 1000], [0, 1000]], "from": 0, "to": 100})");
            const auto hole = [](int shown)
            {
                Json h = Json::parse(R"({"profile": [[400, 400], [600, 400], [600, 600], [400, 600]], "from": -10, "to": 110, "void": true})");
                h["visible"] = shown;
                return h;
            };
            const auto cut = BuiltOf(Json::array({ slab, hole(1) }));
            bool ok = cut && cut->size() == 1 && std::abs(Volume((*cut)[0].tris) - (1000.0 * 1000.0 - 200.0 * 200.0) * 100.0) < 1e-3;
            // 藏起来的什么都不切；离开板的也不切
            const auto hidden = BuiltOf(Json::array({ slab, hole(0) }));
            ok = ok && hidden && std::abs(Volume((*hidden)[0].tris) - 1e8) < 1e-3;
            const Json away = Json::parse(R"({"profile": [[2000, 0], [2100, 0], [2100, 100], [2000, 100]], "to": 100, "void": true})");
            const auto far = BuiltOf(Json::array({ slab, away }));
            ok = ok && far && std::abs(Volume((*far)[0].tris) - 1e8) < 1e-3;
            // 空心形体只切列在它前面的：后面的完整，同窗扇洞里的玻璃
            const Json pane = Json::parse(R"({"profile": [[450, 450], [550, 450], [550, 550], [450, 550]], "from": 0, "to": 100})");
            const auto both = BuiltOf(Json::array({ slab, hole(1), pane }));
            ok = ok && both && both->size() == 2 && std::abs(Volume((*both)[0].tris) - 9.6e7) < 1e-3 && std::abs(Volume((*both)[1].tris) - 1e6) < 1e-3;
            // 方块里挖出圆盆，同洗手盆
            const Json block = Json::parse(R"({"profile": [[-300, -300], [300, -300], [300, 300], [-300, 300]], "from": 0, "to": 200})");
            const Json bowl = Json::parse(R"({"plane": "xz", "profile": [[0, 50], [200, 50], [200, 250], [0, 250]], "axis": [[0, 0], [0, 1]], "to": 360, "void": true})");
            const auto basin = BuiltOf(Json::array({ block, bowl }));
            const double k = 16.0 * std::sin(2.0 * kPi / 32.0);
            ok = ok && basin && std::abs(Volume((*basin)[0].tris) - (600.0 * 600.0 * 200.0 - k * 200.0 * 200.0 * 150.0)) < 1.0;
            Check(ok, "族：空心形体切掉它穿过的形体，自己不显示");
        }
        {
            // 两个 64 角的旋转体，一个切另一个：做完，而且快
            Json ring = Json::array();
            for (size_t k = 0; k < Fam::MaxProfile; ++k)
            {
                const double a = static_cast<double>(k) * 2.0 * kPi / static_cast<double>(Fam::MaxProfile);
                ring.push_back(Json::array({ 400.0 + 100.0 * std::cos(a), 300.0 + 100.0 * std::sin(a) }));
            }
            const Json solid = Json{ { "plane", "xz" }, { "profile", ring }, { "axis", Json::parse("[[0, 0], [0, 1]]") }, { "to", 360 } };
            const Json cut = Json::parse(R"({"profile": [[-1000, -1000], [1000, -1000], [1000, 0], [-1000, 0]], "from": -10, "to": 1000, "void": true})");
            const auto pieces = BuiltOf(Json::array({ solid, cut }));
            Check(pieces && Volume((*pieces)[0].tris) > 0.0, "族：很多三角形的空心形体也切得完");
        }
        {
            // csg.rs：板减去盒子：穿过、顶面的凹槽、角上、整个一侧、里面的空腔、全包住
            const auto slab = Cuboid(P3{ 0, 0, 0 }, P3{ 1000, 1000, 100 });
            const std::tuple<const char*, P3, P3, double, double> cases[] = {
                { "through", P3{ 400, 400, -10 }, P3{ 600, 600, 110 }, 1e8 - 4e6, 2.4e6 },
                { "a pocket", P3{ 400, 400, 50 }, P3{ 600, 600, 100 }, 1e8 - 2e6, 2.44e6 },
                { "a corner", P3{ 0, 0, 0 }, P3{ 500, 500, 50 }, 1e8 - 1.25e7, 2.4e6 },
                { "a side", P3{ -10, 0, 0 }, P3{ 1010, 300, 100 }, 1e8 - 3e7, 1.74e6 },
                { "a cavity", P3{ 400, 400, 40 }, P3{ 600, 600, 60 }, 1e8 - 8e5, 2.496e6 },
                { "all of it", P3{ -1, -1, -1 }, P3{ 1001, 1001, 101 }, 0.0, 0.0 },
            };
            bool ok = true;
            for (const auto& [what, lo, hi, left, surface] : cases)
            {
                Csg::Meter meter;
                std::vector<Tri> got;
                std::string error;
                if (!Csg::Subtract(slab, Cuboid(lo, hi), meter, got, error) || std::abs(Volume(got) - left) >= 1e-3 || std::abs(Surface(got) - surface) >= 1e-3)
                {
                    std::printf("  %s：剩 %f，表面 %f\n", what, Volume(got), Surface(got));
                    ok = false;
                }
            }
            Check(ok, "CSG：空心形体不论在哪都从实体里拿走它的体积");
            // 减去自己什么都不剩；减去远处的原样；只贴着一个面的不拿走什么
            const auto a = Cuboid(P3{ 0, 0, 0 }, P3{ 100, 100, 100 });
            Csg::Meter meter;
            std::vector<Tri> self, away, beside;
            std::string error;
            ok = Csg::Subtract(a, a, meter, self, error) && self.empty() && Csg::Subtract(a, Cuboid(P3{ 500, 500, 500 }, P3{ 600, 600, 600 }), meter, away, error) &&
                 away == a && Csg::Subtract(a, Cuboid(P3{ 100, 0, 0 }, P3{ 200, 100, 100 }), meter, beside, error) && std::abs(Volume(beside) - 1e6) < 1e-3;
            Check(ok, "CSG：减去自己什么都不剩，减去远处的原样");
            // 三角形太多、工夫不够的说出来，不会没完没了；不是数的角点不让它崩
            std::vector<Tri> tube;
            const int n = 256;
            const auto at = [](double z, int k) { return P3{ 200.0 * std::cos(k * 2.0 * kPi / n), 200.0 * std::sin(k * 2.0 * kPi / n), z }; };
            for (int k = 0; k < n; ++k)
            {
                const P3 p = at(0, k), q = at(0, k + 1), r = at(500, k), t = at(500, k + 1);
                tube.push_back(Tri{ p, q, t });
                tube.push_back(Tri{ p, t, r });
                tube.push_back(Tri{ P3{ 0, 0, 500 }, r, t });
                tube.push_back(Tri{ P3{ 0, 0, 0 }, q, p });
            }
            const auto half = Cuboid(P3{ -1000, -1000, -10 }, P3{ 1000, 0, 510 });
            std::vector<Tri> got;
            Csg::Meter plenty;
            ok = Csg::Subtract(tube, half, plenty, got, error) && std::abs(Volume(got) / (Volume(tube) / 2.0) - 1.0) < 1e-3;
            const std::vector<Tri> many(Csg::MaxTriangles + 1, tube[0]);
            Csg::Meter m1, poor{ 1000, 1000 };
            ok = ok && !Csg::Subtract(many, half, m1, got, error) && Contains(error, "20000") && !Csg::Subtract(tube, half, poor, got, error) &&
                 Contains(error, "too many faces");
            const std::vector<Tri> odd{ Tri{ P3{ NAN, NAN, NAN }, P3{ NAN, NAN, NAN }, P3{ NAN, NAN, NAN } },
                                        Tri{ P3{ INFINITY, INFINITY, INFINITY }, P3{ INFINITY, INFINITY, INFINITY }, P3{ INFINITY, INFINITY, INFINITY } },
                                        Tri{ P3{ 0, 0, 0 }, P3{ 0, 0, 0 }, P3{ 0, 0, 0 } } };
            Csg::Meter m2, m3;
            ok = ok && Csg::Subtract(odd, half, m2, got, error) && Csg::Subtract(half, odd, m3, got, error);
            Check(ok, "CSG：很多面的切割做得完，恶意的说出来，不是数的角点不崩");
        }
        {
            // 内置族都检查通过：每种项目语言
            bool ok = true;
            for (const Language l : { Language::English, Language::Portuguese, Language::Spanish })
            {
                const auto lib = FamilyLibrary(l);
                ok = ok && lib.size() == 10 + 6 + 18;
                for (const auto& [n, f] : lib)
                {
                    std::string error;
                    if (!f.Check([](const std::string&) { return true; }, error))
                    {
                        std::printf("  %s：%s\n", n.c_str(), error.c_str());
                        ok = false;
                    }
                }
            }
            Check(ok, "族：新项目带的族都建得出来（各项目语言）");
        }
    }
}

namespace
{
    // ── font.rs ──────────────────────────────────────────────────
    void TestFont()
    {
        using Font::Strokes;
        const auto unknown = Strokes("?", 0, 0, 12);
        Check(Strokes("living", 0, 0, 12) == Strokes("LIVING", 0, 0, 12) && Strokes("\xCF\x89", 0, 0, 12) == unknown && Strokes(" ", 0, 0, 12).empty(),
              "单线字：小写按大写画，没有的字画成问号");
        const auto plain = Strokes("A", 0, 0, 12), acute = Strokes("\xC3\xA1", 0, 0, 12);
        Check(acute.size() == plain.size() + 1 && std::equal(plain.begin(), plain.end(), acute.begin()), "单线字：带附加符号的字母是基础字母加附加符号");
        // 西里尔大写各有各的样子，小写按大写画；与拉丁字母共用的字形相同
        const std::string all = "\xD0\x90\xD0\x91\xD0\x92\xD0\x93\xD0\x94\xD0\x95\xD0\x81\xD0\x96\xD0\x97\xD0\x98\xD0\x99\xD0\x9A\xD0\x9B\xD0\x9C\xD0\x9D\xD0\x9E"
                                "\xD0\x9F\xD0\xA0\xD0\xA1\xD0\xA2\xD0\xA3\xD0\xA4\xD0\xA5\xD0\xA6\xD0\xA7\xD0\xA8\xD0\xA9\xD0\xAA\xD0\xAB\xD0\xAC\xD0\xAD\xD0\xAE\xD0\xAF";
        std::vector<std::vector<std::vector<std::pair<float, float>>>> drawn;
        for (size_t i = 0; i < all.size(); i += 2)
            drawn.push_back(Strokes(all.substr(i, 2), 0, 0, 12));
        bool ok = true;
        for (size_t i = 0; i < drawn.size(); ++i)
        {
            ok = ok && !drawn[i].empty() && drawn[i] != unknown;
            for (size_t j = i + 1; j < drawn.size(); ++j)
                ok = ok && drawn[i] != drawn[j];
        }
        ok = ok && Strokes("\xD0\x90\xD0\x92\xD0\x95\xD0\x9A\xD0\x9C\xD0\x9D\xD0\x9E\xD0\xA0\xD0\xA1\xD0\xA2\xD0\xA5", 0, 0, 12) == Strokes("ABEKMHOPCTX", 0, 0, 12);
        ok = ok && Strokes("\xD1\x88\xD0\xBB\xD0\xB5\xD0\xB9\xD1\x84 \xD1\x91\xD0\xB6", 0, 0, 12) ==
                       Strokes("\xD0\xA8\xD0\x9B\xD0\x95\xD0\x99\xD0\xA4 \xD0\x81\xD0\x96", 0, 0, 12);
        Check(ok, "单线字：西里尔大写各有各的样子，小写按大写画");
        // 有哪些字：带附加符号的、德语的 ß、几个符号；没有希腊字母和汉字
        ok = true;
        for (const char32_t c : std::u32string(U"aÁőŰčŘåŁøßé€!?Жй"))
            ok = ok && Font::Has(c);
        for (const char32_t c : std::u32string(U"ωΣ漢"))
            ok = ok && !Font::Has(c);
        Check(ok && Strokes("\xC5\x90", 0, 0, 12).size() == Strokes("O", 0, 0, 12).size() + 2, "单线字：说得出哪些字有");
        const auto s = Strokes("I", 10, 100, 24);
        float lo = 1e9f, hi = -1e9f;
        for (const auto& line : s)
            for (const auto& [x, y] : line)
            {
                lo = std::min(lo, y);
                hi = std::max(hi, y);
            }
        Check(lo == 76.0f && hi == 100.0f && Font::Width("II", 12) == 19.0f, "单线字：按高度从基线缩放");
    }
}


namespace
{
    // ── 捕捉（snap.rs 的测试）────────────────────────────────────
    namespace SnapTest
    {
        constexpr Snap::Reach Reach{ 100.0, 60.0 };

        // 新项目最低的标高上
        Snap::Found At(const Model& m, P2 p, std::vector<P2> from, const Snap::Snaps& o, bool square = false)
        {
            return Snap::At(m, m.GuidOf(m.Levels().front().first), p, Reach, from, square, o);
        }

        double Degrees(P2 from, P2 to) { return std::atan2(to.y - from.y, to.x - from.x) * 180.0 / 3.14159265358979323846; }
    }

    void TestSnap()
    {
        using namespace SnapTest;
        using Snap::Kind;
        const Snap::Snaps o;
        {
            const Session s = Scripted(R"(wall {"points": [[0,0],[6000,0]]}
wall {"points": [[0,5000],[4000,5000]], "arc": 180})");
            const Model& m = s.GetModel();
            bool ok = At(m, { 30, -20 }, {}, o) == Snap::Found{ { 0, 0 }, Kind::Endpoint, std::nullopt };
            ok = ok && At(m, { 3040, 30 }, {}, o) == Snap::Found{ { 3000, 0 }, Kind::Midpoint, std::nullopt };
            // 圆弧的圆心与它长度的中点
            ok = ok && At(m, { 2030, 4980 }, {}, o) == Snap::Found{ { 2000, 5000 }, Kind::Centre, std::nullopt };
            const auto mid = At(m, { 2030, 3020 }, {}, o);
            ok = ok && mid.kind == Kind::Midpoint && std::abs(mid.at.Dist({ 2000, 5000 }) - 2000.0) < 1e-3;
            // 超出它们的范围：墙的中心线，再是最近的 10 毫米
            ok = ok && At(m, { 1234, 40 }, {}, o) == Snap::Found{ { 1230, 0 }, Kind::Nearest, std::nullopt };
            ok = ok && At(m, { 1234, 900 }, {}, o) == Snap::Found{ { 1230, 900 }, Kind::Free, std::nullopt };
            Check(ok, "捕捉：墙的端点、中点、圆心，墙的中心线，最近的 10 毫米");
        }
        {
            const Session s = Scripted(R"(wall {"points": [[0,0],[6000,0]]}
wall {"points": [[2000,-1000],[2000,3000]]})");
            const Model& m = s.GetModel();
            bool ok = At(m, { 2040, 30 }, {}, o) == Snap::Found{ { 2000, 0 }, Kind::Intersection, std::nullopt };
            // 闭合一圈：点过的第一个点
            const auto f = At(m, { 5030, 5020 }, { { 5000, 5000 }, { 5000, 4000 }, { 3000, 4000 } }, o);
            ok = ok && f.at == P2{ 5000, 5000 } && f.kind == Kind::Endpoint;
            Check(ok, "捕捉：墙相交处，点过的点是端点");
        }
        {
            const Session s = Scripted(R"(wall {"points": [[0,0],[6000,2000]]})");
            const Model& m = s.GetModel();
            const auto f = At(m, { 2130, 680 }, { { 1000, 4000 } }, o);
            bool ok = f.kind == Kind::Perpendicular && f.at.Dist({ 2100, 700 }) < 1e-6 && f.guide && f.guide->from == P2{ 1000, 4000 };
            ok = ok && std::abs(f.guide->angle - std::fmod(Degrees({ 1000, 4000 }, { 2100, 700 }) + 360.0, 360.0)) < 1e-6;
            Snap::Snaps off = o;
            off.perpendicular = false;
            ok = ok && At(m, { 2130, 680 }, { { 1000, 4000 } }, off).kind != Kind::Perpendicular;
            Check(ok, "捕捉：从上一个点作的垂足");
        }
        {
            const Session s0 = Scripted("");
            const Model& m = s0.GetModel();
            // 离第一点 30.2°：落到 30°，长度取到最近的 10 毫米
            auto f = At(m, { 3470, 2020 }, { { 0, 0 } }, o);
            bool ok = f.kind == Kind::Angle && std::abs(Degrees({ 0, 0 }, f.at) - 30.0) < 1e-4;
            ok = ok && std::abs(f.at.Length() / 10.0 - std::round(f.at.Length() / 10.0)) < 1e-3;
            ok = ok && f.guide && f.guide->from == P2{ 0, 0 } && std::round(f.guide->angle) == 30.0 && !f.guide->corner;
            // 22.5° 在两条引导线之间：自由
            f = At(m, { 3000, 1243 }, { { 0, 0 } }, o);
            ok = ok && f.kind == Kind::Free && !f.guide;
            Snap::Snaps none = o;
            none.angle = 0.0;
            ok = ok && At(m, { 3470, 2020 }, { { 0, 0 } }, none) == Snap::Found{ { 3470, 2020 }, Kind::Free, std::nullopt };
            // 从前一段量：20° 的墙之后，与它成 90° 是 110°，不是 15° 的倍数
            const double r20 = 20.0 * 3.14159265358979323846 / 180.0, r110 = 110.0 * 3.14159265358979323846 / 180.0;
            const P2 prev{ 0, 0 }, last = P2{ std::cos(r20), std::sin(r20) } * 3000.0;
            const P2 want = last + P2{ std::cos(r110), std::sin(r110) } * 2000.0;
            f = At(m, want + P2{ 25, 10 }, { prev, last }, o);
            ok = ok && f.kind == Kind::Angle && std::abs(Degrees(last, f.at) - 110.0) < 1e-3 && f.guide && f.guide->corner &&
                 std::abs(*f.guide->corner - 90.0) < 1e-9;
            Check(ok, "捕捉：方向落到从正东、从前一段量的角度上");
            // 225° 从 (4000, 0) 经过 (2000, -2000)，长 2828.4：不取整到 2830
            ok = At(m, { 2000, -2000 }, { { 4000, 0 } }, o) == Snap::Found{ { 2000, -2000 }, Kind::Angle,
                                                                           Snap::Guide{ { 4000, 0 }, 225.0, std::nullopt } };
            ok = ok && At(m, { 2003, -2003 }, { { 4000, 0 } }, o).at == P2{ 2000, -2000 };
            Check(ok, "捕捉：正好在引导线上的整点不动");
            // 方正：从不在整点上的点出发
            const std::vector<P2> from{ { 2501.5, 1000.25 } };
            ok = Snap::At(m, m.GuidOf(m.Levels().front().first), { 5000, 1003 }, Reach, from, true, Snap::Snaps::None()).at == P2{ 5000, 1000.25 };
            ok = ok && Snap::At(m, m.GuidOf(m.Levels().front().first), { 2503, 4000 }, Reach, from, true, Snap::Snaps::None()).at == P2{ 2501.5, 4000 };
            Check(ok, "捕捉：Shift 让一步保持方正");
            // 每条引导线都读得出与前一段的夹角，不管它朝哪
            ok = At(m, { 1000, 2000 }, { { 0, 0 }, { 1000, 0 } }, o).guide.has_value();
            for (const P2 line : { P2{ 1000, 0 }, P2{ 0, 1000 }, P2{ -1000, 0 }, P2{ 0, -1000 }, P2{ 940, 342 } })
                for (int k = 0; k < 24; ++k)
                {
                    const double a = k * 15.0 * 3.14159265358979323846 / 180.0;
                    const auto g = At(m, line + P2{ std::cos(a), std::sin(a) } * 2000.0, { { 0, 0 }, line }, o).guide;
                    ok = ok && g && g->corner;
                }
            Check(ok, "捕捉：引导线的夹角朝哪都读得出");
        }
        {
            const std::vector<P2> from{ { 0, 0 } };
            const auto r7 = Snap::Rays(from, 7.0);
            Check(!r7.empty() && r7.back() == 357.0 && Snap::Rays(from, 170.0) == std::vector<double>{ 0, 170, 340 } &&
                      Snap::Rays(from, 15.0).size() == 24 && Snap::Rays(from, 90.0) == std::vector<double>{ 0, 90, 180, 270 },
                  "捕捉：角度的每一步都有引导线，转到最后一条");
        }
        {
            const Session s = Scripted(R"(wall {"points": [[3000,-3000],[3000,3000]]})");
            const Model& m = s.GetModel();
            // 30° 从原点与 x = 3000 相交于 y = 1732.05
            const auto f = At(m, { 3010, 1700 }, { { 0, 0 } }, o);
            Check(f.kind == Kind::Intersection && f.guide && f.at.Dist({ 3000, 3000 * std::tan(30.0 * 3.14159265358979323846 / 180.0) }) < 1e-2,
                  "捕捉：引导线停在它穿过墙的地方");
        }
        {
            const Session s = Scripted(R"(wall {"points": [[0,0],[6000,0]]})");
            const Model& m = s.GetModel();
            Snap::Snaps a = o, b = o, c = o;
            a.endpoint = false;
            b.midpoint = false;
            c.endpoint = c.nearest = false;
            Check(At(m, { 30, -20 }, {}, a).kind == Kind::Nearest && At(m, { 3040, 30 }, {}, b).kind == Kind::Nearest &&
                      At(m, { 34, -20 }, {}, c).at == P2{ 30, -20 },
                  "捕捉：每种捕捉单独关得掉");
            // 光标、点过的点、开关里什么怪值都不让它崩
            const double nan = std::nan(""), inf = INFINITY;
            const std::vector<std::vector<P2>> chains{ {}, { { 0, 0 } }, { { nan, 1 }, { 1, 1 } }, { { 5, 5 }, { 5, 5 } }, { { inf, -inf } } };
            for (const P2 p : { P2{ nan, 0 }, P2{ inf, -inf }, P2{ 1e300, 1e300 }, P2{ 0, 0 }, P2{ -0.0, 1e-300 } })
                for (const auto& from : chains)
                    for (const double angle : { 15.0, nan, 1.0, 1e9 })
                        for (const bool square : { true, false })
                        {
                            Snap::Snaps x = o;
                            x.angle = angle;
                            (void)Snap::At(m, m.GuidOf(m.Levels().front().first), p, Reach, from, square, x);
                        }
            for (const Snap::Reach r : { Snap::Reach{ 0, 0 }, Snap::Reach{ nan, -1 }, Snap::Reach{ inf, inf } })
                (void)Snap::At(m, m.GuidOf(m.Levels().front().first), { 10, 10 }, r, std::vector<P2>{ { 0, 0 } }, false, o);
            Check(true, "捕捉：怪值不崩");
        }
        {
            const Session s = Scripted(R"(wall {"points": [[0,0],[60000,0]]}
wall {"points": [[0,30000],[10000,30000]], "arc": 180})");
            const Model& m = s.GetModel();
            bool ok = At(m, { 31234, 40 }, {}, o) == Snap::Found{ { 31230, 0 }, Kind::Nearest, std::nullopt };
            // 圆弧鼓出去的地方，离弦 5000 毫米：超出捕捉范围和最厚的墙
            P2 want;
            for (const auto& [id, e] : m.elements.Items())
                if (const Wall* w = e->As<Wall>(); w && w->arc != 0.0)
                {
                    const Walls::Axis a = Walls::Axis::Of(*w);
                    want = a.At(a.Length() / 2.0, 0.0);
                }
            const auto f = At(m, want + P2{ 30, 20 }, {}, o);
            ok = ok && std::abs(want.y - 30000.0) > 4999.0 && f.kind == Kind::Midpoint && f.at.Dist(want) < 1e-3;
            Check(ok, "捕捉：离两端都远的墙、圆弧鼓出去的地方也找得到");
        }
        {
            // 100 毫米长的墙：中点离两端各 50
            const Session s = Scripted(R"(wall {"points": [[0,0],[100,0]]})");
            const Model& m = s.GetModel();
            Check(At(m, { 25, 0 }, {}, o) == Snap::Found{ { 0, 0 }, Kind::Endpoint, std::nullopt } &&
                      At(m, { 50, 0 }, {}, o) == Snap::Found{ { 50, 0 }, Kind::Midpoint, std::nullopt },
                  "捕捉：一样远时端点胜过中点");
        }
        {
            const Snap::Ruler r = Snap::Measure({ 0, 0 }, { 3000, 4000 });
            bool ok = r.length == 5000.0 && r.dx == 3000.0 && r.dy == 4000.0 && std::abs(r.angle - 53.130102354) < 1e-6;
            ok = ok && Snap::Measure({ 0, 0 }, { -1000, 0 }).angle == 180.0 && Snap::Measure({ 0, 0 }, { 0, -1000 }).angle == 270.0 &&
                 Snap::Measure({ 5, 5 }, { 5, 5 }).angle == 0.0;
            ok = ok && Snap::Graduation(1.0) == 10.0 && Snap::Graduation(0.5) == 20.0 && Snap::Graduation(0.1) == 100.0 &&
                 std::abs(Snap::Graduation(0.02) - 500.0) < 1e-9 && std::abs(Snap::Graduation(0.004) - 5000.0) < 1e-9;
            ok = ok && Snap::Graduation(0.0) == 100.0 && Snap::Graduation(std::nan("")) == 100.0;
            Check(ok, "标尺：长度、分量与角度，刻度取整");
        }
        {
            // 门窗所在的一段：两面隔墙之间；离中点近时吸到中点
            const Session s = Scripted(R"(wall {"points": [[0,0],[9000,0]]}
wall {"points": [[3000,0],[3000,4000]]}
wall {"points": [[7000,0],[7000,4000]]})");
            const Model& m = s.GetModel();
            Opening op;
            std::string error;
            const bool made = OpeningOf(m, Json{ { "at", Json::array({ 5100, 0 }) } }, true, op, error);
            const auto span = made ? m.OpeningSpan(op) : std::nullopt;
            const double half = made ? m.Thickness(*m.GetWall(op.host)) / 2.0 : 0.0;
            bool ok = span && std::abs(span->first - (3000 + half)) < 1e-6 && std::abs(span->second - (7000 - half)) < 1e-6;
            ok = ok && m.Centred(op, 200.0) == std::optional<double>(5000.0) && !m.Centred(op, 50.0);
            Check(ok, "门窗：所在的一段量到相交墙的面，近中点时吸过去：" + error);
            // 预览：还不在模型里的门，画出来的各项都归它
            const Render::View v = Render::Fit({ { 0, 0 }, { 9000, 4000 } }, 800, 600, 100.0);
            const Render::Drawing d = Render::Sketch(m, m.GuidOf(m.Levels().front().first), v, Element{ Door{ op } });
            const Id next = m.elements.LastId().value_or(0) + 1;
            ok = !d.prims.empty() && std::all_of(d.owners.begin(), d.owners.end(), [&](const auto& o2) { return o2 && *o2 >= next; });
            Check(ok && m.elements.Size() == s.GetModel().elements.Size(), "平面：预览还不在模型里的元素");
        }
    }
}

namespace
{
    // ── 草图（sketch.rs 的测试，直线与圆弧的部分）──────────────────
    namespace SketchTest
    {
        using Sketch::Problem;
        using K = Problem::Kind;

        SketchLine L(P2 a, P2 b)
        {
            SketchLine l;
            l.start = a;
            l.end   = b;
            return l;
        }

        std::vector<SketchLine> RingOf(std::vector<P2> pts)
        {
            std::vector<SketchLine> out;
            for (size_t i = 0; i < pts.size(); ++i)
                out.push_back(L(pts[i], pts[(i + 1) % pts.size()]));
            return out;
        }

        std::vector<SketchLine> Square(double x, double y, double s) { return RingOf({ { x, y }, { x + s, y }, { x + s, y + s }, { x, y + s } }); }

        std::vector<Problem> Fails(const std::vector<SketchLine>& ls)
        {
            Sketch::Profile p;
            std::vector<Problem> e;
            Sketch::Solve(ls, p, e);
            return e;
        }

        bool Has(const std::vector<Problem>& e, const Problem& x) { return std::find(e.begin(), e.end(), x) != e.end(); }
    }

    void TestSketch()
    {
        using namespace SketchTest;
        std::vector<Problem> e;
        Geom::Region r;
        Sketch::Profile p;
        {
            auto ls = Square(0, 0, 4000);
            std::swap(ls[0], ls[2]);
            ls[1] = L(ls[1].end, ls[1].start);
            ls[3] = L(ls[3].end, ls[3].start);
            ls[0] = L(ls[0].start + P2{ 0.4, 0 }, ls[0].end);
            bool ok = Sketch::SolveOne(ls, r, e) && Geom::Area(r.outer) > 0.0 && r.holes.empty() && std::abs(r.Area() - 16e6) < 2000.0;
            ok = ok && Sketch::SolveOne(RingOf({ { 0, 0 }, { 0, 3000 }, { 3000, 3000 }, { 3000, 0 } }), r, e) && Geom::Area(r.outer) > 0.0;
            Check(ok, "草图：线随便先后、随便方向，接成一个逆时针的外轮廓");
        }
        {
            auto ls = Square(0, 0, 10000);
            for (const auto& more : { Square(2000, 2000, 6000), Square(4000, 4000, 2000), Square(20000, 0, 1000) })
                ls.insert(ls.end(), more.begin(), more.end());
            bool ok = Sketch::Solve(ls, p, e) && p.regions.size() == 3 && p.regions[0].holes.size() == 1 && Geom::Area(p.regions[0].holes[0]) < 0.0 &&
                      p.regions[1].holes.empty() && std::abs(p.Area() - (100e6 - 36e6 + 4e6 + 1e6)) < 1.0;
            ok = ok && !Sketch::SolveOne(ls, r, e) && e == std::vector<Problem>{ Problem{ K::Separate, {}, std::nullopt, 3 } };
            Check(ok, "草图：环里的环是洞，洞里的是岛");
        }
        {
            auto ls = Square(0, 0, 4000);
            ls.pop_back();
            e = Fails(ls);
            bool ok = e == std::vector<Problem>{ Problem{ K::Open, { 0 }, P2{ 0, 0 } }, Problem{ K::Open, { 2 }, P2{ 0, 4000 } } } &&
                      e[0].Said([](size_t i) { return "line " + std::to_string(i); }).find("open") != std::string::npos;
            ls = Square(0, 0, 4000);
            ls[3] = L(ls[3].start, ls[3].end + P2{ 0, 5 });
            e = Fails(ls);
            ok = ok && e.size() == 2 && e[0].kind == K::Open && e[0].lines[0] == 0 && e[1].lines[0] == 3;
            Check(ok, "草图：没闭合的环说出开着的端点");
        }
        {
            e = Fails(RingOf({ { 0, 0 }, { 4000, 4000 }, { 4000, 0 }, { 0, 4000 } }));
            bool ok = e == std::vector<Problem>{ Problem{ K::Cross, { 0, 2 }, P2{ 2000, 2000 } } };
            auto ls = Square(0, 0, 4000);
            ls.push_back(L({ 2000, 0 }, { 2000, 2000 }));
            e = Fails(ls);
            ok = ok && Has(e, Problem{ K::Cross, { 0, 4 }, P2{ 2000, 0 } }) && Has(e, Problem{ K::Open, { 4 }, P2{ 2000, 2000 } });
            ls = Square(0, 0, 4000);
            const auto other = Square(2000, 2000, 4000);
            ls.insert(ls.end(), other.begin(), other.end());
            e = Fails(ls);
            std::set<size_t> in;
            for (const Problem& x : e)
                in.insert(x.lines.begin(), x.lines.end());
            ok = ok && in == std::set<size_t>{ 1, 2, 4, 7 } && std::all_of(e.begin(), e.end(), [](const Problem& x) { return x.kind == K::Cross; });
            Check(ok, "草图：相交的线、一条端在另一条中间");
        }
        {
            auto ls = Square(0, 0, 4000);
            ls.push_back(L({ 1000, 0 }, { 3000, 0 }));
            bool ok = Has(Fails(ls), Problem{ K::Overlap, { 0, 4 } });
            ls = Square(0, 0, 4000);
            ls.push_back(L(ls[2].end, ls[2].start));
            ok = ok && Has(Fails(ls), Problem{ K::Overlap, { 2, 4 } });
            ok = ok && Sketch::SolveOne(RingOf({ { 0, 0 }, { 2000, 0 }, { 4000, 0 }, { 4000, 4000 }, { 0, 4000 } }), r, e);
            Check(ok, "草图：叠在一起的线、画了两遍的线；首尾相接的一条直边没问题");
        }
        {
            auto ls = Square(0, 0, 4000);
            const auto other = Square(4000, 4000, 4000);
            ls.insert(ls.end(), other.begin(), other.end());
            e = Fails(ls);
            Check(e == std::vector<Problem>{ Problem{ K::Branch, { 1, 2, 4, 7 }, P2{ 4000, 4000 } } } &&
                      e[0].Said([](size_t i) { return "line " + std::to_string(i); }).find("line 1, line 2, line 4 and line 7") != std::string::npos,
                  "草图：两个环碰在一个角上，那里分叉");
        }
        {
            auto ls = Square(0, 0, 4000);
            ls.push_back(L({ 9000, 0 }, { 9000.5, 0 }));
            ls.push_back(L({ std::nan(""), 0 }, { 1, 1 }));
            ls.push_back(L({ 0, 0 }, { 2e9, 0 }));
            bool ok = Fails(ls) == std::vector<Problem>{ Problem{ K::TooShort, { 4 } }, Problem{ K::OutOfRange, { 5 } }, Problem{ K::OutOfRange, { 6 } } };
            ok = ok && Fails({}) == std::vector<Problem>{ Problem{ K::Empty } };
            const std::vector<SketchLine> many(Sketch::MaxLines + 1, L({ 0, 0 }, { 10, 0 }));
            e = Fails(many);
            ok = ok && e.size() == 1 && e[0].kind == K::TooMany && e[0].count == Sketch::MaxLines + 1 && e[0].what == "lines";
            Check(ok, "草图：太短、超出范围的线先说");
        }
        {
            e = Fails(RingOf({ { 0, 0 }, { 4000, 0.5 }, { 8000, 0 } }));
            bool ok = !e.empty() && std::all_of(e.begin(), e.end(), [](const Problem& x) { return x.kind == K::Overlap; });
            const double h = 1.5 * std::sqrt(3.0) / 2.0;
            ok = ok && Fails(RingOf({ { 0, 0 }, { 1.5, 0 }, { 0.75, h } })) == std::vector<Problem>{ Problem{ K::NoArea, { 0, 1, 2 } } };
            Check(ok, "草图：扁的环自己叠着，太小的环围不出面积");
        }
        {
            bool ok = true;
            for (const auto& [gap, meets] : { std::pair{ 1.0, true }, std::pair{ 0.999, true }, std::pair{ 1.01, false } })
            {
                auto ls = Square(0, 0, 4000);
                ls[3] = L(ls[3].start, { -gap, 0 });
                ok = ok && Sketch::Solve(ls, p, e) == meets;
            }
            auto ls = Square(0, 0, 4000);
            ls[0] = L({ 0.9, 0.9 }, ls[0].end);
            ls[3] = L(ls[3].start, { 1.1, 1.1 });
            ok = ok && Sketch::Solve(ls, p, e);
            Check(ok, "草图：端点 1 毫米以内接上，再远就不接，落在哪个格子都一样");
        }
        {
            const Ring outline{ { 0, 0 }, { 6000, 0 }, { 6000, 6000 }, { 0, 6000 } }, hole{ { 1000, 1000 }, { 1000, 2000 }, { 2000, 2000 }, { 2000, 1000 } };
            const auto ls = Sketch::LinesOf(outline, { hole });
            bool ok = ls.size() == 8 && Sketch::SolveOne(ls, r, e) && r.outer == outline && r.holes == std::vector<Ring>{ hole };
            ok = ok && Sketch::Draws(Sketch::LinesOf(outline, {}), outline, {}) && Sketch::Draws(Sketch::LinesOf(outline, {}), Ring(outline.rbegin(), outline.rend()), {});
            Ring moved;
            for (const P2& q : outline)
                moved.push_back(q + P2{ 10, 0 });
            ok = ok && !Sketch::Draws(Sketch::LinesOf(outline, {}), moved, {}) && !Sketch::Draws(Sketch::LinesOf(outline, {}), outline, { outline });
            Check(ok, "草图：轮廓与洞变回能解回它们的线；记着的草图从哪起、朝哪转都算画着轮廓");
        }
        {
            std::vector<SketchLine> ls{ L({ 0, 0 }, { 1e6, 0 }) };
            for (int i = 1; i < 500; ++i)
                ls.push_back(L({ i * 1000.0, -500 }, { i * 1000.0, 500 }));
            e = Fails(ls);
            Check(e.size() == Sketch::MaxProblems && Sketch::Say(e, Sketch::Namer(ls)).find("more") != std::string::npos, "草图：问题多时只说前面的");
        }
        {
            // 半圆：直径，再从顶上的圆弧回来
            const std::vector<SketchLine> ls{ L({ 0, 0 }, { 4000, 0 }), Sketch::ArcThrough({ 4000, 0 }, { 0, 0 }, { 2000, 2000 }) };
            const double half = 3.14159265358979323846 * 2000.0 * 2000.0 / 2.0;
            bool ok = Sketch::SolveOne(ls, r, e) && std::abs(r.Area() - half) / half < 0.01 && r.outer.size() > 20 &&
                      Sketch::Middle(ls[1]).Dist({ 2000, 2000 }) < 1.0;
            auto crossed = ls;
            crossed.push_back(L({ 1000, 1000 }, { 1000, 3000 }));
            e = Fails(crossed);
            ok = ok && !e.empty() && e[0].kind == K::Cross && e[0].lines == std::vector<size_t>{ 1, 2 } &&
                 std::count_if(e.begin(), e.end(), [](const Problem& x) { return x.kind == K::Cross; }) == 1;
            ok = ok && Sketch::Points(Sketch::ArcThrough({ 0, 0 }, { 4000, 0 }, { 2000, 0.5 })).size() == 2;
            Check(ok, "草图：圆弧闭合一个环，切成多少段弦都算一条线");
        }
        {
            auto ls = RingOf({ { 0, 0 }, { 4000, 0 }, { 4000, 4000 } });
            ls.resize(2);
            ls.push_back(L({ 4000, 4500 }, { -500, 4500 }));
            ls.push_back(L({ 0, 5000 }, { 0, 0 }));
            e = Fails(ls);
            ls[3].id = 999812;
            const std::string said = Sketch::Say(e, Sketch::Namer(ls));
            Check(!e.empty() && e[0].kind == K::Cross && std::any_of(e.begin(), e.end(), [](const Problem& x) { return x.kind == K::Open; }) &&
                      said.find("line 2 and line 999812 intersect at [0, 4500]") != std::string::npos,
                  "草图：先说相交，再说它留下的开口；有 ID 的线按 ID 叫");
        }
    }
}


int main()
{
    // 不缓冲：崩溃时也看得到崩在哪一项之后
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestGeom();
    TestUnits();
    TestIds();
    TestLanguage();
    TestModel();
    TestWalls();
    TestLibrary();
    TestSkeleton();
    TestFamily();
    TestFont();
    TestSnap();
    TestSketch();
    std::printf("\n%s：%d 项失败\n", g_failed == 0 ? "全部通过" : "有失败", g_failed);
    return g_failed == 0 ? 0 : 1;
}
