#include "Vocal/AudioStitcher.h"

namespace ss
{
    juce::AudioBuffer<float> stitchWithCrossfades (const std::vector<StitchFragment>& fragments)
    {
        if (fragments.empty())
            return {};

        int totalLength = 0;

        for (size_t i = 0; i < fragments.size(); ++i)
        {
            const auto n = fragments[i].audio.getNumSamples();
            const auto overlap = i == 0 ? 0 : juce::jmin (fragments[i].overlapSamples, n, totalLength);
            totalLength += n - overlap;
        }

        juce::AudioBuffer<float> out (1, juce::jmax (0, totalLength));
        out.clear();

        int writePos = 0;

        for (size_t i = 0; i < fragments.size(); ++i)
        {
            const auto& frag = fragments[i].audio;
            const auto n = frag.getNumSamples();
            const auto overlap = i == 0 ? 0 : juce::jmin (fragments[i].overlapSamples, n, writePos);
            const auto joinStart = writePos - overlap;

            for (int s = 0; s < n; ++s)
            {
                const auto destIndex = joinStart + s;
                if (destIndex < 0 || destIndex >= out.getNumSamples())
                    continue;

                float gain = 1.0f;

                if (overlap > 0 && s < overlap)
                {
                    gain = (float) s / (float) overlap;
                    // Fade out what's already written there (the previous
                    // fragment's tail) before adding this fragment's faded-in
                    // sample, so a unity+unity overlap still sums to unity.
                    const auto existing = out.getSample (0, destIndex);
                    out.setSample (0, destIndex, existing * (1.0f - gain));
                }

                out.addSample (0, destIndex, frag.getSample (0, s) * gain);
            }

            writePos = joinStart + n;
        }

        return out;
    }
}
