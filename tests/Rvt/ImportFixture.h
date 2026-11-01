// 导入的测试共用的：合成项目里的墙、楼板与草图线，导入它们、看导入建出的元素与说明，以及受损文件的扫测。
// 移植自 BIMCraft crates/rvt/src/tests/mod.rs、walls.rs 与 floors.rs 的辅助函数（f3bbb33），Apache-2.0 许可（不是 MIT），
// 已改写为 C++，见 src/Rvt/NOTICE
#pragma once

#include "Check.h"
#include "ProjectFixture.h"

#include "Ifc/Ifc.h"
#include "Model/Model.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ImportFixture
{
    namespace Bim = MiniBIM::Bim;
    using Fixture::Bytes;
    using Imported = Bim::Ifc::Imported;
    using ProjectFixture::Box;
    using ProjectFixture::Project;

    inline constexpr int64_t Walls      = -2'000'011;
    inline constexpr int64_t Windows    = -2'000'014;
    inline constexpr int64_t Doors      = -2'000'023;
    inline constexpr int64_t Floors     = -2'000'032;
    inline constexpr int64_t Roofs      = -2'000'035;
    inline constexpr int64_t Ceilings   = -2'000'038;
    inline constexpr int64_t Sketch     = -2'000'045;
    inline constexpr int64_t Columns    = -2'000'100;
    inline constexpr int64_t Rooms      = -2'000'160;
    inline constexpr int64_t Materials  = -2'000'700;
    inline constexpr int64_t Structural = -2'001'330;

    inline std::string Of(uint32_t release) { return "Revit " + std::to_string(release); }

    // 合成项目的导入（以 house 为名）；读不了时记一项失败，结果为空
    Imported Import(const Project& p);
    // 文件的导入；读不了时为空，原因放在 error
    std::optional<Imported> TryImport(const Bytes& file, std::string* error = nullptr);
    // 导入，以及建出的每个元素在文件里是什么（Wall 100）
    std::pair<Imported, std::map<Bim::Id, std::string>> ImportWithOrigins(const Project& p);

    // 模型里 T 类的元素，按 ID
    template <class T>
    std::vector<std::pair<Bim::Id, const T*>> All(const Imported& i)
    {
        std::vector<std::pair<Bim::Id, const T*>> out;
        for (const auto& [id, e] : i.model.elements.Items())
            if (const T* x = e->As<T>())
                out.emplace_back(id, x);
        return out;
    }

    // 类型表里名为 name 的；没有时记一项失败，返回一个空的
    template <class Map>
    const typename Map::mapped_type& Get(const Map& types, const std::string& name)
    {
        static const typename Map::mapped_type none{};
        const auto it = types.find(name);
        if (it == types.end())
        {
            RvtTest::Check(false, "没有名为 " + name + " 的类型");
            return none;
        }
        return it->second;
    }

    // 名为 name 的标高的 GUID：元素的 level 存的
    Bim::Guid LevelNamed(const Imported& i, const std::string& name);
    // 标高的名称与高程，从低到高
    std::vector<std::pair<std::string, double>> LevelList(const Imported& i);
    // 第一条含 what 的说明；没有时为空
    std::string Said(const Imported& i, const std::string& what);
    // 有一条说明含全部 parts
    bool Says(const Imported& i, std::initializer_list<std::string> parts);
    // 全部说明，接在失败的测试名后面
    std::string Skipped(const Imported& i);
    // 模型过 Model::Check
    bool Checks(const Imported& i);
    inline bool Near(double a, double b, double tol) { return std::abs(a - b) < tol; }
    inline bool StartsWith(const std::string& s, const std::string& head) { return s.rfind(head, 0) == 0; }
    inline bool EndsWith(const std::string& s, const std::string& tail) { return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0; }

    // 两个标高（0 与 10 英尺）、两种材质、两层的墙类型（0.25 英尺的砖面层在 0.5 英尺的混凝土外），没有墙
    Project Base(uint32_t release);
    // 20 英尺长的墙，从 (0, 0) 到 (20, 0)，10 英尺高，从标高 1 到标高 2，是 Base 的墙类型
    void Straight(Project& p, uint32_t id, bool flip);
    // 墙的记录与数据：line（直线或圆弧的记录）之后接着朝向，带包围盒 bbox 与引用 refs
    void WallOf(Project& p, uint32_t id, const Box& bbox, std::vector<uint64_t> refs, Bytes line);

    // Base 加上楼板类型（0.5 英尺的板：152.4 毫米）与天花板类型（0.25 英尺）
    Project FloorProject(uint32_t release);
    // owner 的草图线，从 a 到 b（英尺），高 z；data 时曲线在它的数据里
    void Line(Project& p, uint32_t owner, uint32_t id, std::pair<double, double> a, std::pair<double, double> b, double z, bool data);
    // 矩形 [x0, y0, x1, y1] 的草图：四条线，id 从 first 起
    void Rectangle(Project& p, uint32_t owner, uint32_t first, std::array<double, 4> r, double z, bool data);

    // 扫测的份数：同 BIMCraft 的 800。Debug 下每份导入要几十毫秒（新建模型的族库、同步库与 Model::Check，读文件本身只要一两毫秒），
    // 十几组扫测要几分钟，所以只做 100 份；CI 编 Release，做满
#ifdef NDEBUG
    inline constexpr size_t SweepRounds = 800;
#else
    inline constexpr size_t SweepRounds = 100;
#endif

    // 合成项目受损的 rounds 份（改一到八个字节，七份里一份截短）的导入：读得出的每份要过 Model::Check 与 check，
    // 不过时记一项失败（label 带上是第几份）。返回读得出与读不了的份数
    std::pair<size_t, size_t> Sweep(const Project& p, uint64_t seed, size_t rounds, const std::string& label,
                                    const std::function<bool(const Imported&)>& check);
}
