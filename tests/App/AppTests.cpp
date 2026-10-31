// 主窗口里不碰界面控件的部分（apps/Common/src/GUI/Parameters.*、Recent.*）：特性面板的参数表、值的写法与读法，项目浏览器的搜索，最近打开。
// 移植自 BIMCraft crates/ui-egui/src/browser.rs、recent.rs 的测试（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Commands/Commands.h"
#include "Commands/Sample.h"
#include "Commands/Session.h"
#include "GUI/Parameters.h"
#include "GUI/Recent.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace MiniBIM;
using Bim::Json;
using Props::Kind;

namespace
{
    int g_failed = 0;
    void Check(bool ok, const std::string& label)
    {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
        if (!ok)
            ++g_failed;
    }

    bool Has(std::span<const Props::Row> rows, const char* key)
    {
        for (const Props::Row& r : rows)
            if (std::string(r.key) == key)
                return true;
        return false;
    }

    void TestRows()
    {
        Check(Has(Props::RowsOf("wall"), "location") && Has(Props::RowsOf("wall"), "top_level"), "rows: a wall's constraints");
        Check(Has(Props::RowsOf("door"), "mark") && Has(Props::RowsOf("window"), "sill"), "rows: doors and windows");
        Check(Props::RowsOf("stair").empty() && Props::RowsOf("").empty(), "rows: none for a category not ported yet");
        Check(Has(Props::PlanRows(), "view_range.cut"), "rows: a plan's view range");
        Check(std::string(Props::CategoryName("roomseparator", true)) == "Room Separators", "category: plural name");
    }

    void TestFields()
    {
        const Json e = Json::parse(R"({"name":"Level 1","view_range":{"cut":1200,"top":null},"flag":true})");
        Check(Props::FieldOf(e, "view_range.cut", Kind::Length) == Json(1200), "field: through a dot");
        Check(Props::FieldOf(e, "view_range.top", Kind::Limit).is_null(), "field: null is none");
        Check(Props::FieldOf(e, "offset", Kind::Length) == Json(0.0), "field: a missing length is 0");
        Check(Props::FieldOf(e, "mark", Kind::Text).is_null(), "field: a missing text is none");
        Check(Props::Nested("view_range.cut", 900) == Json::parse(R"({"view_range":{"cut":900}})"), "nested: by the dots");
        Check(Props::Nested("mark", "A") == Json::parse(R"({"mark":"A"})"), "nested: no dot");
    }

    void TestShown()
    {
        const Bim::Units mm = Bim::Units::Millimetres;
        Check(Props::Text(Json(3000.0)) == "3000" && Props::Text(Json(2.456)) == "2.46" && Props::Text(Json(-0.001)) == "0",
              "text: two decimals at most");
        Check(Props::Text(Json(true)) == "Yes" && Props::Text(Json::array({ 1, "a" })) == "1, a", "text: flags and lists");
        Check(Props::Shown(Kind::Point, Json::array({ 1000.0, -250.0 }), mm) == "1000, -250", "shown: a point");
        Check(Props::Shown(Kind::AreaShown, Json(21.5), mm) == "21.50 m²", "shown: an area");
        Check(Props::Shown(Kind::MetresShown, Json(18.2), Bim::Units::Metres) == Bim::UnitsOf::Length(Bim::Units::Metres, 18200.0),
              "shown: metres as a length");
        Check(Props::Shown(Kind::Limit, Json(nullptr), mm).empty(), "shown: no limit is blank");
    }

    void TestParse()
    {
        const Bim::Units mm = Bim::Units::Millimetres;
        Json v;
        std::string error;
        Check(Props::Parse(Kind::Length, " 2400 ", mm, v, error) && v == Json(2400.0), "parse: a length");
        Check(Props::Parse(Kind::Length, "3 m", mm, v, error) && v == Json(3000.0), "parse: a length with its unit");
        Check(!Props::Parse(Kind::Length, "tall", mm, v, error) && !error.empty(), "parse: not a length");
        Check(Props::Parse(Kind::Point, "1000, 2000", mm, v, error) && v == Json::array({ 1000.0, 2000.0 }), "parse: a point");
        Check(!Props::Parse(Kind::Point, "1000", mm, v, error), "parse: a point needs x, y");
        Check(Props::Parse(Kind::Point, "10' 6\", 2'", Bim::Units::FeetInches, v, error) && v.size() == 2, "parse: feet and inches hold spaces");
        Check(Props::Parse(Kind::Number, "30", mm, v, error) && v == Json(30.0), "parse: a number");
        Check(!Props::Parse(Kind::Number, "30x", mm, v, error), "parse: not a number");
        Check(Props::Parse(Kind::Limit, "  ", mm, v, error) && v.is_null(), "parse: a blank limit is none");
        Check(Props::Parse(Kind::Text, "D1", mm, v, error) && v == Json("D1"), "parse: text as typed");
    }

