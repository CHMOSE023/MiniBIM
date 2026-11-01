// Revit 读取器的记录层（R4）：元素数据的读法（曲线、层、墙的朝向、名字、放置、名字条目）与草图闭合成区域。
// 移植自 BIMCraft crates/rvt/src/tests/decoders.rs 与 sketch.rs 的测试（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
#include "Check.h"
#include "Fixture.h"
#include "ProjectFixture.h"

#include "Rvt/Curves.h"
#include "Rvt/Data.h"
#include "Rvt/Names.h"
#include "Rvt/Placement.h"
#include "Rvt/Records.h"
#include "Rvt/Sketch.h"
#include "Rvt/Types.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace Rvt = MiniBIM::Rvt;
using Fixture::Bytes;
using ProjectFixture::ArcBytes;
using ProjectFixture::LayersBytes;
using ProjectFixture::LineBytes;
using ProjectFixture::NameBytes;
using ProjectFixture::OrientationBytes;
using RvtTest::Check;
using Vec3 = std::array<double, 3>;

namespace
{
    constexpr double Nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double Inf = std::numeric_limits<double>::infinity();

    Bytes Cat(std::initializer_list<Bytes> parts) { return Fixture::Concat(parts); }
    Bytes Le(uint64_t v, size_t width) { return ProjectFixture::Le(v, width); }
    Bytes F64(double v) { return ProjectFixture::F64(v); }

    bool Near(double a, double b, double eps = 1e-12) { return std::abs(a - b) < eps; }

    // ---------- 曲线 ----------

