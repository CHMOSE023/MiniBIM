// 模型核心（src/Model）：平面几何、单位、ID 与 GUID、项目语言、元素与 Model::Check、墙的几何、库。
// 移植自 BIMCraft crates/core/src/{geom,units,ids,language,model,walls,walls/index,library}.rs 的测试（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Commands/Commands.h"
#include "Commands/Sample.h"
#include "Commands/Session.h"
#include "Model/Geom.h"
#include "Model/Language.h"
#include "Model/Model.h"
#include "Model/ModelJson.h"
#include "Model/Units.h"
#include "Model/Walls.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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
window {"at": [8000, 3000]})");
            // 屋顶在第 2b 步移植：BIMCraft 这个测试的后半（墙贴着双坡屋顶）以后补上
            Check(Walls::Prisms(s.GetModel(), 100003, true).size() == 4 && Walls::Prisms(s.GetModel(), 100003, false).size() == 1,
                  "墙：在门窗处分成棱柱（窗下、窗上、两侧）");
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
            bool ok = m.LibraryInStep() && m.library.Size() == 3 + 1 + 2 + 2 + 17;
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
    std::printf("\n%s：%d 项失败\n", g_failed == 0 ? "全部通过" : "有失败", g_failed);
    return g_failed == 0 ? 0 : 1;
}
