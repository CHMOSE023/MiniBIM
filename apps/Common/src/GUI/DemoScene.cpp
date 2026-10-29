#include "GUI/DemoScene.h"
#include "Core/Math/MathUtils.hpp"
#include "I18n/Translator.h"
#include "Solid/ISolidKernel.h"
#include <algorithm>

namespace MiniBIM
{
    namespace
    {
        using Math::Point3;
        using Math::Vec3;

        struct Opening
        {
            Point3 origin;
            double dx, dy, dz;
        };

        // 固定的两盏平行光 + 环境光：不随视角变，环绕时各个面的明暗保持一致，分得清朝向
        float Shade(const Vec3& n)
        {
            static const Vec3 key  = Vec3{ -0.45, -0.6, 0.66 }.Normalized();
            static const Vec3 fill = Vec3{ 0.6, 0.35, 0.25 }.Normalized();
            const double d = 0.5 + 0.42 * std::max(0.0, Math::Dot(n, key)) + 0.16 * std::max(0.0, Math::Dot(n, fill));
            return static_cast<float>(std::min(d, 1.0));
        }
    }

    bool DemoScene::Build(Solid::ISolidKernel* kernel, std::string& error)
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

        auto add = [&](const Solid::BodyPtr& body, float r, float g, float b) -> bool
        {
            if (!body)
                return false;
            const Solid::Mesh mesh = kernel->Tessellate(body);
            for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
            {
                for (size_t k = 0; k < 3; ++k)
                {
                    const uint32_t idx = mesh.Indices[i + k];
                    const Point3& p = mesh.Positions[idx];
                    const float   s = Shade(mesh.Normals[idx]);
                    m_triangles.push_back({ { static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) },
                                            VertexColor(r * s, g * s, b * s, 1.0f) });
                    m_bounds.Expand(p);
                }
            }
            const Solid::Polylines edges = kernel->Edges(body);
            for (const auto& line : edges.Lines)
            {
                for (size_t i = 0; i + 1 < line.size(); ++i)
                {
                    m_edges.push_back(line[i]);
                    m_edges.push_back(line[i + 1]);
                }
            }
            return true;
        };

        // 墙：长方体减去门窗洞口（单位毫米，Z 向上；外墙轴网 9 m × 6 m，墙厚 200，层高 3 m）
        auto wall = [&](const Point3& origin, double dx, double dy, std::initializer_list<Opening> openings) -> bool
        {
            Solid::BodyPtr body = kernel->Box(origin, dx, dy, 3000.0);
            for (const Opening& o : openings)
            {
                if (!body)
                    break;
                body = kernel->Boolean(Solid::BooleanOp::Difference, body, kernel->Box(o.origin, o.dx, o.dy, o.dz));
            }
            if (!add(body, 0.88f, 0.86f, 0.82f))
                return false;
            ++m_counts.walls;
            return true;
        };

        const bool ok =
            wall({ 0, 0, 0 }, 9000, 200, { { { 1500, -10, 0 }, 1000, 220, 2200 }, { { 5000, -10, 900 }, 2400, 220, 1500 } }) &&
            wall({ 0, 5800, 0 }, 9000, 200, { { { 1500, 5790, 900 }, 1800, 220, 1500 }, { { 5700, 5790, 900 }, 1800, 220, 1500 } }) &&
            wall({ 0, 200, 0 }, 200, 5600, {}) &&
            wall({ 8800, 200, 0 }, 200, 5600, { { { 8790, 2200, 900 }, 220, 1600, 1500 } });
        if (ok && add(kernel->Box({ -300, -300, -250 }, 9600, 6600, 250), 0.72f, 0.74f, 0.77f))
            ++m_counts.floors;
        if (ok && add(kernel->Cylinder({ 4500, 3000, 0 }, 200, 3000), 0.83f, 0.66f, 0.48f))
            ++m_counts.columns;

        if (!ok || m_counts.floors == 0 || m_counts.columns == 0)
        {
            error = kernel->LastError().empty() ? std::string(N_("The solid modeling kernel failed to build the demo model.")) : kernel->LastError();
            return false;
        }
        return true;
    }
}
