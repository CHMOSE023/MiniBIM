// 移植自 BIMCraft crates/core/src/family.rs 的 library、hosted_library 与 fixtures.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Family.h"
#include "Model/ModelJson.h"
#include "Model/Text.h"
#include <map>
#include <utility>
#include <vector>

// 新项目起步的族：都是用公式写出的 JSON 定义，再按族读进来（读不进的略去，同 BIMCraft）。名称与参数名用项目语言的词，
// 门窗族开洞的 Width、Height、Sill Height 与墙厚 Wall Thickness 不随语言
namespace MiniBIM::Bim
{
    namespace
    {
        using Defs = std::vector<std::pair<std::string, Json>>;

        std::string Num(double x) { return Text::Shortest(x); }
        Json F(const std::string& e) { return Json(e); }
        Json N(double x) { return Json(x); }

        // 一个盒子：x 从 x0 到 x1、y 从 y0 到 y1，从 z0 升到 z1，材质 m
        Json Block(const Json& x0, const Json& y0, const Json& x1, const Json& y1, const Json& z0, const Json& z1, const std::string& m)
        {
            return Json{ { "profile", Json::array({ Json::array({ x0, y0 }), Json::array({ x1, y0 }), Json::array({ x1, y1 }), Json::array({ x0, y1 }) }) },
                         { "from", z0 },
                         { "to", z1 },
                         { "material", m } };
        }

        // 切掉列在它前面的实体的形体：同样的形状，没有材质
        Json Void(Json form)
        {
            form.erase("material");
            form["void"] = true;
            return form;
        }

        // 立着的圆柱：圆心 (x, y)，从 z0 到 z1
        Json Post(const Json& x, const Json& y, const Json& r, const Json& z0, const Json& z1, const std::string& m)
        {
            return Json{ { "circle", Json{ { "centre", Json::array({ x, y }) }, { "radius", r } } }, { "from", z0 }, { "to", z1 }, { "material", m } };
        }

        Json Size(const std::string& name, double value) { return Json{ { "name", name }, { "value", value } }; }
        Json MaterialParam(const std::string& name, const std::string& value)
        {
            return Json{ { "name", name }, { "kind", "material" }, { "value", value } };
        }