    // 照 browser.rs 的测试
    void TestSearch()
    {
        Check(Props::Words("  ceiling   2 ") == std::vector<std::string>{ "ceiling", "2" }, "search: words");
        Check(Props::Matches("Level 2", "level") && Props::Matches("Level 2", "EL 2"), "search: a word matches anywhere, case aside");
        Check(Props::Matches("Level 2", "l*2") && Props::Matches("Level 2", "lev?l") && !Props::Matches("Level 2", "lev?x"),
              "search: a star is any run and a question mark any one");
        Check(Props::Matches("标高 2", "标高") && !Props::Matches("标高 2", "楼板"), "search: whatever the letters");
        Check(Props::Matches("anything", "*") && !Props::Matches("Level", "Levels"), "search: a star matches anything; more than the text, nothing");
    }

    // 示例房屋里每个元素的每个可写参数：照面板显示的写回去，modify 都接受（面板的写法与命令一致）
    void TestRoundTrip()
    {
        Bim::Session s;
        std::vector<Json> out;
        std::string error;
        Check(s.Script(Bim::SampleScript, out, error), "round trip: the sample house");
        const Bim::Model m = s.GetModel();
        int edits = 0, failed = 0;
        for (const auto& [id, e] : m.elements.Items())
        {
            const Json j = Bim::ElementJson(m, id);
            for (const Props::Row& r : Props::RowsOf(e->Category()))
            {
                const Json val = Props::FieldOf(j, r.key, r.kind);
                Json to;
                switch (r.kind)
                {
                case Kind::Text: case Kind::Number: case Kind::Length: case Kind::Point: case Kind::Limit: case Kind::LengthIf:
                    if (r.kind == Kind::LengthIf && val.is_null())
                        continue;
                    if (!Props::Parse(r.kind, Props::Shown(r.kind, val, m.units), m.units, to, error))
                    {
                        ++failed;
                        std::printf("  %s %llu %s: %s\n", e->Category(), static_cast<unsigned long long>(id), r.key, error.c_str());
                        continue;
                    }
                    break;
                case Kind::Flag:
                    to = val.is_boolean() && val.get<bool>();
                    break;
                case Kind::LocationLine:
                    to = val.is_string() ? val : Json("centre");
                    break;
                default:
                    continue;
                }
                Json p = Props::Nested(r.key, to);
                p["ids"] = Json::array({ id });
                Json result;
                ++edits;
                if (!s.Execute("modify", p, result, error))
                {
                    ++failed;
                    std::printf("  %s %llu %s: %s\n", e->Category(), static_cast<unsigned long long>(id), r.key, error.c_str());
                }
            }
        }
        Check(edits > 50 && failed == 0, "round trip: every field the palette writes, as it shows it, modify takes (" + std::to_string(edits) + ")");
    }

    // ── 最近打开 ─────────────────────────────────────────────
    std::string Scratch(const std::string& name)
    {
        const std::u8string u8 = (std::filesystem::temp_directory_path() / ("minibim-recent-" + name + ".json")).u8string();
        return std::string(u8.begin(), u8.end());
    }

    std::string Abs(const std::string& p)
    {
        const std::u8string u8 = std::filesystem::absolute(std::filesystem::path(std::u8string(p.begin(), p.end()))).lexically_normal().u8string();
        return std::string(u8.begin(), u8.end());
    }

    void Write(const std::string& path, const std::string& text)
    {
        std::ofstream(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary) << text;
    }

