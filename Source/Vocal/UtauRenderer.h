#pragma once
#include "Core/UtauTypes.h"
#include "Vocal/VoicebankLibrary.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace ss
{
    /** Runs one resampler invocation. Abstracted so tests never need a real
        resampler.exe on the machine - see ExternalResamplerRunner for the
        production implementation and VocalTests.cpp for a fake one. */
    class ResamplerRunner
    {
    public:
        virtual ~ResamplerRunner() = default;

        /** Runs the resampler with `args` (args[0] is the executable itself -
            juce::ChildProcess::start(StringArray) convention). Returns true
            only if the process succeeded AND `expectedOutputWav` now exists. */
        virtual bool run (const juce::StringArray& args, const juce::File& expectedOutputWav) = 0;
    };

    /** Production ResamplerRunner - shells out via juce::ChildProcess, exactly
        like Source/AI/StemSeparator.cpp already does for Demucs. */
    class ExternalResamplerRunner final : public ResamplerRunner
    {
    public:
        bool run (const juce::StringArray& args, const juce::File& expectedOutputWav) override;
    };

    struct RenderResult
    {
        bool ok = false;
        /** Empty on full success. Non-empty but ok==true means "rendered, but
            with per-note warnings" (e.g. an unmatched lyric rendered silent). */
        juce::String errorOrWarnings;
        juce::File renderedFile;
        juce::int64 contentHash = 0;
    };

    /** Renders a UtauClip: resolves each note's oto.ini alias by direct lyric
        match, runs the resampler per note, stitches the fragments with
        AudioStitcher, and writes one WAV to `outputFolder`. Never throws;
        a single unresolvable or failed note becomes silence for that note
        rather than failing the whole render. */
    class UtauRenderer
    {
    public:
        UtauRenderer (VoicebankLibrary& library, ResamplerRunner& runner, const juce::File& resamplerExecutable)
            : voicebanks (library), resamplerRunner (runner), resamplerExe (resamplerExecutable) {}

        RenderResult render (const UtauClip& clip, double tempoBpm, const juce::File& outputFolder);

    private:
        VoicebankLibrary& voicebanks;
        ResamplerRunner& resamplerRunner;
        juce::File resamplerExe;
    };
}