        // ── 家具与卫生洁具（family.rs 的 library）────────────────────
        Defs Furniture(Language language)
        {
            const auto w = [&](const char* s) { return LanguageOf::Word(language, s); };
            const std::string width = w("Width"), depth = w("Depth"), height = w("Height"), length = w("Length");
            const std::string thick = w("Thickness"), leg = w("Leg Width"), seat = w("Seat Height"), mat = w("Material");
            const std::string timber = w("Timber"), fabric = w("Fabric"), porcelain = w("Porcelain"), concrete = w("Concrete");
            const std::string halfW = width + " / 2", halfL = length + " / 2";
            const auto neg = [](const std::string& e) { return F("-(" + e + ")"); };
            Defs families;
            // 四条腿在 w × d 的面板四角、离边 50，高到 z
            const auto legs = [&](const std::string& wd, const std::string& d, const std::string& z)
            {
                std::vector<Json> out;
                for (const auto& [east, north] : { std::pair{ true, true }, std::pair{ true, false }, std::pair{ false, true }, std::pair{ false, false } })
                {
                    const Json x0 = east ? F("(" + wd + ") / 2 - 50 - " + leg) : F("-(" + wd + ") / 2 + 50");
                    const Json x1 = east ? F("(" + wd + ") / 2 - 50") : F("-(" + wd + ") / 2 + 50 + " + leg);
                    const Json y0 = north ? F("(" + d + ") / 2 - 50 - " + leg) : F("-(" + d + ") / 2 + 50");
                    const Json y1 = north ? F("(" + d + ") / 2 - 50") : F("-(" + d + ") / 2 + 50 + " + leg);
                    out.push_back(Block(x0, y0, x1, y1, N(0.0), F(z), mat));
                }
                return out;
            };
            const std::string top = height + " - " + thick;
            {
                Json forms = Json::array({ Block(neg(halfW), F("-" + depth + " / 2"), F(halfW), F(depth + " / 2"), F(top), F(height), mat) });
                for (Json& l : legs(width, depth, top))
                    forms.push_back(std::move(l));
                families.emplace_back(w("Table"),
                                      Json{ { "parameters", Json::array({ Size(width, 1600.0), Size(depth, 900.0), Size(height, 750.0), Size(thick, 40.0),
                                                                          Size(leg, 60.0), MaterialParam(mat, timber) }) },
                                            { "types", Json{ { "1600 x 900", Json::object() },
                                                             { "1200 x 800", Json{ { width, 1200 }, { depth, 800 } } },
                                                             { "2000 x 1000", Json{ { width, 2000 }, { depth, 1000 } } } } },
                                            { "forms", forms } });
            }
            {
                // 椅子：座面、座面后沿上方的靠背、腿
                const std::string under = seat + " - " + thick;
                Json forms = Json::array({
                    Block(neg(halfW), F("-" + depth + " / 2"), F(halfW), F(depth + " / 2"), F(under), F(seat), mat),
                    Json{ { "plane", "xz" },
                          { "profile", Json::array({ Json::array({ neg(halfW), seat }), Json::array({ halfW, seat }), Json::array({ halfW, height }),
                                                     Json::array({ neg(halfW), height }) }) },
                          { "from", depth + " / 2 - " + thick },
                          { "to", depth + " / 2" },
                          { "material", mat } },
                });
                for (Json& l : legs(width + " + 100", depth + " + 100", under))
                    forms.push_back(std::move(l));
                families.emplace_back(w("Chair"),
                                      Json{ { "parameters", Json::array({ Size(width, 450.0), Size(depth, 450.0), Size(seat, 450.0), Size(height, 850.0),
                                                                          Size(thick, 40.0), Size(leg, 40.0), MaterialParam(mat, timber) }) },
                                            { "types", Json{ { "450 x 450", Json::object() } } },
                                            { "forms", forms } });
            }
            {
                // 靠墙的床：床架、床垫、床头板、枕头（双人床两个）
                const std::string head = w("Headboard Height");
                const auto pillow = [&](const std::string& x0, const std::string& x1, const std::string& shown)
                {
                    Json b = Block(F(x0), N(-500.0), F(x1), N(-100.0), F(height), F(height + " + 120"), fabric);
                    b["visible"] = shown;
                    return b;
                };
                families.emplace_back(
                    w("Bed"),
                    Json{ { "parameters", Json::array({ Size(width, 1600.0), Size(length, 2000.0), Size(height, 450.0), Size(head, 900.0), MaterialParam(mat, timber) }) },
                          { "types", Json{ { "1600 x 2000", Json::object() }, { "900 x 2000", Json{ { width, 900 } } }, { "1800 x 2000", Json{ { width, 1800 } } } } },
                          { "forms",
                            Json::array({ Block(neg(halfW), neg(length), F(halfW), N(0.0), N(0.0), N(300.0), mat),
                                          Block(F("-(" + width + ") / 2 + 20"), F("-(" + length + ") + 20"), F(width + " / 2 - 20"), N(-60.0), N(300.0), F(height), fabric),
                                          Json{ { "plane", "xz" },
                                                { "profile", Json::array({ Json::array({ neg(halfW), 0 }), Json::array({ halfW, 0 }), Json::array({ halfW, head }),
                                                                           Json::array({ neg(halfW), head }) }) },
                                                { "from", -60 },
                                                { "to", 0 },
                                                { "material", mat } },
                                          pillow("-(" + width + ") / 2 + 80", "-40", width + " > 1200"),
                                          pillow("40", width + " / 2 - 80", width + " > 1200"),
                                          pillow("-(" + width + ") / 2 + 80", width + " / 2 - 80", width + " <= 1200") }) } });
            }
            {
                // 沙发：扶手之间的座、靠背
                const std::string arm = w("Arm Width");
                families.emplace_back(
                    w("Sofa"),
                    Json{ { "parameters", Json::array({ Size(width, 2000.0), Size(depth, 900.0), Size(seat, 420.0), Size(height, 800.0), Size(arm, 180.0),
                                                        MaterialParam(mat, fabric) }) },
                          { "types", Json{ { "2000 x 900", Json::object() }, { "1600 x 900", Json{ { width, 1600 } } } } },
                          { "forms", Json::array({ Block(F("-(" + width + ") / 2 + " + arm), neg(depth), F(width + " / 2 - " + arm), N(-200.0), N(0.0), F(seat), mat),
                                                   Block(neg(halfW), N(-200.0), F(halfW), N(0.0), N(0.0), F(height), mat),
                                                   Block(neg(halfW), neg(depth), F("-(" + width + ") / 2 + " + arm), N(-200.0), N(0.0), N(600.0), mat),
                                                   Block(F(width + " / 2 - " + arm), neg(depth), F(halfW), N(-200.0), N(0.0), N(600.0), mat) }) } });
            }
            {
                // 书桌：两块侧板上的桌面，之间一块挡板
                families.emplace_back(
                    w("Desk"),
                    Json{ { "parameters", Json::array({ Size(width, 1400.0), Size(depth, 700.0), Size(height, 740.0), Size(thick, 30.0), MaterialParam(mat, timber) }) },
                          { "types", Json{ { "1400 x 700", Json::object() }, { "1200 x 600", Json{ { width, 1200 }, { depth, 600 } } } } },
                          { "forms", Json::array({ Block(neg(halfW), neg(depth), F(halfW), N(0.0), F(top), F(height), mat),
                                                   Block(neg(halfW), F("-(" + depth + ") + 20"), F("-(" + width + ") / 2 + 25"), N(-20.0), N(0.0), F(top), mat),
                                                   Block(F(width + " / 2 - 25"), F("-(" + depth + ") + 20"), F(halfW), N(-20.0), N(0.0), F(top), mat),
                                                   Block(F("-(" + width + ") / 2 + 25"), N(-60.0), F(width + " / 2 - 25"), N(-40.0), N(300.0), F(top), mat) }) } });
            }
            {
                // 衣柜，立在踢脚座上
                families.emplace_back(
                    w("Wardrobe"),
                    Json{ { "category", "casework" },
                          { "parameters", Json::array({ Size(width, 1200.0), Size(depth, 600.0), Size(height, 2100.0), MaterialParam(mat, timber) }) },
                          { "types", Json{ { "1200 x 600", Json::object() }, { "900 x 600", Json{ { width, 900 } } } } },
                          { "forms", Json::array({ Block(neg(halfW), neg(depth), F(halfW), N(0.0), N(80.0), F(height), mat),
                                                   Block(F("-(" + width + ") / 2 + 50"), F("-(" + depth + ") + 50"), F(width + " / 2 - 50"), N(0.0), N(0.0), N(80.0), mat) }) } });
            }
            {
                // 地柜：退进去留脚的踢脚座、柜体、伸到前面的台面
                const std::string worktop = w("Worktop");
                families.emplace_back(
                    w("Base Cabinet"),
                    Json{ { "category", "casework" },
                          { "parameters", Json::array({ Size(width, 1800.0), Size(depth, 600.0), Size(height, 900.0), MaterialParam(mat, timber),
                                                        MaterialParam(worktop, concrete) }) },
                          { "types", Json{ { "1800 x 600", Json::object() }, { "1200 x 600", Json{ { width, 1200 } } }, { "600 x 600", Json{ { width, 600 } } } } },
                          { "forms", Json::array({ Block(neg(halfW), F("-(" + depth + ") + 60"), F(halfW), N(0.0), N(0.0), N(100.0), mat),
                                                   Block(neg(halfW), F("-(" + depth + ") + 20"), F(halfW), N(0.0), N(100.0), F(height + " - 40"), mat),
                                                   Block(neg(halfW), neg(depth), F(halfW), N(0.0), F(height + " - 40"), F(height), worktop) }) } });
            }
            {
                // 坐便器：靠墙的水箱、便盆
                families.emplace_back(
                    w("WC"),
                    Json{ { "category", "plumbing" },
                          { "types", Json{ { "400 x 650", Json::object() } } },
                          { "forms", Json::array({ Block(N(-200.0), N(-180.0), N(200.0), N(0.0), N(380.0), N(800.0), porcelain),
                                                   Block(N(-160.0), N(-380.0), N(160.0), N(-150.0), N(0.0), N(400.0), porcelain),
                                                   Json{ { "circle", Json{ { "centre", Json::array({ 0, -470 }) }, { "radius", 180 } } },
                                                         { "from", 0 },
                                                         { "to", 400 },
                                                         { "material", porcelain } } }) } });
            }
            {
                // 立柱上的洗手盆
                families.emplace_back(
                    w("Washbasin"),
                    Json{ { "category", "plumbing" },
                          { "parameters", Json::array({ Size(width, 600.0), Size(depth, 450.0), Size(height, 850.0) }) },
                          { "types", Json{ { "600 x 450", Json::object() }, { "500 x 400", Json{ { width, 500 }, { depth, 400 } } } } },
                          { "forms", Json::array({ Block(neg(halfW), neg(depth), F(halfW), N(0.0), F(height + " - 160"), F(height), porcelain),
                                                   Json{ { "circle", Json{ { "centre", Json::array({ 0, -150 }) }, { "radius", 90 } } },
                                                         { "from", 0 },
                                                         { "to", height + " - 160" },
                                                         { "material", porcelain } } }) } });
            }
            {
                // 靠墙的浴缸：底，以及围着浴缸的四边
                const std::string rim = height;
                const std::string inner = "-(" + width + ") + 60";
                families.emplace_back(
                    w("Bathtub"),
                    Json{ { "category", "plumbing" },
                          { "parameters", Json::array({ Size(length, 1700.0), Size(width, 750.0), Size(height, 560.0) }) },
                          { "types", Json{ { "1700 x 750", Json::object() }, { "1500 x 700", Json{ { length, 1500 }, { width, 700 } } } } },
                          { "forms", Json::array({ Block(neg(halfL), neg(width), F(halfL), N(0.0), N(0.0), N(120.0), porcelain),
                                                   Block(neg(halfL), N(-60.0), F(halfL), N(0.0), N(120.0), F(rim), porcelain),
                                                   Block(neg(halfL), neg(width), F(halfL), F(inner), N(120.0), F(rim), porcelain),
                                                   Block(neg(halfL), F(inner), F("-(" + length + ") / 2 + 60"), N(-60.0), N(120.0), F(rim), porcelain),
                                                   Block(F(length + " / 2 - 60"), F(inner), F(halfL), N(-60.0), N(120.0), F(rim), porcelain) }) } });
            }
            return families;
        }

