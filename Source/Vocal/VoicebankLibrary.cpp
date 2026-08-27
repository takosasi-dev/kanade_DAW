#include "Vocal/VoicebankLibrary.h"

namespace ss
{
    void VoicebankLibrary::refresh()
    {
        voicebanks.clear();

        for (const auto& folderPath : settings.getUtauVoicebankFolders())
        {
            const juce::File root (folderPath);

            if (! root.isDirectory())
                continue;

            for (const auto& sub : root.findChildFiles (juce::File::findDirectories, false))
                voicebanks[sub.getFileName()] = loadVoicebank (sub);
        }
    }

    juce::StringArray VoicebankLibrary::getVoicebankIds() const
    {
        juce::StringArray ids;

        for (const auto& [id, entries] : voicebanks)
            ids.add (id);

        ids.sort (false);
        return ids;
    }

    const OtoEntry* VoicebankLibrary::findAlias (const juce::String& voicebankId, const juce::String& lyric) const
    {
        const auto bankIt = voicebanks.find (voicebankId);
        if (bankIt == voicebanks.end())
            return nullptr;

        const auto aliasIt = bankIt->second.find (lyric);
        if (aliasIt == bankIt->second.end())
            return nullptr;

        return &aliasIt->second;
    }
}
