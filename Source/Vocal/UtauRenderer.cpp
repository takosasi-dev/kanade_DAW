#include "Vocal/UtauRenderer.h"
#include "Vocal/AudioStitcher.h"
#include "Vocal/ResamplerArgs.h"
#include <juce_core/juce_core.h>

namespace ss
{
    bool ExternalResamplerRunner::run (const juce::StringArray& args, const juce::File& expectedOutputWav)
    {
        juce::ChildProcess process;

        if (! process.start (args))
            return false;

        if (! process.waitForProcessToFinish (30000))
            process.kill();

        return expectedOutputWav.existsAsFile();
    }

    RenderResult UtauRenderer::render (const UtauClip& clip, double tempoBpm, const juce::File& outputFolder)
    {
        RenderResult result;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const auto msPerBeat = 60000.0 / juce::jmax (1.0, tempoBpm);
        const auto tempDir = outputFolder.getChildFile (".utau_render_tmp");
        tempDir.createDirectory();

        std::vector<StitchFragment> fragments;
        juce::StringArray warnings;

        // Real notes report their own reader's sampleRate (a resampler may emit
        // at 48kHz, not 44.1kHz); silence fragments and the final writer need to
        // agree with whatever rate turns out to be real. Falls back to 44100.0
        // only if every single note is silent/failed and no reader was ever opened.
        double detectedSampleRate = 44100.0;

        for (size_t i = 0; i < clip.notes.size(); ++i)
        {
            const auto& note = clip.notes[i];
            const auto lengthMs = note.lengthBeats * msPerBeat;

            if (note.isRest)
            {
                juce::AudioBuffer<float> silence (1, (int) (lengthMs * 0.001 * detectedSampleRate));
                silence.clear();
                fragments.push_back ({ std::move (silence), 0 });
                continue;
            }

            const auto* oto = voicebanks.findAlias (clip.voicebankId, note.lyric);

            if (oto == nullptr)
            {
                warnings.add ("No voicebank alias for lyric \"" + note.lyric + "\" (note " + juce::String ((int) i) + ") - rendered silent.");
                juce::AudioBuffer<float> silence (1, (int) (lengthMs * 0.001 * detectedSampleRate));
                silence.clear();
                fragments.push_back ({ std::move (silence), 0 });
                continue;
            }

            const auto noteOutputWav = tempDir.getChildFile ("note_" + juce::String ((int) i) + ".wav");
            noteOutputWav.deleteFile();

            const auto args = buildResamplerArgs (resamplerExe, oto->sampleFile, noteOutputWav, note, *oto, lengthMs);

            if (! resamplerRunner.run (args, noteOutputWav))
            {
                warnings.add ("Resampler failed for note " + juce::String ((int) i) + " (\"" + note.lyric + "\") - rendered silent.");
                juce::AudioBuffer<float> silence (1, (int) (lengthMs * 0.001 * detectedSampleRate));
                silence.clear();
                fragments.push_back ({ std::move (silence), 0 });
                continue;
            }

            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (noteOutputWav));

            if (reader == nullptr)
            {
                warnings.add ("Could not read the resampler's output for note " + juce::String ((int) i) + " - rendered silent.");
                juce::AudioBuffer<float> silence (1, (int) (lengthMs * 0.001 * detectedSampleRate));
                silence.clear();
                fragments.push_back ({ std::move (silence), 0 });
                continue;
            }

            if (reader->sampleRate > 0.0)
                detectedSampleRate = reader->sampleRate;

            juce::AudioBuffer<float> fragment (1, (int) reader->lengthInSamples);
            reader->read (&fragment, 0, fragment.getNumSamples(), 0, true, false);

            const auto overlapMs = note.voiceOverlapMs >= 0.0 ? note.voiceOverlapMs : oto->overlap;
            const auto overlapSamples = juce::jmax (0, (int) (overlapMs * 0.001 * reader->sampleRate));

            fragments.push_back ({ std::move (fragment), overlapSamples });
        }

        tempDir.deleteRecursively();

        const auto stitched = stitchWithCrossfades (fragments);

        outputFolder.createDirectory();
        const auto finalFile = outputFolder.getChildFile ("utau_clip_" + juce::String ((juce::int64) clip.id) + ".wav");
        finalFile.deleteFile();

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> stream (finalFile.createOutputStream());

        if (stream == nullptr)
        {
            result.ok = false;
            result.errorOrWarnings = "Could not create " + finalFile.getFullPathName();
            return result;
        }

        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), detectedSampleRate, 1u, 16, {}, 0));

        if (writer == nullptr)
        {
            result.ok = false;
            result.errorOrWarnings = "Could not write WAV to " + finalFile.getFullPathName();
            return result;
        }

        stream.release(); // the writer owns it from here
        writer->writeFromAudioSampleBuffer (stitched, 0, stitched.getNumSamples());
        writer.reset(); // flush before the caller reads the file back

        result.ok = true;
        result.errorOrWarnings = warnings.joinIntoString ("\n");
        result.renderedFile = finalFile;
        result.contentHash = clip.currentContentHash();
        return result;
    }
}