        // ── 门窗（family.rs 的 hosted_library）──────────────────────
        // 形体在洞口的坐标里：x 沿墙从洞口中间起，y 横过墙（墙中间为 0，+ 朝外），z 从窗台往上；取所在墙的厚度 `Wall Thickness`
        Defs Hosted(Language language)
        {
            const auto w = [&](const char* s) { return LanguageOf::Word(language, s); };
            const std::string frame = w("Frame Width"), depth = w("Frame Depth"), leaf = w("Leaf Thickness");
            const std::string frameMat = w("Frame Material"), leafMat = w("Leaf Material"), glass = w("Glass"), metal = w("Metal"), timber = w("Timber");
            const std::string width = Fam::Width, height = Fam::Height, wall = Fam::Wall, sill = Fam::Sill;
            // 盒子：沿墙 x[0] 到 x[1]、横过 y[0] 到 y[1]、从 z[0] 升到 z[1]，都是公式
            using Two = std::array<std::string, 2>;
            const auto box = [](const Two& x, const Two& y, const Two& z, const std::string& m)
            {
                return Json{ { "profile", Json::array({ Json::array({ x[0], y[0] }), Json::array({ x[1], y[0] }), Json::array({ x[1], y[1] }),
                                                        Json::array({ x[0], y[1] }) }) },
                             { "from", z[0] },
                             { "to", z[1] },
                             { "material", m } };
            };
            const std::string left = "-" + width + " / 2 + " + frame, right = width + " / 2 - " + frame;
            const std::string halfW = width + " / 2", minusHalfW = "-" + width + " / 2";
            const std::string halfT = wall + " / 2", minusHalfT = "-" + wall + " / 2";
            const std::string underHead = height + " - " + frame;
            // 门框：两根门梃与门头，横过墙厚，在洞口的宽与高里
            const auto doorFrame = [&]
            {
                return std::vector<Json>{ box({ minusHalfW, left }, { minusHalfT, halfT }, { "0", underHead }, frameMat),
                                          box({ right, halfW }, { minusHalfT, halfT }, { "0", underHead }, frameMat),
                                          box({ minusHalfW, halfW }, { minusHalfT, halfT }, { underHead, height }, frameMat) };
            };
            const Two leafY{ "-" + leaf + " / 2", leaf + " / 2" };
            const std::string faceIn = "-" + leaf + " / 2 - 55", faceOut = leaf + " / 2 + 55";
            const auto handle = [&](const Two& x)
            {
                return std::vector<Json>{ box(x, { leafY[1], faceOut }, { "1000", "1030" }, metal), box(x, { faceIn, leafY[0] }, { "1000", "1030" }, metal) };
            };
            const auto sizes = [&](std::initializer_list<std::tuple<const char*, double, double>> names)
            {
                Json out = Json::object();
                for (const auto& [n, wd, h] : names)
                    out[n] = Json{ { width, wd }, { height, h } };
                return out;
            };
            const auto list = [](std::vector<Json> forms)
            {
                Json out = Json::array();
                for (Json& f : forms)
                    out.push_back(std::move(f));
                return out;
            };
            const auto append = [](std::vector<Json>& to, std::vector<Json> more)
            {
                for (Json& f : more)
                    to.push_back(std::move(f));
            };
            Defs out;

            // 单扇门：铰在靠墙起点的门框上，向墙外开
            {
                const std::string run = width + " - 2 * " + frame;
                std::vector<Json> forms = doorFrame();
                forms.push_back(box({ left, right }, leafY, { "0", underHead }, leafMat));
                append(forms, handle({ width + " / 2 - " + frame + " - 120", width + " / 2 - " + frame + " - 40" }));
                out.emplace_back(
                    w("Single Door"),
                    Json{ { "category", "door" },
                          { "operation", "swing" },
                          { "parameters", Json::array({ Size(width, 900.0), Size(height, 2100.0), Size(frame, 50.0), Size(leaf, 40.0), MaterialParam(frameMat, timber),
                                                        MaterialParam(leafMat, timber) }) },
                          { "types", sizes({ { "900 x 2100", 900.0, 2100.0 }, { "800 x 2100", 800.0, 2100.0 }, { "1000 x 2100", 1000.0, 2100.0 } }) },
                          { "forms", list(std::move(forms)) },
                          { "symbols", Json::array({ Json{ { "line", Json::array({ Json::array({ left, halfT }), Json::array({ left, wall + " / 2 + " + run }) }) } },
                                                     Json{ { "arc", Json{ { "centre", Json::array({ left, halfT }) }, { "radius", run }, { "from", 0 }, { "to", 90 } } } } }) } });
            }

            // 双扇门：两扇各铰在自己的门框上，在中间相遇
            {
                const std::string each = "(" + width + " - 2 * " + frame + ") / 2";
                std::vector<Json> forms = doorFrame();
                forms.push_back(box({ left, "-1" }, leafY, { "0", underHead }, leafMat));
                forms.push_back(box({ "1", right }, leafY, { "0", underHead }, leafMat));
                append(forms, handle({ "-120", "-40" }));
                append(forms, handle({ "40", "120" }));
                out.emplace_back(
                    w("Double Door"),
                    Json{ { "category", "door" },
                          { "operation", "double-swing" },
                          { "parameters", Json::array({ Size(width, 1500.0), Size(height, 2100.0), Size(frame, 50.0), Size(leaf, 40.0), MaterialParam(frameMat, timber),
                                                        MaterialParam(leafMat, timber) }) },
                          { "types", sizes({ { "1500 x 2100", 1500.0, 2100.0 }, { "1200 x 2100", 1200.0, 2100.0 }, { "1800 x 2100", 1800.0, 2100.0 } }) },
                          { "forms", list(std::move(forms)) },
                          { "symbols", Json::array({ Json{ { "line", Json::array({ Json::array({ left, halfT }), Json::array({ left, wall + " / 2 + " + each }) }) } },
                                                     Json{ { "arc", Json{ { "centre", Json::array({ left, halfT }) }, { "radius", each }, { "from", 0 }, { "to", 90 } } } },
                                                     Json{ { "line", Json::array({ Json::array({ right, halfT }), Json::array({ right, wall + " / 2 + " + each }) }) } },
                                                     Json{ { "arc", Json{ { "centre", Json::array({ right, halfT }) }, { "radius", each }, { "from", 90 }, { "to", 180 } } } } }) } });
            }

            // 推拉门：门框里两条轨道上的两扇，中间重叠 50；朝外的一扇从另一扇外面滑过。平面图画出每扇所在的轨道
            {
                const std::string pastL = "25", pastR = "-25";
                const Two innerTrack{ "-" + leaf + " - 5", "-5" };
                const Two outerTrack{ "5", leaf + " + 5" };
                const std::string innerY = "-" + leaf + " / 2 - 5", outerY = leaf + " / 2 + 5";
                std::vector<Json> forms = doorFrame();
                forms.push_back(box({ left, pastL }, innerTrack, { "0", underHead }, leafMat));
                forms.push_back(box({ pastR, right }, outerTrack, { "0", underHead }, leafMat));
                forms.push_back(box({ "15", "55" }, { outerTrack[1], leaf + " + 30" }, { "1000", "1150" }, metal));
                out.emplace_back(
                    w("Sliding Door"),
                    Json{ { "category", "door" },
                          { "operation", "sliding" },
                          { "parameters", Json::array({ Size(width, 2000.0), Size(height, 2100.0), Size(frame, 50.0), Size(leaf, 40.0), MaterialParam(frameMat, metal),
                                                        MaterialParam(leafMat, glass) }) },
                          { "types", sizes({ { "2000 x 2100", 2000.0, 2100.0 }, { "1500 x 2100", 1500.0, 2100.0 }, { "2400 x 2100", 2400.0, 2100.0 } }) },
                          { "forms", list(std::move(forms)) },
                          { "symbols", Json::array({ Json{ { "line", Json::array({ Json::array({ left, innerY }), Json::array({ pastL, innerY }) }) } },
                                                     Json{ { "line", Json::array({ Json::array({ pastR, outerY }), Json::array({ right, outerY }) }) } } }) } });
            }

            // 窗框：两根边梃、上槛与窗台，深度取它说的与墙厚中小的那个
            const std::string deep = "min(" + depth + ", " + wall + ")";
            const std::string halfD = deep + " / 2", minusHalfD = "-" + deep + " / 2";
            const std::string innerL = left, innerR = right, innerB = frame, innerT = underHead;
            const auto windowFrame = [&]
            {
                return std::vector<Json>{ box({ minusHalfW, innerL }, { minusHalfD, halfD }, { "0", height }, frameMat),
                                          box({ innerR, halfW }, { minusHalfD, halfD }, { "0", height }, frameMat),
                                          box({ innerL, innerR }, { minusHalfD, halfD }, { innerT, height }, frameMat),
                                          box({ innerL, innerR }, { minusHalfD, halfD }, { "0", innerB }, frameMat) };
            };
            Json types = sizes({ { "1200 x 1500", 1200.0, 1500.0 }, { "600 x 900", 600.0, 900.0 }, { "1800 x 1200", 1800.0, 1200.0 } });
            types["600 x 900"][sill]   = 1100.0;
            types["1800 x 1200"][sill] = 1000.0;
            const auto pane = [&](const Two& x, const Two& z) { return box(x, { "-3", "3" }, z, glass); };
            const Json windowParams = Json::array({ Size(width, 1200.0), Size(height, 1500.0), Size(sill, 900.0), Size(frame, 50.0), Size(depth, 90.0),
                                                    MaterialParam(frameMat, timber) });

            // 固定窗：窗框围着一块玻璃
            {
                std::vector<Json> forms = windowFrame();
                forms.push_back(pane({ innerL, innerR }, { innerB, innerT }));
                out.emplace_back(w("Fixed Window"), Json{ { "category", "window" },
                                                          { "operation", "fixed" },
                                                          { "parameters", windowParams },
                                                          { "types", types },
                                                          { "forms", list(std::move(forms)) } });
            }

            // 平开窗：两扇在中间相遇，每扇是一个盒子，空心形体穿过它，框料宽 40（两扇相接处 20），玻璃放在空心形体之后，所以不被切掉
            {
                std::vector<Json> forms = windowFrame();
                const auto sash = [&](const Two& outer, std::array<int, 2> bars)
                {
                    const Two inside{ "(" + outer[0] + ") + " + std::to_string(bars[0]), "(" + outer[1] + ") - " + std::to_string(bars[1]) };
                    const std::string low = innerB + " + 40", high = innerT + " - 40";
                    Json hole = box(inside, { "-30", "30" }, { low, high }, "");
                    hole["void"] = true;
                    return std::vector<Json>{ box(outer, { "-20", "20" }, { innerB, innerT }, frameMat), hole, pane(inside, { low, high }) };
                };
                append(forms, sash({ innerL, "-1" }, { 40, 20 }));
                append(forms, sash({ "1", innerR }, { 20, 40 }));
                out.emplace_back(w("Casement Window"), Json{ { "category", "window" },
                                                             { "operation", "double-swing" },
                                                             { "parameters", windowParams },
                                                             { "types", types },
                                                             { "forms", list(std::move(forms)) } });
            }

            // 推拉窗：两条轨道上的两扇，每扇是框料宽 40 的框围着玻璃，中间重叠 40；朝外的一扇从另一扇外面滑过
            {
                std::vector<Json> forms = windowFrame();
                const std::string low = innerB + " + 40", high = innerT + " - 40";
                const auto slidingSash = [&](const Two& x, const Two& y)
                {
                    const std::string stileL = "(" + x[0] + ") + 40", stileR = "(" + x[1] + ") - 40";
                    const std::string middle = "((" + y[0] + ") + (" + y[1] + ")) / 2";
                    const Two paneY{ middle + " - 3", middle + " + 3" };
                    return std::vector<Json>{ box({ x[0], stileL }, y, { innerB, innerT }, frameMat), box({ stileR, x[1] }, y, { innerB, innerT }, frameMat),
                                              box({ stileL, stileR }, y, { innerB, low }, frameMat), box({ stileL, stileR }, y, { high, innerT }, frameMat),
                                              box({ stileL, stileR }, paneY, { low, high }, glass) };
                };
                const Two trackIn{ "-" + deep + " / 2 + 5", "-2" }, trackOut{ "2", deep + " / 2 - 5" };
                append(forms, slidingSash({ innerL, "20" }, trackIn));
                append(forms, slidingSash({ "-20", innerR }, trackOut));
                const std::string inY = "-" + deep + " / 4", outY = deep + " / 4";
                Json params = windowParams;
                params[5] = MaterialParam(frameMat, metal);
                out.emplace_back(w("Sliding Window"),
                                 Json{ { "category", "window" },
                                       { "operation", "sliding" },
                                       { "parameters", params },
                                       { "types", types },
                                       { "forms", list(std::move(forms)) },
                                       { "symbols", Json::array({ Json{ { "line", Json::array({ Json::array({ innerL, inY }), Json::array({ "20", inY }) }) } },
                                                                  Json{ { "line", Json::array({ Json::array({ "-20", outY }), Json::array({ innerR, outY }) }) } } }) } });
            }
            return out;
        }

