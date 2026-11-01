// 移植自 BIMCraft crates/core/src/ifc.rs、import.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Model.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// IFC：开放的 BIM 交换格式（ISO 16739），写成 ISO 10303-21 的 STEP 文件，各种 BIM 软件都读。
// 导出写 IFC4：标高是楼层，墙（相接处斜接的平面轮廓，直的或弯的）、楼板、天花板（覆盖层）、房间是拉伸体，
// 屋顶、带坡度的楼板、贴着屋顶的墙是平面围成的闭合壳；每个门窗填在开在墙上的洞口元素里，读的软件据此开洞；
// 复合类型的层写成材质层组。构件按族的类别写成对应的元素，各形体是带材质颜色的三角面集。
// 导入读 IFC2x3 或 IFC4（Revit、别的 BIM 软件与 BIMCraft 写的）：楼层成为标高，墙（直的或弯的）与其中的门窗、楼板（平的或带坡度）、
// 天花板、房间、轴网、屋顶（平的，或矩形上的坡屋顶）、家具与器具等构件成为 MiniBIM 的元素，各按几何量出：
// 拉伸轮廓、Brep 或三角网格都一样，所以文件怎么描述元素不要紧。被屋顶切顶的墙贴着那个屋顶。
// 柱、梁、基础与管线（run）按截面与轴线读出，系统随它们回来。
// 体量是对象类型为 `Mass` 的代理元素（同 Revit），拉伸体或放样的闭合三角面集。
// MiniBIM 还没有的元素（楼梯、坡道、栏杆、管井……）与违反规则的略去并列出原因。
// 元素的项目参数值在属性集 `Project Parameters` 里（导出与导入都认）；BIMCraft 的阶段与设计选项的属性集还没移植
namespace MiniBIM::Bim::Ifc
{
    // 房间分隔线的虚拟元素带的对象类型，导入按它认出来
    inline constexpr const char* Separation = "RoomSeparation";
    // run（梁、管道……）的高度在截面哪条线上、是哪个类别所在的属性集（它的系统是它归入的 IfcSystem）；
    // 导入也认 BIMCraft 写的同样的属性集 BIMCraft_Run
    inline constexpr const char* RunProperties = "MiniBIM_Run";
    // 放样的体量记它的底面轮廓与顶部轮廓怎样点点对应的属性集（形体是不说这个的闭合三角面集）：`Loft`，JSON `{bottom, top}`；
    // 导入也认 BIMCraft 写的 BIMCraft_Mass
    inline constexpr const char* MassProperties = "MiniBIM_Mass";
    // 元素的项目参数值所在的属性集（同 BIMCraft 的名字，两边互认）
    inline constexpr const char* ProjectParameters = "Project Parameters";

    // 128 位写成 IFC 的 GlobalId：22 位 64 进制数字，第一位放最高的 2 位。元素的 GUID 这样写就是它的 GlobalId，
    // 同 IfcOpenShell 的 guid.compress 写 UUID
    std::string Compress(Guid g);
    // GlobalId 的 128 位；不是 22 位这种数字、或第一位超过 3 的为空
    std::optional<Guid> Expand(std::string_view text);

    // 模型写成 IFC4 文件的内容
    std::string Export(const Model& m);

    // 从 IFC 文件读出的模型：文件的模式、略去了什么与原因、为了靠近原点平移了多远
    struct Imported
    {
        Model                    model;
        std::string              schema;
        std::vector<std::string> skipped;
        std::optional<P2>        moved;
    };

    // 读 IFC 文件的内容成为模型，以项目的名称（没有时为 name）命名；读不了时原因放在 error
    bool Import(std::string_view bytes, std::string_view name, Imported& out, std::string& error);
}
