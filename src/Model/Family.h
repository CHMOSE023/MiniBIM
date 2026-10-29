// 移植自 BIMCraft crates/core/src/family.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include "Model/Geom.h"
#include "Model/Ids.h"
#include "Model/Json.h"
#include "Model/Language.h"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// 族：MiniBIM 自己的参数化构件（不是 Autodesk 的 .rfa，也不读它）。族有参数、设定类型参数的类型，以及形体：
// 每个形体是画在草图平面上、扫过平面的轮廓，尺寸由参数定。构件放置族的一个类型。公式的写法同 Revit：`Width / 2`、
// `if(Depth > 600, 4, 2)`
namespace MiniBIM::Bim
{
    enum class Operation;

    namespace Fam
    {
        // 族是什么：IFC 怎样称呼它的构件；门窗族立在墙里，在墙上开出它的 Width × Height
        enum class Category
        {
            Furniture,
            Casework,
            Plumbing,       // 卫生器具：洗手盆、坐便器、洗涤盆、淋浴
            Drain,          // 地漏
            PipeFitting,    // 弯头、三通：管件有接口之前由构件代替
            PipeAccessory,  // 阀门、清扫口
            Equipment,      // 配电箱
            Outlet,         // 插座
            Switch,         // 开关
            AirTerminal,    // 接闪器
            Earthing,       // 接地极
            InspectionBox,  // 接地检查井
            Lighting,
            Generic,
            DataOutlet,     // 数据插座
            VoiceOutlet,    // 语音插座
            PatchPanel,     // 配线架
            Rack,           // 机柜
            Door,
            Window,
        };

        namespace CategoryOf
        {
            // 文件与命令里的写法：furniture、pipefitting……
            const char* Name(Category c);
            std::optional<Category> FromName(std::string_view name);
            // 立在墙里：门窗族，画在洞口自己的坐标里（x 沿墙从洞口中间起，y 横过墙朝墙的左边，z 从窗台往上）
            inline bool Hosted(Category c) { return c == Category::Door || c == Category::Window; }
        }

        // 门窗族的公式读到的墙：厚度，毫米
        inline constexpr const char* Wall = "Wall Thickness";
        // 没有墙时当作这么厚：200 毫米
        inline constexpr double DefaultWall = 200.0;
        // 在墙上开洞的参数：宽、高与窗台高度（英文原文，不随项目语言）
        inline constexpr const char* Width  = "Width";
        inline constexpr const char* Height = "Height";
        inline constexpr const char* Sill   = "Sill Height";

        // 参数存什么
        enum class Kind
        {
            Length,
            Number,
            Angle,
            YesNo,
            Material,
        };

        namespace KindOf
        {
            const char* Name(Kind k);
            std::optional<Kind> FromName(std::string_view name);
        }

        // 参数：类型的（每个类型自己的值），或 instance 时每个构件的
        struct Parameter
        {
            std::string name;
            Kind        kind     = Kind::Length;
            bool        instance = false;
            // 类型和构件都没设定时的值：数（是 / 否为 true、false），或材质名称
            Json        value;
            // 由其他参数算出，同 Revit 的公式
            std::optional<std::string> formula;
            bool operator==(const Parameter&) const = default;
        };

        // 一个数，或族参数上的公式
        using Expr = std::variant<double, std::string>;

        // 形体的轮廓画在哪里：族的 x、y 上，向上扫；或竖着的 x、z 上，沿 y 扫；或 y、z 上，沿 x 扫
        enum class SketchPlane
        {
            Xy,
            Xz,
            Yz,
        };

        // 圆形的轮廓
        struct Circle
        {
            std::array<Expr, 2> centre{ 0.0, 0.0 };
            Expr                radius = 0.0;
            bool operator==(const Circle&) const = default;
        };

        // 一段圆弧：圆心、半径，起止角度（从 +x 逆时针的度数）
        struct Curve
        {
            std::array<Expr, 2> centre{ 0.0, 0.0 };
            Expr                radius = 0.0;
            Expr                from   = 0.0;
            Expr                to     = 0.0;
            bool operator==(const Curve&) const = default;
        };

        // 门窗平面图画的线（开启线、窗台），在它自己的坐标里，别的视图不显示：过各点的 line，或 arc；
        // visible（是 / 否参数，或公式）决定有没有
        struct Symbol
        {
            // 与元素一样的 ID 和 GUID：建时给出，在列表里的位置不变就一直保留
            Id   id = 0;
            Guid guid;
            std::vector<std::array<Expr, 2>> line;
            std::optional<Curve> arc;
            std::optional<Expr>  visible;
            bool operator==(const Symbol&) const = default;
        };