        // ── 各专业的器具（fixtures.rs）──────────────────────────────
        // 每个靠墙立着：背在它的 x 轴上，正面朝 -y，同家具；管件、阀门以管道为中心，放在管道的高度就对齐
        Defs Plumbing(Language language)
        {
            const auto w = [&](const char* s) { return LanguageOf::Word(language, s); };
            const std::string width = w("Width"), depth = w("Depth"), height = w("Height");
            const std::string metal = w("Metal"), porcelain = w("Porcelain"), mat = w("Material");
            const std::string halfW = width + " / 2", minusHalfW = "-" + width + " / 2";
            Defs out;
            {
                // 四条腿上的洗涤盆：钢台面、切进去的盆与旁边的沥水板、后面的龙头
                const auto bowl = [&](double inset)
                {
                    return Block(F("-(" + width + ") / 2 + " + Num(60.0 + inset)), F("-(" + depth + ") + " + Num(60.0 + inset)),
                                 F(width + " / 2 - 300 - " + Num(inset)), N(-60.0 - inset), F(height + " - " + Num(200.0 - inset)), F(height), metal);
                };
                const auto legOf = [&](bool east, bool north)
                {
                    const Json x0 = east ? F(width + " / 2 - 50") : F("-(" + width + ") / 2 + 10");
                    const Json x1 = east ? F(width + " / 2 - 10") : F("-(" + width + ") / 2 + 50");
                    const Json y0 = north ? N(-50.0) : F("-(" + depth + ") + 10");
                    const Json y1 = north ? N(-10.0) : F("-(" + depth + ") + 50");
                    return Block(x0, y0, x1, y1, N(0.0), F(height + " - 40"), metal);
                };
                // 盆的中间，不论洗涤盆多宽：沥水板 300、盆壁 70 宽
                const double tapX = -120.0;
                Json forms = Json::array({ Block(F(minusHalfW), F("-" + depth), F(halfW), N(0.0), F(height + " - 40"), F(height), metal),
                                           // 盆：壁与底，然后是把它打开的切口
                                           Block(F("-(" + width + ") / 2 + 50"), F("-(" + depth + ") + 50"), F(width + " / 2 - 290"), N(-50.0),
                                                 F(height + " - 210"), F(height + " - 40"), metal) });
                forms.push_back(Void(bowl(10.0)));
                for (const auto& [e, nn] : { std::pair{ true, true }, std::pair{ true, false }, std::pair{ false, true }, std::pair{ false, false } })
                    forms.push_back(legOf(e, nn));
                forms.push_back(Post(N(tapX), N(-40.0), N(15.0), F(height), F(height + " + 250"), metal));
                forms.push_back(Block(N(tapX - 15.0), N(-220.0), N(tapX + 15.0), N(-25.0), F(height + " + 220"), F(height + " + 250"), metal));
                out.emplace_back(w("Kitchen Sink"),
                                 Json{ { "category", "plumbing" },
                                       { "parameters", Json::array({ Size(width, 1200.0), Size(depth, 550.0), Size(height, 900.0), MaterialParam(mat, metal) }) },
                                       { "types", Json{ { "1200 x 550", Json::object() }, { "800 x 500", Json{ { width, 800 }, { depth, 500 } } } } },
                                       { "forms", forms } });
            }
            {
                // 淋浴：底盘，以及墙上立管伸出的臂上的喷头
                const Json hollow = Void(Block(F("-(" + width + ") / 2 + 60"), F("-(" + depth + ") + 60"), F(width + " / 2 - 60"), N(-60.0), N(25.0), N(50.0), ""));
                out.emplace_back(w("Shower"),
                                 Json{ { "category", "plumbing" },
                                       { "parameters", Json::array({ Size(width, 900.0), Size(depth, 900.0) }) },
                                       { "types", Json{ { "900 x 900", Json::object() }, { "800 x 800", Json{ { width, 800 }, { depth, 800 } } } } },
                                       { "forms", Json::array({ Block(F(minusHalfW), F("-" + depth), F(halfW), N(0.0), N(0.0), N(50.0), porcelain), hollow,
                                                                Post(N(0.0), N(-60.0), N(12.0), N(50.0), N(2100.0), metal),
                                                                Block(N(-12.0), N(-300.0), N(12.0), N(-48.0), N(2070.0), N(2100.0), metal),
                                                                Post(N(0.0), N(-300.0), N(90.0), N(2040.0), N(2070.0), metal) }) } });
            }
            {
                // 地漏：与地面齐平的框、圆洞、篦子的条
                const std::string& side = width;
                const Json opening = Void(Post(N(0.0), N(0.0), F("(" + side + ") / 2 - 20"), N(-30.0), N(0.0), ""));
                const auto slat = [&](double at)
                { return Block(F("-(" + side + ") / 2 + 20"), N(at - 4.0), F(side + " / 2 - 20"), N(at + 4.0), N(-10.0), N(0.0), metal); };
                out.emplace_back(w("Floor Drain"),
                                 Json{ { "category", "drain" },
                                       { "parameters", Json::array({ Size(width, 150.0) }) },
                                       { "types", Json{ { "150 x 150", Json::object() }, { "100 x 100", Json{ { width, 100 } } } } },
                                       { "forms", Json::array({ Block(F("-(" + side + ") / 2"), F("-(" + side + ") / 2"), F(side + " / 2"), F(side + " / 2"), N(-50.0), N(0.0), metal),
                                                                opening, slat(-20.0), slat(0.0), slat(20.0) }) } });
            }
            return out;
        }

