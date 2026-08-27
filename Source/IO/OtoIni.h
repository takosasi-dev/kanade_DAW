#pragma once
#include <juce_core/juce_core.h>
#include <map>

namespace ss
{
    /** One entry from a voicebank's oto.ini. Every timing value is passed
        straight through to the resampler CLI unchanged in Task 9's
        buildResamplerArgs() - ScoreSmith does not interpret oto.ini's sign
        conventions (e.g. a negative cutoff meaning "from the end of file")
        itself; the resampler already knows how to. */
    struct OtoEntry
    {
        juce::String alias;
        juce::File   sampleFile;
        double offset = 0.0, consonant = 0.0, cutoff = 0.0, preUtterance = 0.0, overlap = 0.0;
    };

    /** Parses one oto.ini file's already-decoded text (see ShiftJis.h) into
        alias -> entry. `sampleFolder` is where the referenced .wav filenames
        are resolved relative to. Lines that don't parse (no '=', too few
        comma fields) are skipped rather than treated as an error - a
        voicebank with one malformed line elsewhere should not become
        entirely unusable. */
    std::map<juce::String, OtoEntry> parseOtoIni (const juce::String& text, const juce::File& sampleFolder);

    /** Scans `voicebankFolder` and every subfolder for files literally named
        "oto.ini", merging all their entries (large voicebanks split entries
        across subfolders by UTAU convention - a later file's alias wins on a
        collision). Reads each file's raw bytes and decodes via
        decodeUstText() before parsing. */
    std::map<juce::String, OtoEntry> loadVoicebank (const juce::File& voicebankFolder);
}
