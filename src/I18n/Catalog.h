#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace MiniBIM::I18n
{
    // 一种语言的词典，从 gettext 的 PO 文件读入（assets/i18n/<语言代码>.po，见 docs/i18n.md）。
    //   - 键为 (msgctxt, msgid)：msgid 是英文原文，msgctxt 区分同一个英文词的不同用法（"Open" 作动词 / 形容词）
    //   - 复数：msgid_plural + msgstr[0..n]，按文件头 Plural-Forms 的公式选哪一条
    //   - 不用的条目：标了 fuzzy 的、译文为空的、已废弃的（#~）——查不到时调用方显示英文原文
    // 解析不依赖系统的 gettext，网页版、Linux 版同样可用
    class Catalog
    {
    public:
        // 解析 PO 文本（UTF-8，可带 BOM）。格式不对的行跳过，原因记入 warnings（"第 12 行：…"）；
        // 至少读出文件头或一条译文时返回 true
        bool Parse(std::string_view text, std::vector<std::string>* warnings = nullptr);
        bool LoadFile(const std::string& utf8Path, std::vector<std::string>* warnings = nullptr);

        // 译文；没有时返回 nullptr
        const std::string* Find(std::string_view context, std::string_view english) const;
        // 复数的译文：按 n 与 Plural-Forms 选一条；没有时返回 nullptr
        const std::string* FindPlural(std::string_view context, std::string_view english, long long n) const;

        const std::string& GetLanguage() const     { return m_language; }       // 文件头 Language（"zh_CN"）
        const std::string& GetLanguageName() const { return m_languageName; }   // 文件头 X-Language-Name（"简体中文"），菜单里显示
        const std::string& GetPluralForms() const  { return m_pluralForms; }
        size_t Size() const { return m_messages.size(); }
        bool   Empty() const { return m_messages.empty(); }

    private:
        static std::string Key(std::string_view context, std::string_view english);
        void ParseHeader(const std::string& header);

        // 键：msgctxt + '\x04' + msgid（gettext 的约定；没有上下文时只有 msgid）
        std::unordered_map<std::string, std::vector<std::string>> m_messages;
        std::string m_language;
        std::string m_languageName;
        std::string m_pluralForms;      // "nplurals=2; plural=(n != 1);"
        std::string m_pluralExpr;       // 只取 plural= 后面的表达式
        int         m_pluralCount = 2;
    };

    // Plural-Forms 里 plural= 的表达式（C 语法的子集：n、整数、!、* / %、+ -、< <= > >=、== !=、&&、||、?:、括号）在 n 处的值。
    // 表达式写错时返回 -1
    long long EvaluatePluralExpression(std::string_view expr, long long n);

    // 语言代码的规范写法："zh-CN"、"zh_cn.UTF-8"、"zh-Hans-CN" → "zh_CN"；"zh-Hant" → "zh_TW"；"EN" → "en"
    std::string NormalizeLanguage(std::string_view code);
}
