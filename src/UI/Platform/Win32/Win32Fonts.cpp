#include "Platform/Win32/Win32Fonts.h"
#include "Text/Font.h"
#include "Text/TextSystem.h"
#include <windows.h>

namespace MiniGUI
{
    std::string GetSystemFontPath(const char* fileName)
    {
        wchar_t dir[MAX_PATH] = {};
        const UINT len = GetWindowsDirectoryW(dir, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return {};

        const int size = WideCharToMultiByte(CP_UTF8, 0, dir, -1, nullptr, 0, nullptr, nullptr);
        std::string path(static_cast<size_t>(size > 0 ? size - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, dir, -1, path.data(), size, nullptr, nullptr);
        return path + "\\Fonts\\" + fileName;
    }

    std::shared_ptr<Font> LoadSystemUIFont()
    {
        // 微软雅黑 UI 行距比微软雅黑紧凑，更适合界面；缺失时依次退回
        if (auto f = Font::LoadFromFile(GetSystemFontPath("msyh.ttc"), 1))   return f;
        if (auto f = Font::LoadFromFile(GetSystemFontPath("msyh.ttf"), 0))   return f;
        if (auto f = Font::LoadFromFile(GetSystemFontPath("simsun.ttc"), 0)) return f;
        return Font::LoadFromFile(GetSystemFontPath("segoeui.ttf"), 0);
    }

    std::shared_ptr<Font> LoadSystemSymbolFont()
    {
        return Font::LoadFromFile(GetSystemFontPath("seguisym.ttf"), 0);
    }

    bool LoadSystemUIFonts(TextSystem& text)
    {
        auto main = LoadSystemUIFont();
        if (!main)
            return false;
        text.AddFont(std::move(main));
        if (auto symbol = LoadSystemSymbolFont())
            text.AddFallbackFont(std::move(symbol));
        // 多语言：微软雅黑已覆盖拉丁、希腊、西里尔字母、中文（含 GBK 里的繁体字）与日文假名，缺的是韩文，
        // 用 Windows 自带的 Malgun Gothic 补上（没装就跳过）。字体整个读进内存，所以只补必需的这一个
        if (auto korean = Font::LoadFromFile(GetSystemFontPath("malgun.ttf"), 0))
            text.AddFallbackFont(std::move(korean));
        return true;
    }
}