    void CurveTests()
    {
        const Vec3 origin{ 1, 2, 3 }, direction{ 0, 1, 0 };
        const Bytes bytes = LineBytes(-2, 5, origin, direction);
        const auto l = Rvt::Curves::FirstLine(bytes);
        Check(l && l->Start() == Vec3{ 1, 0, 3 } && l->End() == Vec3{ 1, 7, 3 }, "直线从原点按参数沿方向走");
        // 不是直线：方向不是单位向量、起点不在终点之前、不是数的值、截断的记录
        Check(!Rvt::Curves::FirstLine(LineBytes(0, 5, origin, { 0, 2, 0 })) && !Rvt::Curves::FirstLine(LineBytes(5, 5, origin, direction))
                  && !Rvt::Curves::FirstLine(LineBytes(0, Nan, origin, direction)) && !Rvt::Curves::FirstLine(LineBytes(0, 5, { Inf, 0, 0 }, direction)),
              "不是直线的值");
        Check(!Rvt::Curves::FirstLine(std::span(bytes).first(bytes.size() - 1)), "截断的直线记录");
        Check(!Rvt::Curves::LineAt(bytes, bytes.size()) && !Rvt::Curves::LineAt({}, 0) && !Rvt::Curves::LineAt(bytes, std::numeric_limits<size_t>::max()), "越界的位置");
        // 第一条曲线记录说了算：圆弧之后的直线不是墙的，直线之后的圆弧也不是
        const Bytes arc = ArcBytes({ 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, 3, { 0, 0, 0 });
        Check(!Rvt::Curves::FirstLine(Cat({ arc, bytes })) && !Rvt::Curves::FirstArc(Cat({ bytes, arc })), "第一条曲线记录说了算");
        Check(Rvt::Curves::FirstLine(Cat({ Fixture::Of("junk"), bytes })) && Rvt::Curves::FirstArc(arc), "前面有别的字节也找得到");

        const Vec3 x{ 1, 0, 0 }, y{ 0, 1, 0 };
        const auto ok = [](std::array<double, 2> angles, Vec3 xa, Vec3 ya, double r) { return Rvt::Curves::FirstArc(ArcBytes(angles, xa, ya, r, { 4, 5, 6 })); };
        const auto a = ok({ 0.5, 2.0 }, x, y, 3.0);
        Check(a && a->CounterClockwise(), "圆弧逆时针");
        if (a)
        {
            const Vec3 p = a->Point(0.0), q = a->Point(std::numbers::pi / 2);
            Check(Near(p[0], 7) && Near(p[1], 5) && Near(q[0], 4) && Near(q[1], 8), "圆弧上的点");
        }
        const auto cw = ok({ 0.5, 2.0 }, x, { 0, -1, 0 }, 3.0);
        Check(cw && !cw->CounterClockwise(), "圆弧顺时针");
        Check(!ok({ 2.0, 2.0 }, x, y, 3.0) && !ok({ 3.0, 2.0 }, x, y, 3.0), "起始角不在终止角之前");
        Check(!ok({ 0.5, 2.0 }, x, y, 0.0) && !ok({ 0.5, 2.0 }, x, y, -1.0), "半径不是正的");
        Check(!ok({ 0.5, 2.0 }, { 2, 0, 0 }, y, 3.0), "轴不是单位向量");
        Check(!ok({ 0.5, 2.0 }, x, { 0.6, 0.8, 0 }, 3.0), "两轴不垂直");
        Check(!ok({ 0.5, Nan }, x, y, 3.0), "不是数的角");
        // 标签前面的标记字必须是 1
        Bytes marked = ArcBytes({ 0.5, 2.0 }, x, y, 3.0, { 0, 0, 0 });
        marked[0] = 0;
        Check(!Rvt::Curves::FirstArc(marked) && !Rvt::Curves::FirstArc(Bytes{ 1, 0, 0 }) && !Rvt::Curves::FirstArc({}), "标记字不是 1 的不是圆弧");

        const auto turned = [](Vec3 xa, Vec3 ya)
        {
            const auto t = Rvt::Curves::FirstArc(ArcBytes({ 0, 1 }, xa, ya, 3, { 0, 0, 0 }));
            return t && t->CounterClockwise();
        };
        Check(turned({ 0, 1, 0 }, { -1, 0, 0 }) && !turned({ 0, 1, 0 }, { 1, 0, 0 }) && turned({ 0.6, 0.8, 0 }, { -0.8, 0.6, 0 }) && !turned({ 0.8, 0.6, 0 }, { 0.6, -0.8, 0 }),
              "转过的坐标系里两轴的叉积朝上就是逆时针");
        const double third = 1.0 / 3.0;
        const auto inPlane = [](Vec3 xa, Vec3 ya) { return Rvt::Curves::FirstArc(ArcBytes({ 0, 1 }, xa, ya, 3, { 0, 0, 0 })).has_value(); };
        // 点积的每一项都参与，且和为 0
        Check(inPlane({ third, 2 * third, 2 * third }, { 2 * third, third, -2 * third }) && inPlane({ third, 2 * third, 2 * third }, { 2 * third, -2 * third, third }),
              "圆弧可以在任何平面里");
        Check(!inPlane({ third, 2 * third, 2 * third }, { 2 * third, third, 2 * third }), "不垂直的两轴");
    }

    // ---------- 类型 ----------

    using Layer = std::tuple<double, std::optional<uint32_t>, uint32_t>;

    void TypeTests()
    {
        const std::vector<uint32_t> ids{ 5, 50, 60 };
        const std::set<uint32_t> materials{ 50, 60 };
        const std::array<uint8_t, 2> tag{ static_cast<uint8_t>(ProjectFixture::Regions), 0 };
        for (const uint32_t release : { 2019u, 2023u, 2024u, 2025u, 2026u, 2027u })
        {
            const auto layout = Rvt::Types::LayoutOf(release);
            if (!layout)
            {
                Check(false, "版本的层布局：" + std::to_string(release));
                continue;
            }
            const auto read = [&](const std::vector<Layer>& layers) { return Rvt::Types::FindLayers(LayersBytes(layers, release), tag, *layout, materials, ids); };
            const auto got = read({ { 0.25, 50, 4 }, { 0.0, std::nullopt, 100 }, { 0.5, 60, 1 } });
            std::vector<Layer> seen;
            if (got)
                for (const auto& l : *got)
                    seen.emplace_back(l.widthFeet, l.material, l.function);
            const std::string of = "，Revit " + std::to_string(release);
            Check(seen == std::vector<Layer>{ { 0.25, 50, 4 }, { 0.0, std::nullopt, 100 }, { 0.5, 60, 1 } }, "各个布局里读得出层" + of);
            // 只有膜与压型板的宽度可以是 0；不是数、负的或超过十英尺的不是层
            Check(!read({ { 0.0, 50, 1 } }) && read({ { 0.0, std::nullopt, 200 } }), "宽度为 0 只给膜与压型板" + of);
            Check(!read({ { -1.0, 50, 1 } }) && !read({ { 10.5, 50, 1 } }) && !read({ { Nan, 50, 1 } }), "不合理的宽度" + of);
            Check(!read({ { 10.0, 50, 1 } }) && read({ { 9.99, 50, 1 } }), "十英尺是上限" + of);
            // 不是文件里的材质、没人用的功能、一层也没有
            Check(!read({ { 1.0, 77, 1 } }) && !read({ { 1.0, 50, 7 } }) && !read({}), "不认得的材质与功能" + of);
            Check(!read(std::vector<Layer>(33, { 1.0, 50, 1 })) && read(std::vector<Layer>(32, { 1.0, 50, 1 })), "最多 32 层" + of);
            // 一层坏了整个列表都不要
            Check(!read({ { 1.0, 50, 1 }, { 1.0, 77, 1 } }), "一层坏了整个列表不要" + of);
        }
        Check(!Rvt::Types::LayoutOf(2014) && !Rvt::Types::LayoutOf(2028), "不认得的版本没有层布局");
        // 没有标签框住时，层数也可以跟在类型名字与一个为 0 的字之后
        const auto layout = *Rvt::Types::LayoutOf(2024);
        Bytes data(3, 0x11);
        data = Cat({ data, Le(5, 4), ProjectFixture::Utf16("Basic"), Le(0, 4) });
        const Bytes layers = LayersBytes({ { 0.5, 50, 1 } }, 2024);
        data.insert(data.end(), layers.begin() + 6, layers.end());
        const auto named = Rvt::Types::FindLayers(data, tag, layout, materials, ids);
        Check(named && named->size() == 1, "层数跟在类型名字之后");
        // 只找一个窗口
        const Bytes far = Cat({ Bytes(0x2100, 0), layers });
        Check(!Rvt::Types::FindLayers(far, tag, layout, materials, ids), "窗口之外不找");

        for (const auto& [release, word] : { std::pair{ 2019u, false }, std::pair{ 2023u, true }, std::pair{ 2024u, true } })
        {
            const auto o = Rvt::Types::Orientation(OrientationBytes(3, 1, true, word), release);
            const std::string of = "，Revit " + std::to_string(release);
            Check(o && o->locationLine == 3 && o->word == (word ? 1u : 0u) && o->flip, "墙的朝向" + of);
            const auto plain = Rvt::Types::Orientation(OrientationBytes(5, 2, false, word), release);
            Check(plain && !plain->flip, "没翻转的墙" + of);
            Check(!Rvt::Types::Orientation(OrientationBytes(6, 1, false, word), release), "定位线超过 5" + of);
        }
        Check(!Rvt::Types::Orientation(OrientationBytes(0, 3, false, true), 2024), "字超过 2");
        Bytes bad = OrientationBytes(0, 1, false, true);
        bad[bad.size() - 4] = 2;
        Check(!Rvt::Types::Orientation(bad, 2024), "翻转既不是 0 也不是 1");
        // 第二个标志：0 与 `00 00`、或 1 与 `ff ff` 都行；1 与 `00 00` 不行
        Bytes second = OrientationBytes(0, 1, true, true);
        const size_t n = second.size();
        second[n - 3] = 1;
        second[n - 2] = 0xff;
        second[n - 1] = 0xff;
        Check(Rvt::Types::Orientation(second, 2024).has_value(), "第二个标志 1 与 ff ff");
        second[n - 2] = 0;
        Check(!Rvt::Types::Orientation(second, 2024), "第二个标志 1 与 00");
        Check(!Rvt::Types::Orientation(Bytes{ 0xff, 0xff, 0xff, 0xff, 1, 0, 0, 0 }, 2024) && !Rvt::Types::Orientation({}, 2024), "只有锚点或什么也没有");

        const std::array<uint8_t, 10> header{ 0xff, 0xff, 0xff, 0xff, 0x10, 0x00, 1, 0, 0, 0 };
        Check(Rvt::Types::FirstName(NameBytes("Generic - 200mm"), header, 2024) == std::string("Generic - 200mm"), "框住的名字");
        // 越过别的框：标签 ffff（没设的格子）、另一个元素的头、空的或带控制字符的字符串
        Bytes names{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 3, 0, 0, 0, 'a', 0, 'b', 0, 'c', 0 };
        names.insert(names.end(), header.begin(), header.end());
        names = Cat({ names, NameBytes(""), NameBytes("a\tb"), NameBytes("   "), NameBytes("Second") });
        Check(Rvt::Types::FirstName(names, header, 2024) == std::string("Second"), "第一个是名字的框住的字符串");
        Check(Rvt::Types::FirstName(NameBytes("x"), header, 2024).has_value() && !Rvt::Types::FirstName(NameBytes("x"), header, 2022)
                  && Rvt::Types::FirstName(NameBytes("xy"), header, 2022).has_value(),
              "2023 以前一个字符是 id 读成了名字");
        // 长度越出数据、超过上限，或有不成对的代理
        Bytes cut = NameBytes("Generic");
        cut.resize(cut.size() - 2);
        Bytes longName = Cat({ Bytes{ 0xff, 0xff, 0xff, 0xff, 0x02, 0x10 }, Le(300, 4), Fixture::Repeat(std::string_view("a\0", 2), 300) });
        Bytes lone = NameBytes("ab");
        lone[10] = 0x00;
        lone[11] = 0xd8;
        Check(!Rvt::Types::FirstName(cut, header, 2024) && !Rvt::Types::FirstName(longName, header, 2024) && !Rvt::Types::FirstName(lone, header, 2024)
                  && !Rvt::Types::FirstName({}, header, 2024),
              "截断、太长或有落单代理的名字");

        const std::set<uint32_t> types{ 5, 6 };
        Check(Rvt::Types::UniqueType({ 1, 2, 5, 9 }, types) == uint32_t(5) && Rvt::Types::UniqueType({ 5, 9, 5 }, types) == uint32_t(5), "列表里唯一的类型，提两次也是一个");
        Check(!Rvt::Types::UniqueType({ 5, 6 }, types) && !Rvt::Types::UniqueType({ 1, 2 }, types) && !Rvt::Types::UniqueType({ ~0ull, 5 + (1ull << 32) }, types)
                  && !Rvt::Types::UniqueType({}, types),
              "两个、没有或超过 u32 的不是唯一的类型");
    }

    // ---------- 放置 ----------

    using Rows = std::array<Vec3, 3>;

    // 放置的十二个数：三行旋转与原点。读取器在数据最后 96 字节之前找这十二个数：后面还有一些，像真实例的数据那样
    Bytes PlacementBytes(const Rows& rows, const Vec3& origin)
    {
        Bytes out;
        for (const auto& row : rows)
            for (const double v : row)
                out = Cat({ out, F64(v) });
        for (const double v : origin)
            out = Cat({ out, F64(v) });
        out.insert(out.end(), 8, 0);
        return out;
    }

    Rows Times(const Rows& a, const Rows& b)
    {
        Rows out{};
        for (size_t i = 0; i < 3; ++i)
            for (size_t j = 0; j < 3; ++j)
                for (size_t k = 0; k < 3; ++k)
                    out[i][j] += a[i][k] * b[k][j];
        return out;
    }

    void PlacementTests()
    {
        const double rad = std::numbers::pi / 180;
        const double s40 = std::sin(40 * rad), c40 = std::cos(40 * rad), s30 = std::sin(30 * rad), c30 = std::cos(30 * rad), s20 = std::sin(20 * rad), c20 = std::cos(20 * rad);
        // 先绕 y 转 20 度，再绕 x 转 30 度，再绕竖轴转 40 度：九个值个个都参与
        const Rows aboutY{ { { c20, 0, s20 }, { 0, 1, 0 }, { -s20, 0, c20 } } };
        const Rows aboutX{ { { 1, 0, 0 }, { 0, c30, -s30 }, { 0, s30, c30 } } };
        const Rows aboutZ{ { { c40, -s40, 0 }, { s40, c40, 0 }, { 0, 0, 1 } } };
        const Rows tilted = Times(aboutZ, Times(aboutX, aboutY));
        bool every = true;
        for (const auto& row : tilted)
            for (const double v : row)
                every = every && std::abs(v) > 0.1;
        Check(every, "斜放的旋转九个值都不为 0");
        const auto t = Rvt::Placement::TransformOf(PlacementBytes(tilted, { 1, 2, 3 }));
        Check(t && t->origin == Vec3{ 1, 2, 3 }, "斜放的实例的原点");
        Check(t && !t->PlanAngle(), "竖轴不是模型的竖轴时没有平面角");
        // 只绕竖轴转的有平面角
        const auto flat = Rvt::Placement::TransformOf(PlacementBytes(aboutZ, { 0, 0, 0 }));
        Check(flat && flat->PlanAngle() && Near(*flat->PlanAngle(), 40 * rad), "平面角");
        // 只在实例数据的前 4096 字节里找这十二个数
        const Bytes early = Cat({ Bytes(3000, 0x77), PlacementBytes(tilted, { 0, 0, 0 }) });
        const Bytes late  = Cat({ Bytes(4200, 0x77), PlacementBytes(tilted, { 0, 0, 0 }) });
        Check(Rvt::Placement::TransformOf(early) && !Rvt::Placement::TransformOf(late), "只找前 4096 字节");
        // 镜像（一行取反）、不是单位长的行、不正交的行都不是放置
        const Rows mirrored{ tilted[0], tilted[1], Vec3{ 0, -s30, -c30 } };
        Check(!Rvt::Placement::TransformOf(PlacementBytes(mirrored, { 0, 0, 0 })), "镜像不是放置");
        Check(!Rvt::Placement::TransformOf(PlacementBytes({ { { 2, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } }, { 0, 0, 0 })), "不是单位长的行");
        Check(!Rvt::Placement::TransformOf(PlacementBytes({ { { 1, 0, 0 }, { 0.6, 0.8, 0 }, { 0, 0, 1 } } }, { 0, 0, 0 })), "不正交的行");
    }

    // ---------- 名字条目与最新的副本 ----------

    // 到 2023 版的名字条目：`01 00 00 00`、id、长度与 UTF-16 的名字
    Bytes Entry32(uint32_t id, uint32_t length, const std::string& name) { return Cat({ Bytes{ 1, 0, 0, 0 }, Le(id, 4), Le(length, 4), ProjectFixture::Utf16(name) }); }

    // 2024 起的名字条目：id、长度、UTF-16 的名字与类别（最后五个字节是 ff）
    Bytes Entry64(uint32_t id, const std::string& name)
    {
        return Cat({ Bytes(3, 0x42), Le(id, 8), Le(ProjectFixture::Units(name), 4), ProjectFixture::Utf16(name), Le(static_cast<uint64_t>(int64_t(-2'000'023)), 8) });
    }

    void NameTests()
    {
        const auto names32 = [](const Bytes& bytes)
        {
            std::vector<Rvt::Names::Entry> out;
            Rvt::Names::Entries32(bytes, { 5 }, out);
            std::vector<std::string> got;
            for (const auto& e : out)
                got.push_back(e.name);
            return got;
        };
        const std::string cafe = "Café ☃";
        Check(names32(Entry32(5, 6, cafe)) == std::vector<std::string>{ cafe }, "到 2023 版的名字条目");
        Check(names32(Entry32(6, 6, cafe)).empty(), "元素表没声明的 id");
        Check(names32(Entry32(5, 0, "")).empty() && names32(Entry32(5, 129, std::string(129, 'x'))).empty(), "没有名字或名字太长");
        for (const std::string bad : { "A\x01" "B", "AB", "AB", "A￰B", "ß" })
            Check(names32(Entry32(5, ProjectFixture::Units(bad), bad)).empty(), "印不出来的名字：" + bad);
        for (const std::string good : { "Pörta 800", "A￯B" })
            Check(names32(Entry32(5, ProjectFixture::Units(good), good)) == std::vector<std::string>{ good }, "印得出来的名字：" + good);

        const auto names64 = [](const Bytes& bytes)
        {
            std::vector<Rvt::Names::Entry> out;
            Rvt::Names::Entries64(bytes, { 5 }, out);
            std::vector<std::pair<std::string, std::optional<int64_t>>> got;
            for (const auto& e : out)
                got.emplace_back(e.name, e.category);
            return got;
        };
        Check(names64(Entry64(5, "Single-Flush")) == decltype(names64({})){ { "Single-Flush", -2'000'023 } }, "2024 起的名字条目带类别");
        Check(names64(Entry64(5, "   ")).empty() && names64(Entry64(5, "A\x01" "B")).empty() && names64(Entry64(6, "Single-Flush")).empty(), "空白、带控制字符或没声明的不要");

        const auto copies = [](const Bytes& a, const Bytes& b) { return std::vector<Rvt::Data::Copy>{ { 1, a }, { 2, b } }; };
        const auto first = [](std::span<const uint8_t> b) { return b.empty() ? std::nullopt : std::optional<uint8_t>(b[0]); };
        Check(Rvt::Data::Newest<uint8_t>(copies(Fixture::Of("x"), Fixture::Of("y")), first) == std::pair<uint8_t, bool>{ 'y', true }, "最新的副本，更早的读得不一样");
        Check(Rvt::Data::Newest<uint8_t>(copies(Fixture::Of("y"), Fixture::Of("y")), first) == std::pair<uint8_t, bool>{ 'y', false }, "各份读得一样");
        // 读不出东西的旧副本不算不一样
        Check(Rvt::Data::Newest<uint8_t>(copies({}, Fixture::Of("y")), first) == std::pair<uint8_t, bool>{ 'y', false }, "读不出的旧副本不算不一样");

        Rvt::Names::Latest<std::string> found;
        found.Offer(1, 5, "a");
        found.Offer(1, 5, "a");
        found.Offer(2, 5, "a");
        found.Offer(2, 5, "b");
        found.Offer(3, 5, "old");
        found.Offer(3, 7, "new");
        found.Offer(3, 6, "older");
        const auto got = found.Values();
        Check(got.contains(1) && got.at(1) == "a" && !got.contains(2) && got.contains(3) && got.at(3) == "new", "一个分区里两份一致才算，后面的分区胜出");

        // 一条记录：id、大小（头与身体）、常数与计数；然后 16 字节的尾巴写标志并重复大小。常数是标记最后的标签减 12
        std::array<uint8_t, 8> marker{};
        marker[6] = 12 + 40;
        const auto record = [](uint64_t id, uint32_t size, uint16_t constant)
        {
            Bytes f = Cat({ Le(id, 8), Le(size, 4), Le(constant, 2), Bytes(2, 0) });
            f.resize(size, 0x55);
            return Cat({ f, Bytes(8, 0), Le(0, 4), Le(size, 4) });
        };
        const auto ids = [&](const Bytes& whole)
        {
            std::vector<uint64_t> out;
            for (const auto& s : Rvt::Records::Chain(whole, marker))
                out.push_back(s.id);
            return out;
        };
        Check(ids(Cat({ record(7, 32, 40), record(8, 40, 40), record(9, 32, 40) })) == std::vector<uint64_t>{ 7, 8, 9 }, "记录链");
        Check(ids(Cat({ record(7, 32, 40), record(8, 40, 41), record(9, 32, 40) })) == std::vector<uint64_t>{ 7 }, "常数不同的头结束记录链");
        Check(ids(Cat({ record(7, 32, 40), record(8, 12, 40) })).size() == 1, "太小的头结束记录链");
    }

    // ---------- 草图 ----------

    using Rvt::Sketch::Edge;
    using Rvt::Sketch::Pt;

    Edge E(Pt a, Pt b) { return { a, b, {} }; }

    // 过 points 的闭合多边形的边
    std::vector<Edge> Polygon(const std::vector<Pt>& points)
    {
        std::vector<Edge> out;
        for (size_t i = 0; i < points.size(); ++i)
            out.push_back(E(points[i], points[(i + 1) % points.size()]));
        return out;
    }

    std::vector<Edge> Join(std::vector<Edge> a, const std::vector<Edge>& b)
    {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }

    void SketchTests()
    {
        using namespace Rvt::Sketch;
        bool all = true;
        for (const double scale : { 1.0, 1e-5, 1000.0 })
        {
            const auto pieces = FromEdges(Polygon({ { 0, 0 }, { 10 * scale, 0 }, { 0, 10 * scale } }));
            all = all && pieces && pieces->size() == 1 && (*pieces)[0].outer.size() == 3 && (*pieces)[0].holes.empty();
        }
        Check(all, "三条曲线在任何尺度下围成三角形");
        Check(!FromEdges(Polygon({ { 0, 0 }, { 10, 0 } })), "两条什么也围不成");

        // (3, 4) 到 (23, 16) 的边画成两条曲线，在 (13, 10) 相接；另两条边是角
        const auto onSide = FromEdges(Polygon({ { 3, 4 }, { 13, 10 }, { 23, 16 }, { 3, 16 } }));
        Check(onSide && (*onSide)[0].outer.size() == 3, "落在两邻点连线上的顶点去掉");
        // 轻微但不是没有的弯留着
        const auto bent = FromEdges(Polygon({ { 0, 0 }, { 10, 0.001 }, { 20, 0 }, { 10, 10 } }));
        Check(bent && (*bent)[0].outer.size() == 4, "轻微的弯留着");

        const auto triangle = Polygon({ { 0, 0 }, { 10, 0 }, { 0, 10 } });
        Edge curved = E({ 20, 0 }, { 30, 0 });
        curved.mid = { { 25, 3 } };
        const auto lens = FromEdges(Join(triangle, { E({ 20, 0 }, { 30, 0 }), curved }));
        Check(lens && lens->size() == 2, "环可以是同样两端之间的两条曲线");
        // 同一条曲线两次（不论哪个方向）是损坏
        Check(!FromEdges(Join(triangle, { E({ 20, 0 }, { 30, 0 }), E({ 20, 0 }, { 30, 0 }) })) && !FromEdges(Join(triangle, { E({ 20, 0 }, { 30, 0 }), E({ 30, 0 }, { 20, 0 }) })),
              "同一条曲线两次是损坏");
        // 两端在同一点的曲线，三条曲线相交于一点
        Check(!FromEdges(Join(triangle, { E({ 20, 0 }, { 20, 0 }) })) && !FromEdges(Join(triangle, { E({ 0, 0 }, { -5, -5 }) })), "两端重合或三条交于一点");

        // Snowdon 样例的弧形楼板：绕 (0, 0) 从 (5, 0) 到 (0, 5) 的四分之一圆弧的记录也读得成 (1, 0) 到 (1, 1.5708) 的直线，
        // 它在包围盒里，所以端点合适就取直线会把曲线变成什么也围不住的短线。包围盒是圆弧自己的范围，只有圆弧填满它
        const double quarter = std::numbers::pi / 2;
        const auto arc = EdgeOf(ArcBytes({ 0, quarter }, { 1, 0, 0 }, { 0, 1, 0 }, 5, { 0, 0, 0 }), { 0, 0, 0, 5, 5, 0 });
        Check(arc && Near(arc->a.x, 5) && Near(arc->a.y, 0) && Near(arc->b.x, 0) && Near(arc->b.y, 5) && !arc->mid.empty(), "圆弧的记录读成圆弧，画成弦");
        // 包围盒是直线自己的，直线记录仍是直线
        const auto straight = EdgeOf(LineBytes(0, 2, { 1, 1, 0 }, { 0, 1, 0 }), { 1, 1, 0, 1, 3, 0 });
        Check(straight && straight->a == Pt{ 1, 1 } && straight->b == Pt{ 1, 3 }, "直线记录仍是直线");

        // 结构样例的楼板矩形里浮着一条线（什么也不围的草图线）
        auto edges = Polygon({ { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } });
        Check(FromEdges(edges).has_value(), "矩形");
        edges.push_back(E({ 2, 2 }, { 2, 8 }));
        Check(!FromEdges(edges), "什么也不围的曲线不是边界");
        const auto open = FromEdgesOpen(edges);
        Check(open && open->second == std::vector<size_t>{ 4 } && open->first.size() == 1 && open->first[0].outer.size() == 4, "去掉什么也不围的曲线，矩形仍闭合");
        auto beside = Polygon({ { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } });
        beside.push_back(E({ 30, 30 }, { 30, 40 }));
        const auto ring = FromEdgesOpen(beside);
        Check(ring && ring->first.size() == 1 && ring->second == std::vector<size_t>{ 4 }, "旁边的散线去掉");
        Check(!FromEdgesOpen({ E({ 0, 0 }, { 10, 0 }), E({ 10, 0 }, { 10, 10 }) }), "一个环也不闭合时没有楼板");

        // 一边画短了的楼板，像手画的草图：对齐闭合不了，连一条直线可以
        auto gap = Polygon({ { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } });
        gap[3].b = { 0, 0.6 };
        Check(!FromEdges(gap) && !FromEdgesSnapped(gap) && !FromEdgesOpen(gap), "留着缺口的草图");
        const auto closed = FromEdgesClosed(gap);
        Check(closed && closed->size() == 1 && (*closed)[0].outer.size() == 4, "缺口用直线连上");
        // 房间轮廓在一个角上开着：两个端点用直线连上，那正是草图的意思
        const auto corner = FromEdgesClosed({ E({ 0, 0 }, { 10, 0 }), E({ 10, 0 }, { 10, 10 }), E({ 10, 10 }, { 2, 10 }), E({ 2, 10 }, { 0, 6 }) });
        Check(corner && corner->size() == 1 && (*corner)[0].outer.size() >= 5, "开着一个角的轮廓连上");
        // 开着的端点比草图会有的多：是损坏，什么也不猜
        auto many = Polygon({ { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } });
        for (int k = 0; k < 5; ++k)
            many.push_back(E({ double(k), -5 }, { double(k), -4 }));
        Check(!FromEdgesClosed(many), "开着的端点太多");

        // Snowdon 样例有一块楼板的两个端点差 0.06 毫米（0.0002 英尺）：是画图的取整，不是缺口
        auto miss = Polygon({ { 0, 0 }, { 30, 0 }, { 30, 40 }, { 0, 40 } });
        miss[3].b = { 0.0002, -0.0001 };
        Check(!FromEdges(miss), "差一点也不是同一个顶点");
        const auto snapped = FromEdgesSnapped(miss);
        Check(snapped && snapped->size() == 1 && (*snapped)[0].outer.size() == 4, "差不到一毫米的端点对齐后闭合");
        auto wide = Polygon({ { 0, 0 }, { 30, 0 }, { 30, 40 }, { 0, 40 } });
        wide[3].b = { 0.03, 0 };
        Check(!FromEdgesSnapped(wide), "一厘米的缺口是缺口");

        const auto circle = [](size_t n)
        {
            std::vector<Pt> out;
            for (size_t k = 0; k < n; ++k)
            {
                const double a = 2 * std::numbers::pi * double(k) / double(n);
                out.push_back({ 1000 * std::cos(a), 1000 * std::sin(a) });
            }
            return out;
        };
        const auto most = FromEdges(Polygon(circle(MaxEdges)));
        Check(most && (*most)[0].outer.size() == MaxEdges, "草图最多读 MaxEdges 条曲线");
        Check(!FromEdges(Polygon(circle(MaxEdges + 1))), "多一条就不读");

        // 这几种做法曾经把每个端点与别的每个比较：几万条哪里也不相接的曲线要几分钟
        std::vector<Edge> loose;
        for (int k = 0; k < 100'000; ++k)
            loose.push_back(E({ k * 10.0, 0 }, { k * 10.0 + 5, 0 }));
        const auto start = std::chrono::steady_clock::now();
        Check(!FromEdgesClosed(loose) && !FromEdgesSnapped(loose) && !FromEdgesOpen(loose), "大量散的曲线每种闭合做法都拒绝");
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        Check(seconds < 10, "立刻拒绝，用了 " + std::to_string(seconds) + " 秒");

        // 坐标系转过（x 朝北、y 朝西），这样圆弧不同时是一条合包围盒的直线：它读成的直线从 (0, 1) 起往西走，出了包围盒
        const std::array<double, 6> bbox{ 0, 0, 0, 10, 10, 0 };
        const auto turn = [](double end) { return ArcBytes({ 0, end }, { 0, 1, 0 }, { -1, 0, 0 }, 4, { 6, 6, 0 }); };
        const auto nearly = EdgeOf(turn(6.0), bbox);
        Check(nearly && nearly->mid.size() > 10, "差不多一整圈的圆弧画成弦");
        Check(!EdgeOf(turn(7.0), bbox), "超过一整圈的圆弧不是草图的曲线");
    }
}

namespace RvtTest
{
    void TestDecoders()
    {
        CurveTests();
        TypeTests();
        PlacementTests();
        NameTests();
    }

    void TestSketch() { SketchTests(); }
}
