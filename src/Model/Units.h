// 移植自 BIMCraft crates/core/src/units.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// 项目单位：长度、面积怎么显示，输入的长度怎么读。模型和每条命令都用毫米，单位只影响图纸、明细表和界面的显示。
// 英尺英寸舍入到 1/16 英寸
namespace MiniBIM::Bim
{
    enum class Units
    {
        Millimetres,
        Centimetres,
        Metres,
        Inches,         // 分数英寸
        Feet,           // 小数英尺
        FeetInches,     // 英尺加分数英寸
    };

    namespace UnitsOf
    {
        inline constexpr std::array<Units, 6> All = { Units::Millimetres, Units::Centimetres, Units::Metres,
                                                      Units::Inches, Units::Feet, Units::FeetInches };

        // 命令与文件里的名称：mm、cm、m、in、ft、ft-in
        const char* Name(Units u);
        // 菜单上的名称（英文原文，显示前 Tr）
        const char* Label(Units u);
        std::optional<Units> FromName(std::string_view name);
        bool Imperial(Units u);

        // 图纸上显示的长度：3000、300、3.00、118 1/8"、9.84'、9'-10 1/8"
        std::string Length(Units u, double mm);
        // 带单位的长度，界面标注用：3000 mm、3.00 m、9'-10 1/8"
        std::string WithUnit(Units u, double mm);
        // 面积：平方米，英寸、英尺时为平方英尺；和单位
        std::pair<double, const char*> Area(Units u, double mm2);
        // 这种单位下视图可选的比例 1 : n
        std::span<const double> Scales(Units u);
        // 视图没有设比例时的默认值：1 : 100，或 1/8" = 1'-0"（1 : 96）
        double DefaultScale(Units u);
        // 比例 1 : n 的名称：1 : 50；英制为建筑比例 1/4" = 1'-0"、工程比例 1" = 20'-0"，其余 1 : n
        std::string Scale(Units u, double n);
        // 读输入的比例，得到 1 : n 的 n：100、1:100，或图纸长度 = 模型长度（1/8" = 1'-0"、1" = 20'、1 mm = 100 mm）
        std::optional<double> ParseScale(Units u, std::string_view text);
        // 读输入的长度，得到毫米：单独的数按这种单位（英尺英寸时为英尺）；带单位（mm、cm、m、ft 或 '、in 或 "）的按它的单位。
        // 英尺英寸可以一起写，英寸可以带分数：10' 6 1/2"、10'-6"、6 1/2"
        std::optional<double> Parse(Units u, std::string_view text);
    }
}
