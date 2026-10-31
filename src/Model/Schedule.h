// 移植自 BIMCraft crates/core/src/schedule.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Ids.h"
#include "Model/Json.h"
#include "Model/Units.h"
#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 明细表：一个类别的每个元素一行，同 Revit 的明细表视图。字段是它的列：类别自己的、构件的族参数，
// 或用公式由别的字段算出（同族的公式）。过滤器留下每条都成立的元素；行按排序字段依次排，成组时同一组值合成一行。
// 视图过滤器（Model/Graphics.h）也用这里的规则挑元素。项目参数随第 5f 步移植，阶段与设计选项的字段随以后的步骤
namespace MiniBIM::Bim
{
    struct Model;
    struct Schedule;

    namespace Schedules
    {
        // 字段存什么：文字、数、长度（毫米）、面积（平方米）、是 / 否（公式里为 1 或 0）
        enum class Kind
        {
            Text,
            Number,
            Length,
            Area,
            YesNo,
        };

        // 明细表的一列：类别的一个字段，或有 formula 时为它自己算出的字段
        struct Field
        {
            std::string                name;
            // 由这一行别的字段算出，同族的公式：长度为毫米，面积为平方米
            std::optional<std::string> formula;
            // 算出的字段是什么：没写时为数
            std::optional<Kind>        kind;
            // 列标题，不是字段名时
            std::optional<std::string> heading;
            // 在最后一行合计，成组时每组也合计
            bool                       total = false;
            bool operator==(const Field&) const = default;
        };

        // 过滤器怎么拿字段的值与它的值比
        enum class Op
        {
            Equals,
            NotEquals,
            Greater,
            GreaterOrEqual,
            Less,
            LessOrEqual,
            Contains,
            NotContains,
            BeginsWith,
            NotBeginsWith,
            EndsWith,
            NotEndsWith,
            HasValue,
            HasNoValue,
        };

        // 留下 field 的值与 value 按 op 成立的元素（文字不分大小写）
        struct Filter
        {
            std::string field;
            Op          op = Op::Equals;
            Json        value;
            bool operator==(const Filter&) const = default;
        };

        struct Sort
        {
            std::string field;
            bool        descending = false;
            bool operator==(const Sort&) const = default;
        };

        // 字段、过滤器、排序字段最多各有多少
        inline constexpr size_t MaxFields = 64;

        // 表的一列：标题与格子里存什么
        struct Column
        {
            std::string heading;
            Kind        kind = Kind::Text;
        };

        struct Table
        {
            std::vector<Column>            columns;
            std::vector<std::vector<Json>> rows;
            // 每行列出的元素：一个，或成组时一组的
            std::vector<std::vector<Id>>   ids;
            // 合计的字段的和，各在它的列下，别的列为 null；没有合计的字段时没有
            std::optional<std::vector<Json>> totals;
        };

        // 每个项目都有的明细表：类别与英文名称（界面与项目的语言里翻译）。改过或放上图纸后成为项目自己的
        struct Builtin
        {
            const char* category;
            const char* name;
        };
        extern const std::array<Builtin, 6> Views;

        namespace KindOf
        {
            // 文件与命令里的写法：text、number、length、area、yesno
            const char* Key(Kind k);
            std::optional<Kind> Parse(std::string_view s);
        }

        namespace OpOf
        {
            // 文件与命令里的写法：equals、not-equals、greater-or-equal……
            const char* Key(Op op);
            std::optional<Op> Parse(std::string_view s);
        }

        // 每个项目都有的明细表之一：按名称（三种语言都认、不分大小写），或没有名称时按类别
        std::optional<Builtin> BuiltinOf(const std::string* name, const std::string* category);
        // 项目里名为 name 的明细表：内置的不管用哪种语言、大小写说，否则名称相同的
        std::optional<Id> Named(const Model& m, const std::string& name);
        // 类别 cat 每个项目都有的明细表：项目已有自己的就是它，否则照每个项目的样子，名称用项目的语言
        std::pair<std::optional<Id>, Schedule> Standard(const Model& m, const std::string& cat);
        // 能做明细表的类别
        std::vector<std::string> Categories();
        bool IsCategory(std::string_view cat);
        // name 是类别 cat 自己的字段吗
        bool OwnField(std::string_view cat, std::string_view name);
        // 类别 cat 的字段，各一次，及它存什么：自己的，再是（构件的）项目里族的参数
        std::vector<std::pair<std::string, Kind>> Fields(const Model& m, std::string_view cat);
        // 元素的字段 name 照它类别的明细表显示的样子（长度、面积用项目单位，是 / 否为 Yes、No）：标记显示的。没有这个字段或没有值时为空
        std::optional<std::string> FieldText(const Model& m, Id id, const std::string& name);
        // 类别 cat 的明细表不选字段时显示的
        std::vector<Field> Defaults(std::string_view cat);
        // 明细表是否成立：类别有明细表；字段是类别的或由它们算出（各一次，公式读数、不绕回自己），合计的是数；
        // 过滤与排序按它的字段，过滤器比较的是字段存的那种值
        bool Check(const Model& m, const Schedule& sc, std::string& error);
        // 规则对 cats 里每个类别的元素是否成立：过滤的字段每个类别都有
        bool CheckRules(const Model& m, const std::vector<std::string>& cats, const std::vector<Filter>& rules, std::string& error);
        // cats 里每条规则都成立的元素
        std::set<Id> Picks(const Model& m, const std::vector<std::string>& cats, const std::vector<Filter>& rules);
        // 明细表的表：每个字段一列，过滤器留下的每个元素一行，按排序字段排（没说时按第一列），成组时每组值一行；最后合计
        bool TableOf(const Model& m, const Schedule& sc, Table& out, std::string& error);
        // 列标题照 units 显示：面积的带上 m² 或 ft²
        std::string Heading(Units units, const Column& column);
        // 格子照 units 显示：长度同图纸，面积两位小数，别的数到百分位，是 / 否为 Yes、No（英文，调用方翻译），文字原样
        std::string Shown(Units units, Kind kind, const Json& v);
        // 两段文字按自然顺序比较：数（与"2""10"这样的编号）按大小在前，再是文字，不分大小写
        int NaturalText(const std::string& a, const std::string& b);
        // 表写成 CSV：标题（公制）、各行、合计。电子表格会当公式的文字（= + - @ 开头）前加 '
        std::string ToCsv(const Table& t);
    }

    // 明细表视图：of 为类别，字段、过滤器、排序与是否成组
    struct Schedule
    {
        std::string                    name;
        std::string                    of;
        std::vector<Schedules::Field>  fields;
        std::vector<Schedules::Filter> filters;
        std::vector<Schedules::Sort>   sort;
        bool                           grouped = false;
        bool operator==(const Schedule&) const = default;
    };
}