        // 族的一个实体，在族自己的坐标里（x 沿构件的角度，y 在它左边，z 从它的底往上）：画在 plane 上的轮廓从 from 扫到 to；
        // 有 axis 时绕它从 from 转到 to 度；或文件带来的三角形。材质名称或材质参数名说它用什么做；visible 决定有没有。
        // void 不是实体：它切掉列在它前面、它穿过的实体形体，自己从不显示
        struct Form
        {
            Id   id = 0;
            Guid guid;
            SketchPlane plane = SketchPlane::Xy;
            std::vector<std::array<Expr, 2>> profile;
            std::optional<Circle> circle;
            Expr from = 0.0;
            Expr to   = 0.0;
            // 旋转体：轮廓绕草图平面里的这条线转，右手方向从轴的第一点指向第二点；from、to 是角度，最多一整圈
            std::optional<std::array<std::array<Expr, 2>, 2>> axis;
            bool        isVoid = false;
            std::string material;
            std::optional<Expr> visible;
            // 共享：撤销步骤复制模型时不复制它
            std::shared_ptr<const std::vector<Tri>> mesh;

            bool HasMesh() const { return mesh && !mesh->empty(); }
            bool operator==(const Form& o) const;
        };

        // 参数算出的值：数（是 / 否为 1 或 0），或材质名称
        using Val = std::variant<double, std::string>;
        using Values = std::map<std::string, Val>;

        // 形体在族坐标里的实体：三角形（端面的在前）；从上往下看的轮廓、从底到顶、用什么做；圆的侧面算一个曲面
        struct Piece
        {
            std::vector<Tri> tris;
            size_t           caps   = 0;
            Ring             outline;
            double           bottom = 0.0;
            double           top    = 0.0;
            std::string      material;
            bool             round  = false;
        };

        inline constexpr size_t MaxProfile    = 64;
        inline constexpr size_t MaxChain      = 16;
        inline constexpr size_t MaxBuilt      = 400000;
        inline constexpr size_t MaxTriangles  = 100000;
        inline constexpr size_t MaxParameters = 64;
        inline constexpr size_t MaxForms      = 256;
        inline constexpr size_t MaxSymbols    = 64;
        inline constexpr size_t MaxTypes      = 1000;
        inline constexpr size_t MaxFormula    = 1024;

        // formula 读到的名称（各个 if 分支的都算）；读不懂、调用了没有的函数或参数个数不对时返回 false
        bool Names(std::string_view formula, std::vector<std::string>& out, std::string& error);
        // 算出 formula，参数的值由 get 给出：数、毫米的长度、度的角度；比较成立为 1，否则为 0
        bool Eval(std::string_view formula, const std::function<bool(const std::string&, double&, std::string&)>& get, double& out,std::string& error);
        // 公式能说出的名称：字母或 `_` 开头，然后是字母、数字、`_` 和空格，不以空格结尾，最长 MaxText
        bool Nameable(std::string_view name);
        // 存的值按参数 p 的种类读出
        bool Read(const Parameter& p, const Json& v, Val& out, std::string& error);
    }

    // 族：参数、类型（各自不同于参数默认值的类型参数值）与形体
    struct Family
    {
        Fam::Category category = Fam::Category::Furniture;
        std::vector<Fam::Parameter> parameters;
        std::map<std::string, std::map<std::string, Json>> types;
        std::vector<Fam::Form> forms;
        // 门窗平面图除形体外画的线
        std::vector<Fam::Symbol> symbols;
        // 门窗扇怎么开，IFC 与明细表要；没有时门是平开
        std::optional<Operation> operation;

        // 它用到的每个材质名称交给 f，f 可以换成别的：材质参数的值、类型里它们的值、形体自己的材质
        void MaterialRefsMut(const std::function<void(std::string&)>& f);
        bool MaterialParameter(std::string_view name) const;
        // 它用到的材质（不含空的）
        std::vector<std::string> Materials() const;
        void RenameMaterial(const std::string& from, const std::string& to);

        // typeName 类型的参数值，加上构件的 instance 值，算出公式；门窗族在常用厚度的墙里
        bool Values(const std::string& typeName, const std::map<std::string, Json>& instance, Fam::Values& out, std::string& error) const;
        // 同 Values，门窗族在 wall 毫米厚的墙里：公式读到的 `Wall Thickness`
        bool ValuesIn(const std::string& typeName, const std::map<std::string, Json>& instance, double wall, Fam::Values& out, std::string& error) const;
        // 是否健全：参数名各不相同、存的值合乎种类；类型只设没有公式的类型参数；材质在库里；每个类型都建得出来
        bool Check(const std::function<bool(const std::string&)>& hasMaterial, std::string& error) const;
        // 可见形体按 values 定尺寸的实体，减去空心形体切掉的
        bool Pieces(const Fam::Values& values, std::vector<Fam::Piece>& out, std::string& error) const;
        // 可见符号按 values 定尺寸的线，在它自己的坐标里：圆弧为短弦组成的折线
        bool Strokes(const Fam::Values& values, std::vector<Ring>& out, std::string& error) const;

        bool operator==(const Family&) const = default;
    };

    // 新项目起步的族，MiniBIM 自己的（移植自 BIMCraft）：家具、卫生器具、门窗与各专业的器具，名称与公式用 language 的词
    std::map<std::string, Family> FamilyLibrary(Language language);
}
