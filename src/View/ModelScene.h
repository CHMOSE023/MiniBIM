// 移植自 BIMCraft crates/core/src/view3d.rs（f3bbb33）的实体生成部分，MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Core/GeomKernel/AABB.hpp"
#include "Core/Math/Point3.hpp"
#include "Render/VertexTypes.hpp"
#include <string>
#include <vector>

namespace MiniBIM
{
    namespace Solid { class ISolidKernel; }
    namespace Bim { struct Model; }

    // 模型的三维场景：每个元素经 ISolidKernel（SMLib）建成实体，再离散成三角形与边线，交给 IRenderer 画。
    // BIMCraft 自己拼三角形、用软件光栅化画三维视图，这里只照它决定画什么（墙扣掉门窗洞、楼板与天花板扣掉洞口、
    // 屋顶每个坡面一块板、没有族的门扇和玻璃是墙中间 50 毫米厚的板，构件与带族的门窗是族的形体，按材质上色）和用什么颜色。
    // 族的形体本来就是三角形，直接画；房间与房间分隔线不画
    class ModelScene
    {
    public:
        struct Counts
        {
            int walls   = 0;
            int floors  = 0;
            int doors   = 0;
            int windows = 0;
            int ceilings = 0;
            int roofs   = 0;
            int components = 0;
        };

        // kernel 为空（构建时关掉了 SMLib）时返回 false；个别元素建模失败时其余照样显示，也返回 false。
        // 原因放在 error（英文原文，显示前 Tr；元素的失败另记日志）
        bool Build(const Bim::Model& model, Solid::ISolidKernel* kernel, std::string& error);

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
