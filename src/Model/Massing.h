// 移植自 BIMCraft crates/core/src/massing.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once

// 由体量做出的元素记着它：存在 params 的保留名下（同 BIMCraft 存阶段与涂色的做法）。
// mass_floors 做的楼板在 FloorMark 下存 {mass, level}，再跑一次时更新它而不是再做一块；
// 按面做的墙、楼板、屋顶在 Face 下存 {mass, face}（墙为边的编号，楼板、屋顶为 "top"），update_by_face 照体量现在的样子重建它。
// 剖面切体量的 section_lines 随剖面移植
#include "Model/Model.h"
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace MiniBIM::Bim::Massing
{
    inline constexpr const char* FloorMark = "Mass Floor";
    inline constexpr const char* Face      = "Mass Face";

    // 平面上离 p 最近（reach 毫米以内）的体量的面：哪个体量、底面轮廓从第一点起第几条边，同 wall_by_face 的写法（拾取面用）
    std::optional<std::pair<Id, size_t>> FaceAt(const Model& m, P2 p, double reach);
    // name 是体量记号用的名称吗：项目参数、族参数都不能叫这个名字
    bool Reserved(std::string_view name);
    // params 里的记号指的体量与标高交给 f，f 可以换成别的
    void RefsMut(std::map<std::string, Json>& params, const std::function<void(Guid&)>& f);
    std::vector<Guid> Refs(const std::map<std::string, Json>& params);
    // 元素 e 在 key（保留名）下存的记号对不对：项目里的体量，以及它的标高或面
    bool CheckValue(const Model& m, const std::string& who, const std::string& key, const Json& v, const Element& e, std::string& error);
    // 按面做的元素记着的体量与面：面为边的编号，没有时为顶面
    std::optional<std::pair<Guid, std::optional<size_t>>> FaceOf(const Element& e);
    // mass_floors 由 mass 做的、还在记号所说的标高上的楼板，与那个标高
    std::vector<std::pair<Id, Guid>> FloorsOf(const Model& m, Guid mass);
    // 去掉体量的记号：复制出来的、拆开的是普通元素
    void Detach(std::map<std::string, Json>& params);
    // 要删掉 gone 里的体量（删之前调用，认体量要它还在）：指向它们的记号拿掉，由它们做的元素从此是普通的
    void Forget(Model& m, const std::set<Guid>& gone);
    // 在体量 ms 的面 face（墙为边的编号，没有时为顶面）上做的元素 e 照体量现在的样子重建：墙立在边上、和体量一样高，
    // 楼板、屋顶在顶面上；之后对它做的（类型、屋顶的坡度）不变。面没了时为 false，原因放在 why
    bool Follow(Element& e, const Mass& ms, Id mass, std::optional<size_t> face, std::string& why);
    // 原地镜像的体量轮廓反过来走：n 条边里的第 f 条现在是第 n - 2 - f 条（模 n），它的墙记着的面重新编号，跟着同一条边
    void Mirrored(Model& m, Id mass);
    // 放样的体量从底（0）到顶（1）的 t 处的轮廓：每个角点沿直线走向它在顶部的那一点
    Ring RingAt(const Ring& bottom, const Ring& top, double t);
    // 面 f 是竖直的吗：顶边正在底边上方，和它一样高的墙放得上
    bool Plumb(const Ring& bottom, const Ring& top, size_t f);
    // 放样体量的表面，三角形朝外：底面轮廓在 z0、顶部轮廓在 z1（都逆时针，点点对应），平的侧面两个三角形，扭的分成若干条，再加上下两个盖
    std::vector<Tri> Tris(const Ring& bottom, const Ring& top, double z0, double z1);
    // 在 bottom 与 top 之间放样有什么不对：点点对应、同向，从下到上轮廓的面积不缩没
    std::optional<std::string> LoftProblem(const Ring& bottom, const Ring& top);
}
