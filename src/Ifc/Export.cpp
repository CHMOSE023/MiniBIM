// 移植自 BIMCraft crates/core/src/ifc.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Ifc/Ifc.h"
#include "Model/Text.h"
#include "Model/Walls.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

namespace MiniBIM::Bim::Ifc
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        // GlobalId 是做什么用的，所以每次导出都一样（同 BIMCraft 的编号；还没移植的元素用的号空着）
        constexpr uint64_t kProject            = 1;
        constexpr uint64_t kSite               = 2;
        constexpr uint64_t kBuilding           = 3;
        constexpr uint64_t kStorey             = 4;
        constexpr uint64_t kElement            = 5;
        constexpr uint64_t kOpening            = 6;
        constexpr uint64_t kVoids              = 7;
        constexpr uint64_t kFills              = 8;
        constexpr uint64_t kAggregates         = 9;
        constexpr uint64_t kContains           = 10;
        constexpr uint64_t kSpaces             = 11;
        constexpr uint64_t kMaterials          = 12;
        constexpr uint64_t kSlabMaterials      = 18;
        constexpr uint64_t kComponentMaterials = 19;
        constexpr uint64_t kCommon             = 43;
        constexpr uint64_t kCommonDefines      = 44;
        constexpr uint64_t kQuantities         = 49;
        constexpr uint64_t kQuantityDefines    = 50;

        // GlobalId 的各位数字，按顺序
        constexpr char kDigits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$";

        // 以 angle 度（从东逆时针）为方向的单位向量
        P2 Heading(double angle) { return P2(std::cos(angle * kPi / 180.0), std::sin(angle * kPi / 180.0)); }

        // STEP 的字符串：`'` 与 `\` 写两次，可打印 ASCII 以外的写成 `\X2\…\X0\`（UCS-2），超出基本平面的写成 `\X4\…\X0\`（UCS-4）
        std::string S(std::string_view text)
        {
            std::string out = "'";
            char buf[32];
            for (const char32_t c : Text::Utf32(text))
            {
                if (c == '\'')
                    out += "''";
                else if (c == '\\')
                    out += "\\\\";
                else if (c >= ' ' && c <= '~')
                    out.push_back(static_cast<char>(c));
                else if (c > 0xFFFF)
                {
                    std::snprintf(buf, sizeof(buf), "\\X4\\%08X\\X0\\", static_cast<unsigned>(c));
                    out += buf;
                }
                else
                {
                    std::snprintf(buf, sizeof(buf), "\\X2\\%04X\\X0\\", static_cast<unsigned>(c));
                    out += buf;
                }
            }
            out.push_back('\'');
            return out;
        }

        // x 保留 digits 位小数，去掉末尾的 0（总带小数点）
        std::string Fixed(double x, int digits)
        {
            if (!std::isfinite(x) || x == 0.0)
                x = 0.0;
            char buf[400];
            const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), x, std::chars_format::fixed, digits);
            std::string out = ec == std::errc() ? std::string(buf, end) : std::string("0.");
            while (!out.empty() && out.back() == '0')
                out.pop_back();
            return out;
        }

        // STEP 的实数：总带小数点
        std::string Real(double x) { return Fixed(x, 6); }

        // 方向的分量：实数，到小数点后 12 位，转角读回来准到百万分之一度
        std::string Ratio(double x) { return Fixed(x, 12); }

        // 128 位数 v 右移 shift 位后的低 6 位
        unsigned Bits(Guid v, int shift)
        {
            uint64_t x = 0;
            if (shift >= 64)
                x = v.hi >> (shift - 64);
            else if (shift == 0)
                x = v.lo;
            else
                x = (v.lo >> shift) | (v.hi << (64 - shift));
            return static_cast<unsigned>(x & 63);
        }

        // 一个 22 位的 IFC GlobalId：同一模型、同一用途、同一元素每次导出都一样
        std::string GuidOf(uint64_t uid, uint64_t role, uint64_t id)
        {
            const uint64_t hi = Mix(uid ^ Mix(role * 0x9E3779B97F4A7C15ull ^ id));
            const uint64_t lo = Mix(hi ^ ((id << 17) | (id >> 47)) ^ role);
            return Compress(Guid{ hi, lo });
        }

        // GUID 的 128 位折成 64 位：元素在旁边写的东西由它得出
        uint64_t Fold(Guid g) { return g.hi ^ g.lo; }

        // Unix 时间的 UTC 日期与时间，ISO 8601
        std::string Utc(uint64_t secs)
        {
            // 天数换成公历日期：Howard Hinnant 的算法
            const int64_t z   = static_cast<int64_t>(secs / 86400) + 719468;
            const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
            const int64_t doe = z - era * 146097;
            const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const int64_t mp  = (5 * doy + 2) / 153;
            const int64_t d   = doy - (153 * mp + 2) / 5 + 1;
            const int64_t mo  = mp < 10 ? mp + 3 : mp - 9;
            const int64_t y   = yoe + era * 400 + (mo <= 2 ? 1 : 0);
            const uint64_t t  = secs % 86400;
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lldT%02llu:%02llu:%02llu", static_cast<long long>(y), static_cast<long long>(mo),
                          static_cast<long long>(d), static_cast<unsigned long long>(t / 3600), static_cast<unsigned long long>(t % 3600 / 60),
                          static_cast<unsigned long long>(t % 60));
            return buf;
        }

        std::string Refs(const std::vector<uint64_t>& ids)
        {
            std::string out;
            for (size_t i = 0; i < ids.size(); ++i)
            {
                if (i)
                    out.push_back(',');
                out += "#" + std::to_string(ids[i]);
            }
            return out;
        }

        std::string Ref(uint64_t id) { return "#" + std::to_string(id); }

        // 坐标到 0.001 毫米的整数：同一个角点只写一次
        int64_t Key(double x)
        {
            const double r = std::round(x * 1e3);
            return std::isfinite(r) ? static_cast<int64_t>(std::clamp(r, -9.2e18, 9.2e18)) : 0;
        }

        // 类型 name（类别 category）的各层：墙的从外往里，其余的从上往下；没有层时为空
        const std::vector<Layer>& TypeLayers(const Model& m, std::string_view category, const std::string& name)
        {
            static const std::vector<Layer> none;
            for (const Model::Layered& t : m.LayeredTypes())
                if (category == t.category && *t.name == name)
                    return *t.layers;
            return none;
        }

        // 墙的 Qto_WallBaseQuantities：沿定位线的长度、厚度与高度，侧面积（长 × 高，平方米）与减去其中门窗（holes：宽、高、窗台，
        // 各截到墙高）的，以及两者乘厚度（立方米），不管接头。贴着屋顶或楼板的墙没有一个高度，只有长度与厚度
        std::vector<std::tuple<std::string, std::string, double>> WallQuantities(const Model& m, const Wall& w,
                                                                                const std::vector<std::array<double, 3>>& holes)
        {
            const double length = Walls::Axis::Of(w).Length(), width = m.Thickness(w);
            std::vector<std::tuple<std::string, std::string, double>> values = { { "Length", "LENGTH", length }, { "Width", "LENGTH", width } };
            if (!w.attachTop && !w.attachBase)
            {
                const double height = m.TopZ(w) - m.BaseZ(w);
                const double gross  = length * height;
                double cut = 0.0;
                for (const auto& [wd, h, sill] : holes)
                    cut += std::min(wd, length) * std::max(std::min(h, height - sill), 0.0);
                const double net = std::max(gross - cut, 0.0);
                values.insert(values.end(), { { "Height", "LENGTH", height },
                                              { "GrossSideArea", "AREA", gross / 1e6 },
                                              { "NetSideArea", "AREA", net / 1e6 },
                                              { "GrossVolume", "VOLUME", gross * width / 1e9 },
                                              { "NetVolume", "VOLUME", net * width / 1e9 } });
            }
            return values;
        }

        // 写 STEP 的数据段：每个实体一行，编号依次递增
        class Writer
        {
        public:
            std::string out;
            uint64_t    next = 0;
            uint64_t    uid  = 0;
            // 各元素的 GUID，按 ID
            std::map<Id, Guid> own;
            // 共用的：原点处的坐标系、+Z、模型的表示上下文与它的 Body 子上下文
            uint64_t world = 0, z = 0, context = 0, body = 0;
            // 写过的材质：它，以及着它颜色的表面样式
            std::map<std::string, std::pair<uint64_t, std::optional<uint64_t>>> materials;
            // 各元素的 Pset_*Common：它的实体、属性集名称与属性，最后统一写
            std::map<Id, std::tuple<uint64_t, std::string, std::vector<uint64_t>>> common;

            // 写 `#n=ENTITY;`，返回 n
            uint64_t Add(const std::string& entity)
            {
                ++next;
                out += "#" + std::to_string(next) + "=" + entity + ";\n";
                return next;
            }

            // 为 id 做 role 的东西的 GlobalId：元素与楼层自己的是它的 GUID；元素写的别的东西（洞口、关系）也由它的 GUID 得出，
            // 所以换了 ID 也都不变
            std::string GuidFor(uint64_t role, uint64_t id) const
            {
                const auto it = own.find(id);
                if (it != own.end() && (role == kElement || role == kStorey))
                    return S(Compress(it->second));
                if (it != own.end())
                    return S(GuidOf(uid, role, Fold(it->second)));
                return S(GuidOf(uid, role, id));
            }

            // ent 的属性集 name，GlobalId 由 (set, relation) 与 key 得出
            void Define(uint64_t ent, const std::string& name, const std::vector<uint64_t>& props, std::pair<uint64_t, uint64_t> roles, uint64_t key)
            {
                const uint64_t set = Add("IFCPROPERTYSET(" + GuidFor(roles.first, key) + ",$," + S(name) + ",$,(" + Refs(props) + "))");
                Add("IFCRELDEFINESBYPROPERTIES(" + GuidFor(roles.second, key) + ",$,$,$,(" + Ref(ent) + ")," + Ref(set) + ")");
            }

            // ent 的数量集 name，各为（名称、种类、值）：种类 LENGTH 为毫米，AREA 为平方米，VOLUME 为立方米
            void Quantities(uint64_t ent, const std::string& name, const std::vector<std::tuple<std::string, std::string, double>>& values, uint64_t key)
            {
                std::vector<uint64_t> qs;
                for (const auto& [n, kind, v] : values)
                    qs.push_back(Add("IFCQUANTITY" + kind + "(" + S(n) + ",$,$," + Real(v) + ",$)"));
                const uint64_t set = Add("IFCELEMENTQUANTITY(" + GuidFor(kQuantities, key) + ",$," + S(name) + ",$,$,(" + Refs(qs) + "))");
                Add("IFCRELDEFINESBYPROPERTIES(" + GuidFor(kQuantityDefines, key) + ",$,$,$,(" + Ref(ent) + ")," + Ref(set) + ")");
            }

            // 按名称的材质，只写一次；库里有的带它的颜色，供查看器着色
            uint64_t Material(const Model& m, const std::string& name) { return MaterialStyle(m, name).first; }

            // 按名称的材质，只写一次；以及着它颜色的表面样式
            std::pair<uint64_t, std::optional<uint64_t>> MaterialStyle(const Model& m, const std::string& name)
            {
                if (const auto it = materials.find(name); it != materials.end())
                    return it->second;
                const uint64_t id = Add("IFCMATERIAL(" + S(name) + ",$,$)");
                std::optional<uint64_t> style;
                if (const auto it = m.materials.find(name); it != m.materials.end())
                {
                    const auto& c = it->second.color;
                    const uint64_t colour  = Add("IFCCOLOURRGB($," + Real(c[0] / 255.0) + "," + Real(c[1] / 255.0) + "," + Real(c[2] / 255.0) + ")");
                    const uint64_t shading = Add("IFCSURFACESTYLESHADING(" + Ref(colour) + ",$)");
                    style = Add("IFCSURFACESTYLE(" + S(name) + ",.BOTH.,(" + Ref(shading) + "))");
                    const uint64_t item = Add("IFCSTYLEDITEM($,(" + Ref(*style) + "),$)");
                    const uint64_t rep  = Add("IFCSTYLEDREPRESENTATION(" + Ref(context) + ",'Style','Material',(" + Ref(item) + "))");
                    Add("IFCMATERIALDEFINITIONREPRESENTATION($,$,(" + Ref(rep) + ")," + Ref(id) + ")");
                }
                materials[name] = { id, style };
                return { id, style };
            }

            // 三角形围成的闭合面，每个角点只写一次
            uint64_t Faceset(const std::vector<Tri>& tris)
            {
                std::map<std::array<int64_t, 3>, size_t> index;
                std::vector<P3> pts;
                std::string faces;
                for (const Tri& t : tris)
                {
                    size_t c[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        const P3& p = t[k];
                        const auto [it, fresh] = index.try_emplace({ Key(p[0]), Key(p[1]), Key(p[2]) }, pts.size() + 1);
                        if (fresh)
                            pts.push_back(p);
                        c[k] = it->second;
                    }
                    if (c[0] != c[1] && c[1] != c[2] && c[0] != c[2])
                    {
                        if (!faces.empty())
                            faces.push_back(',');
                        faces += "(" + std::to_string(c[0]) + "," + std::to_string(c[1]) + "," + std::to_string(c[2]) + ")";
                    }
                }
                std::string coords;
                for (const P3& p : pts)
                {
                    if (!coords.empty())
                        coords.push_back(',');
                    coords += "(" + Real(p[0]) + "," + Real(p[1]) + "," + Real(p[2]) + ")";
                }
                const uint64_t list = Add("IFCCARTESIANPOINTLIST3D((" + coords + "))");
                return Add("IFCTRIANGULATEDFACESET(" + Ref(list) + ",$,.T.,(" + faces + "),$)");
            }

            // 由三角面集 items（闭合的）组成的实体
            uint64_t Tessellated(const std::vector<uint64_t>& items)
            {
                const uint64_t rep = Add("IFCSHAPEREPRESENTATION(" + Ref(body) + ",'Body','Tessellation',(" + Refs(items) + "))");
                return Add("IFCPRODUCTDEFINITIONSHAPE($,$,(" + Ref(rep) + "))");
            }

            uint64_t Point(P2 p) { return Add("IFCCARTESIANPOINT((" + Real(p.x) + "," + Real(p.y) + "))"); }

            // 在 (x, y, z) 处、x 轴沿 dir 的坐标系
            uint64_t Axes(P3 at, std::optional<P2> dir)
            {
                const uint64_t o = Add("IFCCARTESIANPOINT((" + Real(at[0]) + "," + Real(at[1]) + "," + Real(at[2]) + "))");
                if (dir)
                {
                    const uint64_t d = Add("IFCDIRECTION((" + Ratio(dir->x) + "," + Ratio(dir->y) + ",0.))");
                    return Add("IFCAXIS2PLACEMENT3D(" + Ref(o) + "," + Ref(z) + "," + Ref(d) + ")");
                }
                return Add("IFCAXIS2PLACEMENT3D(" + Ref(o) + ",$,$)");
            }

            // 相对 to（没有时为世界）在 at 处、x 轴沿 dir 的放置
            uint64_t Place(std::optional<uint64_t> to, P3 at, std::optional<P2> dir)
            {
                const uint64_t a = Axes(at, dir);
                return Add("IFCLOCALPLACEMENT(" + (to ? Ref(*to) : std::string("$")) + "," + Ref(a) + ")");
            }

            // 过 ring 的闭合折线
            uint64_t Polyline(const Ring& ring)
            {
                std::vector<uint64_t> pts;
                for (const P2& p : ring)
                    pts.push_back(Point(p));
                if (!pts.empty())
                    pts.push_back(pts.front());
                return Add("IFCPOLYLINE((" + Refs(pts) + "))");
            }

            // 以 centre 为中心、size 大小的矩形轮廓
            uint64_t Rectangle(P2 centre, P2 size)
            {
                const uint64_t c  = Point(centre);
                const uint64_t at = Add("IFCAXIS2PLACEMENT2D(" + Ref(c) + ",$)");
                return Add("IFCRECTANGLEPROFILEDEF(.AREA.,$," + Ref(at) + "," + Real(size.x) + "," + Real(size.y) + ")");
            }

            // 沿 outer、去掉 holes 的轮廓
            uint64_t Profile(const Ring& outer, const std::vector<Ring>& holes)
            {
                const uint64_t o = Polyline(outer);
                if (holes.empty())
                    return Add("IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$," + Ref(o) + ")");
                std::vector<uint64_t> inner;
                for (const Ring& h : holes)
                    inner.push_back(Polyline(h));
                return Add("IFCARBITRARYPROFILEDEFWITHVOIDS(.AREA.,$," + Ref(o) + ",(" + Refs(inner) + "))");
            }

            // 实体：profile 从 bottom 向上拉伸 depth，以及另外的表示
            uint64_t Body(uint64_t profile, double bottom, double depth, const std::vector<uint64_t>& extra)
            {
                return Bodies({ { profile, bottom, depth } }, extra);
            }

            // 几个拉伸体组成的实体，各为轮廓从 bottom 向上拉伸 depth；以及另外的表示
            uint64_t Bodies(const std::vector<std::tuple<uint64_t, double, double>>& solids, const std::vector<uint64_t>& extra)
            {
                std::vector<uint64_t> items;
                for (const auto& [profile, bottom, depth] : solids)
                {
                    const uint64_t at = bottom == 0.0 ? world : Axes({ 0.0, 0.0, bottom }, std::nullopt);
                    items.push_back(Add("IFCEXTRUDEDAREASOLID(" + Ref(profile) + "," + Ref(at) + "," + Ref(z) + "," + Real(depth) + ")"));
                }
                const uint64_t rep = Add("IFCSHAPEREPRESENTATION(" + Ref(body) + ",'Body','SweptSolid',(" + Refs(items) + "))");
                // Body 在前：取第一个表示的软件拿到的是实体
                std::vector<uint64_t> reps = { rep };
                reps.insert(reps.end(), extra.begin(), extra.end());
                return Add("IFCPRODUCTDEFINITIONSHAPE($,$,(" + Refs(reps) + "))");
            }

            // 闭合壳组成的实体：各个逆时针的平面 faces 与它们上方 rise 处的副本围成；高度从 base 量
            uint64_t Breps(const std::vector<std::vector<P3>>& faces, double base, double rise)
            {
                std::vector<uint64_t> breps;
                for (const auto& face : faces)
                    breps.push_back(Brep(face, base, rise));
                return Solid(breps, {});
            }

            // 闭合壳组成的实体，以及另外的表示
            uint64_t Solid(const std::vector<uint64_t>& breps, const std::vector<uint64_t>& extra)
            {
                const uint64_t rep = Add("IFCSHAPEREPRESENTATION(" + Ref(body) + ",'Body','Brep',(" + Refs(breps) + "))");
                std::vector<uint64_t> reps = { rep };
                reps.insert(reps.end(), extra.begin(), extra.end());
                return Add("IFCPRODUCTDEFINITIONSHAPE($,$,(" + Refs(reps) + "))");
            }

            // 逆时针的平面 face 与它上方 rise 处的副本围成的闭合壳，面朝外；高度从 base 量
            uint64_t Brep(const std::vector<P3>& face, double base, double rise)
            {
                const auto ring = [&](double up)
                {
                    std::vector<P3> out;
                    for (const P3& p : face)
                        out.push_back({ p[0], p[1], p[2] - base + up });
                    return out;
                };
                return Shell(ring(0.0), ring(rise));
            }

            // 从逆时针的环 bottom 到 top（同样的点在它上方）围成的闭合壳，面朝外
            uint64_t Shell(const std::vector<P3>& bottomPts, const std::vector<P3>& topPts)
            {
                const auto ring = [&](const std::vector<P3>& pts)
                {
                    std::vector<uint64_t> out;
                    for (const P3& p : pts)
                        out.push_back(Add("IFCCARTESIANPOINT((" + Real(p[0]) + "," + Real(p[1]) + "," + Real(p[2]) + "))"));
                    return out;
                };
                const std::vector<uint64_t> bottom = ring(bottomPts), top = ring(topPts);
                std::vector<std::vector<uint64_t>> loops = { std::vector<uint64_t>(bottom.rbegin(), bottom.rend()), top };
                const size_t n = std::min(bottom.size(), top.size());
                for (size_t i = 0; i < n; ++i)
                {
                    const size_t j = (i + 1) % n;
                    loops.push_back({ bottom[i], bottom[j], top[j], top[i] });
                }
                std::vector<uint64_t> faces;
                for (const auto& l : loops)
                {
                    const uint64_t lp    = Add("IFCPOLYLOOP((" + Refs(l) + "))");
                    const uint64_t bound = Add("IFCFACEOUTERBOUND(" + Ref(lp) + ",.T.)");
                    faces.push_back(Add("IFCFACE((" + Ref(bound) + "))"));
                }
                const uint64_t shell = Add("IFCCLOSEDSHELL((" + Refs(faces) + "))");
                return Add("IFCFACETEDBREP(" + Ref(shell) + ")");
            }

            // 结构构件的 Pset_*Common：它承重
            void LoadBearing(uint64_t ent, const std::string& pset, Id id)
            {
                const uint64_t load = Add("IFCPROPERTYSINGLEVALUE('LoadBearing',$,IFCBOOLEAN(.T.),$)");
                auto it = common.try_emplace(id, ent, pset, std::vector<uint64_t>{}).first;
                std::get<2>(it->second).push_back(load);
            }

            // 族的实体块写成三角面集，各着它材质的颜色；made 收集用到的材质
            std::vector<uint64_t> Pieces(const Model& m, const std::vector<Fam::Piece>& pieces, std::vector<uint64_t>& made)
            {
                std::vector<uint64_t> items;
                for (const Fam::Piece& piece : pieces)
                {
                    const uint64_t set = Faceset(piece.tris);
                    const std::string material(Text::Trim(piece.material));
                    if (!material.empty())
                    {
                        const auto [mat, style] = MaterialStyle(m, material);
                        if (style)
                            Add("IFCSTYLEDITEM(" + Ref(set) + ",(" + Ref(*style) + "),$)");
                        if (std::find(made.begin(), made.end(), mat) == made.end())
                            made.push_back(mat);
                    }
                    items.push_back(set);
                }
                return items;
            }

            // 用什么做：一种材质，或几种的材质组成集（名为 name，已是 STEP 字符串）
            std::optional<uint64_t> Relating(const std::vector<uint64_t>& made, const std::string& name)
            {
                if (made.empty())
                    return std::nullopt;
                if (made.size() == 1)
                    return made.front();
                std::vector<uint64_t> parts;
                for (const uint64_t x : made)
                    parts.push_back(Add("IFCMATERIALCONSTITUENT($,$," + Ref(x) + ",$,$)"));
                return Add("IFCMATERIALCONSTITUENTSET(" + name + ",$,(" + Refs(parts) + "))");
            }
        };

        const char* FunctionCategory(Function f)
        {
            switch (f)
            {
            case Function::Structure: return "'LoadBearing'";
            case Function::Thermal:   return "'Insulation'";
            case Function::Finish:    return "'Finish'";
            case Function::Substrate: return "'Substrate'";
            case Function::Membrane:  return "'Membrane'";
            }
            return "'Substrate'";
        }
    }

    std::string Compress(Guid g)
    {
        std::string out;
        out.push_back(kDigits[Bits(g, 126)]);
        for (int i = 20; i >= 0; --i)
            out.push_back(kDigits[Bits(g, 6 * i)]);
        return out;
    }

    std::optional<Guid> Expand(std::string_view text)
    {
        if (text.size() != 22)
            return std::nullopt;
        const auto digit = [](char c) -> int
        {
            for (int d = 0; d < 64; ++d)
                if (kDigits[d] == c)
                    return d;
            return -1;
        };
        if (const int first = digit(text[0]); first < 0 || first >= 4)
            return std::nullopt;
        Guid v;
        for (const char c : text)
        {
            const int d = digit(c);
            if (d < 0 || (v.hi >> 58) != 0)
                return std::nullopt;
            v.hi = (v.hi << 6) | (v.lo >> 58);
            v.lo = (v.lo << 6) | static_cast<uint64_t>(d);
        }
        return v;
    }

    std::string Export(const Model& m)
    {
        Writer w;
        w.uid = m.uid;
        for (const auto& [id, _] : m.elements.Items())
            if (const auto g = m.elements.GuidOf(id))
                w.own[id] = *g;
        const uint64_t origin = w.Add("IFCCARTESIANPOINT((0.,0.,0.))");
        w.z     = w.Add("IFCDIRECTION((0.,0.,1.))");
        w.world = w.Add("IFCAXIS2PLACEMENT3D(" + Ref(origin) + ",$,$)");
        const uint64_t ctx = w.Add("IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-05," + Ref(w.world) + ",$)");
        w.context = ctx;
        w.body = w.Add("IFCGEOMETRICREPRESENTATIONSUBCONTEXT('Body','Model',*,*,*,*," + Ref(ctx) + ",$,.MODEL_VIEW.,$)");
        const uint64_t axis = w.Add("IFCGEOMETRICREPRESENTATIONSUBCONTEXT('Axis','Model',*,*,*,*," + Ref(ctx) + ",$,.GRAPH_VIEW.,$)");
        std::vector<uint64_t> units;
        for (const char* u : { "IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.)", "IFCSIUNIT(*,.AREAUNIT.,$,.SQUARE_METRE.)",
                               "IFCSIUNIT(*,.VOLUMEUNIT.,$,.CUBIC_METRE.)", "IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.)" })
            units.push_back(w.Add(u));
        const uint64_t assignment = w.Add("IFCUNITASSIGNMENT((" + Refs(units) + "))");
        const uint64_t project = w.Add("IFCPROJECT(" + w.GuidFor(kProject, 0) + ",$," + S(m.name) + ",$,$,$,$,(" + Ref(ctx) + ")," + Ref(assignment) + ")");
        const uint64_t siteAt = w.Place(std::nullopt, { 0.0, 0.0, 0.0 }, std::nullopt);
        const uint64_t site = w.Add("IFCSITE(" + w.GuidFor(kSite, 0) + ",$,'Site',$,$," + Ref(siteAt) + ",$,$,.ELEMENT.,$,$,$,$,$)");
        const uint64_t buildingAt = w.Place(siteAt, { 0.0, 0.0, 0.0 }, std::nullopt);
        const uint64_t building = w.Add("IFCBUILDING(" + w.GuidFor(kBuilding, 0) + ",$," + S(m.name) + ",$,$," + Ref(buildingAt) + ",$,$,.ELEMENT.,$,$,$)");
        w.Add("IFCRELAGGREGATES(" + w.GuidFor(kAggregates, kProject) + ",$,$,$," + Ref(project) + ",(" + Ref(site) + "))");
        w.Add("IFCRELAGGREGATES(" + w.GuidFor(kAggregates, kSite) + ",$,$,$," + Ref(site) + ",(" + Ref(building) + "))");

        // 楼层：各标高的（实体、放置）
        std::map<Guid, std::pair<uint64_t, uint64_t>> storeys;
        for (const auto& [id, l] : m.Levels())
        {
            const uint64_t at = w.Place(buildingAt, { 0.0, 0.0, l->elevation }, std::nullopt);
            const uint64_t st = w.Add("IFCBUILDINGSTOREY(" + w.GuidFor(kStorey, id) + ",$," + S(l->name) + ",$,$," + Ref(at) + ",$,$,.ELEMENT.," +
                                      Real(l->elevation) + ")");
            storeys[m.GuidOf(id)] = { st, at };
        }
        std::vector<uint64_t> all;
        for (const auto& [_, s] : storeys)
            all.push_back(s.first);
        if (!all.empty())
            w.Add("IFCRELAGGREGATES(" + w.GuidFor(kAggregates, kBuilding) + ",$,$,$," + Ref(building) + ",(" + Refs(all) + "))");

        const auto storeyAt = [&](Guid level) -> std::optional<uint64_t>
        {
            const auto it = storeys.find(m.GuidOf(level));
            return it == storeys.end() ? std::nullopt : std::optional<uint64_t>(it->second.second);
        };
        std::map<Guid, std::vector<uint64_t>> contained, spaces;
        // 墙的（实体、放置）：给开在它上面的洞口
        std::map<Id, std::pair<uint64_t, uint64_t>> placed;
        // 楼板、天花板、屋顶的实体：给材质层组
        std::map<Id, uint64_t> hosts;
        // 各墙里的门窗：（宽、高、窗台），量墙面时减去
        std::map<Id, std::vector<std::array<double, 3>>> holes;
        for (const auto& [_, e] : m.elements.Items())
            if (const Opening* o = e->AsOpening())
            {
                const auto t = m.OpeningTypeOf(*e);
                const auto host = m.elements.IdOf(o->host);
                if (t && host)
                    holes[*host].push_back({ t->width, t->height, o->sill });
            }
        const std::vector<std::array<double, 3>> noHoles;

        for (const auto& [id, ptr] : m.elements.Items())
        {
            const Element& e = *ptr;
            if (const Wall* wl = e.As<Wall>())
            {
                const auto storey = storeyAt(wl->level);
                if (!storey)
                    continue;
                const Walls::Axis path = Walls::Axis::Of(*wl);
                const double base = m.OwnBase(*wl);
                // 放在起点，x 轴沿它出发的方向：定位线、轮廓、洞口都在这个坐标系里
                const P2 u = path.Tangent(0.0);
                const auto local = [&](P2 p) { return P2((p - wl->start).Dot(u), (p - wl->start).Dot(u.Perp())); };
                const uint64_t at = w.Place(storey, { wl->start.x, wl->start.y, wl->baseOffset }, u);
                std::vector<uint64_t> pts;
                for (const P2& p : path.Points(0.0, path.Length(), 0.0))
                    pts.push_back(w.Point(local(p)));
                const uint64_t line    = w.Add("IFCPOLYLINE((" + Refs(pts) + "))");
                const uint64_t axisRep = w.Add("IFCSHAPEREPRESENTATION(" + Ref(axis) + ",'Axis','Curve2D',(" + Ref(line) + "))");
                // 它的轮廓（与平面图一样在相接处斜接）向上拉伸；贴着屋顶或楼板时，每块在它的一个平面下（或上）的部分一个闭合壳
                uint64_t shape = 0;
                if (!wl->attachTop && !wl->attachBase)
                {
                    Ring outline;
                    for (const P2& p : Walls::Outline(m, id))
                        outline.push_back(local(p));
                    const uint64_t profile = w.Profile(outline, {});
                    shape = w.Body(profile, 0.0, m.OwnTop(*wl) - base, { axisRep });
                }
                else
                {
                    std::vector<uint64_t> breps;
                    for (const Walls::Prism& p : Walls::Prisms(m, id, false))
                    {
                        Ring ring = p.ring;
                        if (Geom::Area(ring) < 0.0)
                            std::reverse(ring.begin(), ring.end());
                        std::vector<P3> bottom, top;
                        for (const P2& q : ring)
                        {
                            const P2 l = local(q);
                            bottom.push_back({ l.x, l.y, p.bottom.At(q) - base });
                            top.push_back({ l.x, l.y, p.top.At(q) - base });
                        }
                        breps.push_back(w.Shell(bottom, top));
                    }
                    const uint64_t rep = w.Add("IFCSHAPEREPRESENTATION(" + Ref(w.body) + ",'Body','Brep',(" + Refs(breps) + "))");
                    shape = w.Add("IFCPRODUCTDEFINITIONSHAPE($,$,(" + Ref(rep) + "," + Ref(axisRep) + "))");
                }
                const uint64_t ent = w.Add("IFCWALL(" + w.GuidFor(kElement, id) + ",$," + S(wl->typeName) + ",$," + S(wl->typeName) + "," + Ref(at) + "," +
                                           Ref(shape) + ",'" + std::to_string(id) + "',.STANDARD.)");
                contained[m.GuidOf(wl->level)].push_back(ent);
                placed[id] = { ent, at };
                const auto h = holes.find(id);
                w.Quantities(ent, "Qto_WallBaseQuantities", WallQuantities(m, *wl, h == holes.end() ? noHoles : h->second), id);
            }
            else if (const Floor* f = e.As<Floor>())
            {
                const auto storey = storeyAt(f->level);
                if (!storey)
                    continue;
                const uint64_t at = w.Place(storey, { 0.0, 0.0, 0.0 }, std::nullopt);
                // 平的：轮廓减去洞口向上拉伸到顶面；带坡度的：每块一个闭合壳
                const double depth = m.FloorDepth(*f);
                const bool level = m.FloorTop(*f).g == P2(0.0, 0.0);
                uint64_t shape = 0;
                if (level)
                {
                    const uint64_t profile = w.Profile(f->boundary, f->openings);
                    shape = w.Body(profile, f->offset - depth, depth, {});
                }
                else
                {
                    std::vector<std::vector<P3>> faces;
                    for (auto& [face, _] : m.FloorSlabs(*f))
                        faces.push_back(face);
                    shape = w.Breps(faces, m.Elevation(f->level), depth);
                }
                const uint64_t ent = w.Add("IFCSLAB(" + w.GuidFor(kElement, id) + ",$," + S(f->typeName) + ",$," + S(f->typeName) + "," + Ref(at) + "," +
                                           Ref(shape) + ",'" + std::to_string(id) + "',.FLOOR.)");
                contained[m.GuidOf(f->level)].push_back(ent);
                hosts[id] = ent;
                if (f->structural)
                    w.LoadBearing(ent, "Pset_SlabCommon", id);
                // 平的板按轮廓减去洞口、乘厚度量；带坡度的没有平面面积可给，只有厚度
                double holesArea = 0.0;
                for (const Ring& o : f->openings)
                    holesArea += std::abs(Geom::Area(o));
                const double gross = std::abs(Geom::Area(f->boundary)), net = gross - holesArea;
                std::vector<std::tuple<std::string, std::string, double>> values = { { "Width", "LENGTH", depth } };
                if (level)
                    values.insert(values.end(), { { "Perimeter", "LENGTH", Geom::Perimeter(f->boundary) },
                                                  { "GrossArea", "AREA", gross / 1e6 },
                                                  { "NetArea", "AREA", net / 1e6 },
                                                  { "GrossVolume", "VOLUME", gross * depth / 1e9 },
                                                  { "NetVolume", "VOLUME", net * depth / 1e9 } });
                w.Quantities(ent, "Qto_SlabBaseQuantities", values, id);
            }
            else if (const Ceiling* c = e.As<Ceiling>())
            {
                const auto storey = storeyAt(c->level);
                if (!storey)
                    continue;
                const uint64_t at = w.Place(storey, { 0.0, 0.0, 0.0 }, std::nullopt);
                // 轮廓减去洞口，向上拉伸它的厚度
                const uint64_t profile = w.Profile(c->boundary, c->openings);
                const auto t = m.types.ceiling.find(c->typeName);
                const uint64_t shape = w.Body(profile, c->offset, t == m.types.ceiling.end() ? 0.0 : t->second.thickness, {});
                const uint64_t ent = w.Add("IFCCOVERING(" + w.GuidFor(kElement, id) + ",$," + S(c->typeName) + ",$," + S(c->typeName) + "," + Ref(at) +
                                           "," + Ref(shape) + ",'" + std::to_string(id) + "',.CEILING.)");
                contained[m.GuidOf(c->level)].push_back(ent);
                hosts[id] = ent;
            }
            else if (const Room* r = e.As<Room>())
            {
                const auto storey = storeyAt(r->level);
                if (!storey)
                    continue;
                const uint64_t at = w.Place(storey, { 0.0, 0.0, 0.0 }, std::nullopt);
                // 没围住的房间没有形状可给
                const auto up = m.LevelAbove(r->level);
                const double height = up ? m.Elevation(*up) - m.Elevation(r->level) : DefaultHeight;
                std::string shape = "$";
                if (const auto region = m.RoomRegion(*r))
                {
                    const uint64_t profile = w.Profile(region->outer, region->holes);
                    shape = Ref(w.Body(profile, 0.0, height, {}));
                }
                const uint64_t ent = w.Add("IFCSPACE(" + w.GuidFor(kElement, id) + ",$," + S(r->number) + ",$,$," + Ref(at) + "," + shape + "," +
                                           S(r->name) + ",.ELEMENT.,.SPACE.,$)");
                // 两种面积：毛面积量到墙中心线，净面积量到墙面
                const auto gross = m.RoomRegionAt(*r, RoomArea::Centre), net = m.RoomRegionAt(*r, RoomArea::Finish);
                if (gross && net)
                    w.Quantities(ent, "Qto_SpaceBaseQuantities",
                                 { { "Height", "LENGTH", height },
                                   { "GrossPerimeter", "LENGTH", gross->Perimeter() },
                                   { "NetPerimeter", "LENGTH", net->Perimeter() },
                                   { "GrossFloorArea", "AREA", gross->Area() / 1e6 },
                                   { "NetFloorArea", "AREA", net->Area() / 1e6 } },
                                 id);
                spaces[m.GuidOf(r->level)].push_back(ent);
            }
            // 构件：它的类别的元素，按族与类型命名（同 Revit），每个形体是着它材质颜色的闭合三角面集
            else if (const Component* cp = e.As<Component>())
            {
                const auto storey = storeyAt(cp->level);
                const auto fam = m.families.find(cp->family);
                if (!storey || fam == m.families.end())
                    continue;
                const std::vector<Fam::Piece> pieces = m.ComponentLocalPieces(*cp);
                if (pieces.empty())
                    continue;
                const uint64_t at = w.Place(storey, { cp->at.x, cp->at.y, cp->offset }, Heading(cp->angle));
                std::vector<uint64_t> made;
                const std::vector<uint64_t> items = w.Pieces(m, pieces, made);
                const uint64_t shape = w.Tessellated(items);
                // 门窗的族放在墙里，从不作为构件
                const Fam::CategoryOf::IfcClass cls = Fam::CategoryOf::Ifc(fam->second.category);
                const std::string name   = S(cp->family + ":" + cp->typeName);
                const std::string object = cls.objectType ? S(cls.objectType) : name;
                const uint64_t ent = w.Add(std::string(cls.entity) + "(" + w.GuidFor(kElement, id) + ",$," + name + ",$," + object + "," + Ref(at) + "," +
                                           Ref(shape) + ",'" + std::to_string(id) + "'," + cls.kind + ")");
                contained[m.GuidOf(cp->level)].push_back(ent);
                if (const auto relating = w.Relating(made, name))
                    w.Add("IFCRELASSOCIATESMATERIAL(" + w.GuidFor(kComponentMaterials, id) + ",$,$,$,(" + Ref(ent) + ")," + Ref(*relating) + ")");
            }
            // 房间分隔线：沿它的线的虚拟元素，只围合房间
            else if (const RoomSeparator* rs = e.As<RoomSeparator>())
            {
                const auto storey = storeyAt(rs->level);
                if (!storey)
                    continue;
                const uint64_t at = w.Place(storey, { 0.0, 0.0, 0.0 }, std::nullopt);
                const uint64_t a = w.Point(rs->start), b = w.Point(rs->end);
                const uint64_t line  = w.Add("IFCPOLYLINE((" + Ref(a) + "," + Ref(b) + "))");
                const uint64_t rep   = w.Add("IFCSHAPEREPRESENTATION(" + Ref(axis) + ",'Axis','Curve2D',(" + Ref(line) + "))");
                const uint64_t shape = w.Add("IFCPRODUCTDEFINITIONSHAPE($,$,(" + Ref(rep) + "))");
                const uint64_t ent = w.Add("IFCVIRTUALELEMENT(" + w.GuidFor(kElement, id) + ",$,'Room Separation Line',$,'" + Separation + "'," + Ref(at) +
                                           "," + Ref(shape) + ",'" + std::to_string(id) + "')");
                contained[m.GuidOf(rs->level)].push_back(ent);
            }
            else if (const Roof* rf = e.As<Roof>())
            {
                const auto storey = storeyAt(rf->level);
                if (!storey)
                    continue;
                const auto pieces = m.RoofSlabs(*rf);
                const uint64_t at = w.Place(storey, { 0.0, 0.0, 0.0 }, std::nullopt);
                std::vector<uint64_t> breps;
                for (const auto& [face, rise] : pieces)
                    breps.push_back(w.Brep(face, m.Elevation(rf->level), rise));
                const uint64_t shape = w.Solid(breps, {});
                const char* kind = nullptr;
                if (rf->extrusion)
                {
                    const size_t k = rf->extrusion->profile.size() < 2 ? 0 : rf->extrusion->profile.size() - 2;
                    kind = k == 0 ? ".SHED_ROOF." : k == 1 ? ".GABLE_ROOF." : ".FREEFORM.";
                }
                else if (rf->slope == 0.0)
                    kind = ".FLAT_ROOF.";
                else if (rf->gable || !rf->gables.empty())
                    kind = ".GABLE_ROOF.";
                else
                    kind = ".HIP_ROOF.";
                const uint64_t ent = w.Add("IFCROOF(" + w.GuidFor(kElement, id) + ",$," + S(rf->typeName) + ",$," + S(rf->typeName) + "," + Ref(at) + "," +
                                           Ref(shape) + ",'" + std::to_string(id) + "'," + kind + ")");
                contained[m.GuidOf(rf->level)].push_back(ent);
                hosts[id] = ent;
            }
        }

        for (const auto& [id, ptr] : m.elements.Items())
        {
            const Element& e = *ptr;
            const Opening* o = e.AsOpening();
            if (!o)
                continue;
            const auto t      = m.OpeningTypeOf(e);
            const Wall* wl    = m.GetWall(o->host);
            const auto hostId = m.elements.IdOf(o->host);
            const auto hosted = hostId ? placed.find(*hostId) : placed.end();
            if (!t || !wl || hosted == placed.end())
                continue;
            const auto [wall, wallAt] = hosted->second;
            // 从门框朝墙的起点，在它中间垂直于墙，在墙的坐标系里
            const Walls::Axis ax = Walls::Axis::Of(*wl);
            const P2 u = ax.Tangent(0.0), way = ax.Tangent(o->offset);
            const P2 jamb = ax.At(o->offset, Walls::Centre(m, *wl)) - way * (t->width / 2.0) - wl->start;
            const uint64_t at = w.Place(wallAt, { jamb.Dot(u), jamb.Dot(u.Perp()), o->sill }, P2(way.Dot(u), way.Dot(u.Perp())));
            // 洞口伸出两个墙面各 10 毫米，切得干净；穿过弧墙的还要越过弧的拱起
            double bow = 0.0;
            if (const auto radius = ax.Radius())
            {
                const double inner = *radius - m.Thickness(*wl) / 2.0 - std::abs(Walls::Centre(m, *wl));
                bow = inner > t->width / 2.0 ? inner - std::sqrt(inner * inner - t->width * t->width / 4.0) : t->width;
            }
            const uint64_t profile = w.Rectangle(P2(t->width / 2.0, 0.0), P2(t->width, m.Thickness(*wl) + 20.0 + 2.0 * bow));
            const uint64_t holeShape = w.Body(profile, 0.0, t->height, {});
            const uint64_t opening = w.Add("IFCOPENINGELEMENT(" + w.GuidFor(kOpening, id) + ",$,'Opening',$,$," + Ref(at) + "," + Ref(holeShape) +
                                           ",$,.OPENING.)");
            w.Add("IFCRELVOIDSELEMENT(" + w.GuidFor(kVoids, id) + ",$,$,$," + Ref(wall) + "," + Ref(opening) + ")");
            // 门窗本身：在墙中间的 50 毫米厚板；族的，同构件一样是各形体着材质颜色的闭合三角面集，立在洞口中间
            std::vector<uint64_t> made;
            uint64_t fillAt = 0, shape = 0;
            const auto local = o->family ? m.OpeningLocal(e) : std::nullopt;
            if (local)
            {
                const std::vector<uint64_t> items = w.Pieces(m, local->first, made);
                const uint64_t rep = w.Add("IFCSHAPEREPRESENTATION(" + Ref(w.body) + ",'Body','Tessellation',(" + Refs(items) + "))");
                fillAt = w.Place(at, { t->width / 2.0, 0.0, 0.0 }, std::nullopt);
                shape  = w.Add("IFCPRODUCTDEFINITIONSHAPE($,$,(" + Ref(rep) + "))");
            }
            else
            {
                const uint64_t panel = w.Rectangle(P2(t->width / 2.0, 0.0), P2(t->width, 50.0));
                fillAt = w.Place(at, { 0.0, 0.0, 0.0 }, std::nullopt);
                shape  = w.Body(panel, 0.0, t->height, {});
            }
            const std::string h = Real(t->height), wd = Real(t->width);
            // 按 族:类型 命名，同 Revit；项目自己的类型按它的名称
            const std::string name = S(o->family ? *o->family + ":" + o->typeName : o->typeName);
            // 怎么开：按它的族或类型；没说的门是平开门
            uint64_t ent = 0;
            if (e.IsDoor())
                ent = w.Add("IFCDOOR(" + w.GuidFor(kElement, id) + ",$," + name + ",$," + name + "," + Ref(fillAt) + "," + Ref(shape) + "," + S(o->mark) +
                            "," + h + "," + wd + ",.DOOR.," + OperationOf::IfcDoor(t->operation, o->flipHand) + ",$)");
            else
                ent = w.Add("IFCWINDOW(" + w.GuidFor(kElement, id) + ",$," + name + ",$," + name + "," + Ref(fillAt) + "," + Ref(shape) + "," +
                            S(o->mark) + "," + h + "," + wd + ",.WINDOW.," + OperationOf::IfcWindowPartitioning(t->operation) + ",$)");
            w.Add("IFCRELFILLSELEMENT(" + w.GuidFor(kFills, id) + ",$,$,$," + Ref(opening) + "," + Ref(ent) + ")");
            w.Quantities(ent, e.IsDoor() ? "Qto_DoorBaseQuantities" : "Qto_WindowBaseQuantities",
                         { { "Width", "LENGTH", t->width },
                           { "Height", "LENGTH", t->height },
                           { "Perimeter", "LENGTH", 2.0 * (t->width + t->height) },
                           { "Area", "AREA", t->width * t->height / 1e6 } },
                         id);
            // 族的门窗用什么做：一种材质，或几种的集
            if (const auto r = w.Relating(made, S(o->typeName)))
                w.Add("IFCRELASSOCIATESMATERIAL(" + w.GuidFor(kComponentMaterials, id) + ",$,$,$,(" + Ref(ent) + ")," + Ref(*r) + ")");
            contained[m.GuidOf(wl->level)].push_back(ent);
        }

        // 复合类型的层：用到的每个类型一个材质层组，每层的材质只写一次，带颜色供查看器着色；
        // 每个墙类型与墙立在的定位线，一个层组的用法，从外侧面往里
        const auto layerSet = [&](const std::string& name, const std::vector<Layer>& layers)
        {
            std::vector<uint64_t> ids;
            for (const Layer& l : layers)
            {
                const std::string named(Text::Trim(l.material));
                const std::string material = named.empty() ? std::string("$") : Ref(w.Material(m, named));
                ids.push_back(w.Add("IFCMATERIALLAYER(" + material + "," + Real(l.thickness) + ",$,$,$," + FunctionCategory(l.function) + ",$)"));
            }
            return w.Add("IFCMATERIALLAYERSET((" + Refs(ids) + ")," + S(name) + ",$)");
        };
        std::map<std::string, uint64_t> sets;
        std::set<std::string> used;
        for (const auto& [id, _] : placed)
            if (const Wall* wl = m.GetWall(id))
                used.insert(wl->typeName);
        for (const auto& [name, t] : m.types.wall)
            if (!t.layers.empty() && used.count(name))
                sets[name] = layerSet(name, t.layers);
        // 复合楼板的层组从上往下，每个类型与它的顶面高出标高多少一个用法；屋顶、天花板的层组直接用
        std::map<std::tuple<std::string, std::string, std::string>, std::vector<uint64_t>> slabUses;
        for (const auto& [id, ent] : hosts)
        {
            const Element* e = m.elements.Get(id);
            std::tuple<std::string, std::string, std::string> key;
            if (const Floor* f = e ? e->As<Floor>() : nullptr)
                key = { "floor", f->typeName,
                        Real(m.FloorTop(*f).At(f->boundary.empty() ? P2() : f->boundary.front()) - m.Elevation(f->level)) };
            else if (const Roof* r = e ? e->As<Roof>() : nullptr)
                key = { "roof", r->typeName, "" };
            else if (const Ceiling* c = e ? e->As<Ceiling>() : nullptr)
                key = { "ceiling", c->typeName, "" };
            else
                continue;
            if (!TypeLayers(m, std::get<0>(key), std::get<1>(key)).empty())
                slabUses[key].push_back(ent);
        }
        std::map<std::pair<std::string, std::string>, uint64_t> slabSets;
        uint64_t k = 0;
        for (const auto& [key, ents] : slabUses)
        {
            const auto& [cat, name, offset] = key;
            auto it = slabSets.find({ cat, name });
            if (it == slabSets.end())
                it = slabSets.emplace(std::pair{ cat, name }, layerSet(name, TypeLayers(m, cat, name))).first;
            const uint64_t relating =
                cat == "floor" ? w.Add("IFCMATERIALLAYERSETUSAGE(" + Ref(it->second) + ",.AXIS3.,.NEGATIVE.," + offset + ",$)") : it->second;
            w.Add("IFCRELASSOCIATESMATERIAL(" + w.GuidFor(kSlabMaterials, k++) + ",$,$,$,(" + Refs(ents) + ")," + Ref(relating) + ")");
        }
        std::map<std::pair<std::string, std::string>, std::vector<uint64_t>> uses;
        for (const auto& [id, p] : placed)
            if (const Wall* wl = m.GetWall(id); wl && sets.count(wl->typeName))
                uses[{ wl->typeName, Real(Walls::Faces(m, *wl).first) }].push_back(p.first);
        k = 0;
        for (const auto& [key, ents] : uses)
        {
            const uint64_t usage = w.Add("IFCMATERIALLAYERSETUSAGE(" + Ref(sets[key.first]) + ",.AXIS2.,.NEGATIVE.," + key.second + ",$)");
            w.Add("IFCRELASSOCIATESMATERIAL(" + w.GuidFor(kMaterials, k++) + ",$,$,$,(" + Refs(ents) + ")," + Ref(usage) + ")");
        }
        for (const auto& [level, s] : storeys)
        {
            const Id levelId = m.elements.IdOf(level).value_or(0);
            if (const auto it = contained.find(level); it != contained.end())
                w.Add("IFCRELCONTAINEDINSPATIALSTRUCTURE(" + w.GuidFor(kContains, levelId) + ",$,$,$,(" + Refs(it->second) + ")," + Ref(s.first) + ")");
            if (const auto it = spaces.find(level); it != spaces.end())
                w.Add("IFCRELAGGREGATES(" + w.GuidFor(kSpaces, levelId) + ",$,$,$," + Ref(s.first) + ",(" + Refs(it->second) + "))");
        }
        for (auto& [id, c] : std::exchange(w.common, {}))
            w.Define(std::get<0>(c), std::get<1>(c), std::get<2>(c), { kCommon, kCommonDefines }, id);

        const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string app = S("MiniBIM");
        return "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\nFILE_NAME(" + S(m.name + ".ifc") + ",'" +
               Utc(now > 0 ? static_cast<uint64_t>(now) : 0) + "',(''),('')," + app + "," + app + ",'');\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n" +
               w.out + "ENDSEC;\nEND-ISO-10303-21;\n";
    }
}
