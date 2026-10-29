#pragma once
#include "Core/GeomKernel/AABB.hpp"
#include "Core/Math/Point3.hpp"
#include "Render/VertexTypes.hpp"
#include <string>
#include <vector>

namespace MiniBIM
{
    namespace Solid { class ISolidKernel; }

    // 临时的演示场景：一圈带门窗洞口的墙、一块楼板、一根圆柱，全部经 ISolidKernel（SMLib）建模、离散化。
    // 只用来检验内核、三维视口与界面串得起来；BIMCraft 的模型（docs/PLAN.md 第 2 阶段）移植过来后删掉
    class DemoScene
    {
    public:
        struct Counts
        {
            int walls   = 0;
            int floors  = 0;
            int columns = 0;
        };

        // kernel 为空（构建时关掉了 SMLib）或建模失败时返回 false，原因（英文原文，显示前 Tr）放在 error
        bool Build(Solid::ISolidKernel* kernel, std::string& error);

        bool                                Empty() const { return m_triangles.empty(); }
        const AABB&                         GetBounds() const { return m_bounds; }
        const Counts&                       GetCounts() const { return m_counts; }
        // 已按固定的平行光着色的三角形（每 3 个顶点一个）
        const std::vector<Vertex_P3_C4>&    GetTriangles() const { return m_triangles; }
        // 实体的边，每两个点一段
        const std::vector<Math::Point3>&    GetEdges() const { return m_edges; }

    private:
        std::vector<Vertex_P3_C4> m_triangles;
        std::vector<Math::Point3> m_edges;
        AABB                      m_bounds = AABB::Empty();
        Counts                    m_counts;
    };
}
