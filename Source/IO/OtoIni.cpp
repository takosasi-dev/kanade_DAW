#include "IO/OtoIni.h"
#include "IO/ShiftJis.h"

namespace ss
{
    std::map<juce::String, OtoEntry> parseOtoIni (const juce::String& text, const juce::File& sampleFolder)
    {
        std::map<juce::String, OtoEntry> result;

        for (const auto& rawLine : juce::StringArray::fromLines (text))
        {
            const auto line = rawLine.trim();
            if (line.isEmpty())
                continue;

            const auto eq = line.indexOfChar ('=');
            if (eq < 0)
                continue;

            const auto fileName = line.substring (0, eq).trim();
            const auto fields = juce::StringArray::fromTokens (line.substring (eq + 1), ",", "");

            if (fields.size() < 6)
                continue;

            OtoEntry entry;
            entry.alias        = fields[0].isNotEmpty() ? fields[0] : fileName.upToLastOccurrenceOf (".", false, false);
            entry.sampleFile   = sampleFolder.getChildFile (fileName);
            entry.offset       = fields[1].getDoubleValue();
            entry.consonant    = fields[2].getDoubleValue();
            entry.cutoff       = fields[3].getDoubleValue();
            entry.preUtterance = fields[4].getDoubleValue();
            entry.overlap      = fields[5].getDoubleValue();

            result[entry.alias] = entry;
        }

        return result;
    }

    std::map<juce::String, OtoEntry> loadVoicebank (const juce::File& voicebankFolder)
    {
        std::map<juce::String, OtoEntry> merged;

        for (const auto& otoFile : voicebankFolder.findChildFiles (juce::File::findFiles, true, "oto.ini"))
        {
            juce::MemoryBlock raw;
            otoFile.loadFileAsData (raw);
            const auto text = decodeUstText (raw.getData(), raw.getSize());

            for (auto& entry : parseOtoIni (text, otoFile.getParentDirectory()))
                merged[entry.first] = entry.second;
        }

        return merged;
    }
}
