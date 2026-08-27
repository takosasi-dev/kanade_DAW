#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace ss
{
    struct StitchFragment
    {
        juce::AudioBuffer<float> audio;
        /** How many samples of THIS fragment's start overlap with the previous
            fragment's tail. Ignored (treated as 0) for the first fragment. */
        int overlapSamples = 0;
    };

    /** Concatenates fragments end-to-end, linearly crossfading each
        `overlapSamples`-long overlap so consecutive UTAU notes blend instead
        of clicking at the join. This is ScoreSmith's replacement for
        wavtool.exe (see docs/superpowers/specs/2026-08-26-utau-integration-
        phase1-design.md) - mono only, matching a vocal chain. */
    juce::AudioBuffer<float> stitchWithCrossfades (const std::vector<StitchFragment>& fragments);
}
