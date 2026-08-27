#include "Engine/Transport.h"
#include <cmath>

namespace ss
{
    namespace
    {
        constexpr double fallbackBpm = 120.0;   // only used while no project is attached

        juce::int64 toSamples (double seconds, double sampleRate) noexcept
        {
            return (juce::int64) std::llround (seconds * sampleRate);
        }
    }

    void Transport::setProject (Project* newProject) noexcept
    {
        project.store (newProject);
    }

    void Transport::setSampleRate (double newSampleRate) noexcept
    {
        if (newSampleRate > 0.0)
            sampleRate.store (newSampleRate);
    }

    void Transport::play() noexcept
    {
        playing.store (true);
    }

    void Transport::stop() noexcept
    {
        playing.store (false);
        recording.store (false);
    }

    void Transport::togglePlay() noexcept
    {
        if (playing.load())
            stop();
        else
            play();
    }

    void Transport::returnToStart() noexcept
    {
        positionSamples.store (0);
    }

    void Transport::setRecording (bool shouldRecord) noexcept
    {
        recording.store (shouldRecord);
    }

    void Transport::setPositionSamples (juce::int64 newPosition) noexcept
    {
        positionSamples.store (juce::jmax ((juce::int64) 0, newPosition));
    }

    void Transport::setPositionBeats (double beats) noexcept
    {
        const auto rate = sampleRate.load();

        if (auto* p = project.load())
            setPositionSamples (toSamples (p->tempo.beatsToSeconds (beats), rate));
        else
            setPositionSamples (toSamples (beats * 60.0 / fallbackBpm, rate));
    }

    double Transport::getPositionSeconds() const noexcept
    {
        return (double) positionSamples.load() / sampleRate.load();
    }

    double Transport::getPositionBeats() const noexcept
    {
        const auto seconds = getPositionSeconds();

        if (auto* p = project.load())
            return p->tempo.secondsToBeats (seconds);

        return seconds * fallbackBpm / 60.0;
    }

    juce::int64 Transport::advance (int numSamples) noexcept
    {
        const auto blockStart = positionSamples.load();

        if (numSamples <= 0 || ! playing.load())
            return blockStart;

        auto next = blockStart + numSamples;

        // ponytail: the audio thread reads the tempo map and loop points straight out of
        // Project.  Safe while every edit goes through AudioEngine's suspend/resume;
        // snapshot them into atomics if that ever stops being true.
        if (auto* p = project.load())
        {
            if (p->loopEnabled && p->loopEndBeats > p->loopStartBeats)
            {
                const auto rate       = sampleRate.load();
                const auto loopStart  = toSamples (p->tempo.beatsToSeconds (p->loopStartBeats), rate);
                const auto loopEnd    = toSamples (p->tempo.beatsToSeconds (p->loopEndBeats),   rate);
                const auto loopLength = loopEnd - loopStart;

                if (loopLength > 0 && next >= loopEnd)
                    next = loopStart + (next - loopEnd) % loopLength;
            }
        }

        positionSamples.store (next);
        return blockStart;
    }
}
