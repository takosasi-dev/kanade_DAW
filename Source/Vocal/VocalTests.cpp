#include "Vocal/VoicebankLibrary.h"
#include "Vocal/AudioStitcher.h"
#include "Vocal/ResamplerArgs.h"
#include "Vocal/UtauRenderer.h"
#include "Core/Settings.h"
#include <juce_events/juce_events.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace ss
{

class VocalUnitTests final : public juce::UnitTest
{
public:
    VocalUnitTests() : juce::UnitTest ("ScoreSmith vocal", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("VoicebankLibrary indexes a fake voicebank folder and resolves an alias");
        {
            auto tempRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("ScoreSmithVocalTest")
                                .getChildFile (juce::String (juce::Random::getSystemRandom().nextInt()));
            auto bankFolder = tempRoot.getChildFile ("TestBank");
            bankFolder.createDirectory();

            bankFolder.getChildFile ("oto.ini")
                .replaceWithText ("a.wav=a,10.0,20.0,-30.0,5.0,2.0\r\n");
            bankFolder.getChildFile ("a.wav").create();

            Settings settings;
            settings.setUtauVoicebankFolders ({ tempRoot.getFullPathName() });

            VoicebankLibrary library (settings);
            library.refresh();

            const auto ids = library.getVoicebankIds();
            expect (ids.contains ("TestBank"), "the voicebank folder's name should be its id");

            const auto* entry = library.findAlias ("TestBank", "a");
            expect (entry != nullptr);
            expectWithinAbsoluteError (entry->offset, 10.0, 1.0e-9);

            expect (library.findAlias ("TestBank", "nonexistent") == nullptr);
            expect (library.findAlias ("NoSuchBank", "a") == nullptr);

            tempRoot.deleteRecursively();
        }

        beginTest ("stitchWithCrossfades concatenates with no overlap");
        {
            juce::AudioBuffer<float> a (1, 4), b (1, 4);
            a.clear(); a.setSample (0, 0, 1.0f);
            b.clear(); b.setSample (0, 0, 2.0f);

            const auto out = stitchWithCrossfades ({ { a, 0 }, { b, 0 } });

            expectEquals (out.getNumSamples(), 8);
            expectWithinAbsoluteError (out.getSample (0, 0), 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (out.getSample (0, 4), 2.0f, 1.0e-6f);
        }

        beginTest ("stitchWithCrossfades keeps unity amplitude across a crossfade of two unity signals");
        {
            const int n = 100, overlap = 20;
            juce::AudioBuffer<float> a (1, n), b (1, n);

            for (int i = 0; i < n; ++i) { a.setSample (0, i, 1.0f); b.setSample (0, i, 1.0f); }

            const auto out = stitchWithCrossfades ({ { a, 0 }, { b, overlap } });

            expectEquals (out.getNumSamples(), n + n - overlap);

            for (int i = 0; i < out.getNumSamples(); ++i)
                expectWithinAbsoluteError (out.getSample (0, i), 1.0f, 1.0e-4f,
                                          "a linear crossfade between two unity signals must stay at unity, sample " + juce::String (i));
        }

        beginTest ("stitchWithCrossfades handles a single fragment and an empty list");
        {
            juce::AudioBuffer<float> a (1, 4);
            a.clear();
            a.setSample (0, 2, 5.0f);

            const auto single = stitchWithCrossfades ({ { a, 0 } });
            expectEquals (single.getNumSamples(), 4);
            expectWithinAbsoluteError (single.getSample (0, 2), 5.0f, 1.0e-6f);

            const auto empty = stitchWithCrossfades ({});
            expectEquals (empty.getNumSamples(), 0);
        }

        beginTest ("midiNoteToUtauPitch names notes correctly");
        {
            expectEquals (midiNoteToUtauPitch (60), juce::String ("C4"));
            expectEquals (midiNoteToUtauPitch (69), juce::String ("A4"));
            expectEquals (midiNoteToUtauPitch (61), juce::String ("C#4"));
        }

        beginTest ("buildResamplerArgs produces the classic 13-argument resampler CLI shape");
        {
            UtauNote note;
            note.pitch = 60;
            note.velocity = 90;
            note.intensity = 80;
            note.modulation = 5;
            note.flags = "g-5";
            note.preUtteranceMs = -1.0; // use oto's value
            note.voiceOverlapMs = -1.0;

            OtoEntry oto;
            oto.offset = 100.0;
            oto.consonant = 40.0;
            oto.cutoff = -200.0;
            oto.preUtterance = 30.0;
            oto.overlap = 15.0;

            const juce::File resampler ("C:/fake/resampler.exe");
            const juce::File input ("C:/fake/a.wav");
            const juce::File output ("C:/fake/out.wav");

            const auto args = buildResamplerArgs (resampler, input, output, note, oto, 500.0);

            // argv[0] is the executable itself, per juce::ChildProcess::start(StringArray) convention.
            expectEquals (args[0], resampler.getFullPathName());
            expectEquals (args[1], input.getFullPathName());
            expectEquals (args[2], output.getFullPathName());
            expectEquals (args[3], juce::String ("C4"));           // pitch
            expectEquals (args[4], juce::String ("90"));           // velocity
            expectEquals (args[5], juce::String ("g-5"));          // flags
            expectWithinAbsoluteError (args[6].getDoubleValue(), 130.0, 1.0e-6); // oto.offset + oto.preUtterance (note left it at -1)
            expectWithinAbsoluteError (args[7].getDoubleValue(), 500.0, 1.0e-6); // lengthMs
            expectWithinAbsoluteError (args[8].getDoubleValue(), 40.0, 1.0e-6);  // consonant
            expectWithinAbsoluteError (args[9].getDoubleValue(), -200.0, 1.0e-6); // cutoff, passed through unchanged
            expectEquals (args[10], juce::String ("80"));          // intensity
            expectEquals (args[11], juce::String ("5"));           // modulation
            expectEquals (args.size(), 14, "the resampler argv shape is fixed at 14 entries, including the trailing pitchbend slot");
            expectEquals (args[args.size() - 1], juce::String(), "Phase 1 renders flat pitch - the pitchbend argument is always empty");
        }

        beginTest ("buildResamplerArgs prefers the note's own preUtterance/overlap when set");
        {
            UtauNote note;
            note.preUtteranceMs = 55.0;
            note.voiceOverlapMs = 12.0;

            OtoEntry oto;
            oto.offset = 100.0;
            oto.preUtterance = 999.0; // must be ignored - the note overrides it

            const auto args = buildResamplerArgs ({}, {}, {}, note, oto, 0.0);
            expectWithinAbsoluteError (args[6].getDoubleValue(), 155.0, 1.0e-6); // 100 (offset) + 55 (note's own preUtterance)
        }

        beginTest ("UtauRenderer renders a two-note clip end to end with a fake resampler");
        {
            auto tempRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("ScoreSmithRenderTest")
                                .getChildFile (juce::String (juce::Random::getSystemRandom().nextInt()));
            auto bankFolder = tempRoot.getChildFile ("Voicebanks").getChildFile ("TestBank");
            bankFolder.createDirectory();
            bankFolder.getChildFile ("oto.ini").replaceWithText ("a.wav=a,0.0,0.0,0.0,0.0,0.0\r\ni.wav=i,0.0,0.0,0.0,0.0,0.0\r\n");
            bankFolder.getChildFile ("a.wav").create();
            bankFolder.getChildFile ("i.wav").create();

            auto outputFolder = tempRoot.getChildFile ("Media");
            outputFolder.createDirectory();

            Settings settings;
            settings.setUtauVoicebankFolders ({ tempRoot.getChildFile ("Voicebanks").getFullPathName() });
            VoicebankLibrary library (settings);
            library.refresh();

            // Fake runner: instead of really invoking a resampler, writes a
            // short constant-amplitude mono WAV, so the test never depends on
            // an external executable being present on the machine.
            struct FakeRunner final : public ResamplerRunner
            {
                bool run (const juce::StringArray&, const juce::File& expectedOutputWav) override
                {
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::FileOutputStream> stream (expectedOutputWav.createOutputStream());
                    std::unique_ptr<juce::AudioFormatWriter> writer (
                        wav.createWriterFor (stream.get(), 44100.0, 1u, 16, {}, 0));
                    if (writer == nullptr) return false;
                    stream.release();

                    juce::AudioBuffer<float> buffer (1, 4410); // 100ms of constant tone
                    for (int i = 0; i < buffer.getNumSamples(); ++i)
                        buffer.setSample (0, i, 0.5f);
                    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
                    return true;
                }
            } fakeRunner;

            UtauClip clip;
            clip.voicebankId = "TestBank";
            UtauNote n1; n1.lyric = "a"; n1.lengthBeats = 1.0;
            UtauNote n2; n2.lyric = "i"; n2.lengthBeats = 1.0;
            clip.notes = { n1, n2 };

            UtauRenderer renderer (library, fakeRunner, juce::File ("C:/fake/resampler.exe"));
            const auto result = renderer.render (clip, 120.0, outputFolder);

            expect (result.ok, result.errorOrWarnings);
            expect (result.renderedFile.existsAsFile());

            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (result.renderedFile));
            expect (reader != nullptr);
            expect (reader->lengthInSamples > 0, "the rendered file should contain audio");

            tempRoot.deleteRecursively();
        }

        beginTest ("UtauRenderer reports ok with a warning when a lyric has no matching alias");
        {
            Settings settings;
            VoicebankLibrary library (settings); // no folders configured - every lookup misses

            struct AlwaysFailRunner final : public ResamplerRunner
            {
                bool run (const juce::StringArray&, const juce::File&) override { return false; }
            } failRunner;

            UtauClip clip;
            clip.voicebankId = "NoSuchBank";
            UtauNote n; n.lyric = "xyz"; n.lengthBeats = 1.0;
            clip.notes = { n };

            auto tempFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("ScoreSmithRenderTest2");
            tempFolder.createDirectory();

            UtauRenderer renderer (library, failRunner, juce::File ("C:/fake/resampler.exe"));
            const auto result = renderer.render (clip, 120.0, tempFolder);

            // An unmatched lyric renders as silence for that note rather than
            // failing the whole clip - the design doc requires that one note's
            // failure never stops the whole render.
            expect (result.ok);
            expect (result.errorOrWarnings.isNotEmpty(), "a warning should be recorded for the unmatched lyric");

            tempFolder.deleteRecursively();
        }
    }
};

static VocalUnitTests vocalUnitTests;

}
