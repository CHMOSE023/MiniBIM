// 移植自 BIMCraft crates/core/src/cloud.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Json.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 点云：激光扫描带进来的点。文件的点照公开的规范读（PTS 与 XYZ 文本、PLY、ASPRS LAS 1.0 到 1.4、ASTM E57），换成毫米，
// 抽稀到项目能放的，绕它们的中心存成 float（在 Clouds::MaxSpan 之内精确到毫米）。项目放每次扫描一份（Cloud），元素 PointCloud 放它
namespace MiniBIM::Bim
{
    namespace Clouds
    {
        // 一个点云最多留几个点：多的抽稀到这么多
        inline constexpr size_t MaxPoints = 5'000'000;
        // 文件最大多少字节
        inline constexpr size_t MaxBytes = size_t(2) << 30;
        // 文件的坐标离它自己的原点最远多少毫米：测量坐标可以很远，但不超过它
        inline constexpr double MaxFileCoord = 1e12;
        // 点云在各轴上最宽多少毫米：绕中心的 float 在这么远处还精确到毫米
        inline constexpr double MaxSpan = 2e7;
        // 项目最多放几次扫描
        inline constexpr size_t MaxClouds = 16;
        // 一次扫描最多从几个文件（测站）带进来
        inline constexpr size_t MaxStations = 256;
        // 没有颜色也没有强度的点的灰色：在别的点有颜色的文件里，以及视图里没有颜色的扫描
        inline constexpr uint8_t Grey = 160;
        // 视图最多画几个点（各扫描加起来），以及每个点画多少像素宽
        inline constexpr size_t Drawn = 2'000'000;
        inline constexpr int    Dot   = 2;
        // 一共 total 个点时每隔几个画一个，不超过 Drawn
        inline size_t Every(size_t total) { return total == 0 ? 1 : std::max<size_t>((total + Drawn - 1) / Drawn, 1); }

        using F3  = std::array<float, 3>;
        using D3  = std::array<double, 3>;
        using Rgb = std::array<uint8_t, 3>;

        // 点的文件是什么格式
        enum class Format
        {
            Pts,    // 文本：一行个数，再是 x y z，带或不带强度与颜色
            Xyz,    // 文本：x y z，带或不带颜色
            Ply,    // Polygon File Format：文本或二进制
            Las,    // ASPRS LAS 1.0 到 1.4，不压缩的
            E57,    // ASTM E57：文件里的每次扫描，按它的姿态放
        };
        // 扩展名说的格式；读不了时返回 false，原因放在 error
        bool FormatFrom(std::string_view ext, Format& out, std::string& error);
        // 单位的毫米数：mm、cm、m、in、ft
        std::optional<double> Unit(std::string_view name);

        // 怎么读一个文件
        struct Options
        {
            double scale   = 1000.0;      // 文件的单位是几毫米：米为 1000
            double spacing = 0.0;         // 每个这么宽（毫米）的立方体里留一个点；0 为全留
            size_t cap     = MaxPoints;   // 最多留几个点
        };

        // 从文件读出的点
        struct Scan
        {
            D3                              origin{};   // 点云自己的零点在文件坐标里的哪里，毫米：外框的中心
            std::vector<F3>                 points;     // 每个点离 origin 多远，毫米
            std::optional<std::vector<Rgb>> colours;    // 每个点的颜色，或强度当灰色；文件两样都没有时没有
            size_t                          read = 0;   // 抽稀前文件有几个点

            // 第 i 个点在文件坐标里，毫米
            std::optional<D3> Absolute(size_t i) const;
            bool operator==(const Scan&) const = default;
        };

        // 一次扫描是从哪个文件带进来的：它的名称，以及扫描的点里接着前面的测站有几个来自它
        struct Station
        {
            std::string source;
            size_t      points = 0;
            bool operator==(const Station&) const = default;
        };

        // 几个文件的扫描合成一个，绕它们全体的中心，每个文件一个测站。spacing 大于 0 时每个那么宽的立方体留一个点，
        // 测站重叠处不留两遍。有颜色的文件里，没有颜色的那个是灰色
        bool Merge(std::vector<std::pair<std::string, Scan>> scans, double spacing, Scan& out, std::vector<Station>& stations, std::string& error);

