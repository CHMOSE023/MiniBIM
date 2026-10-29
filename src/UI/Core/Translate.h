#pragma once
#include <functional>
#include <string>
#include <string_view>

namespace MiniGUI
{
    // 界面文字的翻译入口：界面库自带的文字（输入框右键菜单、标题栏按钮提示、功能区……）、界面描述文件里的标题，
    // 以及命令的名称与提示（UiLayout 每次应用时翻译）都经过这里。
    // 原文一律写英文（gettext 的约定：原文就是词典的键）；宿主用 SetTranslator 接上自己的词典，不设置时原样返回原文。
    // 只在界面线程使用
    using TranslateFn = std::function<std::string(std::string_view english)>;

    void        SetTranslator(TranslateFn translate);
    std::string Tr(std::string_view english);
}