        // 管件与阀门，以它们连接的管道为中心（管道中心线在它们的 z = 0，沿 x，支管朝 +y）：管件有接口之前用构件
        Defs Pipework(Language language)
        {
            const auto w = [&](const char* s) { return LanguageOf::Word(language, s); };
            const std::string diameter = w("Diameter"), mat = w("Material"), pvc = w("PVC"), metal = w("Metal");
            const std::string r = diameter + " / 2";
            // 承口伸出管轴的长度：管径的一倍半，再加 30
            const std::string hub = diameter + " * 1.5 + 30";
            const auto alongX = [&](const std::string& a, const std::string& b, const std::string& m)
            {
                return Json{ { "plane", "yz" }, { "circle", Json{ { "centre", Json::array({ 0, 0 }) }, { "radius", r } } }, { "from", a }, { "to", b }, { "material", m } };
            };
            const auto alongY = [&](const std::string& a, const std::string& b, const std::string& m)
            {
                return Json{ { "plane", "xz" }, { "circle", Json{ { "centre", Json::array({ 0, 0 }) }, { "radius", r } } }, { "from", a }, { "to", b }, { "material", m } };
            };
            const auto sizes = [&](std::initializer_list<const char*> names)
            {
                Json out = Json::object();
                for (const char* n : names)
                    out[n] = Json{ { diameter, Text::ParseNumber(n).value_or(50.0) } };
                return out;
            };
            const auto neg = [](const std::string& e) { return "-(" + e + ")"; };
            Defs out;
            out.emplace_back(w("Elbow 90"), Json{ { "category", "pipefitting" },
                                                  { "parameters", Json::array({ Size(diameter, 50.0), MaterialParam(mat, pvc) }) },
                                                  { "types", sizes({ "25", "50", "110" }) },
                                                  { "forms", Json::array({ alongX(neg(hub), r, mat), alongY(neg(r), hub, mat) }) } });
            out.emplace_back(w("Tee"), Json{ { "category", "pipefitting" },
                                             { "parameters", Json::array({ Size(diameter, 50.0), MaterialParam(mat, pvc) }) },
                                             { "types", sizes({ "25", "50", "110" }) },
                                             { "forms", Json::array({ alongX(neg(hub), hub, mat), alongY("0", hub, mat) }) } });
            out.emplace_back(w("Gate Valve"),
                             Json{ { "category", "pipeaccessory" },
                                   { "parameters", Json::array({ Size(diameter, 50.0), MaterialParam(mat, metal) }) },
                                   { "types", sizes({ "25", "50" }) },
                                   { "forms", Json::array({ alongX(neg(hub), hub, mat),
                                                            Post(Json(0), Json(0), F(diameter + " * 0.7"), F(neg(r)), F(diameter + " * 0.7"), mat),
                                                            Post(Json(0), Json(0), F(diameter + " * 0.25"), F(diameter + " * 0.7"), F(diameter + " * 1.6"), mat),
                                                            Post(Json(0), Json(0), F(diameter + " * 0.8"), F(diameter + " * 1.6"), F(diameter + " * 1.7"), mat) }) } });
            return out;
        }

