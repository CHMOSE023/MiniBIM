// 多语言（src/I18n）：PO 解析、复数公式、语言代码规范化、按语言回退、Format，
// 以及仓库里 assets/i18n 下的每个词典都能无警告读出
#include "I18n/Translator.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace MiniBIM;
using namespace MiniBIM::I18n;

namespace
{
    int g_failed = 0;
    void Check(bool ok, const std::string& label)
    {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
        g_failed += ok ? 0 : 1;
    }

    const char* kChinese = R"PO(# 测试用的小词典
msgid ""
msgstr ""
"Language: zh_CN\n"
"X-Language-Name: 简体中文\n"
"Plural-Forms: nplurals=1; plural=0;\n"

msgid "Save"
msgstr "保存"

msgctxt "verb"
msgid "Open"
msgstr "打开"

msgctxt "state"
msgid "Open"
msgstr "开启"

#, fuzzy
msgid "Close"
msgstr "关"

msgid "Untranslated"
msgstr ""

msgid "{0} wall"
msgid_plural "{0} walls"
msgstr[0] "{0} 面墙"

msgid "Line one\n"
"line \"two\""
msgstr "第一行\n"
"第\"二\"行"

#~ msgid "Gone"
#~ msgstr "已删除"
)PO";

    // 俄语：三种复数形式
    const char* kRussian = R"PO(msgid ""
msgstr ""
"Language: ru\n"
"Plural-Forms: nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);\n"

