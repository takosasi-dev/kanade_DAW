#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ss
{
    /** JA/EN UI strings (spec 9.7).  Wraps juce::LocalisedStrings so call sites
        are just TRANS("...") with English as the key language. */
    void setUiLanguage (const juce::String& langCode);   // "ja" | "en"
    juce::String getUiLanguage();
    juce::StringArray getAvailableLanguages();
}
