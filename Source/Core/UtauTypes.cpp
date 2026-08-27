#include "Core/UtauTypes.h"

namespace ss
{
    juce::int64 UtauClip::currentContentHash() const noexcept
    {
        juce::String s = voicebankId;
        s << ";";

        for (const auto& n : notes)
        {
            s << n.startBeats << "|" << n.lengthBeats << "|" << n.pitch << "|" << n.lyric << "|"
              << n.velocity << "|" << n.intensity << "|" << n.modulation << "|"
              << n.preUtteranceMs << "|" << n.voiceOverlapMs << "|" << n.flags << ";";
        }

        return s.hashCode64();
    }
}
