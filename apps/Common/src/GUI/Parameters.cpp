// 移植自 BIMCraft crates/ui-egui/src/properties.rs、browser.rs、lang.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "GUI/Parameters.h"
#include "I18n/Translator.h"
#include "Model/Model.h"
#include "Model/Text.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace MiniBIM::Props
{
    namespace
    {
        using enum Kind;     // 函数 Text、Shown 与同名的编辑方式重名：函数一律写 Props::Text

        // 还没移植的类别（楼梯、注释、视图……）的行等那些元素移植后再加
        constexpr Row Wall[] = {
            { N_("Constraints"), "location", N_("Location Line"), LocationLine },
            { "Constraints", "level", N_("Base Constraint"), Level },
            { "Constraints", "base_offset", N_("Base Offset"), Length },
            { "Constraints", "attach_base", N_("Base is Attached"), Attached },
            { "Constraints", "top_level", N_("Top Constraint"), TopLevel },
            { "Constraints", "height", N_("Unconnected Height"), Height },
            { "Constraints", "attach_top", N_("Top is Attached"), Attached },
            { N_("Dimensions"), "length", N_("Length"), LengthShown },
            { "Dimensions", "radius", N_("Radius"), LengthShown },
            { N_("Other"), "start", N_("Start"), Point },
            { "Other", "end", N_("End"), Point },
        };
        constexpr Row Door[] = {
            { "Constraints", "level", N_("Level"), LevelName },
            { N_("Identity Data"), "mark", N_("Mark"), Text },
            { "Other", "host", N_("Host Wall"), Shown },
            { "Other", "offset", N_("Offset"), Length },
            { "Other", "flip_facing", N_("Flip Facing"), Flag },
            { "Other", "flip_hand", N_("Flip Hand"), Flag },
        };
        constexpr Row Window[] = {
            { "Constraints", "level", "Level", LevelName },
            { "Constraints", "sill", N_("Sill Height"), Length },
            { "Identity Data", "mark", "Mark", Text },
            { "Other", "host", "Host Wall", Shown },
            { "Other", "offset", "Offset", Length },
            { "Other", "flip_facing", "Flip Facing", Flag },
        };
        constexpr Row Room[] = {
            { "Constraints", "level", "Level", Level },
            { "Dimensions", "area_m2", N_("Area"), AreaShown },
            { "Dimensions", "perimeter_m", N_("Perimeter"), MetresShown },
            { "Identity Data", "number", N_("Number"), Text },
            { "Identity Data", "name", N_("Name"), Text },
            { "Other", "point", N_("Location"), Point },
        };
        constexpr Row Floor[] = {
            { "Constraints", "level", "Level", Level },
            { "Constraints", "offset", N_("Height Offset From Level"), Length },
            { "Dimensions", "slope", N_("Slope (°)"), Number },
            { "Dimensions", "area_m2", "Area", AreaShown },
            { "Dimensions", "thickness", N_("Thickness"), LengthShown },
            { N_("Structural"), "structural", N_("Structural"), Flag },
            { N_("Graphics"), "pattern_angle", N_("Pattern Angle (°)"), Number },
        };
        constexpr Row Ceiling[] = {
            { "Constraints", "level", "Level", Level },
            { "Constraints", "offset", "Height Offset From Level", Length },
            { "Dimensions", "area_m2", "Area", AreaShown },
            { "Dimensions", "thickness", "Thickness", LengthShown },
        };
        constexpr Row LevelRows[] = {
            { "Constraints", "elevation", N_("Elevation"), Length },
            { "Identity Data", "name", "Name", Text },
        };
        constexpr Row Grid[] = {
            { "Identity Data", "name", "Name", Text },
            { "Other", "start", "Start", Point },
            { "Other", "end", "End", Point },
        };
        constexpr Row Component[] = {
            { "Constraints", "level", "Level", Level },
            { "Constraints", "offset", "Height Offset From Level", Length },
            { "Other", "at", "Location", Point },
            { "Other", "angle", N_("Rotation (°)"), Number },
        };
        // 柱与结构柱：底部在标高上（可偏移），顶部到上面的标高或未连接时的高度
        constexpr Row Column[] = {
            { "Constraints", "level", "Base Level", Level },
            { "Constraints", "base_offset", "Base Offset", Length },
            { "Constraints", "top_level", N_("Top Level"), TopLevel },
            { "Constraints", "height", "Unconnected Height", Height },
            { "Other", "at", "Location", Point },
            { "Other", "angle", "Rotation (°)", Number },
        };
        // 梁、条形基础与别的 run：两端在参照标高上多高、高度在截面哪条线上
        constexpr Row Run[] = {
            { "Constraints", "level", N_("Reference Level"), Level },
            { "Constraints", "offset", N_("Start Level Offset"), Length },
            { "Constraints", "end_offset", N_("End Level Offset"), Limit, N_("Same as start") },
            { "Constraints", "justify", N_("Z Justification"), Justification },
            { "Dimensions", "length", "Length", LengthShown },
            { "Identity Data", "system", N_("System"), Text },
            { "Other", "start", "Start", Point },
            { "Other", "end", "End", Point },
        };
        constexpr Row Mass[] = {
            { "Constraints", "level", "Base Constraint", Level },
            { "Constraints", "base_offset", "Base Offset", Length },
            { "Constraints", "top_level", "Top Constraint", TopLevel },
            { "Constraints", "height", "Unconnected Height", Height },
            { "Dimensions", "area_m2", "Area", AreaShown },
            { "Dimensions", "volume_m3", N_("Volume (m³)"), Shown },
            { "Dimensions", "gross_floor_area_m2", N_("Gross Floor Area"), AreaShown },
            { "Identity Data", "name", "Name", Text },
        };
        // 注释：它在的平面与读出的值；文字可改，标记的引线可开关
        constexpr Row DimensionRows[] = {
            { "Constraints", "level", N_("Level"), LevelName },
        };
        constexpr Row NoteRows[] = {
            { "Constraints", "level", N_("Level"), LevelName },
            { "Text", "text", N_("Text"), Text },
            { "Text", "type", N_("Text Type"), Shown },
            { "Graphics", "angle", N_("Angle"), Number },
        };
        constexpr Row SpotRows[] = {
            { "Constraints", "level", N_("Level"), LevelName },
            { "Dimensions", "elevation", N_("Elevation"), LengthShown },
        };
        constexpr Row TagRows[] = {
            { "Constraints", "level", N_("Level"), LevelName },
            { "Text", "text", N_("Text"), Shown },
            { "Graphics", "leader", N_("Leader Line"), Flag },
        };
        constexpr Row LineRows[] = {
            { "Constraints", "level", N_("Level"), LevelName },
            { "Graphics", "style", N_("Line Style"), Shown },
            { "Dimensions", "length", "Length", LengthShown },
        };
        constexpr Row RegionRows[] = {
            { "Constraints", "level", N_("Level"), LevelName },
            { "Graphics", "type", N_("Region Type"), Shown },
            { "Dimensions", "area_m2", "Area", AreaShown },
        };
        constexpr Row Roof[] = {
            { "Constraints", "level", N_("Base Level"), Level },
            { "Constraints", "offset", "Base Offset", Length },
            { "Constraints", "extrusion.from", N_("Extrusion Start"), LengthIf },
            { "Constraints", "extrusion.to", N_("Extrusion End"), LengthIf },
            { "Dimensions", "slope", "Slope (°)", Number },
            { "Dimensions", "ridge", N_("Max Ridge Height"), LengthShown },
            { "Dimensions", "area_m2", "Area", AreaShown },
            { "Other", "gable", N_("Gable Ends"), Flag },
        };
        // 楼层平面的参数，读写它的标高（剖切范围是标高的）
        constexpr Row Plan[] = {
            { "Identity Data", "name", N_("View Name"), Text },
            { N_("Extents"), "id", N_("Associated Level"), LevelName },
            { "Extents", "elevation", N_("Level Elevation"), Length },
            { N_("View Range"), "view_range.top", N_("Top"), Limit, N_("Level Above") },
            { "View Range", "view_range.cut", N_("Cut Plane"), Length },
            { "View Range", "view_range.bottom", N_("Bottom"), Length },
            { "View Range", "view_range.depth", N_("View Depth"), Length },
        };

        // 平面视图：名称、标高与视图范围（相关视图的视图范围是父视图的，在父视图里改）
        constexpr Row PlanViewOf[] = {
            { "Identity Data", "name", "View Name", Text },
            { "Extents", "level", "Associated Level", LevelName },
            { "View Range", "view_range.top", "Top", Limit, "Level Above" },
            { "View Range", "view_range.cut", "Cut Plane", Length },
            { "View Range", "view_range.bottom", "Bottom", Length },
            { "View Range", "view_range.depth", "View Depth", Length },
        };

        // 保留两位小数，去掉末尾的 0（同 Rust 的 {} 写 f64）
        std::string Number2(double x)
        {
            x = std::round(x * 100.0) / 100.0 + 0.0;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.2f", x);
            std::string s = buf;
            while (!s.empty() && s.back() == '0')
                s.pop_back();
            if (!s.empty() && s.back() == '.')
                s.pop_back();
            return s == "-0" ? "0" : s;
        }

        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.remove_suffix(1);
            return s;
        }

        bool ReadNumber(std::string_view s, double& out)
        {
            const std::string t(Trim(s));
            if (t.empty())
                return false;
            char* end = nullptr;
            out = std::strtod(t.c_str(), &end);
            return end && *end == '\0' && std::isfinite(out);
        }

        bool ReadLength(std::string_view s, Bim::Units units, double& out, std::string& error)
        {
            const auto mm = Bim::UnitsOf::Parse(units, s);
            if (!mm)
            {
                error = Format(Tr("`{0}` is not a length"), { std::string(Trim(s)) });
                return false;
            }
            out = *mm;
            return true;
        }

        char32_t Fold(char32_t c) { return c < 128 ? static_cast<char32_t>(std::tolower(static_cast<int>(c))) : c; }
    }

    std::span<const Row> RowsOf(std::string_view category)
    {
        if (category == "wall") return Wall;
        if (category == "door") return Door;
        if (category == "window") return Window;
        if (category == "room") return Room;
        if (category == "floor") return Floor;
        if (category == "ceiling") return Ceiling;
        if (category == "level") return LevelRows;
        if (category == "grid") return Grid;
        if (category == "component") return Component;
        if (category == "column" || category == "structuralcolumn" || category == "footing") return Column;
        if (Bim::IsRunCategory(category)) return Run;
        if (category == "roof") return Roof;
        if (category == "mass") return Mass;
        if (category == "dimension") return DimensionRows;
        if (category == "text") return NoteRows;
        if (category == "spot") return SpotRows;
        if (category == "tag") return TagRows;
        if (category == "line") return LineRows;
        if (category == "region") return RegionRows;
        return {};
    }

    std::span<const Row> PlanRows() { return Plan; }
    std::span<const Row> PlanViewRows() { return PlanViewOf; }

    Bim::Json FieldOf(const Bim::Json& e, std::string_view key, Kind kind)
    {
        const Bim::Json* v = &e;
        size_t at = 0;
        while (true)
        {
            const size_t dot = key.find('.', at);
            const std::string k(key.substr(at, dot == std::string_view::npos ? std::string_view::npos : dot - at));
            if (!v->is_object() || !v->contains(k))
                return kind == Length || kind == Number ? Bim::Json(0.0) : Bim::Json(nullptr);
            v = &(*v)[k];
            if (dot == std::string_view::npos)
                return *v;
            at = dot + 1;
        }
    }

    Bim::Json Nested(std::string_view key, Bim::Json val)
    {
        size_t end = key.size();
        while (true)
        {
            const size_t dot = key.rfind('.', end == 0 ? 0 : end - 1);
            const size_t from = dot == std::string_view::npos ? 0 : dot + 1;
            Bim::Json o = Bim::Json::object();
            o[std::string(key.substr(from, end - from))] = std::move(val);
            val = std::move(o);
            if (dot == std::string_view::npos)
                return val;
            end = dot;
        }
    }

    std::string Text(const Bim::Json& v)
    {
        if (v.is_number())
            return Number2(v.get<double>());
        if (v.is_string())
            return v.get<std::string>();
        if (v.is_boolean())
            return v.get<bool>() ? N_("Yes") : N_("No");
        if (v.is_array())
        {
            std::string out;
            for (const Bim::Json& x : v)
                out += (out.empty() ? "" : ", ") + Props::Text(x);
            return out;
        }
        if (v.is_null())
            return {};
        return v.dump();
    }

    std::string Shown(Kind kind, const Bim::Json& v, Bim::Units units)
    {
        // 一个数，或一列数（墙的起点、楼板顶面的高低）
        auto each = [&](auto f)
        {
            if (!v.is_array())
                return v.is_number() ? f(v.get<double>()) : std::string();
            std::string out;
            for (const Bim::Json& x : v)
                if (x.is_number())
                    out += (out.empty() ? "" : ", ") + f(x.get<double>());
            return out;
        };
        switch (kind)
        {
        case Length: case LengthIf: case Height: case Limit: case Point: case LengthShown:
            return each([&](double x) { return Bim::UnitsOf::Length(units, x); });
        case MetresShown:
            return each([&](double x) { return Bim::UnitsOf::Length(units, x * 1000.0); });
        case AreaShown:
            return each([&](double x)
            {
                const auto [area, unit] = Bim::UnitsOf::Area(units, x * 1e6);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.2f %s", area, unit);
                return std::string(buf);
            });
        default:
            return Props::Text(v);
        }
    }

    bool Parse(Kind kind, std::string_view text, Bim::Units units, Bim::Json& out, std::string& error)
    {
        double x = 0.0;
        switch (kind)
        {
        case Number:
            if (!ReadNumber(text, x))
            {
                error = Format(Tr("`{0}` is not a number"), { std::string(Trim(text)) });
                return false;
            }
            out = x;
            return true;
        case Length: case LengthIf: case Height:
            if (!ReadLength(text, units, x, error))
                return false;
            out = x;
            return true;
        case Limit:
            if (Trim(text).empty())
            {
                out = nullptr;
                return true;
            }
            if (!ReadLength(text, units, x, error))
                return false;
            out = x;
            return true;
        case Point:
        {
            // 英尺英寸的长度里有空格：x 与 y 用逗号隔开
            const size_t comma = text.find(',');
            if (comma == std::string_view::npos || text.find(',', comma + 1) != std::string_view::npos)
            {
                error = Format(Tr("`{0}` is not a point; type x, y"), { std::string(Trim(text)) });
                return false;
            }
            double y = 0.0;
            if (!ReadLength(text.substr(0, comma), units, x, error) || !ReadLength(text.substr(comma + 1), units, y, error))
                return false;
            out = Bim::Json::array({ x, y });
            return true;
        }
        default:
            out = std::string(text);
            return true;
        }
    }

    const char* CategoryName(std::string_view category, bool several)
    {
        static const struct { const char* category; const char* one; const char* many; } names[] = {
            { "wall", N_("Wall"), N_("Walls") },           { "door", N_("Door"), N_("Doors") },
            { "window", N_("Window"), N_("Windows") },     { "floor", N_("Floor"), N_("Floors") },
            { "room", N_("Room"), N_("Rooms") },           { "level", N_("Level"), N_("Levels") },
            { "grid", N_("Grid"), N_("Grids") },           { "component", N_("Component"), N_("Components") },
            { "roof", N_("Roof"), N_("Roofs") },           { "ceiling", N_("Ceiling"), N_("Ceilings") },
            { "roomseparator", N_("Room Separator"), N_("Room Separators") },
            { "column", N_("Column"), N_("Columns") },     { "structuralcolumn", N_("Structural Column"), N_("Structural Columns") },
            { "footing", N_("Footing"), N_("Footings") },  { "stripfooting", N_("Strip Footing"), N_("Strip Footings") },
            { "beam", N_("Beam"), N_("Beams") },           { "pipe", N_("Pipe"), N_("Pipes") },
            { "conduit", N_("Conduit"), N_("Conduits") },  { "cabletray", N_("Cable Tray"), N_("Cable Trays") },
            { "wire", N_("Wire"), N_("Wires") },           { "cable", N_("Cable"), N_("Cables") },
            { "downconductor", N_("Down Conductor"), N_("Down Conductors") },
            { "groundconductor", N_("Earthing Conductor"), N_("Earthing Conductors") },
            { "mass", N_("Mass"), N_("Masses") },
            { "dimension", N_("Dimension"), N_("Dimensions") }, { "text", N_("Text Note"), N_("Text Notes") },
            { "spot", N_("Spot Elevation"), N_("Spot Elevations") }, { "tag", N_("Tag"), N_("Tags") },
            { "line", N_("Detail Line"), N_("Detail Lines") }, { "region", N_("Filled Region"), N_("Filled Regions") },
        };
        for (const auto& n : names)
            if (category == n.category)
                return several ? n.many : n.one;
        return "";
    }

    std::vector<std::string> Words(std::string_view find)
    {
        std::vector<std::string> out;
        size_t i = 0;
        while (i < find.size())
        {
            while (i < find.size() && std::isspace(static_cast<unsigned char>(find[i])))
                ++i;
            const size_t start = i;
            while (i < find.size() && !std::isspace(static_cast<unsigned char>(find[i])))
                ++i;
            if (i > start)
                out.emplace_back(find.substr(start, i - start));
        }
        return out;
    }

    bool Matches(std::string_view text, std::string_view word)
    {
        std::u32string t = Bim::Text::Utf32(text), p = Bim::Text::Utf32(word);
        for (char32_t& c : t)
            c = Fold(c);
        for (char32_t& c : p)
            c = Fold(c);
        // 从每个位置起，模式能否匹配一段前缀（* 任意一串，? 任意一个）：逐个模式字符推进可达的位置
        const size_t n = t.size();
        for (size_t start = 0; start <= n; ++start)
        {
            std::vector<bool> can(n + 1, false);
            can[start] = true;
            bool any = true;
            for (const char32_t c : p)
            {
                std::vector<bool> next(n + 1, false);
                any = false;
                if (c == U'*')
                {
                    size_t first = 0;
                    while (first <= n && !can[first])
                        ++first;
                    for (size_t i = first; i <= n; ++i)
                        next[i] = any = true;
                }
                else
                    for (size_t i = 0; i < n; ++i)
                        if (can[i] && (c == U'?' || t[i] == c))
                            next[i + 1] = any = true;
                can = std::move(next);
                if (!any)
                    break;
            }
            if (any)
                return true;
        }
        return false;
    }
}