        // 电气：配电箱、吸顶灯与嵌入式灯、插座、开关。墙上的器具背在 x 轴上、正面朝 -y，在类型给的高度（`Mounting Height`：
        // 配电箱是底、器具是中心）；灯从给的高度往下挂，默认是楼板底
        Defs Electrical(Language language)
        {
            const auto w = [&](const char* s) { return LanguageOf::Word(language, s); };
            const std::string width = w("Width"), depth = w("Depth"), height = w("Height"), mounting = w("Mounting Height");
            const std::string diameter = w("Diameter"), mat = w("Material"), metal = w("Metal"), plaster = w("Plaster"), glass = w("Glass");
            const std::string halfW = width + " / 2", minusHalfW = "-(" + width + ") / 2";
            // 安装高度，再高出 d 毫米
            const auto up = [&](double d) { return F(mounting + " + (" + Num(d) + ")"); };
            Defs out;
            out.emplace_back(
                w("Distribution Board"),
                Json{ { "category", "equipment" },
                      { "parameters", Json::array({ Size(width, 400.0), Size(height, 500.0), Size(depth, 120.0), Size(mounting, 1300.0), MaterialParam(mat, metal) }) },
                      { "types", Json{ { "12 circuits", Json::object() },
                                       { "24 circuits", Json{ { width, 500 }, { height, 700 } } },
                                       { "36 circuits", Json{ { width, 600 }, { height, 900 } } } } },
                      { "forms", Json::array({ Block(F(minusHalfW), F("-(" + depth + ")"), F(halfW), N(0.0), F(mounting), F(mounting + " + " + height), mat),
                                               Block(F(minusHalfW + " + 15"), F("-(" + depth + ") - 12"), F(halfW + " - 15"), F("-(" + depth + ")"), up(15.0),
                                                     F(mounting + " + " + height + " - 15"), mat) }) } });
            const std::string r = diameter + " / 2";
            out.emplace_back(w("Ceiling Luminaire"),
                             Json{ { "category", "lighting" },
                                   { "parameters", Json::array({ Size(diameter, 300.0), Size(mounting, 3000.0) }) },
                                   { "types", Json{ { "300", Json::object() }, { "400", Json{ { diameter, 400 } } } } },
                                   { "forms", Json::array({ Post(N(0.0), N(0.0), F(r), up(-40.0), F(mounting), plaster),
                                                            Post(N(0.0), N(0.0), F(r + " * 0.9"), up(-60.0), up(-40.0), glass) }) } });
            out.emplace_back(
                w("Recessed Luminaire"),
                Json{ { "category", "lighting" },
                      { "parameters", Json::array({ Size(width, 600.0), Size(depth, 600.0), Size(mounting, 3000.0) }) },
                      { "types", Json{ { "600 x 600", Json::object() }, { "1200 x 300", Json{ { width, 1200 }, { depth, 300 } } } } },
                      { "forms", Json::array({ Block(F(minusHalfW), F("-(" + depth + ") / 2"), F(halfW), F(depth + " / 2"), up(-30.0), F(mounting), metal),
                                               Block(F(minusHalfW + " + 15"), F("-(" + depth + ") / 2 + 15"), F(halfW + " - 15"), F(depth + " / 2 - 15"), up(-35.0),
                                                     up(-30.0), glass) }) } });
            // 插座与开关：墙上 100 的面板，以类型给的高度为中心，插座切出插孔，开关有翘板
            const Json plate = Block(N(-50.0), N(-12.0), N(50.0), N(0.0), up(-50.0), up(50.0), plaster);
            const Json socket = Void(Json{ { "plane", "xz" },
                                           { "circle", Json{ { "centre", Json::array({ 0, mounting }) }, { "radius", 25 } } },
                                           { "from", -12 },
                                           { "to", -6 } });
            out.emplace_back(w("Outlet"), Json{ { "category", "outlet" },
                                                { "parameters", Json::array({ Size(mounting, 300.0) }) },
                                                { "types", Json{ { "Low 300", Json::object() },
                                                                 { "Medium 1100", Json{ { mounting, 1100 } } },
                                                                 { "High 2100", Json{ { mounting, 2100 } } } } },
                                                { "forms", Json::array({ plate, socket }) } });
            out.emplace_back(w("Switch"), Json{ { "category", "switch" },
                                                { "parameters", Json::array({ Size(mounting, 1100.0) }) },
                                                { "types", Json{ { "Medium 1100", Json::object() }, { "Low 300", Json{ { mounting, 300 } } } } },
                                                { "forms", Json::array({ plate, Block(N(-12.0), N(-18.0), N(12.0), N(-12.0), up(-30.0), up(30.0), plaster) }) } });
            return out;
        }

