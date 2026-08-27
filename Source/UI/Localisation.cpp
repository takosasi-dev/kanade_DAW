#include "Core/Localisation.h"
#include <BinaryData.h>

namespace ss
{
    // English is the key language, so "en" needs no mapping table at all - the
    // TRANS() literals already are the English strings.  ja.txt is still loaded
    // when the language is Japanese.  Both files are compiled in as binary data
    // (see juce_add_binary_data in CMakeLists.txt) so there is nothing to install
    // next to the executable.
    static juce::String currentLanguage { "en" };

    static juce::String loadLanguageFile (const juce::String& langCode)
    {
        int size = 0;
        if (auto* data = BinaryData::getNamedResource ((langCode + "_txt").toRawUTF8(), size))
            return juce::String::fromUTF8 (data, size);

        return {};
    }

    void setUiLanguage (const juce::String& langCode)
    {
        const auto code = getAvailableLanguages().contains (langCode) ? langCode : juce::String ("en");
        currentLanguage = code;

        const auto contents = loadLanguageFile (code);

        // A missing or empty table is not fatal: falling through to the key
        // language leaves the UI in English rather than blank.
        if (contents.isNotEmpty())
            juce::LocalisedStrings::setCurrentMappings (new juce::LocalisedStrings (contents, true));
        else
            juce::LocalisedStrings::setCurrentMappings (nullptr);
    }

    juce::String getUiLanguage()
    {
        return currentLanguage;
    }

    juce::StringArray getAvailableLanguages()
    {
        return { "en", "ja" };
    }
}
