#include "Core/Translate.h"

namespace MiniGUI
{
    namespace
    {
        TranslateFn& Translator()
        {
            static TranslateFn fn;
            return fn;
        }
    }

    void SetTranslator(TranslateFn translate)
    {
        Translator() = std::move(translate);
    }

    std::string Tr(std::string_view english)
    {
        const TranslateFn& fn = Translator();
        return fn ? fn(english) : std::string(english);
    }
}
