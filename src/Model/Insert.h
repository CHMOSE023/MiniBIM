// 移植自 BIMCraft crates/core/src/insert.rs 与 link.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Model.h"
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

// 别的项目的元素放进这个项目（Revit 的"从文件插入"）与链接的模型（Revit 的"链接 Revit"）：
//   - Insert：要的元素，以及它们指着、这个项目没有的东西。这个项目没有用它的 ID 时元素保留 ID，否则取下一个；
//     保留 GUID，除非这个项目已有那个 GUID（同一个项目插进自己，或它的副本）：那时它是副本，新 GUID。
//     它们之间互相指着的跟着它们；指着这个项目有的（按 GUID，或同名的标高）就指这里的。
//     用到的库条目：这个项目按 GUID 有的就是它自己的（在这里叫什么就叫什么）；只按名称有的用这个项目的；没有的连 GUID 带进来
//   - Links：视图画的模型（Links::Seen）是这个项目加上每个读到的链接造的东西，ID 从 Linked 起；链接的标高与这个项目同高的就站在它上面，
//     别的作为链接自己的标高进来，名称前加链接的名称。链接的东西不属于项目：不能选、不进明细表、检查与 IFC
namespace MiniBIM::Bim
{
    // 一次插入做了什么
    struct Inserted
    {
        // 带进来的每个元素：在那个项目里的 ID → 这里的 ID
        std::map<Id, Id> ids;
        // 这里是副本（这个项目已有它们的 GUID）、有自己 GUID 的元素
        std::vector<Id> copies;
        // 带进来的类型、族、材质、参数……各带 GUID，写作"种类: 名称"
        std::vector<std::string> library;
        // 元素上略去了什么，为什么
        std::vector<std::string> leftOut;
    };

    // from 的元素 ids（空时全部），以及它们指着、to 没有的东西，放进 to。不检查：命令执行完由 Session 检查模型。
    // linked 时每个元素的 ID 从 Linked 起，视图靠这个认出它
    bool Insert(Model& to, const Model& from, const std::vector<Id>& ids, bool linked, Inserted& out, std::string& error);

    namespace Links
    {
        // 这两个标高的高度差在这以内（毫米），链接的标高就站在这个项目的标高上
        inline constexpr double SameLevel = 1.0;

        // id 是链接里的元素：视图画的模型（Seen）里才有，项目里从来没有
        inline bool IsLinked(Id id) { return id >= Linked; }

        // 平面上链接造的东西一起填色的拥有者：没有元素用它，拾取不到，随链接放在项目的下面
        inline constexpr Id Union = std::numeric_limits<Id>::max();

        // 视图画的模型：m 加上每个读到的链接造的东西；没有链接时就是 m。带不进来的链接略去
        std::shared_ptr<const Model> Seen(const std::shared_ptr<const Model>& m);
    }
}
