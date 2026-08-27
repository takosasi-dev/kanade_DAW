#include "Engine/SessionClock.h"

namespace ss
{
    juce::int64 SessionClock::nextBarBoundarySample (const TempoMap& tempo) const noexcept
    {
        const auto elapsedSeconds = (double) currentSample() / sampleRate;
        const auto elapsedBeats   = tempo.secondsToBeats (elapsedSeconds);

        int bar = 0;
        double beatInBar = 0.0;
        tempo.barAndBeat (elapsedBeats, bar, beatInBar);

        const auto ts = tempo.timeSignatureAt (elapsedBeats);
        const auto beatsPerBar = ts.numerator * 4.0 / ts.denominator;
        const auto beatsToBoundary = (beatInBar < 1.0e-6) ? 0.0 : (beatsPerBar - beatInBar);

        const auto boundarySeconds = tempo.beatsToSeconds (elapsedBeats + beatsToBoundary);
        return (juce::int64) std::llround (boundarySeconds * sampleRate);
    }
}