msgid "{0} wall"
msgid_plural "{0} walls"
msgstr[0] "{0} стена"
msgstr[1] "{0} стены"
msgstr[2] "{0} стен"
)PO";

    void TestParse()
    {
        Catalog c;
        std::vector<std::string> warnings;
        Check(c.Parse(kChinese, &warnings), "解析成功");
        Check(warnings.empty(), "没有警告");
        Check(c.GetLanguage() == "zh_CN", "文件头 Language");
        Check(c.GetLanguageName() == "简体中文", "文件头 X-Language-Name");
        Check(c.Find("", "Save") && *c.Find("", "Save") == "保存", "普通条目");
        Check(c.Find("verb", "Open") && *c.Find("verb", "Open") == "打开", "上下文 verb");
        Check(c.Find("state", "Open") && *c.Find("state", "Open") == "开启", "上下文 state");
        Check(!c.Find("", "Open"), "没有上下文的 Open 查不到");
        Check(!c.Find("", "Close"), "fuzzy 条目不用");
        Check(!c.Find("", "Untranslated"), "空译文不用");
        Check(!c.Find("", "Gone"), "废弃条目不用");
        Check(c.Find("", "Line one\nline \"two\"") && *c.Find("", "Line one\nline \"two\"") == "第一行\n第\"二\"行", "续行与转义");
        Check(c.FindPlural("", "{0} wall", 5) && *c.FindPlural("", "{0} wall", 5) == "{0} 面墙", "nplurals=1 的复数");
    }

    void TestMalformed()
    {
        Catalog c;
        std::vector<std::string> warnings;
        c.Parse("msgid \"A\"\nmsgstr \"甲\n\nmsgid \"B\"\nmsgstr \"乙\"\nnonsense\n", &warnings);
        Check(warnings.size() == 2, "格式错误的行记入警告（" + std::to_string(warnings.size()) + " 条）");
        Check(c.Find("", "B") && *c.Find("", "B") == "乙", "错误之后的条目照常读出");
    }

    void TestPlural()
    {
        const char* ru = "(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2)";
        Check(EvaluatePluralExpression(ru, 1) == 0 && EvaluatePluralExpression(ru, 21) == 0, "俄语 1、21 → 0");
        Check(EvaluatePluralExpression(ru, 3) == 1 && EvaluatePluralExpression(ru, 24) == 1, "俄语 3、24 → 1");
        Check(EvaluatePluralExpression(ru, 11) == 2 && EvaluatePluralExpression(ru, 5) == 2 && EvaluatePluralExpression(ru, 0) == 2, "俄语 11、5、0 → 2");
        Check(EvaluatePluralExpression("n != 1", 1) == 0 && EvaluatePluralExpression("n != 1", 2) == 1, "英语");
        Check(EvaluatePluralExpression("n>1", 0) == 0 && EvaluatePluralExpression("n>1", 2) == 1, "法语");
        Check(EvaluatePluralExpression("n +", 1) == -1 && EvaluatePluralExpression("n % 0", 1) == -1, "写错的表达式返回 -1");

        Catalog c;
        c.Parse(kRussian);
        Check(c.FindPlural("", "{0} wall", 1) && *c.FindPlural("", "{0} wall", 1) == "{0} стена", "俄语词典 1");
        Check(c.FindPlural("", "{0} wall", 3) && *c.FindPlural("", "{0} wall", 3) == "{0} стены", "俄语词典 3");
        Check(c.FindPlural("", "{0} wall", 7) && *c.FindPlural("", "{0} wall", 7) == "{0} стен", "俄语词典 7");
    }

    void TestNormalize()
    {
        Check(NormalizeLanguage("zh-CN") == "zh_CN", "zh-CN");
        Check(NormalizeLanguage("zh_cn.UTF-8") == "zh_CN", "zh_cn.UTF-8");
        Check(NormalizeLanguage("zh-Hans-CN") == "zh_CN", "zh-Hans-CN");
        Check(NormalizeLanguage("zh-Hans") == "zh_CN", "zh-Hans");
        Check(NormalizeLanguage("zh-Hant") == "zh_TW", "zh-Hant");
        Check(NormalizeLanguage("EN-us") == "en_US", "EN-us");
        Check(NormalizeLanguage("pt") == "pt", "pt");
        Check(NormalizeLanguage("C") == "en", "C");
        Check(NormalizeLanguage("") == "", "空串");
    }

    void TestTranslatorFallback()
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "MiniBIM_I18nTests";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        auto write = [&](const char* name, const std::string& text)
        {
            std::ofstream(dir / name, std::ios::binary) << text;
        };
        write("zh_CN.po", kChinese);
        write("pt_BR.po", "msgid \"\"\nmsgstr \"\"\n\"Language: pt_BR\\n\"\n\"X-Language-Name: Português (Brasil)\\n\"\n\nmsgid \"Save\"\nmsgstr \"Salvar\"\n");

        Translator& t = Translator::Instance();
        const std::u8string u8 = dir.u8string();
        t.SetCatalogDir(std::string(u8.begin(), u8.end()));

        const auto langs = t.GetLanguages();
        Check(langs.size() == 3 && langs[0].code == "en" && langs[1].code == "pt_BR" && langs[2].code == "zh_CN", "语言列表：英文 + 目录里的两个");
        Check(langs.size() == 3 && langs[2].name == "简体中文", "语言名称取自文件头");

        Check(t.SetLanguage("zh-CN") == "zh_CN" && Tr("Save") == "保存", "zh-CN → zh_CN");
        Check(TrC("verb", "Open") == "打开", "TrC");
        Check(Tr("Not in catalog") == "Not in catalog", "查不到时显示英文");
        Check(TrN("{0} wall", "{0} walls", 2) == "{0} 面墙", "TrN");
        Check(t.SetLanguage("zh") == "zh_CN", "zh → 同语种的 zh_CN");
        Check(t.SetLanguage("pt") == "pt_BR" && Tr("Save") == "Salvar", "pt → pt_BR");
        Check(t.SetLanguage("fr_FR") == "en" && Tr("Save") == "Save", "没有词典的语言 → 英文");
        Check(TrN("{0} wall", "{0} walls", 1) == "{0} wall" && TrN("{0} wall", "{0} walls", 2) == "{0} walls", "英文的单复数");
        const uint64_t v = t.GetVersion();
        t.SetLanguage("en");
        Check(t.GetVersion() == v + 1, "每次切换版本号加一");
        fs::remove_all(dir, ec);
    }

    void TestFormat()
    {
        Check(Format("{0} of {1}", { "3", "5" }) == "3 of 5", "按编号替换");
        Check(Format("{1}，共 {0}", { "5", "3" }) == "3，共 5", "语序可以不同");
        Check(Format("{{0}} {2}", { "a" }) == "{0} {2}", "{{ 转义、超出的编号原样保留");
    }

    // 仓库里的词典：每个都要无警告读出，而且写明语言名称（"文件 > 语言"菜单里显示）
    void TestShippedCatalogs()
    {
        namespace fs = std::filesystem;
        int count = 0;
        const std::string dir = MINIBIM_I18N_DIR;
        for (const auto& entry : fs::directory_iterator(fs::path(std::u8string(dir.begin(), dir.end()))))
        {
            if (entry.path().extension() != ".po")
                continue;
            ++count;
            const std::u8string u8 = entry.path().u8string();
            Catalog c;
            std::vector<std::string> warnings;
            const bool ok = c.LoadFile(std::string(u8.begin(), u8.end()), &warnings);
            const std::string name = entry.path().filename().string();
            Check(ok && warnings.empty(), name + " 无警告读出");
            for (const std::string& w : warnings)
                std::printf("       %s\n", w.c_str());
            Check(!c.GetLanguageName().empty(), name + " 有 X-Language-Name");
            Check(NormalizeLanguage(entry.path().stem().string()) == c.GetLanguage(), name + " 的文件名与文件头 Language 一致");
        }
        Check(count > 0, "assets/i18n 下至少有一个词典");
    }
}

int main()
{
    TestParse();
    TestMalformed();
    TestPlural();
    TestNormalize();
    TestTranslatorFallback();
    TestFormat();
    TestShippedCatalogs();
    std::printf("%s：%d 项失败\n", g_failed ? "FAILED" : "OK", g_failed);
    return g_failed ? 1 : 0;
}
