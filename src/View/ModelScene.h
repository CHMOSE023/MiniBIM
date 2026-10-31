// 移植自 BIMCraft crates/core/src/view3d.rs（f3bbb33）的实体生成部分，MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Core/GeomKernel/AABB.hpp"
#include "Core/Math/Point3.hpp"
#include "Model/Geom.h"
#include "Model/Ids.h"
#include "Render/VertexTypes.hpp"
#include <array>
#include <functional>
#include <optional>
#include <utility>
#include <string>
#include <vector>

namespace MiniBIM
{
    namespace Solid { class ISolidKernel; }
    namespace Bim { struct Model; }

    // 模型的三维场景：每个元素经 ISolidKernel（SMLib）建成实体，再离散成三角形与边线，交给 IRenderer 画。
    // BIMCraft 自己拼三角形、用软件光栅化画三维视图，这里只照它决定画什么（墙扣掉门窗洞、楼板与天花板扣掉洞口、
    // 屋顶每个坡面一块板、没有族的门扇和玻璃是墙中间 50 毫米厚的板，构件与带族的门窗是族的形体，按材质上色，体量是底面轮廓拉伸的实体、放样的是两个轮廓之间的三角形）和用什么颜色。
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
            int columns = 0;
            // 梁、基础与其他 run（管道、线管……）
            int runs    = 0;
            int masses  = 0;
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
        // 从 origin 沿 dir 的射线最先碰到的三角形是哪个元素的：三维视图里单击拾取（正交相机，射线都朝视线方向）
        std::optional<Bim::Id> Pick(const Math::Point3& origin, const Math::Vec3& dir) const;
        // 元素 id 的三角形在 GetTriangles() 里的顶点范围 [first, last)；没有时为空范围
        std::pair<size_t, size_t> RangeOf(Bim::Id id) const;

        // 剖面或立面看到的（移植自 BIMCraft view3d.rs 的 cuts 与 cut_in，做法不同：用场景的三角形算）
        struct SectionCut
        {
            // 剖到的：各元素的环（沿线离起点、向上的毫米），外圈逆时针、洞顺时针
            std::vector<std::pair<Bim::Id, std::vector<Bim::Ring>>> cuts;
            // 线后（深度以内）的三角形（白色）与边：画成消隐线
            std::vector<Vertex_P3_C4> faces;
            std::vector<Math::Point3> edges;
            // 剖到的与看到的，沿线与高度的范围；什么也没有时 any 为假
            double first = 0.0, last = 0.0, lo = 0.0, hi = 0.0;
            bool   any   = false;
        };
        // start → end 的竖直剖切面，看线的左边：depth 为看到线后多远（没给时全部）；crop（底、顶）给了时只留沿线起点到终点、
        // 高度在底与顶之间的。shows 认的元素才算
        SectionCut Cut(Bim::P2 start, Bim::P2 end, std::optional<double> depth, std::optional<std::array<double, 2>> crop,
                       const std::function<bool(Bim::Id)>& shows) const;

    private:
        std::vector<Vertex_P3_C4> m_triangles;
        std::vector<Math::Point3> m_edges;
        std::vector<std::pair<size_t, Bim::Id>> m_owners;   // 每个元素的三角形从 m_triangles 的哪个顶点起（按顺序，到下一个元素为止）
        std::vector<std::pair<size_t, Bim::Id>> m_edgeOwners;   // 同上，边从 m_edges 的哪个点起
        AABB                      m_bounds = AABB::Empty();
        Counts                    m_counts;
    };
}