        // 格式为 format 的文件的点；读不了时返回 false，原因放在 error
        bool Read(std::string_view bytes, Format format, const Options& o, Scan& out, std::string& error);

        // 文件里的一个点：文件的单位，强度与颜色在文件自己的范围里
        struct Raw
        {
            D3                p{};
            std::optional<double> intensity;
            std::optional<D3>     rgb;
        };
        // 读到一个点时调它；返回 false 时停下，原因放在 error
        using Each = std::function<bool(const Raw&, std::string& error)>;

        namespace E57
        {
            // 读之前文件能要多少：一个位打包的字段可以一位都不占（常量），几兆字节就能说有几十亿个点，每个点每个字段都要算一步
            struct Caps
            {
                size_t   scans  = 4096;          // 文件里几次扫描
                uint64_t points = 100'000'000;   // 全部扫描的点
                size_t   fields = 32;            // 一个点由几个字段组成；扫描仪写十几个
                size_t   ahead  = size_t(4) << 20; // 一个字段的流等着别的流时最多攒多少字节；一个包最多 64 KiB
            };
            // E57 文件每次扫描的每个点交给 f，文件放它的地方，米
            bool Read(std::string_view bytes, const Each& f, std::string& error, const Caps& caps = Caps{});
        }

        // 测试用：Cloud::Nearest 在这个线程上看过几个点（测试按它断言，不按时间）
        size_t& Looked();
    }

    // 项目放的一次扫描：绕中心的点（毫米），文件有颜色时的颜色，以及从哪里来。复制很便宜：点是共用的
    class Cloud
    {
    public:
        Cloud() = default;

        // 从文件 source（人认它的名称）读出的扫描；放不下时返回 false，原因放在 error
        static bool Make(std::string_view source, Clouds::Scan scan, Cloud& out, std::string& error);
        // 从几个文件 stations 带进来的一次扫描；叫第一个的名称
        static bool OfStations(Clouds::Scan scan, std::vector<Clouds::Station> stations, Cloud& out, std::string& error);
        // 项目文件里存的样子（ToJson 的反向）
        static bool FromJson(const Json& j, Cloud& out, std::string& error);
        // 项目文件里存的样子：点是小端 float、颜色是字节，各自压过（zlib）再写成 base64。第一次存时算好
        const Json& ToJson() const;

        const std::string& Source() const { return m_source; }
        // 从几个文件带进来时，每个文件与它的点数
        const std::vector<Clouds::Station>& Stations() const { return m_stations; }
        // 它的中心在文件坐标里的哪里，毫米
        const Clouds::D3& Origin() const { return m_origin; }
        // 抽稀前文件有几个点
        size_t Read() const { return m_read; }
        size_t Size() const { return m_points ? m_points->size() : 0; }
        bool   Empty() const { return Size() == 0; }
        // 每个点离中心多远，毫米
        const std::vector<Clouds::F3>& Points() const;
        const std::vector<Clouds::Rgb>* Colours() const { return m_colours.get(); }
        // 点外面的框，离中心，毫米
        std::pair<Clouds::D3, Clouds::D3> Bounds() const;
        // 离中心的高度在 z 之间的点里，从上往下看离 at（离中心，毫米）最近、不超过 reach 的：平面捕捉的。只看 reach 碰到的格子
        std::optional<Clouds::F3> Nearest(std::array<double, 2> at, double reach, std::pair<double, double> z) const;

        bool operator==(const Cloud& o) const;

    private:
        static bool Checked(std::string source, std::vector<Clouds::Station> stations, Clouds::D3 origin, size_t read, std::vector<Clouds::F3> points,
                            std::optional<std::vector<Clouds::Rgb>> colours, Cloud& out, std::string& error);

        struct Pack;
        struct Grid;

        std::string                                    m_source;
        std::vector<Clouds::Station>                   m_stations;
        Clouds::D3                                     m_origin{};
        size_t                                         m_read = 0;
        std::shared_ptr<const std::vector<Clouds::F3>>  m_points;
        std::shared_ptr<const std::vector<Clouds::Rgb>> m_colours;
        Clouds::F3                                     m_lo{}, m_hi{};
        std::shared_ptr<Pack>                          m_packed;
        std::shared_ptr<Grid>                          m_grid;
    };
}
