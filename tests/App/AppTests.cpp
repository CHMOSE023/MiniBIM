// 主窗口里不碰界面控件的部分（apps/Common/src/GUI/Parameters.*）：特性面板的参数表、值的写法与读法，项目浏览器的搜索。
// 移植自 BIMCraft crates/ui-egui/src/browser.rs 的测试（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Commands/Commands.h"
#include "Commands/Sample.h"
#include "Commands/Session.h"
#include "GUI/Parameters.h"
#include <cstdio>
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
}

int main()
{
    TestRows();
    TestFields();
    TestShown();
    TestParse();
    TestSearch();
    TestRoundTrip();
    std::printf("\n%s：%d 项失败\n", g_failed ? "失败" : "全部通过", g_failed);
    return g_failed ? 1 : 0;
}
