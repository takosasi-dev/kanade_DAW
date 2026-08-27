#pragma once
#include "Core/Project.h"
#include <atomic>

namespace ss
{
    /** Playhead + loop state.  Written by the audio thread, read by everyone,
        so the position is a plain atomic in samples - no locks anywhere.      */
    class Transport
    {
    public:
        Transport() = default;

        /** May be null between documents. */
        void setProject (Project*) noexcept;
        Project* getProject() const noexcept { return project.load(); }

        void setSampleRate (double) noexcept;
        double getSampleRate() const noexcept { return sampleRate.load(); }

        void play() noexcept;
        void stop() noexcept;
        void togglePlay() noexcept;
        void returnToStart() noexcept;
        bool isPlaying() const noexcept { return playing.load(); }

        void setRecording (bool) noexcept;
        bool isRecording() const noexcept { return recording.load(); }

        void   setPositionSamples (juce::int64) noexcept;
        void   setPositionBeats (double) noexcept;
        juce::int64 getPositionSamples() const noexcept { return positionSamples.load(); }
        double getPositionSeconds() const noexcept;
        double getPositionBeats() const noexcept;

        /** Called once per audio block by AudioEngine; handles loop wrap-around
            and returns the block's start position in samples. */
        juce::int64 advance (int numSamples) noexcept;

        void setMetronomeEnabled (bool b) noexcept { metronome.store (b); }
        bool isMetronomeEnabled() const noexcept   { return metronome.load(); }
        void setCountInBars (int b) noexcept       { countInBars.store (b); }
        int  getCountInBars() const noexcept       { return countInBars.load(); }

    private:
        std::atomic<Project*> project { nullptr };
        std::atomic<double>      sampleRate { 48000.0 };
        std::atomic<juce::int64> positionSamples { 0 };
        std::atomic<bool> playing { false }, recording { false }, metronome { false };
        std::atomic<int>  countInBars { 0 };
    };
}
