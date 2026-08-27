#pragma once
#include "Core/Project.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>

namespace ss
{
    /** Multi-channel disk recorder (spec 8.4.3, 11-P1).
        Audio thread pushes blocks into a FIFO; a background thread writes WAV.
        Never allocates or blocks on the audio thread - a dropped block is
        counted and reported rather than glitching the stream.                 */
    class Recorder
    {
    public:
        Recorder();
        ~Recorder();

        struct ArmedInput
        {
            TrackId  trackId = invalidTrackId;
            int      hardwareChannel = 0;
            juce::File destination;
        };

        /** Opens one WAV writer per armed track.  `startBeat` is where the
            resulting clips will be placed when finish() is called.            */
        juce::Result start (const std::vector<ArmedInput>&, double sampleRate,
                            int bitDepth, double startBeat);

        /** [audio thread] `inputs` is the device input buffer. */
        void processBlock (const juce::AudioBuffer<float>& inputs, int numSamples) noexcept;

        /** Closes the files and returns one finished clip per armed track.
            `tempo` is what turns each take's own written length into beats, so a
            channel that dropped blocks gets a shorter clip instead of the
            transport's idea of the range.  `endBeat` is only used when the device
            sample rate is unknown. */
        struct Take { TrackId trackId; juce::File file; double startBeats; double lengthBeats; };
        std::vector<Take> finish (double endBeat, const TempoMap& tempo);

        bool isRecording() const noexcept { return active.load(); }
        int  getDroppedBlockCount() const noexcept { return dropped.load(); }

        /** Live input peak for meters, per armed track index. */
        float getInputPeak (int armedIndex) const noexcept;

    private:
        struct Channel;
        std::vector<std::unique_ptr<Channel>> channels;
        juce::TimeSliceThread writerThread { "ScoreSmith Recorder" };
        std::atomic<bool> active { false };
        std::atomic<int>  dropped { 0 };
        double startBeatOfTake = 0.0;
        double sampleRateOfTake = 0.0;

        JUCE_DECLARE_NON_COPYABLE (Recorder)
    };
}
