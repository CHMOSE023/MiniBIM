// 移植自 BIMCraft crates/rvt/src/project_fixture.rs（f3bbb33），Apache-2.0 许可（不是 MIT），已改写为 C++，见 src/Rvt/NOTICE
//
// 测试用的合成项目：带 BasicFileInfo、类表、元素表与一个或几个分区的容器，装着测试说的元素与标高，用读取器认得的两种记录形式
// （到 2023 版与 2024 起）写。Autodesk 的文件从不放进仓库；这里的字节照 Rvt/Records.h 与 Rvt/Inventory.h 里的布局，
// 那些布局在真文件上核对过
#pragma once

#include "Fixture.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace ProjectFixture
{
    using Fixture::Bytes;

    // 样例的类表给各个类的标签（12 是类表的第一个标签）
    inline constexpr uint16_t Outline       = 12;
    inline constexpr uint16_t Parents       = 13;
    inline constexpr uint16_t Header        = 14;
    inline constexpr uint16_t Plane         = 15;
    inline constexpr uint16_t CellList      = 16;
    inline constexpr uint16_t Regions       = 17;
    inline constexpr uint16_t Material      = 18;
    inline constexpr uint16_t Physical      = 19;
    inline constexpr uint16_t Pattern       = 20;
    inline constexpr uint16_t ProfileRoof   = 21;
    inline constexpr uint16_t ExtrusionRoof = 22;   // Roof：拉伸屋顶的类
    // 样例的类型数据里框住名字的标签（不是 ffff 的哪个都行）
    inline constexpr uint16_t NameTag = 0x1002;

    inline constexpr uint32_t KindType     = 0xffff'8080;
    inline constexpr uint32_t KindMaterial = 0xffff'8020;

    using Box = std::array<double, 6>;

    // 一条记录：category 类别的一个元素，是不是放置的实例（否则是类型的族符号）
    struct Rec
    {
        uint32_t                id       = 0;
        int64_t                 category = 0;
        bool                    placed   = true;
        uint16_t                classTag = 100;
        std::optional<uint32_t> inside;            // 在另一条记录的身体里、由那条记录给它 id 的记录（第二序言），2024 起才有
        bool                    declared = true;   // 为 false 时它的 id 不在元素表里
        Box                     bbox{ 0, 0, 0, 1, 1, 1 };   // 模型的包围盒（英尺）
        std::vector<uint64_t>   refs;              // 包围盒之后的两个引用列表
        std::vector<uint64_t>   refs2;

        Rec(uint32_t id, int64_t category) : id(id), category(category) {}
        // 换成这个包围盒与这些引用
        Rec At(const Box& box, std::vector<uint64_t> list) const
        {
            Rec r = *this;
            r.bbox = box;
            r.refs = std::move(list);
            return r;
        }
    };

    // 没有包围盒的记录：类型（种类 0xffff8080）或材质（0xffff8020）
    struct TypeRec
    {
        uint32_t id       = 0;
        int64_t  category = 0;
        uint32_t kind     = KindType;
        bool     inside   = false;   // 2024 起：类型的框架没有自己的 id，在一条它的 id 的链记录身体里（第二序言）

        TypeRec(uint32_t id, int64_t category) : id(id), category(category) {}
    };

    // 元素的序列化数据：头、它的 id 与这些字节
    struct Block
    {
        uint32_t id = 0;
        Bytes    payload;
    };

    // 族或族类型的名字条目，带它的类别
    struct NameEntry
    {
        uint32_t    id = 0;
        std::string name;
        int64_t     category = 0;
    };

    // 给自己起名字的材质对象
    struct Mat
    {
        uint32_t    id = 0;
        std::string name;
    };

    struct Lev
    {
        uint32_t                id = 0;
        std::string             name;
        double                  feet      = 0;
        bool                    placed    = true;
        bool                    named     = true;   // 为 false 时不写它的名字块
        bool                    confirmed = true;   // 为 false 时不写确认高程的第二份
        std::optional<bool>     story;              // 块里的"建筑楼层"设置；为空时不写那个框
        std::optional<uint32_t> owner;              // 在别的元素里的标高，它的块写着那个元素（2024 起才有）

        Lev(uint32_t id, std::string name, double feet) : id(id), name(std::move(name)), feet(feet) {}
    };

    // 材质对象把名字放在哪
    enum class NameStyle
    {
        Own,             // 类标签之后：两个为 0 的字、计数与文字
        PatternFramed,   // 由 `ff ff ff ff` 与 PatternHelper 的标签框住（2024 起）
        ParamEntry,      // 在内置参数 -1001203 与四个 0 字节之后
        Closed,          // 以八个 0 字节、一个 ElementId 与八个 ff 收尾（2024 起）
    };

    struct Project
    {
        uint32_t                                                release = 2024;
        std::vector<Rec>                                        recs;
        std::vector<Lev>                                        levels;
        std::vector<TypeRec>                                    types;
        std::vector<Block>                                      data;
        std::vector<Mat>                                        mats;
        std::vector<NameEntry>                                  names;
        std::vector<std::tuple<uint32_t, std::string, std::string>> roomNames;   // 房间的对象：id、编号与名称
        std::vector<std::tuple<uint32_t, Box, Box>>             nodes;            // 柱的几何节点：id 与它前面 96、48 字节处的两个包围盒（英尺）
        std::vector<uint32_t>                                   extraIds;         // 元素表在上面这些之外再声明的 id
        size_t                                                  members = 1;      // 分区的字节切成几段 gzip（一条记录可以跨两段）
        std::string                                             savedBy;          // 文件记的上次保存的人（Username）
        NameStyle                                               nameStyle = NameStyle::Own;
        std::vector<std::pair<int64_t, uint32_t>>               categoryMaterials;   // 对象样式说的类别的材质（类别与材质的 id）：各写一次

        explicit Project(uint32_t release) : release(release) {}

        bool Wide() const { return release >= 2024; }
        // 分区解开后的字节
        Bytes PartitionBytes() const;
        // 元素表
        Bytes ElementTable() const;
        static Bytes SchemaBytes();
        // 当作 Revit 文件的容器
        Bytes File() const;
        // 分区是这些字节（切成项目说的段数）的容器
        Bytes FileFrom(const Bytes& all) const;
    };

    // 有界直线的记录，像墙的数据存它的定位线：起点与终点参数、原点与方向
    Bytes LineBytes(double start, double end, std::array<double, 3> origin, std::array<double, 3> direction);
    // 圆弧的记录：标记字 1，然后是角度、两轴、半径与圆心
    Bytes ArcBytes(std::array<double, 2> angles, std::array<double, 3> x, std::array<double, 3> y, double radius, std::array<double, 3> centre);
    // 墙的朝向：锚点、定位线、那个字与翻转和它的第二个标志
    Bytes OrientationBytes(uint32_t locationLine, uint32_t word, bool flip, bool wide);
    // 框住的名字：`ff ff ff ff`、一个标签、带计数的 UTF-16 字符串
    Bytes NameBytes(const std::string& name);
    // 类型的层，由 `ff ff ff ff` 与 VerticalRegionsStructure 的标签框住：（宽度英尺、材质 id 或没有、功能）。版本决定记录的布局
    Bytes LayersBytes(const std::vector<std::tuple<double, std::optional<uint32_t>, uint32_t>>& layers, uint32_t release);
    // 族实例的放置：绕竖轴转 angle 弧度的旋转（按行）与原点（英尺）
    Bytes TransformBytes(double angle, std::array<double, 3> origin);

    // UTF-8 文字的 UTF-16LE 字节与码元数
    Bytes Utf16(const std::string& utf8);
    uint32_t Units(const std::string& utf8);
    // 小端的整数与浮点数
    Bytes Le(uint64_t v, size_t width);
    Bytes F64(double v);
}
