#pragma once
#include "Core/Settings.h"
#include "IO/OtoIni.h"
#include <map>

namespace ss
{
    /** Indexes the voicebanks configured in Settings::getUtauVoicebankFolders().
        Each configured folder's immediate subfolders are treated as one
        voicebank apiece (folder name == voicebank id), matching how UTAU's own
        voicebank picker organises things. */
    class VoicebankLibrary
    {
    public:
        explicit VoicebankLibrary (Settings& s) : settings (s) {}

        /** Rescans every configured folder. Call after the user changes
            Settings::getUtauVoicebankFolders(), and once at startup. */
        void refresh();

        juce::StringArray getVoicebankIds() const;

        /** Direct alias lookup only (Phase 1 - no VCV/CVVC connection logic).
            Returns nullptr if the voicebank id or the alias isn't found. */
        const OtoEntry* findAlias (const juce::String& voicebankId, const juce::String& lyric) const;

    private:
        Settings& settings;
        std::map<juce::String, std::map<juce::String, OtoEntry>> voicebanks; // id -> (alias -> entry)
    };
}