        // 防雷（NBR 5419）：接闪器、接地极、检查井。接闪器立在底座上；接地极与检查井从放置的标高往下，那里是地面
        Defs Lightning(Language language)
        {
            const auto w = [&](const char* s) { return LanguageOf::Word(language, s); };
            const std::string height = w("Height"), diameter = w("Diameter"), mat = w("Material"), metal = w("Metal"), concrete = w("Concrete");
            const auto sizes = [](const std::string& name, std::initializer_list<std::pair<const char*, double>> items)
            {
                Json out = Json::object();
                for (const auto& [k, v] : items)
                    out[k] = Json{ { name, v } };
                return out;
            };
            Defs out;
            out.emplace_back(w("Air Terminal"), Json{ { "category", "airterminal" },
                                                      { "parameters", Json::array({ Size(height, 500.0), Size(diameter, 10.0), MaterialParam(mat, metal) }) },
                                                      { "types", sizes(height, { { "500", 500.0 }, { "1000", 1000.0 }, { "2000", 2000.0 } }) },
                                                      { "forms", Json::array({ Post(N(0.0), N(0.0), N(60.0), N(0.0), N(15.0), mat),
                                                                               Post(N(0.0), N(0.0), F(diameter + " / 2"), N(15.0), F(height), mat) }) } });
            out.emplace_back(w("Earthing Electrode"),
                             Json{ { "category", "earthing" },
                                   { "parameters", Json::array({ Size(height, 2400.0), Size(diameter, 16.0), MaterialParam(mat, metal) }) },
                                   { "types", sizes(height, { { "2400", 2400.0 }, { "3000", 3000.0 } }) },
                                   { "forms", Json::array({ Post(N(0.0), N(0.0), F(diameter + " / 2"), F("-(" + height + ")"), N(0.0), mat) }) } });
            out.emplace_back(w("Inspection Box"),
                             Json{ { "category", "inspectionbox" },
                                   { "parameters", Json::array({ Size(diameter, 300.0), Size(height, 300.0), MaterialParam(mat, concrete) }) },
                                   { "types", sizes(diameter, { { "300", 300.0 }, { "250", 250.0 } }) },
                                   { "forms", Json::array({ Post(N(0.0), N(0.0), F(diameter + " / 2"), F("-(" + height + ")"), N(0.0), mat),
                                                            Void(Post(N(0.0), N(0.0), F(diameter + " / 2 - 20"), F("-(" + height + ") + 20"), N(0.0), "")) }) } });
            return out;
        }

