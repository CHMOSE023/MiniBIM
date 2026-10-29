// 移植自 BIMCraft crates/core/src/language.rs 与 translations/project/{pt,es}.po（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Language.h"
#include "Model/Text.h"

namespace MiniBIM::Bim::LanguageOf
{
    namespace
    {
        struct Words
        {
            const char* english;
            const char* portuguese;
            const char* spanish;
        };

        // 第 2a 步用到的词（新项目的名称、标高、材质）。BIMCraft 的项目词典有几百条，移植图纸、明细表时再补
        constexpr Words kWords[] = {
            { "Project",        "Projeto",           "Proyecto" },
            { "Level",          "Nível",             "Nivel" },
            { "Brick",          "Tijolo",            "Ladrillo" },
            { "Insulation",     "Isolamento",        "Aislamiento" },
            { "Concrete block", "Bloco de concreto", "Bloque de hormigón" },
            { "Plaster",        "Reboco",            "Enlucido" },
            { "Concrete",       "Concreto",          "Hormigón" },
            { "Screed",         "Contrapiso",        "Recrecido" },
            { "Timber",         "Madeira",           "Madera" },
            { "Gypsum board",   "Placa de gesso",    "Placa de yeso" },
            { "Ceramic tile",   "Cerâmica",          "Baldosa cerámica" },
            { "Roof tiles",     "Telhas",            "Tejas" },
            { "Glass",          "Vidro",             "Vidrio" },
            { "Copper",         "Cobre",             "Cobre" },
        };
    }

    const char* Code(Language l)
    {
        switch (l)
        {
        case Language::English:    return "en";
        case Language::Portuguese: return "pt";
        case Language::Spanish:    return "es";
        }
        return "en";
    }

    const char* Label(Language l)
    {
        switch (l)
        {
        case Language::English:    return "English";
        case Language::Portuguese: return "Português (Brasil)";
        case Language::Spanish:    return "Español";
        }
        return "English";
    }

    std::optional<Language> FromCode(std::string_view code)
    {
        const std::string lower = Text::AsciiLower(Text::Trim(code));
        const std::string first = lower.substr(0, lower.find_first_of("-_."));
        if (first == "en" || first == "english")
            return Language::English;
        // "português"、"español" 的非 ASCII 字母 AsciiLower 不动，开头大写的照样认
        if (first == "pt" || first == "portuguese" || first == "português" || first == "portuguÊs")
            return Language::Portuguese;
        if (first == "es" || first == "spanish" || first == "español" || first == "espaÑol")
            return Language::Spanish;
        return std::nullopt;
    }

    std::string Word(Language l, std::string_view english)
    {
        if (l == Language::English)
            return std::string(english);
        for (const Words& w : kWords)
            if (english == w.english)
                return l == Language::Portuguese ? w.portuguese : w.spanish;
        return std::string(english);
    }
}
