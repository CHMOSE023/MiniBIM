// 移植自 BIMCraft crates/rvt/src/import.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
//
// 导入各类元素（Rvt/Walls.h、Rvt/Floors.h……）共用的：读的过程中拿着的东西（模型、略去了什么、每个元素在文件里是什么）、
// 第一遍找到的事实，以及英尺换成毫米。只给读取器自己用，公开的入口在 Rvt/Import.h
#pragma once

#include "Model/Model.h"
#include "Rvt/Inventory.h"
#include "Rvt/Records.h"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace MiniBIM::Rvt
{
    // OST_Materials
    inline constexpr int64_t OstMaterials = -2'000'700;

    // 英尺到毫米，到微米
    double Mm(double feet);
    // 毫米的长度，到微米
    double Micron(double v);
    // 英尺的点，换成模型的毫米
    Bim::P2 Point(double x, double y);
    // text 截到名称能有的长度
    std::string Clip(std::string_view text);
    // base，或 taken 里还没有的 base (2)、base (3)……；然后 taken 里就有了
    std::string Fresh(std::set<std::string>& taken, std::string_view base);

    // map 里与 same 说的一样的类型：叫 base 或 base (2)、base (3)……；没有时以 made 新建
    template <class T, class Same>
    std::string KindOf(std::map<std::string, T>& map, std::string_view base, const Same& same, T made)
    {
        const std::string clipped = Clip(base);
        for (size_t n = 1;; ++n)
        {
            const std::string name = n == 1 ? clipped : clipped + " (" + std::to_string(n) + ")";
            const auto it = map.find(name);
            if (it == map.end())
            {
                map.emplace(name, std::move(made));
                return name;
            }
            if (same(it->second))
                return name;
        }
    }

    // 导入读的过程中拿着的：模型、略去了什么、每个元素在文件里是什么
    struct Importing
    {
        Bim::Model               model;
        std::vector<std::string> skipped;
        // 建出的每个元素在文件里是什么（Wall 423099）：说明它为什么被去掉时用
        std::map<Bim::Id, std::string> from;
        // 每个 Revit 标高在模型里的标高
        std::map<uint32_t, Bim::Guid> levels;
        // 建出的每面 Revit 墙在模型里的墙，以及因没有层（幕墙）而略去的墙：门窗的宿主
        std::map<uint32_t, Bim::Id> walls;
        std::set<uint32_t>          curtain;
        // 建出的每个 Revit 屋顶在模型里的屋顶：顶部跟着屋顶的墙指它
        std::map<uint32_t, Bim::Id> roofs;

        // Revit id 为 id 的一个 kind（wall）因 reason 略去
        void LeftOut(std::string_view kind, uint32_t id, std::string_view reason);
        // 关于建出来的一个 kind（wall）要说的
        void Note(std::string_view kind, uint32_t id, std::string_view text);
        // 模型的元素 id 建出后又因 why 去掉：作为它那类的 Revit 元素略去（Wall 423099 建过，所以是 wall、423099）；
        // 标高有名字没有 id，单独一条
        void Dropped(Bim::Id id, std::string_view why);
        // 略去的与要说的，每类每个原因一行
        void Tell();

    private:
        // 略去的或要说的，按（是否略去、类、原因）：涉及的 id
        std::map<std::tuple<bool, std::string, std::string>, std::vector<uint32_t>> m_groups;
    };

    // 第一遍找到的，按各类元素的读法整理好
    struct Facts
    {
        uint32_t              release = 0;
        Tags                  tags;
        std::vector<uint32_t> ids;
        // 要的类别的每个放置的元素的当前副本
        std::map<uint32_t, Record> current;
        // 每个类别的类型的 id
        std::map<int64_t, std::set<uint32_t>> types;
        // 材质的 id
        std::set<uint32_t> materials;
        // 每个类别的族符号（构件、门窗的类型）的 id：有包围盒但不是放置的
        std::map<int64_t, std::set<uint32_t>> symbols;
        // 每个族符号的包围盒（在族自己的坐标里），按 id，取最新的记录
        std::map<uint32_t, std::array<double, 6>> symbolBoxes;
        // 每个项目标高的高程（英尺），按 Revit id
        std::map<uint32_t, double> elevations;

        // 一个类别的当前记录，按 id
        std::vector<const Record*> OfCategory(int64_t category) const;
    };
}