        // 弱电（NBR 14565）：数据与语音插座、配线架、机柜。墙上的器具同电气的，背在 x 轴上，在类型给的高度（这里是底）
        Defs Telecom(Language language)
        {
            const auto w = [&](const char* s) { return LanguageOf::Word(language, s); };
            const std::string width = w("Width"), depth = w("Depth"), height = w("Height"), mounting = w("Mounting Height");
            const std::string mat = w("Material"), metal = w("Metal"), plaster = w("Plaster");
            const auto up = [&](double d) { return F(mounting + " + (" + Num(d) + ")"); };
            const std::string halfW = width + " / 2", minusHalfW = "-(" + width + ") / 2";
            // 墙上的面板，每个端口切一个插孔
            const auto plate = [&](size_t ports)
            {
                Json forms = Json::array({ Block(N(-42.0), N(-12.0), N(42.0), N(0.0), F(mounting), up(85.0), plaster) });
                for (size_t k = 0; k < ports; ++k)
                {
                    const double x = (static_cast<double>(k) - (static_cast<double>(ports) - 1.0) / 2.0) * 28.0;
                    forms.push_back(Void(Block(N(x - 8.0), N(-12.0), N(x + 8.0), N(-5.0), up(30.0), up(55.0), "")));
                }
                return forms;
            };
            Defs out;
            out.emplace_back(w("Data Outlet"), Json{ { "category", "dataoutlet" },
                                                     { "parameters", Json::array({ Size(mounting, 300.0) }) },
                                                     { "types", Json{ { "Low 300", Json::object() }, { "Medium 1100", Json{ { mounting, 1100 } } } } },
                                                     { "forms", plate(2) } });
            out.emplace_back(w("Voice Outlet"), Json{ { "category", "voiceoutlet" },
                                                      { "parameters", Json::array({ Size(mounting, 300.0) }) },
                                                      { "types", Json{ { "Low 300", Json::object() }, { "Medium 1100", Json{ { mounting, 1100 } } } } },
                                                      { "forms", plate(1) } });
            // 墙上的配线架，19 英寸宽、1U 高
            out.emplace_back(w("Patch Panel"), Json{ { "category", "patchpanel" },
                                                     { "parameters", Json::array({ Size(mounting, 1500.0), MaterialParam(mat, metal) }) },
                                                     { "types", Json{ { "24 ports", Json::object() }, { "48 ports", Json::object() } } },
                                                     { "forms", Json::array({ Block(N(-241.5), N(-50.0), N(241.5), N(0.0), F(mounting), up(44.0), mat) }) } });
            // 落地机柜，19 英寸：柜体与前门
            out.emplace_back(w("Rack"),
                             Json{ { "category", "rack" },
                                   { "parameters", Json::array({ Size(width, 600.0), Size(depth, 1000.0), Size(height, 2000.0), MaterialParam(mat, metal) }) },
                                   { "types", Json{ { "600 x 1000", Json::object() }, { "800 x 1000", Json{ { width, 800 } } }, { "600 x 800", Json{ { depth, 800 } } } } },
                                   { "forms", Json::array({ Block(F(minusHalfW), F("-(" + depth + ")"), F(halfW), N(0.0), N(0.0), F(height), mat),
                                                            Block(F(minusHalfW + " + 20"), F("-(" + depth + ") - 25"), F(halfW + " - 20"), F("-(" + depth + ")"), N(60.0),
                                                                  F(height + " - 40"), plaster) }) } });
            return out;
        }
    }

    std::map<std::string, Family> FamilyLibrary(Language language)
    {
        // 每种语言只建一次：每个新项目都要，写出 JSON 再读进来不便宜
        static thread_local std::map<Language, std::map<std::string, Family>> built;
        if (const auto it = built.find(language); it != built.end())
            return it->second;
        Defs all = Furniture(language);
        for (Defs more : { Hosted(language), Plumbing(language), Pipework(language), Electrical(language), Lightning(language), Telecom(language) })
            for (auto& d : more)
                all.push_back(std::move(d));
        std::map<std::string, Family> out;
        for (const auto& [name, v] : all)
        {
            Family f;
            std::string error;
            if (FromJson(v, f, error))
                out[name] = std::move(f);
        }
        built[language] = out;
        return out;
    }
}
