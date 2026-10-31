// 移植自 BIMCraft crates/core/src/view3d.rs（f3bbb33）的实体生成部分，MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "View/ModelScene.h"
#include "Core/Log.h"
#include "Core/Math/Mat4.hpp"
#include "Core/Math/MathUtils.hpp"
#include "I18n/Translator.h"
#include "Model/Massing.h"
#include "Model/Model.h"
#include "Model/Walls.h"
#include "Solid/ISolidKernel.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace MiniBIM
{
    namespace
    {
        using Bim::P2;
        using Bim::Ring;
        using Math::Point3;
        using Math::Vec3;

        struct Rgb
        {
            float r, g, b;
        };

        // 颜色同 BIMCraft 的三维视图
        constexpr Rgb kWall  = { 236 / 255.0f, 236 / 255.0f, 232 / 255.0f };
        constexpr Rgb kSlab  = { 214 / 255.0f, 214 / 255.0f, 210 / 255.0f };
        constexpr Rgb kDoor  = { 198 / 255.0f, 168 / 255.0f, 128 / 255.0f };
        constexpr Rgb kGlass = { 166 / 255.0f, 200 / 255.0f, 228 / 255.0f };
        constexpr Rgb kRoof  = { 176 / 255.0f, 118 / 255.0f, 96 / 255.0f };
        constexpr Rgb kCeiling = { 244 / 255.0f, 242 / 255.0f, 236 / 255.0f };
        // 构件的形体用的材质库里没有时
        constexpr Rgb kComponent = { 214 / 255.0f, 204 / 255.0f, 188 / 255.0f };
        // 结构柱（BIMCraft 的 STRUCTURE）；建筑柱同墙
        constexpr Rgb kStructure = { 182 / 255.0f, 184 / 255.0f, 190 / 255.0f };
        constexpr Rgb kMass      = { 158 / 255.0f, 190 / 255.0f, 224 / 255.0f };

        // 门洞扣得比墙面再出去一点，免得布尔运算遇到重合的面
        constexpr double kPast = 50.0;
        // 门扇、玻璃的厚度：墙中间 50 毫米的板
        constexpr double kPanel = 50.0;

        // 固定的两盏平行光 + 环境光：不随视角变，环绕时各个面的明暗保持一致，分得清朝向
        float Shade(const Vec3& n)
        {
            static const Vec3 key  = Vec3{ -0.45, -0.6, 0.66 }.Normalized();
            static const Vec3 fill = Vec3{ 0.6, 0.35, 0.25 }.Normalized();
            const double d = 0.5 + 0.42 * std::max(0.0, Math::Dot(n, key)) + 0.16 * std::max(0.0, Math::Dot(n, fill));
            return static_cast<float>(std::min(d, 1.0));
        }

        // z 处的水平轮廓，逆时针
        Solid::Profile ProfileOf(const Ring& ring, double z)
        {
            const bool ccw = Bim::Geom::Area(ring) >= 0.0;
            Solid::Profile p;
            for (size_t i = 0; i < ring.size(); ++i)
            {
                const P2& q = ccw ? ring[i] : ring[ring.size() - 1 - i];
                p.Vertices.push_back({ Point3{ q.x, q.y, z }, 0.0 });
            }
            return p;
        }

        // ring 从 z 拉伸 height
        Solid::BodyPtr Prism(Solid::ISolidKernel& k, const Ring& ring, double z, double height)
        {
            if (ring.size() < 3 || !(height > 0.0))
                return nullptr;
            return k.Extrude(ProfileOf(ring, z), height);
        }

        // 墙定位线左边 a 到 b 之间、沿墙 s0 到 s1 的一条（圆弧墙上分成短直段）
        Ring Band(const Bim::Walls::Axis& axis, double s0, double s1, double a, double b)
        {
            Ring ring = axis.Points(s0, s1, a);
            const Ring back = axis.Points(s1, s0, b);
            ring.insert(ring.end(), back.begin(), back.end());
            return ring;
        }

        // 底面与顶面不平（贴着坡楼板）的棱柱：围成封闭的网格再转成实体
        Solid::BodyPtr SlopedPrism(Solid::ISolidKernel& k, const Bim::Walls::Prism& p)
        {
            Ring ring = p.ring;
            if (Bim::Geom::Area(ring) < 0.0)
                std::reverse(ring.begin(), ring.end());
            Solid::Mesh mesh;
            const size_t n = ring.size();
            for (const P2& q : ring)
                mesh.Positions.push_back(Point3{ q.x, q.y, p.bottom.At(q) });
            for (const P2& q : ring)
                mesh.Positions.push_back(Point3{ q.x, q.y, p.top.At(q) });
            mesh.Normals.assign(mesh.Positions.size(), Vec3{ 0, 0, 1 });
            auto tri = [&](size_t a, size_t b, size_t c)
            {
                mesh.Indices.push_back(static_cast<uint32_t>(a));
                mesh.Indices.push_back(static_cast<uint32_t>(b));
                mesh.Indices.push_back(static_cast<uint32_t>(c));
            };
            for (const auto& t : Bim::Geom::Triangulate(ring))
            {
                tri(t[0], t[2], t[1]);              // 底面朝下
                tri(n + t[0], n + t[1], n + t[2]);  // 顶面朝上
            }
            for (size_t i = 0; i < n; ++i)
            {
                const size_t j = (i + 1) % n;
                tri(i, j, n + j);
                tri(i, n + j, n + i);
            }
            return k.FromMesh(mesh);
        }
    }

    bool ModelScene::Build(const Bim::Model& m, Solid::ISolidKernel* kernel, std::string& error)
    {
        m_triangles.clear();
        m_edges.clear();
        m_owners.clear();
        m_edgeOwners.clear();
        m_bounds = AABB::Empty();
        m_counts = {};
        if (!kernel)
        {
            error = N_("This build has no solid modeling kernel (MINIBIM_WITH_SOLID is off).");
            return false;
        }
        Solid::ISolidKernel& k = *kernel;

        // own：网格建成的实体自己给出的边线（不画三角剖分的对角线）；为空时取内核的边
        auto add = [&](const Solid::BodyPtr& body, const Rgb& c, const std::vector<Point3>* own = nullptr)
        {
            const Solid::Mesh mesh = k.Tessellate(body);
            for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
            {
                for (size_t j = 0; j < 3; ++j)
                {
                    const uint32_t idx = mesh.Indices[i + j];
                    const Point3& p = mesh.Positions[idx];
                    const float s = Shade(mesh.Normals[idx]);
                    m_triangles.push_back({ { static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) },
                                            VertexColor(c.r * s, c.g * s, c.b * s, 1.0f) });
                    m_bounds.Expand(p);
                }
            }
            if (own)
            {
                m_edges.insert(m_edges.end(), own->begin(), own->end());
                return;
            }
            for (const auto& line : k.Edges(body).Lines)
            {
                for (size_t i = 0; i + 1 < line.size(); ++i)
                {
                    m_edges.push_back(line[i]);
                    m_edges.push_back(line[i + 1]);
                }
            }
        };
        // 族的形体：已经是朝外的三角形（族的网格与空心切割移植自 BIMCraft，不经实体内核），按面法线着色；
        // 边只画折角处与开口的边（相邻两个三角形不共面，或只有一个三角形）
        auto addPiece = [&](const Bim::Fam::Piece& piece, const Rgb& fallback)
        {
            const auto mat = m.materials.find(piece.material);
            const Rgb c = mat == m.materials.end() ? fallback
                                                   : Rgb{ mat->second.color[0] / 255.0f, mat->second.color[1] / 255.0f, mat->second.color[2] / 255.0f };
            std::vector<Vec3> normals;
            normals.reserve(piece.tris.size());
            for (const Bim::Tri& t : piece.tris)
            {
                const Bim::P3 n = Bim::V3::Cross(Bim::V3::Sub(t[1], t[0]), Bim::V3::Sub(t[2], t[0]));
                const double len = std::sqrt(Bim::V3::Dot(n, n));
                const Vec3 unit = len > 0.0 ? Vec3{ n[0] / len, n[1] / len, n[2] / len } : Vec3{ 0, 0, 1 };
                normals.push_back(unit);
                const float s = Shade(unit);
                for (const Bim::P3& p : t)
                {
                    m_triangles.push_back({ { static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]) },
                                            VertexColor(c.r * s, c.g * s, c.b * s, 1.0f) });
                    m_bounds.Expand(Point3{ p[0], p[1], p[2] });
                }
            }
            // 每条边（不分方向）挨着的三角形
            std::map<std::pair<Bim::P3, Bim::P3>, std::vector<size_t>> sides;
            for (size_t i = 0; i < piece.tris.size(); ++i)
                for (size_t j = 0; j < 3; ++j)
                {
                    Bim::P3 a = piece.tris[i][j], b = piece.tris[i][(j + 1) % 3];
                    if (b < a)
                        std::swap(a, b);
                    sides[{ a, b }].push_back(i);
                }
            for (const auto& [edge, tris] : sides)
            {
                const bool fold = tris.size() != 2 || Math::Dot(normals[tris[0]], normals[tris[1]]) < std::cos(10.0 * 3.14159265358979323846 / 180.0);
                if (!fold)
                    continue;
                m_edges.push_back(Point3{ edge.first[0], edge.first[1], edge.first[2] });
                m_edges.push_back(Point3{ edge.second[0], edge.second[1], edge.second[2] });
            }
        };
        // 斜棱柱的边：底圈、顶圈与各角的竖边
        auto prismEdges = [](const Bim::Walls::Prism& p)
        {
            std::vector<Point3> out;
            const size_t n = p.ring.size();
            for (size_t i = 0; i < n; ++i)
            {
                const P2 a = p.ring[i], b = p.ring[(i + 1) % n];
                for (const Bim::Plane* pl : { &p.bottom, &p.top })
                {
                    out.push_back(Point3{ a.x, a.y, pl->At(a) });
                    out.push_back(Point3{ b.x, b.y, pl->At(b) });
                }
                out.push_back(Point3{ a.x, a.y, p.bottom.At(a) });
                out.push_back(Point3{ a.x, a.y, p.top.At(a) });
            }
            return out;
        };

        const Bim::Walls::Index index(m);
        // 默认三维视图隐藏的不建
        const Bim::Look look = m.LookOf3D();
        int failed = 0;
        for (const auto& [id, ptr] : m.elements.Items())
        {
            if (!look.Shows(id))
                continue;
            const Bim::Element& e = *ptr;
            m_owners.emplace_back(m_triangles.size(), id);
            m_edgeOwners.emplace_back(m_edges.size(), id);
            bool ok = true;
            if (const Bim::Wall* w = e.As<Bim::Wall>())
            {
                if (!w->attachTop && !w->attachBase)
                {
                    // 墙的平面轮廓从底拉伸到顶，再扣掉门窗洞：门窗洞四周的边是整面墙的边
                    const double z0 = m.OwnBase(*w), z1 = m.OwnTop(*w);
                    Solid::BodyPtr body = Prism(k, Bim::Walls::Outline(m, id, &index), z0, z1 - z0);
                    const Bim::Walls::Axis axis = Bim::Walls::Axis::Of(*w);
                    const auto [ext, inner] = Bim::Walls::Faces(m, *w);
                    const Bim::Guid guid = m.GuidOf(id);
                    for (const Bim::Id h : index.Openings(guid))
                    {
                        const Bim::Element* oe = m.elements.Get(h);
                        const Bim::Opening* o = oe ? oe->AsOpening() : nullptr;
                        const auto t = oe ? m.OpeningTypeOf(*oe) : std::nullopt;
                        if (!body || !o || !t)
                            continue;
                        const double s0 = std::max(0.0, o->offset - t->width / 2.0), s1 = std::min(axis.Length(), o->offset + t->width / 2.0);
                        const Solid::BodyPtr hole = Prism(k, Band(axis, s0, s1, ext + kPast, inner - kPast), z0 + o->sill, t->height);
                        if (hole)
                            body = k.Boolean(Solid::BooleanOp::Difference, body, hole);
                    }
                    if ((ok = body != nullptr))
                        add(body, kWall);
                }
                else
                {
                    // 贴着楼板的墙：照 BIMCraft 分成棱柱，各自从底部平面到顶部平面
                    for (const Bim::Walls::Prism& p : Bim::Walls::Prisms(m, id, true, &index))
                    {
                        // 没有高度的（窗台为 0 的门下面那块）什么都不是
                        double tallest = 0.0;
                        for (const P2& q : p.ring)
                            tallest = std::max(tallest, p.top.At(q) - p.bottom.At(q));
                        if (tallest <= 1e-6)
                            continue;
                        const bool flat = p.bottom.g == P2{} && p.top.g == P2{};
                        const Solid::BodyPtr body = flat ? Prism(k, p.ring, p.bottom.z, p.top.z - p.bottom.z) : SlopedPrism(k, p);
                        const std::vector<Point3> edges = prismEdges(p);
                        if (body)
                            add(body, kWall, &edges);
                        else
                        {
                            ok = false;
                            std::string pts;
                            for (const P2& q : p.ring)
                                pts += " (" + std::to_string(q.x) + ", " + std::to_string(q.y) + ", " + std::to_string(p.bottom.At(q)) + "-" +
                                       std::to_string(p.top.At(q)) + ")";
                            LOG_WARN("三维场景：墙 %llu 的棱柱建模失败：%s", static_cast<unsigned long long>(id), pts.c_str());
                        }
                    }
                }
                ++m_counts.walls;
            }
            else if (const Bim::Floor* f = e.As<Bim::Floor>())
            {
                // 平放时拉伸、扣掉洞口；有坡度时再按顶面的坡度错切
                const Bim::Plane top = m.FloorTop(*f);
                const double depth = m.FloorDepth(*f);
                Solid::BodyPtr body = Prism(k, f->boundary, -depth, depth);
                for (const Ring& hole : f->openings)
                    if (body)
                        if (const Solid::BodyPtr cut = Prism(k, hole, -depth - kPast, depth + 2.0 * kPast))
                            body = k.Boolean(Solid::BooleanOp::Difference, body, cut);
                if (body)
                {
                    Math::Mat4 shear = Math::Mat4::Identity();
                    shear.m[2]  = top.g.x;
                    shear.m[6]  = top.g.y;
                    shear.m[14] = top.z;
                    body = k.Transform(body, shear);
                }
                if ((ok = body != nullptr))
                    add(body, kSlab);
                ++m_counts.floors;
            }
            else if (const Bim::Ceiling* c = e.As<Bim::Ceiling>())
            {
                // 平的板：拉伸、扣掉洞口
                const auto t = m.types.ceiling.find(c->typeName);
                const double thick = t == m.types.ceiling.end() ? 0.0 : t->second.thickness;
                const double z = m.Elevation(c->level) + c->offset;
                Solid::BodyPtr body = Prism(k, c->boundary, z, thick);
                for (const Ring& hole : c->openings)
                    if (body)
                        if (const Solid::BodyPtr cut = Prism(k, hole, z - kPast, thick + 2.0 * kPast))
                            body = k.Boolean(Solid::BooleanOp::Difference, body, cut);
                if ((ok = body != nullptr))
                    add(body, kCeiling);
                ++m_counts.ceilings;
            }
            else if (const Bim::Roof* r = e.As<Bim::Roof>())
            {
                // 每个坡面一块：底面在坡面上，顶面高出类型的厚度（铅垂量）
                std::vector<std::pair<Ring, Bim::Plane>> pieces;
                std::string why;
                if ((ok = m.RoofPieces(*r, pieces, why)))
                    for (const auto& [ring, plane] : pieces)
                    {
                        Bim::Walls::Prism p;
                        p.ring   = ring;
                        p.bottom = plane;
                        p.top    = Bim::Plane{ plane.z + m.RoofRise(*r, plane), plane.g };
                        const bool flat = plane.g == P2{};
                        const Solid::BodyPtr body = flat ? Prism(k, ring, plane.z, p.top.z - plane.z) : SlopedPrism(k, p);
                        const std::vector<Point3> edges = prismEdges(p);
                        if (body)
                            add(body, kRoof, &edges);
                        else
                            ok = false;
                    }
                else
                    LOG_WARN("三维场景：roof %llu 没有坡面：%s", static_cast<unsigned long long>(id), why.c_str());
                ++m_counts.roofs;
            }
            else if (const Bim::Column* col = e.AsColumn())
            {
                // 截面从底拉伸到顶
                const auto [bottom, top] = m.ColumnSpan(*col);
                const Solid::BodyPtr body = Prism(k, m.ColumnOutline(e), bottom, top - bottom);
                if ((ok = body != nullptr))
                    add(body, e.As<Bim::Column>() ? kWall : kStructure);
                ++(e.As<Bim::Footing>() ? m_counts.runs : m_counts.columns);
            }
            else if (const auto g = m.RunGeom(e))
            {
                // 矩形截面的是斜的棱柱，圆的是圆管；用类型材质的颜色
                const Bim::RunType* t = m.RunTypeOf(e);
                const auto mat = m.materials.find(t->material);
                const Rgb c = mat == m.materials.end() ? kStructure
                                                       : Rgb{ mat->second.color[0] / 255.0f, mat->second.color[1] / 255.0f, mat->second.color[2] / 255.0f };
                if (g->round)
                {
                    Bim::Fam::Piece piece;
                    std::tie(piece.tris, piece.caps) = g->Tube();
                    piece.material = t->material;
                    piece.round    = true;
                    addPiece(piece, c);
                }
                else
                {
                    const auto [ring, bottom, top] = g->Prism();
                    Bim::Walls::Prism p;
                    p.ring   = ring;
                    p.bottom = bottom;
                    p.top    = top;
                    const bool flat = bottom.g == P2{};
                    const Solid::BodyPtr body = flat ? Prism(k, ring, bottom.z, top.z - bottom.z) : SlopedPrism(k, p);
                    const std::vector<Point3> edges = prismEdges(p);
                    if ((ok = body != nullptr))
                        add(body, c, flat ? nullptr : &edges);
                }
                ++m_counts.runs;
            }
            else if (const Bim::Mass* ms = e.As<Bim::Mass>())
            {
                // 放样的是从底面轮廓到顶部轮廓的三角形，否则底面轮廓拉伸
                const auto [bottom, top] = m.MassSpan(*ms);
                if (ms->Lofted())
                {
                    Bim::Fam::Piece piece;
                    piece.tris = Bim::Massing::Tris(ms->Footprint(), ms->TopRing(), bottom, top);
                    addPiece(piece, kMass);
                }
                else
                {
                    const Solid::BodyPtr body = Prism(k, ms->Footprint(), bottom, top - bottom);
                    if ((ok = body != nullptr))
                        add(body, kMass);
                }
                ++m_counts.masses;
            }
            else if (const Bim::Component* c = e.As<Bim::Component>())
            {
                // 族的各个形体，按材质上色
                for (const Bim::Fam::Piece& piece : m.ComponentPieces(*c))
                    addPiece(piece, kComponent);
                ++m_counts.components;
            }
            else if (const Bim::Opening* o = e.AsOpening(); o && o->family)
            {
                // 族的门窗：各个形体按材质上色，没有材质的用门或玻璃的颜色
                for (const Bim::Fam::Piece& piece : m.OpeningPieces(e))
                    addPiece(piece, e.IsDoor() ? kDoor : kGlass);
                ++(e.IsDoor() ? m_counts.doors : m_counts.windows);
            }
            else if (const Bim::Opening* o = e.AsOpening())
            {
                // 门扇或玻璃：墙中间 50 毫米厚的板，与墙正交
                const Bim::Wall* host = m.GetWall(o->host);
                const auto t = m.OpeningTypeOf(e);
                if (host && t)
                {
                    const Bim::Walls::Axis axis = Bim::Walls::Axis::Of(*host);
                    const P2 u = axis.Tangent(o->offset), n = u.Perp();
                    const P2 c = axis.At(o->offset, Bim::Walls::Centre(m, *host));
                    const Ring ring = { c + u * (-t->width / 2.0) + n * (-kPanel / 2.0), c + u * (t->width / 2.0) + n * (-kPanel / 2.0),
                                        c + u * (t->width / 2.0) + n * (kPanel / 2.0), c + u * (-t->width / 2.0) + n * (kPanel / 2.0) };
                    const Solid::BodyPtr body = Prism(k, ring, m.OwnBase(*host) + o->sill, t->height);
                    if ((ok = body != nullptr))
                        add(body, e.IsDoor() ? kDoor : kGlass);
                }
                ++(e.IsDoor() ? m_counts.doors : m_counts.windows);
            }
            if (!ok)
            {
                ++failed;
                LOG_WARN("三维场景：%s %llu 建模失败：%s", e.Category(), static_cast<unsigned long long>(id), k.LastError().c_str());
            }
        }
        if (failed > 0)
        {
            error = N_("Some elements could not be modelled in 3D; see the log for which.");
            return false;
        }
        return true;
    }

    std::optional<Bim::Id> ModelScene::Pick(const Math::Point3& origin, const Math::Vec3& dir) const
    {
        // Möller–Trumbore：最近的正向交点
        auto cross = [](const Math::Vec3& a, const Math::Vec3& b) { return Math::Vec3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; };
        auto dot   = [](const Math::Vec3& a, const Math::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        auto at    = [&](size_t i) { const auto& p = m_triangles[i].pos; return Math::Point3{ p.x, p.y, p.z }; };
        double best = INFINITY;
        std::optional<Bim::Id> hit;
        size_t owner = 0;
        for (size_t i = 0; i + 2 < m_triangles.size(); i += 3)
        {
            while (owner + 1 < m_owners.size() && m_owners[owner + 1].first <= i)
                ++owner;
            const Math::Point3 a = at(i), b = at(i + 1), c = at(i + 2);
            const Math::Vec3 e1 = b - a, e2 = c - a, pv = cross(dir, e2);
            const double det = dot(e1, pv);
            if (std::abs(det) < 1e-12)
                continue;
            const Math::Vec3 tv = origin - a;
            const double u = dot(tv, pv) / det;
            if (u < 0.0 || u > 1.0)
                continue;
            const Math::Vec3 qv = cross(tv, e1);
            const double v = dot(dir, qv) / det;
            if (v < 0.0 || u + v > 1.0)
                continue;
            const double t = dot(e2, qv) / det;
            if (t > 0.0 && t < best && owner < m_owners.size())
            {
                best = t;
                hit  = m_owners[owner].second;
            }
        }
        return hit;
    }

    namespace
    {
        // 点在剖面里的坐标：沿线离起点、离线多远（看的方向为正）、高度
        struct Local
        {
            double along, past, z;
        };

        // 凸多边形（各点的 Local）裁到 keep(p) >= 0 的一侧
        std::vector<Local> ClipPoly(const std::vector<Local>& poly, const std::function<double(const Local&)>& keep)
        {
            std::vector<Local> out;
            const size_t n = poly.size();
            for (size_t i = 0; i < n; ++i)
            {
                const Local& a = poly[i];
                const Local& b = poly[(i + 1) % n];
                const double ka = keep(a), kb = keep(b);
                if (ka >= 0.0)
                    out.push_back(a);
                if ((ka >= 0.0) != (kb >= 0.0))
                {
                    const double t = ka / (ka - kb);
                    out.push_back(Local{ a.along + (b.along - a.along) * t, a.past + (b.past - a.past) * t, a.z + (b.z - a.z) * t });
                }
            }
            return out;
        }

        // 一串首尾相接的线段连成环：每段的两端按 0.01 毫米认同一个点；连不回起点的丢掉
        std::vector<Ring> Chain(const std::vector<std::pair<P2, P2>>& segs)
        {
            const auto key = [](P2 p) { return std::pair{ std::llround(p.x * 100.0), std::llround(p.y * 100.0) }; };
            std::multimap<std::pair<long long, long long>, size_t> at;
            for (size_t i = 0; i < segs.size(); ++i)
            {
                at.emplace(key(segs[i].first), i);
                at.emplace(key(segs[i].second), i);
            }
            std::vector<bool> used(segs.size(), false);
            std::vector<Ring> rings;
            for (size_t s = 0; s < segs.size(); ++s)
            {
                if (used[s])
                    continue;
                used[s] = true;
                Ring ring{ segs[s].first, segs[s].second };
                const auto start = key(segs[s].first);
                bool closed = false;
                while (!closed)
                {
                    const auto here = key(ring.back());
                    if (here == start)
                    {
                        closed = true;
                        break;
                    }
                    std::optional<size_t> next;
                    const auto [lo, hi] = at.equal_range(here);
                    for (auto it = lo; it != hi && !next; ++it)
                        if (!used[it->second])
                            next = it->second;
                    if (!next)
                        break;
                    used[*next] = true;
                    ring.push_back(key(segs[*next].first) == here ? segs[*next].second : segs[*next].first);
                }
                ring.pop_back();  // 回到起点的那个点
                if (closed && ring.size() >= 3 && std::abs(Bim::Geom::Area(ring)) > 1.0)
                    rings.push_back(std::move(ring));
            }
            // 外圈逆时针、洞（套在奇数层里的）顺时针，一起按非零规则填时洞是空的
            for (size_t i = 0; i < rings.size(); ++i)
            {
                int depth = 0;
                for (size_t j = 0; j < rings.size(); ++j)
                    if (i != j && std::abs(Bim::Geom::Area(rings[j])) > std::abs(Bim::Geom::Area(rings[i])) &&
                        Bim::Geom::Contains(rings[j], rings[i].front()))
                        ++depth;
                const bool ccw = Bim::Geom::Area(rings[i]) > 0.0;
                if (ccw != (depth % 2 == 0))
                    std::reverse(rings[i].begin(), rings[i].end());
            }
            return rings;
        }
    }

    ModelScene::SectionCut ModelScene::Cut(P2 start, P2 end, std::optional<double> depth, std::optional<std::array<double, 2>> crop,
                                           const std::function<bool(Bim::Id)>& shows) const
    {
        SectionCut out;
        const P2 u = (end - start).Unit(), n = u.Perp();
        const double len = start.Dist(end);
        const auto local = [&](float x, float y, float z)
        {
            const P2 d = P2{ x, y } - start;
            return Local{ d.Dot(u), d.Dot(n), z };
        };
        const auto world = [&](const Local& l)
        {
            const P2 p = start + u * l.along + n * l.past;
            return Point3{ p.x, p.y, l.z };
        };
        const auto grow = [&](double along, double z)
        {
            if (!out.any)
            {
                out.first = out.last = along;
                out.lo = out.hi = z;
                out.any = true;
                return;
            }
            out.first = std::min(out.first, along);
            out.last  = std::max(out.last, along);
            out.lo    = std::min(out.lo, z);
            out.hi    = std::max(out.hi, z);
        };
        // 看到的：线后、深度以内；裁剪了的再裁到起点与终点之间、底与顶之间
        std::vector<std::function<double(const Local&)>> planes{ [](const Local& l) { return l.past; } };
        if (depth)
            planes.push_back([d = *depth](const Local& l) { return d - l.past; });
        if (crop)
        {
            planes.push_back([](const Local& l) { return l.along; });
            planes.push_back([len](const Local& l) { return len - l.along; });
            planes.push_back([b = (*crop)[0]](const Local& l) { return l.z - b; });
            planes.push_back([t = (*crop)[1]](const Local& l) { return t - l.z; });
        }
        const VertexColor white(1.0f, 1.0f, 1.0f, 1.0f);
        for (size_t o = 0; o < m_owners.size(); ++o)
        {
            const Bim::Id id = m_owners[o].second;
            if (!shows(id))
                continue;
            const size_t first = m_owners[o].first, last = o + 1 < m_owners.size() ? m_owners[o + 1].first : m_triangles.size();
            std::vector<std::pair<P2, P2>> segs;
            for (size_t i = first; i + 2 < last; i += 3)
            {
                std::array<Local, 3> t;
                for (size_t j = 0; j < 3; ++j)
                {
                    const auto& p = m_triangles[i + j].pos;
                    t[j] = local(p.x, p.y, p.z);
                }
                // 剖切面穿过的：两条边上的交点连成一段。落在面上的点当作在面后一点点，共面的三角形就不出线段
                std::array<double, 3> d{};
                for (size_t j = 0; j < 3; ++j)
                    d[j] = std::abs(t[j].past) < 1e-6 ? 1e-6 : t[j].past;
                std::vector<P2> hits;
                for (size_t j = 0; j < 3; ++j)
                {
                    size_t a = j, b = (j + 1) % 3;
                    if ((d[a] > 0.0) == (d[b] > 0.0))
                        continue;
                    // 两端按固定的顺序插值：相邻三角形的同一条边算出同一个点
                    if (std::tie(t[b].along, t[b].z, t[b].past) < std::tie(t[a].along, t[a].z, t[a].past))
                        std::swap(a, b);
                    const double s = d[a] / (d[a] - d[b]);
                    hits.push_back(P2{ t[a].along + (t[b].along - t[a].along) * s, t[a].z + (t[b].z - t[a].z) * s });
                }
                if (hits.size() == 2 && hits[0].Dist(hits[1]) > 1e-6)
                    segs.emplace_back(hits[0], hits[1]);
                // 看到的部分
                std::vector<Local> poly(t.begin(), t.end());
                for (const auto& keep : planes)
                {
                    poly = ClipPoly(poly, keep);
                    if (poly.size() < 3)
                        break;
                }
                if (poly.size() < 3)
                    continue;
                for (size_t k = 1; k + 1 < poly.size(); ++k)
                    for (const Local* l : { &poly[0], &poly[k], &poly[k + 1] })
                    {
                        const Point3 w = world(*l);
                        out.faces.push_back({ { static_cast<float>(w.x), static_cast<float>(w.y), static_cast<float>(w.z) }, white });
                        grow(l->along, l->z);
                    }
            }
            if (!segs.empty())
            {
                std::vector<Ring> rings = Chain(segs);
                // 裁剪了的：剖到的环也只留起点到终点、底到顶之间的
                if (crop)
                    for (Ring& r : rings)
                    {
                        r = Bim::Geom::Clip(r, [](P2 q) { return q.x; });
                        r = Bim::Geom::Clip(r, [len](P2 q) { return len - q.x; });
                        r = Bim::Geom::Clip(r, [b = (*crop)[0]](P2 q) { return q.y - b; });
                        r = Bim::Geom::Clip(r, [t = (*crop)[1]](P2 q) { return t - q.y; });
                    }
                std::erase_if(rings, [](const Ring& r) { return r.size() < 3; });
                for (const Ring& r : rings)
                    for (const P2& q : r)
                        grow(q.x, q.y);
                if (!rings.empty())
                    out.cuts.emplace_back(id, std::move(rings));
            }
        }
        // 看到的边，同样裁
        for (size_t o = 0; o < m_edgeOwners.size(); ++o)
        {
            if (!shows(m_edgeOwners[o].second))
                continue;
            const size_t first = m_edgeOwners[o].first, last = o + 1 < m_edgeOwners.size() ? m_edgeOwners[o + 1].first : m_edges.size();
            for (size_t i = first; i + 1 < last; i += 2)
            {
                const Point3& p = m_edges[i];
                const Point3& q = m_edges[i + 1];
                Local a = local(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
                Local b = local(static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z));
                bool keep = true;
                for (const auto& f : planes)
                {
                    const double ka = f(a), kb = f(b);
                    if (ka < 0.0 && kb < 0.0)
                    {
                        keep = false;
                        break;
                    }
                    const double s = ka / (ka - kb);
                    const Local x{ a.along + (b.along - a.along) * s, a.past + (b.past - a.past) * s, a.z + (b.z - a.z) * s };
                    if (ka < 0.0)
                        a = x;
                    else if (kb < 0.0)
                        b = x;
                }
                if (!keep)
                    continue;
                out.edges.push_back(world(a));
                out.edges.push_back(world(b));
            }
        }
        return out;
    }

    std::pair<size_t, size_t> ModelScene::RangeOf(Bim::Id id) const
    {
        for (size_t i = 0; i < m_owners.size(); ++i)
            if (m_owners[i].second == id)
                return { m_owners[i].first, i + 1 < m_owners.size() ? m_owners[i + 1].first : m_triangles.size() };
        return { 0, 0 };
    }
}
