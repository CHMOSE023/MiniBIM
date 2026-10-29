#pragma once
#include "I18n/Catalog.h"
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// 多语言（说明见 docs/i18n.md）：
//   - 代码里给人看的文字一律写英文原文，经 Tr / TrC / TrN 取当前语言的译文；没有译文时显示英文
//   - 译文在 assets/i18n/<语言代码>.po（gettext 格式，可以用 Poedit 等工具编辑）；
//     tools/extract_messages.py 从源码和界面描述文件收集原文，更新 assets/i18n/minibim.pot 与各个 .po
//   - 收集工具只认字面量：Tr("…") 的参数写一个字符串字面量，不要拼接、不要传变量；
//     要先存起来、之后再翻译的原文（表格、命令名称）用 N_("…") 标记，用的时候再 Tr(变量)
//   - 句子里有数值或名称时用带编号的占位 Format(Tr("{0} walls on {1}"), …)：不同语言的语序不一样，不要把译文拆成几段拼接
namespace MiniBIM::I18n
{
    struct LanguageInfo
    {
        std::string code;       // 规范写法（NormalizeLanguage）："en"、"zh_CN"
        std::string name;       // 用这种语言写的名称："English"、"简体中文"
    };

    // 当前语言。只在界面线程使用（切换语言要重建界面，见 MainFrame::SetLanguage）
    class Translator
    {
    public:
        static Translator& Instance();

        // 词典所在的目录（UTF-8）：其中每个 <语言代码>.po 是一种语言
        void               SetCatalogDir(std::string utf8Dir) { m_dir = std::move(utf8Dir); }
        const std::string& GetCatalogDir() const { return m_dir; }

        // 能选的语言：英文（原文）加目录里的每个 .po，按代码排序
        std::vector<LanguageInfo> GetLanguages() const;

        // 选择语言：先找同名的词典（zh_CN），再找只写语种的（zh），再找同语种别的地区（pt → pt_BR），都没有时用英文。
        // 返回实际使用的语言代码；读词典的警告写进日志
        std::string        SetLanguage(std::string_view code);
        const std::string& GetLanguage() const { return m_language; }
        // 每次 SetLanguage 递增：界面据此判断要不要重建
        uint64_t           GetVersion() const { return m_version; }

        // 直接装入词典（测试用；language 为空时用词典文件头的 Language）
        void UseCatalog(Catalog catalog, std::string language = {});

        std::string Translate(std::string_view context, std::string_view english) const;
        std::string TranslatePlural(std::string_view context, std::string_view english, std::string_view englishPlural, long long n) const;

    private:
        std::string FindCatalogFile(const std::string& code) const;

        std::string m_dir;
        std::string m_language = "en";
        Catalog     m_catalog;
        uint64_t    m_version = 0;
    };

    // 把 {0}、{1}… 换成 args 里对应的值（"{{" 写出 "{"）；编号超出 args 时原样保留
    std::string Format(std::string_view pattern, std::initializer_list<std::string_view> args);
}

namespace MiniBIM
{
    // 英文原文 → 当前语言的译文
    inline std::string Tr(std::string_view english) { return I18n::Translator::Instance().Translate({}, english); }
    // 带上下文：同一个英文词在不同地方意思不同时（context 也写英文，如 "verb"、"wall layer"）
    inline std::string TrC(std::string_view context, std::string_view english) { return I18n::Translator::Instance().Translate(context, english); }
    // 复数：n 决定用哪一种形式（英文为 n == 1 时单数）；译文里的数值仍要用 Format 填进去
    inline std::string TrN(std::string_view english, std::string_view englishPlural, long long n)
    {
        return I18n::Translator::Instance().TranslatePlural({}, english, englishPlural, n);
    }
    // 只做标记，原样返回：让收集工具找到之后才翻译的原文
    constexpr const char* N_(const char* english) { return english; }

    using I18n::Format;
}
