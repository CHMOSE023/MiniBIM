// 移植自 BIMCraft crates/core/src/cad.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Dxf.h"
#include <memory>
#include <set>

// 带进项目的 CAD 图纸：导入 CAD 与链接 CAD。项目每张图纸只存一份，即 Cad：读它的 DXF 原样存着（项目文件里压过、写成 base64），
// 打开项目时照现在的读法再读；以及 Dxf 读到的图纸。CadImport 元素把一张图纸放在一个标高上（这个标高的每个平面，或只一个视图）。
// 链接的记着它的文件，reload_cad（与打开项目）再读它
namespace MiniBIM::Bim
{
    struct CadImport;

    namespace Cads
    {
        // 一个项目最多几张图纸；它们一共最多多少字节的 DXF
        inline constexpr size_t MaxCads = 64;
        inline constexpr size_t MaxHeld = size_t(512) << 20;
        // 一个导入最多隐藏几个图层；毫米到图纸单位最小、最大
        inline constexpr size_t MaxHidden = 10000;
        inline constexpr double MinScale  = 1e-6;
        inline constexpr double MaxScale  = 1e7;
    }

    // 导入的颜色怎么画：照图纸的、反过来（暗底上的亮色变成亮底上的暗色），或都用视图的墨色
    enum class CadColors
    {
        Preserve,
        Invert,
        Black,
    };
    std::optional<CadColors> CadColorsFrom(std::string_view name);
    const char* Name(CadColors c);

    // 项目存的一张图纸。复制很便宜：图纸是共用的
    class Cad
    {
    public:
        // 一个 DXF 文件的 bytes 读到的图纸，叫 source，给了 path 时链接到它；存不了时返回 false，原因放在 error
        static bool Make(std::string_view source, std::optional<std::string> path, std::string bytes, Cad& out, std::string& error);
        // 项目文件存的样子（packed：Zlib::Pack 的 DXF）读回来，最多解开 most 字节
        static bool FromFile(std::string_view source, std::optional<std::string> path, const std::string& packed, size_t most, Cad& out,
                             std::string& error);

        const std::string& Source() const { return m_source; }
        // 链接到的文件；不是链接的为空
        const std::optional<std::string>& Path() const { return m_path; }
        // 同一张图纸，链接到 path 或不链接
        Cad Linked(std::optional<std::string> path) const;
        const Dxf::Drawing& Drawing() const { return *m_dxf; }
        // 它存的 DXF 有多少字节
        size_t Size() const { return m_bytes ? m_bytes->size() : 0; }
        bool   SameBytes(std::string_view bytes) const { return m_bytes && *m_bytes == bytes; }
        // 图纸主体外面的框，图纸单位：离其余的远的图元不算。居中与让视图看全都照它
        std::optional<std::array<P2, 2>> Bulk() const { return m_bulk; }
        // 几个图元离其余的远：让图纸范围变得很大的零星线
        size_t Far() const { return m_far; }
        // 项目文件存的 DXF：Zlib::Pack，第一次存时算一次
        const std::string& Packed() const;

        // colors 说怎样画时图元的颜色；用视图的墨色时为空：随层的取图层的，块外随块的、7 号是墨色
        std::optional<Dxf::Rgb> ColorOf(const Dxf::Entity& e, CadColors colors) const;
        // 图元的线宽，毫米：它自己的，否则图层的；都没有时为空（最细的笔）
        std::optional<double> WeightOf(const Dxf::Entity& e) const;
        // 图元的线型（它自己的，否则图层的）有划时，它的划与空，图纸单位；乘过 ltscale
        std::optional<std::array<double, 2>> Dash(const Dxf::Entity& e) const;
        // hidden 这些图层隐藏时图元画不画
        bool Shows(const Dxf::Entity& e, const std::set<std::string>& hidden) const { return !hidden.count(e.layer); }

        bool operator==(const Cad& o) const;

    private:
        std::string                               m_source;
        std::optional<std::string>                m_path;
        std::shared_ptr<const std::string>        m_bytes;
        std::shared_ptr<const Dxf::Drawing>       m_dxf;
        std::optional<std::array<P2, 2>>          m_bulk;
        size_t                                    m_far = 0;
        struct Pack;
        std::shared_ptr<Pack>                     m_packed;
    };

    namespace Cads
    {
        // 形状画的线，图纸单位：每段点，以及是否闭合。圆与圆弧是上面的点；点与文字不画线
        std::vector<std::pair<std::vector<P2>, bool>> Paths(const Dxf::Shape& s);
        // 形状来自哪种图元，照 cad_query 说的
        const char* Kind(const Dxf::Shape& s);
        // 放好的图纸在平面上离 at 最近、在 reach 毫米以内的显示的图元，以及多远（毫米）
        std::optional<std::pair<const Dxf::Entity*, double>> Nearest(const Cad& cad, const CadImport& ci, P2 at, double reach);
    }
}
