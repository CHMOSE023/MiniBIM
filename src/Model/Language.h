// 移植自 BIMCraft crates/core/src/language.rs（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>

// 项目语言：图纸上印的字和命令给新建的东西起的名字（标高、视图……）。英语、巴西葡萄牙语或西班牙语。
// 已经起好的名字不变；核心的报错信息（给 AI 代理读的）一律英文。
// 与界面语言（I18n）无关：项目语言跟着项目文件走
namespace MiniBIM::Bim
{
    enum class Language
    {
        English,
        Portuguese,
        Spanish,
    };

    namespace LanguageOf
    {
        inline constexpr std::array<Language, 3> All = { Language::English, Language::Portuguese, Language::Spanish };

        // 命令与文件里的代码：en、pt、es
        const char* Code(Language l);
        // 用它自己的语言写的名称
        const char* Label(Language l);
        // 按代码或名称，或系统给的区域设置（pt-BR、es_419.UTF-8）找语言
        std::optional<Language> FromCode(std::string_view code);
        // 核心写的一个英文词或名称在这种语言里的写法；没有译文时原样返回
        std::string Word(Language l, std::string_view english);
    }
}