    void TestRecent()
    {
        RecentFiles r;
        for (size_t n = 0; n < RecentFiles::MaxFiles + 3; ++n)
            r.Push("/projects/p" + std::to_string(n) + ".minibim");
        Check(r.Paths().size() == RecentFiles::MaxFiles && r.Paths()[0] == Abs("/projects/p12.minibim"), "recent: newest first, so many kept");
        // 再打开的文件移到最上面，不出现两次
        r.Push("/projects/p5.minibim");
        Check(r.Paths().size() == RecentFiles::MaxFiles && r.Paths()[0] == Abs("/projects/p5.minibim") &&
                  std::count(r.Paths().begin(), r.Paths().end(), Abs("/projects/p5.minibim")) == 1,
              "recent: opened again, it moves to the top once");
        r.Push("/projects/sub/../p6.minibim");
        Check(r.Paths()[0] == Abs("/projects/p6.minibim") && r.Paths().size() == RecentFiles::MaxFiles, "recent: the same file another way is one entry");
        r.Remove("/projects/p5.minibim");
        Check(std::count(r.Paths().begin(), r.Paths().end(), Abs("/projects/p5.minibim")) == 0, "recent: removed");
        r.Clear();
        Check(r.Paths().empty(), "recent: cleared");

        RecentFiles named;
        for (const char* p : { "/a/house.minibim", "/b/house.minibim", "/a/barn.minibim" })
            named.Push(p);
        Check(named.Labels() == std::vector<std::string>{ "barn.minibim", "house.minibim (b)", "house.minibim (a)" },
              "recent: a name two files share shows its folder");

        const std::string file = Scratch("kept");
        RecentFiles kept;
        for (const char* p : { "/a.minibim", "/b.ifc", "/c.bimcraft" })
            kept.Push(p);
        std::string error;
        Check(kept.Save(file, error) && RecentFiles::Load(file) == kept, "recent: kept in a file and read back in order");

        const std::string odd = Scratch("odd");
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(std::u8string(odd.begin(), odd.end())), ec);
        Check(RecentFiles::Load(odd).Paths().empty(), "recent: no file is an empty list");
        Write(odd, "{not json");
        Check(RecentFiles::Load(odd).Paths().empty(), "recent: a damaged file is an empty list");
        Write(odd, R"({"files": ["/a.minibim", 7, null, "", "/b.minibim"]})");
        Check(RecentFiles::Load(odd).Paths() == std::vector<std::string>{ Abs("/a.minibim"), Abs("/b.minibim") }, "recent: odd entries are dropped");
        Bim::Json many = Bim::Json::array();
        for (size_t n = 0; n < RecentFiles::MaxFiles * 3; ++n)
            many.push_back("/p" + std::to_string(n) + ".minibim");
        Write(odd, Bim::Json{ { "files", many } }.dump());
        Check(RecentFiles::Load(odd).Paths().size() == RecentFiles::MaxFiles, "recent: a file that lists more is cut");
        Write(odd, std::string(RecentFiles::MaxFileBytes + 1, ' '));
        Check(RecentFiles::Load(odd).Paths().empty(), "recent: an oversized file is an empty list");
        std::filesystem::remove(std::filesystem::path(std::u8string(file.begin(), file.end())), ec);
        std::filesystem::remove(std::filesystem::path(std::u8string(odd.begin(), odd.end())), ec);

        // 开始页卡片上的"多久以前"（没设语言时是英文原文）
        Check(RecentFiles::Age(30) == "Just now" && RecentFiles::Age(5 * 60) == "5 min ago" && RecentFiles::Age(3 * 3600) == "3 h ago",
              "age: minutes and hours");
        Check(RecentFiles::Age(30 * 3600) == "Yesterday" && RecentFiles::Age(3 * 86400) == "3 days ago" &&
                  RecentFiles::Age(40 * 86400) == "1 month ago" && RecentFiles::Age(800 * 86400) == "2 years ago",
              "age: days, months, years");
    }
}

int main()
{
    TestRows();
    TestFields();
    TestShown();
    TestParse();
    TestSearch();
    TestRoundTrip();
    TestRecent();
    std::printf("\n%s：%d 项失败\n", g_failed ? "失败" : "全部通过", g_failed);
    return g_failed ? 1 : 0;
}
