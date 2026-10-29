// 移植自 BIMCraft crates/core/src/view3d.rs（f3bbb33）的实体生成部分，MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "View/ModelScene.h"
#include "Core/Log.h"
#include "Core/Math/Mat4.hpp"
#include "Core/Math/MathUtils.hpp"
#include "I18n/Translator.h"
#include "Model/Model.h"
#include "Model/Walls.h"
#include "Solid/ISolidKernel.h"
#include <algorithm>
#include <cmath>

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
        m_bounds = AABB::Empty();
        m_counts = {};
        if (!kernel)
        {
            error = N_("This build has no solid modeling kernel (MINIBIM_WITH_SOLID is off).");
            return false;
        }
        Solid::ISolidKernel& k = *kernel;

        auto add = [&](const Solid::BodyPtr& body, const Rgb& c)
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
            for (const auto& line : k.Edges(body).Lines)
            {
                for (size_t i = 0; i + 1 < line.size(); ++i)
                {
                    m_edges.push_back(line[i]);
                    m_edges.push_back(line[i + 1]);
                }
            }
        };

        const Bim::Walls::Index index(m);
        int failed = 0;
        for (const auto& [id, ptr] : m.elements.Items())
        {
            const Bim::Element& e = *ptr;
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
                        const bool flat = p.bottom.g == P2{} && p.top.g == P2{};
                        const Solid::BodyPtr body = flat ? Prism(k, p.ring, p.bottom.z, p.top.z - p.bottom.z) : SlopedPrism(k, p);
                        if (body)
                            add(body, kWall);
                        else
                            ok = false;
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
}
