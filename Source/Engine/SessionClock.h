#pragma once
#include "Core/Project.h"
#include <atomic>

namespace ss
{
    /** Free-running sample clock for Session view launches (spec: Session
        view, Phase 3).  Deliberately independent of Transport - it never
        stops or waits on Transport::isPlaying(), so a Session cell can be
        clicked and heard even while the arrangement is stopped.  It shares
        only the project's TempoMap, passed in per call rather than held, so
        it stays trivially testable without a Project. */
    class SessionClock
    {
    public:
        void prepare (double sampleRateToUse) noexcept { sampleRate = sampleRateToUse > 0.0 ? sampleRateToUse : 48000.0; }

        /** Called once per audio block, unconditionally - never gated on
            Transport's play/stop state. */
        void advance (int numSamples) noexcept { samplesElapsed.fetch_add (numSamples, std::memory_order_release); }

        /** Called when a project loads, so bar-boundary math starts fresh. */
        void reset() noexcept { samplesElapsed.store (0, std::memory_order_release); }

        juce::int64 currentSample() const noexcept { return samplesElapsed.load (std::memory_order_acquire); }

        /** Absolute sample (on this clock's own timeline) of the next 1-bar
            boundary at or after currentSample().  Returns currentSample()
            itself (no wait) if already within a sample of a boundary. */
        juce::int64 nextBarBoundarySample (const TempoMap& tempo) const noexcept;

    private:
        double sampleRate = 48000.0;
        std::atomic<juce::int64> samplesElapsed { 0 };
    };
}
