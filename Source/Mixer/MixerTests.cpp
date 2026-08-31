#include "Mixer/Mixer.h"
#include "Core/Settings.h"
#include "Core/UtauTypes.h"
#include "Plugins/BasicSynth.h"
#include "Plugins/PluginManager.h"

#include <cmath>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

namespace ss
{

/*  Only the parts that can be checked without an audio device or a plugin: the
    parameter contract every built-in effect has to honour for project
    persistence, and the pan law.  DSP quality is a listening test.             */
class MixerUnitTests final : public juce::UnitTest
{
public:
    MixerUnitTests() : juce::UnitTest ("ScoreSmith mixer", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("built-in effect registry");
        {
            const auto types = getBuiltinEffectTypes();
            expect (types.size() >= 8, "the spec 8.4.6 suite should all be registered");

            for (const auto& type : types)
            {
                expect (createBuiltinEffect (type) != nullptr, "no factory entry for " + type);
                expect (getBuiltinEffectDisplayName (type).isNotEmpty(), "no display name for " + type);
            }

            expect (createBuiltinEffect ("no-such-effect") == nullptr);
        }

        beginTest ("parameters round-trip through NamedValueSet");
        {
            for (const auto& type : getBuiltinEffectTypes())
            {
                auto effect = createBuiltinEffect (type);

                if (effect == nullptr)
                    continue;

                expectEquals (effect->getType(), type);

                const auto info = effect->getParameterInfo();
                expect (! info.empty(), type + " exposes no parameters");

                juce::StringArray ids;
                juce::NamedValueSet wanted;

                for (const auto& param : info)
                {
                    expect (param.id.isNotEmpty(), type + " has an unnamed parameter");
                    expect (param.label.isNotEmpty(), type + "." + param.id + " has no label");
                    expect (! ids.contains (param.id), type + " repeats the id " + param.id);
                    ids.add (param.id);

                    expect (param.max > param.min, type + "." + param.id + " has an empty range");
                    expect (param.defaultValue >= param.min && param.defaultValue <= param.max,
                            type + "." + param.id + " defaults outside its own range");

                    // Deliberately neither the default nor a range endpoint.
                    wanted.set (param.id, (double) (param.min + (param.max - param.min) * 0.37f));
                }

                effect->setParameters (wanted);
                const auto readBack = effect->getParameters();

                expectEquals (readBack.size(), wanted.size());

                for (const auto& param : info)
                    expectWithinAbsoluteError ((float) (double) readBack[param.id],
                                               (float) (double) wanted[param.id], 1.0e-4f,
                                               type + "." + param.id + " did not survive the round-trip");

                // Values from a hand-edited project file must be clamped, not stored.
                juce::NamedValueSet outOfRange;

                for (const auto& param : info)
                    outOfRange.set (param.id, (double) param.max + 1000.0);

                effect->setParameters (outOfRange);
                const auto clamped = effect->getParameters();

                for (const auto& param : info)
                    expectWithinAbsoluteError ((float) (double) clamped[param.id], param.max, 1.0e-4f,
                                               type + "." + param.id + " was not clamped to its range");
            }
        }

        beginTest ("equal-power pan law");
        {
            float l = 0.0f, r = 0.0f;

            equalPowerPanGains (-1.0f, l, r);
            expectWithinAbsoluteError (l, 1.0f, 1.0e-5f);
            expectWithinAbsoluteError (r, 0.0f, 1.0e-5f);

            equalPowerPanGains (1.0f, l, r);
            expectWithinAbsoluteError (l, 0.0f, 1.0e-5f);
            expectWithinAbsoluteError (r, 1.0f, 1.0e-5f);

            equalPowerPanGains (0.0f, l, r);
            expectWithinAbsoluteError (l, r, 1.0e-6f);
            expectWithinAbsoluteError (juce::Decibels::gainToDecibels (l), -3.0103f, 0.01f);

            // Constant power across the range, and clamped outside it.
            for (float pan = -2.0f; pan <= 2.0f; pan += 0.05f)
            {
                equalPowerPanGains (pan, l, r);
                expectWithinAbsoluteError (l * l + r * r, 1.0f, 1.0e-5f);
                expect (l >= 0.0f && r >= 0.0f, "pan law produced a negative gain");
            }
        }

        beginTest ("automation curve evaluation");
        {
            // A lane with no breakpoints has to leave the parameter alone.
            expectWithinAbsoluteError (automationValueAt ({}, 4.0, 0.25f), 0.25f, 1.0e-6f);

            const std::vector<std::pair<double, float>> ramp { { 0.0, 0.0f }, { 4.0, 1.0f }, { 8.0, 0.5f } };

            // Held either side, never extrapolated past what the user drew.
            expectWithinAbsoluteError (automationValueAt (ramp, -10.0, 9.0f), 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (automationValueAt (ramp, 1000.0, 9.0f), 0.5f, 1.0e-6f);

            // Exact on the breakpoints, linear between them.
            expectWithinAbsoluteError (automationValueAt (ramp, 0.0, 9.0f), 0.0f,  1.0e-6f);
            expectWithinAbsoluteError (automationValueAt (ramp, 1.0, 9.0f), 0.25f, 1.0e-6f);
            expectWithinAbsoluteError (automationValueAt (ramp, 2.0, 9.0f), 0.5f,  1.0e-6f);
            expectWithinAbsoluteError (automationValueAt (ramp, 3.0, 9.0f), 0.75f, 1.0e-6f);
            expectWithinAbsoluteError (automationValueAt (ramp, 4.0, 9.0f), 1.0f,  1.0e-6f);
            expectWithinAbsoluteError (automationValueAt (ramp, 6.0, 9.0f), 0.75f, 1.0e-6f);
            expectWithinAbsoluteError (automationValueAt (ramp, 8.0, 9.0f), 0.5f,  1.0e-6f);

            // Monotonic across the whole lane, and always inside the drawn range.
            for (double beat = -1.0; beat <= 9.0; beat += 0.1)
            {
                const auto v = automationValueAt (ramp, beat, 9.0f);
                expect (v >= 0.0f && v <= 1.0f, "automation left the range it was drawn in");
            }

            // One breakpoint is a constant, not a ramp from nowhere.
            const std::vector<std::pair<double, float>> single { { 3.0, 0.6f } };

            for (const auto beat : { 0.0, 3.0, 99.0 })
                expectWithinAbsoluteError (automationValueAt (single, beat, 0.0f), 0.6f, 1.0e-6f);

            // Two breakpoints on the same beat are a step, not a divide by zero.
            const std::vector<std::pair<double, float>> step {
                { 0.0, 0.0f }, { 2.0, 0.0f }, { 2.0, 1.0f }, { 4.0, 1.0f } };

            expectWithinAbsoluteError (automationValueAt (step, 1.9, 0.0f), 0.0f, 1.0e-3f);
            expectWithinAbsoluteError (automationValueAt (step, 3.0, 0.0f), 1.0f, 1.0e-6f);
            expect (std::isfinite (automationValueAt (step, 2.0, 0.0f)),
                    "a zero-length segment produced a non-finite value");
        }

        beginTest ("normalised parameter writes (the automation -> effect path)");
        {
            for (const auto& type : getBuiltinEffectTypes())
            {
                auto effect = createBuiltinEffect (type);

                if (effect == nullptr)
                    continue;

                const auto info = effect->getParameterInfo();

                for (int i = 0; i < (int) info.size(); ++i)
                {
                    const auto& param = info[(size_t) i];

                    const auto readBack = [&effect, &param]
                    {
                        return (float) (double) effect->getParameters()[param.id];
                    };

                    effect->setParameterNormalised (i, 0.0f);
                    expectWithinAbsoluteError (readBack(), param.min, 1.0e-4f,
                                               type + "." + param.id + ": 0 should be the minimum");

                    effect->setParameterNormalised (i, 1.0f);
                    expectWithinAbsoluteError (readBack(), param.max, 1.0e-4f,
                                               type + "." + param.id + ": 1 should be the maximum");

                    effect->setParameterNormalised (i, 0.5f);
                    expectWithinAbsoluteError (readBack(), param.min + (param.max - param.min) * 0.5f,
                                               (param.max - param.min) * 1.0e-4f,
                                               type + "." + param.id + " did not map linearly");

                    // A lane value from a hand-edited file must clamp, not wrap.
                    effect->setParameterNormalised (i, 4.0f);
                    expectWithinAbsoluteError (readBack(), param.max, 1.0e-4f);

                    effect->setParameterNormalised (i, -4.0f);
                    expectWithinAbsoluteError (readBack(), param.min, 1.0e-4f);
                }

                // Out of bounds is a no-op, not a write past the parameter table.
                effect->setParameterNormalised (-1, 1.0f);
                effect->setParameterNormalised ((int) info.size(), 1.0f);
                expectEquals ((int) effect->getParameters().size(), (int) info.size());
            }
        }

        beginTest ("buses, sends and the master chain round-trip through .ssproj");
        {
            Project project;

            auto& bus = project.addBus ("Drums");
            bus.gainDb = -3.0f;
            bus.pan    = 0.25f;
            bus.muted  = true;
            bus.builtinFx.push_back ({ "reverb", true, {} });

            project.masterChain.push_back ({ "limiter", false, {} });

            auto& track = project.addTrack (TrackType::audio, "Kick");
            track.outputBus       = bus.id;
            track.inputMonitoring = true;
            track.sends.push_back ({ bus.id, 0.5f });
            track.plugins.push_back ({ "some.plugin", "Synth", false, true, {} });

            const auto busId = bus.id;

            Project reloaded;
            expect (reloaded.loadFromVar (project.toVar()));

            expectEquals ((int) reloaded.buses.size(), 1);
            expectEquals ((int) reloaded.masterChain.size(), 1);

            if (! reloaded.buses.empty())
            {
                const auto& b = reloaded.buses.front();
                expectEquals (b.id, busId);
                expectEquals (b.name, juce::String ("Drums"));
                expectWithinAbsoluteError (b.gainDb, -3.0f, 1.0e-4f);
                expectWithinAbsoluteError (b.pan, 0.25f, 1.0e-4f);
                expect (b.muted);
                expectEquals ((int) b.builtinFx.size(), 1);
                expect (b.builtinFx.front().bypassed);
            }

            if (! reloaded.masterChain.empty())
                expectEquals (reloaded.masterChain.front().type, juce::String ("limiter"));

            expectEquals (reloaded.getNumTracks(), 1);

            if (reloaded.getNumTracks() == 1)
            {
                auto& t = reloaded.getTrack (0);
                expectEquals (t.outputBus, busId);
                expect (t.inputMonitoring, "input monitoring was not persisted");
                expectEquals ((int) t.sends.size(), 1);

                if (! t.sends.empty())
                {
                    expectEquals (t.sends.front().busId, busId);
                    expectWithinAbsoluteError (t.sends.front().level, 0.5f, 1.0e-4f);
                }

                expectEquals ((int) t.plugins.size(), 1);

                if (! t.plugins.empty())
                    expect (t.plugins.front().isInstrument,
                            "the instrument flag was not persisted");
            }

            // The bus id watermark is part of the document: a new bus must not
            // reuse an id a track is still pointing at.
            expect (reloaded.addBus ({}).id != busId, "a reloaded project reused a bus id");

            // Removing a bus re-points what fed it instead of dangling.
            reloaded.removeBus (busId);
            expect (reloaded.findBus (busId) == nullptr);
            expectEquals (reloaded.getTrack (0).outputBus, 0);
            expect (reloaded.getTrack (0).sends.empty(), "a send outlived its bus");
        }

        /*  The one place the whole block path can be exercised without a device or
            a plugin: monitored hardware input is a signal source the mixer owns.
            It covers input monitoring, bus routing, sends, solo and gain
            automation in a single graph.                                         */
        beginTest ("input monitoring, bus routing, sends and gain automation");
        {
            constexpr int blockSize = 64;
            constexpr float inputLevel = 0.5f;

            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            Project project;

            auto& track = project.addTrack (TrackType::audio, "Mic");
            track.recordArmed     = true;
            track.inputMonitoring = true;
            track.inputChannel    = 0;
            track.pan             = -1.0f;    // hard left: the left gain is exactly 1

            mixer.setProject (&project);
            mixer.prepare (48000.0, blockSize, 2);

            juce::AudioBuffer<float> input (1, blockSize), output (2, blockSize);
            juce::MidiBuffer noMidi;

            for (int i = 0; i < blockSize; ++i)
                input.setSample (0, i, inputLevel);

            /*  Two blocks every time: every gain change is ramped across one block
                so a click is impossible, which makes only the second block steady. */
            const auto renderLeft = [&] (bool isPlaying)
            {
                mixer.rebuild();

                for (int block = 0; block < 2; ++block)
                {
                    noMidi.clear();
                    mixer.process (output, 0, noMidi, input, isPlaying);
                }

                return output.getSample (0, blockSize / 2);
            };

            expectWithinAbsoluteError (renderLeft (false), inputLevel, 1.0e-4f,
                                       "a monitored input should be audible while stopped");

            track.inputMonitoring = false;
            expectWithinAbsoluteError (renderLeft (false), 0.0f, 1.0e-5f,
                                       "monitoring off should be silent");

            track.inputMonitoring = true;
            track.recordArmed = false;
            expectWithinAbsoluteError (renderLeft (false), 0.0f, 1.0e-5f,
                                       "monitoring should need the track to be armed");

            track.recordArmed = true;

            // --- through a bus rather than straight to the master ---------------
            auto& bus = project.addBus ("Group");
            bus.pan = -1.0f;
            track.outputBus = bus.id;

            expectWithinAbsoluteError (renderLeft (false), inputLevel, 1.0e-4f,
                                       "a track routed to a bus should still reach the master");

            bus.muted = true;
            expectWithinAbsoluteError (renderLeft (false), 0.0f, 1.0e-5f,
                                       "a muted bus should not pass its tracks");

            bus.muted = false;
            bus.gainDb = -6.0206f;      // exactly half
            expectWithinAbsoluteError (renderLeft (false), inputLevel * 0.5f, 1.0e-3f,
                                       "the bus fader should scale everything routed to it");

            // A centred bus is unity: routing through a group must not cost 3 dB.
            bus.gainDb = 0.0f;
            bus.pan    = 0.0f;
            expectWithinAbsoluteError (renderLeft (false), inputLevel, 1.0e-3f,
                                       "a centred bus applied a pan law of its own");

            bus.pan = -1.0f;

            // --- a send is on top of the main path, not instead of it -----------
            bus.gainDb = 0.0f;
            track.outputBus = 0;
            track.sends.push_back ({ bus.id, 0.5f });

            expectWithinAbsoluteError (renderLeft (false), inputLevel * 1.5f, 1.0e-3f,
                                       "a post-fader send should add to the direct output");

            track.sends.clear();

            // --- solo takes the strip out of the sum, it does not just mute it --
            auto& other = project.addTrack (TrackType::midi, "Solo me");
            other.soloed = true;

            expectWithinAbsoluteError (renderLeft (false), 0.0f, 1.0e-5f,
                                       "a non-soloed track should not reach the master");

            other.soloed = false;
            expectWithinAbsoluteError (renderLeft (false), inputLevel, 1.0e-4f);

            // --- gain automation, at the block's own position -------------------
            track.automation.push_back ({ "gain", { { 0.0, 0.0f } } });   // 0 -> -60 dB
            expect (renderLeft (false) < inputLevel * 0.01f,
                    "a gain lane at 0 should pull the strip down to -60 dB");

            track.automation.front().points = { { 0.0, 1.0f } };          // 1 -> +6 dB
            expectWithinAbsoluteError (renderLeft (false),
                                       inputLevel * juce::Decibels::decibelsToGain (6.0f), 1.0e-3f,
                                       "a gain lane at 1 should reach +6 dB");

            // A lane with no breakpoints must leave the fader alone.
            track.gainDb = -6.0206f;
            track.automation.front().points.clear();
            expectWithinAbsoluteError (renderLeft (false), inputLevel * 0.5f, 1.0e-3f,
                                       "an empty lane overrode the fader");

            mixer.setProject (nullptr);
        }

        beginTest ("a rendered UtauClip plays back through the mixer like an audio clip");
        {
            auto tempFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("ScoreSmithMixerUtauTest_" + juce::String (juce::Random::getSystemRandom().nextInt()) + ".wav");

            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::FileOutputStream> stream (tempFile.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), 44100.0, 1u, 16, {}, 0));
                stream.release();
                juce::AudioBuffer<float> tone (1, 44100); // 1 second at 0.8 amplitude
                for (int i = 0; i < tone.getNumSamples(); ++i) tone.setSample (0, i, 0.8f);
                writer->writeFromAudioSampleBuffer (tone, 0, tone.getNumSamples());
            }

            Project project;
            project.tempo.setEvents ({ { 0.0, 120.0 } });
            auto& track = project.addTrack (TrackType::utau, "Vocal");

            UtauClip clip;
            clip.id = 1;
            clip.startBeats = 0.0;
            clip.lengthBeats = 4.0;
            clip.renderedFile = tempFile;
            clip.notesHashAtRender = clip.currentContentHash(); // a fresh, non-stale render
            track.utauClips.push_back (clip);

            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.setProject (&project);
            mixer.prepare (44100.0, 512, 2);
            mixer.rebuild();

            juce::AudioBuffer<float> output (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            // Give the buffering/resampling chain time to spin up before asserting -
            // this is genuine background-thread work (see the "replacing a track's
            // audio clip" test below for the same reasoning), so poll for it rather
            // than trusting a fixed block count to always be enough under load.
            float firstMagnitude = 0.0f;
            int blockIndex = 0;

            for (; blockIndex < 500; ++blockIndex)
            {
                output.clear();
                mixer.process (output, blockIndex * 512, midi, noInput, true);
                firstMagnitude = output.getMagnitude (0, 0, 512);

                if (firstMagnitude > 0.1f)
                    break;
            }

            expect (firstMagnitude > 0.1f,
                    "a rendered UtauClip's audio should reach the mixer output");

            // Re-render onto the SAME path (UtauRenderer always writes a fixed,
            // id-derived filename): the signature must pick up the new mtime, or
            // buildClips's "nothing changed" fast path keeps the stale reader alive.
            {
                tempFile.deleteFile();
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::FileOutputStream> stream (tempFile.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), 44100.0, 1u, 16, {}, 0));
                stream.release();
                juce::AudioBuffer<float> tone (1, 44100); // 1 second at 0.2 amplitude
                for (int i = 0; i < tone.getNumSamples(); ++i) tone.setSample (0, i, 0.2f);
                writer->writeFromAudioSampleBuffer (tone, 0, tone.getNumSamples());
            }

            // Force the mtime unambiguously forward rather than relying on a sleep +
            // real elapsed wall-clock time - filesystem write-back/caching lag can
            // otherwise leave the two writes indistinguishable within a short window,
            // making the test flaky rather than deterministic.
            tempFile.setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::seconds (2.0));

            mixer.rebuild();

            float secondMagnitude = 0.0f;
            bool sawStaleContent = false;
            const auto secondPhaseEnd = blockIndex + 500;

            for (++blockIndex; blockIndex < secondPhaseEnd; ++blockIndex)
            {
                output.clear();
                mixer.process (output, blockIndex * 512, midi, noInput, true);
                secondMagnitude = output.getMagnitude (0, 0, 512);

                if (secondMagnitude > firstMagnitude * 0.5f)
                {
                    sawStaleContent = true;
                    break;
                }

                if (secondMagnitude > 0.02f)
                    break;
            }

            expect (! sawStaleContent,
                    "re-rendering a UtauClip at the same path should invalidate the mixer's cached "
                    "reader, not keep playing the old (louder) content after the edit");
            expect (secondMagnitude > 0.02f,
                    "the re-rendered clip's audio never reached the mixer output within the generous "
                    "wait window - the buffering/resampling chain never caught up");

            mixer.setProject (nullptr);
            tempFile.deleteFile();
        }

        beginTest ("replacing a track's audio clip is picked up without a full rebuild()");
        {
            // Reproduces a real-machine bug report: deleting a clip and dropping a
            // new one onto the same track kept playing the OLD file. Root cause -
            // topologyFingerprint() (AudioEngine.cpp) deliberately excludes clip
            // data, so a pure clip edit never reaches Mixer::rebuild(); the only
            // path clip changes are meant to take is Mixer::syncClips(), which
            // this test calls directly (AudioEngine wires it into its own
            // ChangeListener callback - not exercised here, no audio device).
            auto makeToneFile = [] (float amplitude) -> juce::File
            {
                auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("ScoreSmithMixerReplaceTest_"
                                               + juce::String (juce::Random::getSystemRandom().nextInt()) + ".wav");
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), 44100.0, 1u, 16, {}, 0));
                stream.release();
                juce::AudioBuffer<float> tone (1, 44100);
                for (int i = 0; i < tone.getNumSamples(); ++i) tone.setSample (0, i, amplitude);
                writer->writeFromAudioSampleBuffer (tone, 0, tone.getNumSamples());
                return file;
            };

            const auto firstFile = makeToneFile (0.8f);
            const auto secondFile = makeToneFile (0.2f);

            Project project;
            auto& track = project.addTrack (TrackType::audio, "Audio");

            AudioClip clip;
            clip.id = 1;
            clip.startBeats = 0.0;
            clip.lengthBeats = 4.0;
            clip.sourceFile = firstFile;
            track.audioClips.push_back (clip);

            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.setProject (&project);
            mixer.prepare (44100.0, 512, 2);
            mixer.rebuild();

            juce::AudioBuffer<float> output (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            for (int i = 0; i < 20; ++i)
            {
                output.clear();
                mixer.process (output, i * 512, midi, noInput, true);
            }

            const auto firstMagnitude = output.getMagnitude (0, 0, 512);
            expect (firstMagnitude > 0.1f, "the first clip's audio should reach the mixer output");

            // Simulate the UI operation the bug report describes: delete the old
            // clip, drop in a new one - same track, same slot, different file.
            track.audioClips.clear();

            AudioClip replacement;
            replacement.id = 2;
            replacement.startBeats = 0.0;
            replacement.lengthBeats = 4.0;
            replacement.sourceFile = secondFile;
            track.audioClips.push_back (replacement);

            // Deliberately NOT calling mixer.rebuild() - that would mask the bug
            // this test exists to catch. syncClips() is the only call a pure clip
            // edit is supposed to need.
            mixer.syncClips();

            // BufferingAudioSource's read-ahead fill is real background-thread work
            // (see juce_BufferingAudioSource.cpp), not something a fixed block count
            // can time deterministically under load - poll instead of guessing a
            // window, exactly the failure mode the original bug report described
            // ("plays fine once it's had a moment to catch up"). Bounded generously
            // (~5.8s of audio) so a genuine regression still fails instead of hanging.
            float secondMagnitude = 0.0f;
            bool sawStaleContent = false;

            for (int i = 20; i < 500; ++i)
            {
                output.clear();
                mixer.process (output, i * 512, midi, noInput, true);
                secondMagnitude = output.getMagnitude (0, 0, 512);

                if (secondMagnitude > firstMagnitude * 0.5f)
                {
                    sawStaleContent = true;
                    break;
                }

                if (secondMagnitude > 0.02f)
                    break;
            }

            expect (! sawStaleContent,
                    "replacing a track's clip should invalidate the mixer's cached reader, not keep "
                    "playing the old (louder) file after the edit");
            expect (secondMagnitude > 0.02f,
                    "the new clip's audio never reached the mixer output within the generous wait "
                    "window - the buffering/resampling chain never caught up");

            mixer.setProject (nullptr);
            firstFile.deleteFile();
            secondFile.deleteFile();
        }

        beginTest ("launchSessionClip on an audio-kind clip plays only after its boundary sample, then loops");
        {
            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& track = project.addTrack (TrackType::audio, "Sampler");
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;
            // Deliberately no sourceFile: launchSessionClip treats a clip
            // whose sourceFile does not exist as a valid zero-length buffer
            // (silent, lengthSamples == 0) rather than an error, so
            // isSessionClipActive still flips true - "active" reflects
            // scheduling state, not audio content. This keeps the test free
            // of real file I/O.

            const juce::int64 boundarySample = 1500;
            mixer.launchSessionClip (trackId, clip, boundarySample);

            expect (mixer.isSessionClipQueued (trackId));
            expect (! mixer.isSessionClipActive (trackId));

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            // Block 1: samples [0, 512) - entirely before the boundary at 1500.
            // sessionPositionSamples is passed as its own argument (SessionClock's
            // timeline, not Transport's positionSamples) - same value sequence as
            // positionSamples here, since this test isn't exercising the two
            // clocks' independence (see the dedicated "stopped transport" test
            // below for that).
            mixer.process (out, 0, midi, noInput, true, 0);
            expect (! mixer.isSessionClipActive (trackId));

            // Block 2: samples [512, 1024) - still before 1500.
            mixer.process (out, 512, midi, noInput, true, 512);
            expect (! mixer.isSessionClipActive (trackId));

            // Block 3: samples [1024, 1536) - crosses the boundary at 1500.
            mixer.process (out, 1024, midi, noInput, true, 1024);
            expect (mixer.isSessionClipActive (trackId));
            expect (! mixer.isSessionClipQueued (trackId));
        }

        beginTest ("stopSessionClip stops playback only once its boundary sample is reached");
        {
            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& track = project.addTrack (TrackType::audio, "Sampler");
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;
            mixer.launchSessionClip (trackId, clip, 0);   // active immediately (boundary already at sample 0)

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;
            mixer.process (out, 0, midi, noInput, true, 0);
            expect (mixer.isSessionClipActive (trackId));

            mixer.stopSessionClip (trackId, 2000);
            expect (mixer.isSessionClipQueued (trackId));

            mixer.process (out, 512, midi, noInput, true, 512);    // [512, 1024) - before 2000
            expect (mixer.isSessionClipActive (trackId));

            mixer.process (out, 1536, midi, noInput, true, 1536);   // [1536, 2048) - crosses 2000
            expect (! mixer.isSessionClipActive (trackId));
        }

        beginTest ("launching a second session clip on the same track replaces the first at the new boundary");
        {
            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& track = project.addTrack (TrackType::audio, "Sampler");
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip first;
            first.kind = SessionClip::Kind::audio;
            mixer.launchSessionClip (trackId, first, 0);

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;
            mixer.process (out, 0, midi, noInput, true, 0);
            expect (mixer.isSessionClipActive (trackId));

            SessionClip second;
            second.kind = SessionClip::Kind::audio;
            mixer.launchSessionClip (trackId, second, 2000);

            expect (mixer.isSessionClipActive (trackId));   // the first clip keeps playing until the boundary
            expect (mixer.isSessionClipQueued (trackId));

            mixer.process (out, 1536, midi, noInput, true, 1536);  // crosses 2000
            expect (mixer.isSessionClipActive (trackId));
            expect (! mixer.isSessionClipQueued (trackId));
        }

        /*  Final-review regression: AudioEngine::renderToFile used to call
            mixer->process() without ever passing sessionPositionSamples,
            silently defaulting to 0 - a valid-looking real position - so an
            active session clip got rendered into the exported file at a bogus
            phase AND had its live position/hand-off state advanced by the
            (potentially many) offline blocks, scrambling live playback for
            whatever kept looping after the export finished. Mixer::process()
            now treats a negative sessionPositionSamples as an explicit "no
            real SessionClock position for this call" sentinel and skips the
            whole session block entirely; AudioEngine::renderToFile passes -1
            explicitly rather than relying on a default argument. */
        beginTest ("the offline-render sentinel (-1) produces no session output for that call");
        {
            auto toneFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("ScoreSmithSessionOfflineSentinelTest_"
                                               + juce::String (juce::Random::getSystemRandom().nextInt()) + ".wav");

            constexpr int toneLength = 4096;   // longer than one block
            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::FileOutputStream> stream (toneFile.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (
                    wav.createWriterFor (stream.get(), 48000.0, 1u, 16, {}, 0));
                stream.release();
                juce::AudioBuffer<float> tone (1, toneLength);
                for (int i = 0; i < toneLength; ++i)
                    tone.setSample (0, i, 0.8f);
                writer->writeFromAudioSampleBuffer (tone, 0, toneLength);
            }

            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& track = project.addTrack (TrackType::audio, "Sampler");
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;
            clip.sourceFile = toneFile;
            mixer.launchSessionClip (trackId, clip, 0);   // active immediately

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            mixer.process (out, 0, midi, noInput, true, 0);
            expect (out.getMagnitude (0, 0, 512) > 0.3f,
                    "sanity: the session clip should be audible under a normal, real call");
            expect (mixer.isSessionClipActive (trackId));

            // Simulate AudioEngine::renderToFile: the arrangement's own position
            // advances normally, but sessionPositionSamples carries the sentinel,
            // exactly like the real call site does now.
            out.clear();
            mixer.process (out, 4096, midi, noInput, true, -1);

            expect (out.getMagnitude (0, 0, 512) < 1.0e-6f,
                    "an active session clip must produce no output for an offline-render call");
            expect (mixer.isSessionClipActive (trackId),
                    "the offline call must not have stopped the live session clip either");

            mixer.setProject (nullptr);
            toneFile.deleteFile();
        }

        beginTest ("the offline-render sentinel (-1) does not adopt a pending session hand-off, "
                   "proving it never touches the clip's internal position/generation state");
        {
            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& track = project.addTrack (TrackType::audio, "Sampler");
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;   // no sourceFile - a valid, silent, zero-length clip
            mixer.launchSessionClip (trackId, clip, 0);

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            mixer.process (out, 0, midi, noInput, true, 0);
            expect (mixer.isSessionClipActive (trackId));

            // Queue a replacement close enough (boundary 100) that the OLD,
            // unguarded code - which compared `sessionPositionSamples + n >=
            // pendingLaunchAtSample` with no regard for sessionPositionSamples's
            // sign - would have wrongly adopted it on the very next call, even
            // one carrying the sentinel.
            SessionClip second;
            second.kind = SessionClip::Kind::audio;
            mixer.launchSessionClip (trackId, second, 100);
            expect (mixer.isSessionClipQueued (trackId));

            mixer.process (out, 4096, midi, noInput, true, -1);   // the offline-render call

            expect (mixer.isSessionClipQueued (trackId),
                    "an offline-render call must not adopt a pending session hand-off - its "
                    "internal position/generation state must be left completely untouched");

            // The live hand-off mechanism still works normally once a real position
            // resumes - proving the sentinel call didn't corrupt or lose it.
            mixer.process (out, 512, midi, noInput, true, 512);
            expect (! mixer.isSessionClipQueued (trackId),
                    "resuming with a real position should still cross the original boundary normally");
        }

        beginTest ("two different tracks loop their own session clips independently");
        {
            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& trackA = project.addTrack (TrackType::audio, "A");
            auto& trackB = project.addTrack (TrackType::audio, "B");

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;

            mixer.launchSessionClip (trackA.getId(), clip, 0);

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;
            mixer.process (out, 0, midi, noInput, true, 0);

            expect (mixer.isSessionClipActive (trackA.getId()));
            expect (! mixer.isSessionClipActive (trackB.getId()));

            mixer.launchSessionClip (trackB.getId(), clip, 512);
            mixer.process (out, 512, midi, noInput, true, 512);

            expect (mixer.isSessionClipActive (trackA.getId()));
            expect (mixer.isSessionClipActive (trackB.getId()));
        }

        /*  Regression test for a Critical review finding: the session hand-off
            used to compare its boundary against `positionSamples` (Transport's
            block-start position, which freezes while stopped), instead of the
            SessionClock timeline `launchAtSample` is actually expressed on. A
            Session cell must be launchable "even while the arrangement is
            stopped" (SessionClock.h's own doc comment) - this drives Mixer at
            exactly that call shape: `positionSamples`/`isPlaying` frozen as if
            Transport were stopped, while only `sessionPositionSamples` (the new,
            separate parameter) advances - proving the two clocks are genuinely
            decoupled, not that they happen to agree because a test drove them
            with the same numbers. */
        beginTest ("session clip launches at its own clock's boundary even while transport is stopped");
        {
            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& track = project.addTrack (TrackType::audio, "Sampler");
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;

            const juce::int64 boundarySample = 1500;
            mixer.launchSessionClip (trackId, clip, boundarySample);

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            // Transport frozen at position 0, isPlaying == false, for every
            // block - exactly what AudioEngine keeps feeding Mixer::process()
            // while stopped. Only sessionPositionSamples moves.
            mixer.process (out, 0, midi, noInput, false, 0);
            expect (! mixer.isSessionClipActive (trackId));

            mixer.process (out, 0, midi, noInput, false, 512);
            expect (! mixer.isSessionClipActive (trackId));

            mixer.process (out, 0, midi, noInput, false, 1024);
            expect (mixer.isSessionClipActive (trackId),
                    "a session clip must launch off SessionClock's own advancing "
                    "position, not Transport's - which never moves while stopped");
        }

        /*  Regression test for Important finding I2: gatherSession's strict
            `sample < blockEnd` check meant a note-off clamped to exactly
            `lengthSamples` was silently dropped whenever a render block landed
            exactly on the loop boundary - permanently, since the cursor then
            reset for the next iteration without ever having delivered it. Uses
            the real BasicSynth::identifier instrument (always available, no
            external plugin needed - see PluginManager::createInstance) so the
            missing note-off is observable as audio that never releases, the
            same shape as BasicSynth's own "note-off releases to silence" test. */
        beginTest ("a MIDI session clip whose note-off lands exactly on the loop boundary still releases");
        {
            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            // 5625 bpm makes one beat exactly 512 samples at 48kHz, so a
            // 2-beat clip's own length (1024 samples) lands exactly on a
            // render block boundary - the precise alignment I2 needs.
            project.tempo.setEvents ({ { 0.0, 5625.0 } });

            auto& track = project.addTrack (TrackType::midi, "Synth");
            track.plugins.push_back ({ BasicSynth::identifier, "Basic Synth", false, true, {} });
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip clip;
            clip.kind = SessionClip::Kind::midi;
            clip.lengthBeats = 2.0;

            Note note;
            note.pitch = 60;
            note.startBeats = 0.0;
            note.lengthBeats = 2.0;   // fills the whole clip: its note-off lands exactly at lengthSamples
            note.velocity = 100;
            note.channel = 1;
            clip.notes.push_back (note);

            mixer.launchSessionClip (trackId, clip, 0);   // fires immediately

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            mixer.process (out, 0, midi, noInput, true, 0);      // note-on
            mixer.process (out, 512, midi, noInput, true, 512);  // crosses the loop boundary at 1024

            // Stop right away so no further loop iteration retriggers a fresh
            // note - isolates whether THIS note's own note-off was delivered.
            mixer.stopSessionClip (trackId, 1024);

            // Comfortably past BasicSynth's default 0.25s release tail.
            float lastMagnitude = 0.0f;

            for (int i = 0; i < 60; ++i)
            {
                out.clear();
                const auto sessionPos = 1024 + (juce::int64) i * 512;
                mixer.process (out, sessionPos, midi, noInput, true, sessionPos);
                lastMagnitude = out.getMagnitude (0, 0, out.getNumSamples());
            }

            expect (lastMagnitude < 1.0e-3f,
                    "a note whose note-off lands exactly on the clip's own loop boundary "
                    "should still release, not sustain forever");
        }

        /*  Regression test: MixerView::showPluginEditor used to call
            getPluginInstance() with the project's own track.plugins slot index -
            but pluginFx is compacted (whichever slot became the instrument is
            held separately, not left as a gap), so getPluginInstance() is a raw
            position into that compacted array. For any effect slot after an
            instrument slot, that either named the wrong plugin or returned
            nullptr, showing "Plugin not loaded" for a plugin that had in fact
            loaded fine. getPluginForSlot() is the slot-index-aware lookup that
            already existed for exactly this (Mixer.cpp's own automation
            resolution uses it) - this pins its behaviour down directly. */
        beginTest ("getPluginForSlot resolves the correct instance when an instrument precedes an effect slot");
        {
            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& track = project.addTrack (TrackType::midi, "Synth");
            track.plugins.push_back ({ BasicSynth::identifier, "Instrument", false, true, {} });
            track.plugins.push_back ({ BasicSynth::identifier, "Effect", false, false, {} });
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            auto* strip = mixer.getStripForTrack (trackId);
            expect (strip != nullptr);

            if (strip != nullptr)
            {
                expect (strip->getPluginForSlot (0) == strip->getInstrument(),
                        "slot 0 (the instrument) must resolve to getInstrument()");
                expect (strip->getPluginForSlot (1) != nullptr,
                        "slot 1 (the effect after the instrument) must still resolve to a live instance");
                expect (strip->getPluginForSlot (1) != strip->getInstrument(),
                        "the effect slot must not resolve to the instrument's instance");
            }
        }

        /*  Final-review regression: the loop-wrap fix above (addAllNotesOff at the
            wrap point) left two adjacent, identical defects unfixed - stopping a
            session clip, and replacing one with another on the same track, both
            discard whatever notes were sounding without ever sending their
            note-offs.  Same BasicSynth technique as the loop-boundary test above:
            a note that outlives the stop point is only observably "released" if
            the mixer actually stops rendering audio for it. */
        beginTest ("stopping a MIDI session clip releases any note still sounding, not just a note that loops");
        {
            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            project.tempo.setEvents ({ { 0.0, 120.0 } });

            auto& track = project.addTrack (TrackType::midi, "Synth");
            track.plugins.push_back ({ BasicSynth::identifier, "Basic Synth", false, true, {} });
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip clip;
            clip.kind = SessionClip::Kind::midi;
            clip.lengthBeats = 8.0;   // long clip - well short of its own note-off when stopped

            Note note;
            note.pitch = 60;
            note.startBeats = 0.0;
            note.lengthBeats = 8.0;   // fills the whole clip
            note.velocity = 100;
            note.channel = 1;
            clip.notes.push_back (note);

            mixer.launchSessionClip (trackId, clip, 0);   // fires immediately

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            mixer.process (out, 0, midi, noInput, true, 0);   // note-on
            expect (out.getMagnitude (0, 0, 512) > 0.01f, "the note should be sounding before the stop");

            // Stop long before the note's own note-off (at beat 8) would ever fire.
            mixer.stopSessionClip (trackId, 512);
            mixer.process (out, 512, midi, noInput, true, 512);   // crosses the stop boundary
            expect (! mixer.isSessionClipActive (trackId));

            // Comfortably past BasicSynth's default 0.25s release tail.
            float lastMagnitude = 0.0f;

            for (int i = 0; i < 60; ++i)
            {
                out.clear();
                const auto sessionPos = 1024 + (juce::int64) i * 512;
                mixer.process (out, sessionPos, midi, noInput, true, sessionPos);
                lastMagnitude = out.getMagnitude (0, 0, out.getNumSamples());
            }

            expect (lastMagnitude < 1.0e-3f,
                    "stopping a session clip must release any note it left sounding, not just "
                    "a note that happens to be silenced by a lucky loop wrap");
        }

        beginTest ("replacing an active session clip with another releases any note still sounding from the first");
        {
            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            project.tempo.setEvents ({ { 0.0, 120.0 } });

            auto& track = project.addTrack (TrackType::midi, "Synth");
            track.plugins.push_back ({ BasicSynth::identifier, "Basic Synth", false, true, {} });
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip first;
            first.kind = SessionClip::Kind::midi;
            first.lengthBeats = 8.0;

            Note note;
            note.pitch = 60;
            note.startBeats = 0.0;
            note.lengthBeats = 8.0;   // sustained well past where the replace happens
            note.velocity = 100;
            note.channel = 1;
            first.notes.push_back (note);

            mixer.launchSessionClip (trackId, first, 0);   // fires immediately

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            mixer.process (out, 0, midi, noInput, true, 0);   // note-on for the first clip
            expect (out.getMagnitude (0, 0, 512) > 0.01f, "the first clip's note should be sounding");

            // Replace with a silent second clip (no notes at all) before the first
            // clip's own note-off would ever fire - exactly the "switch candidate
            // scenes on the same track" shape Task 8 exists for.
            SessionClip second;
            second.kind = SessionClip::Kind::midi;
            second.lengthBeats = 8.0;
            mixer.launchSessionClip (trackId, second, 512);

            mixer.process (out, 512, midi, noInput, true, 512);   // crosses the replace boundary
            expect (mixer.isSessionClipActive (trackId));

            // Comfortably past BasicSynth's default 0.25s release tail.
            float lastMagnitude = 0.0f;

            for (int i = 0; i < 60; ++i)
            {
                out.clear();
                const auto sessionPos = 1024 + (juce::int64) i * 512;
                mixer.process (out, sessionPos, midi, noInput, true, sessionPos);
                lastMagnitude = out.getMagnitude (0, 0, out.getNumSamples());
            }

            expect (lastMagnitude < 1.0e-3f,
                    "replacing a session clip must release any note the OLD clip left "
                    "sounding - the new (silent) clip's own content never turns off a "
                    "note it didn't start");
        }

        /*  I4: every other session test above uses a zero-length (silent, no
            sourceFile) audio clip, so renderSessionAudio's actual sample-copy
            and wraparound logic has never been exercised by real content. */
        beginTest ("a looping audio session clip actually copies and wraps real sample content");
        {
            auto toneFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("ScoreSmithSessionAudioTest_"
                                               + juce::String (juce::Random::getSystemRandom().nextInt()) + ".wav");

            constexpr int toneLength = 200;   // shorter than the 512-sample block: wraps more than once per block
            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::FileOutputStream> stream (toneFile.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (
                    wav.createWriterFor (stream.get(), 48000.0, 1u, 16, {}, 0));
                stream.release();
                juce::AudioBuffer<float> tone (1, toneLength);
                for (int i = 0; i < toneLength; ++i)
                    tone.setSample (0, i, 0.8f);
                writer->writeFromAudioSampleBuffer (tone, 0, toneLength);
            }

            Settings settings;
            PluginManager pluginManager (settings);
            Mixer mixer (pluginManager);
            mixer.prepare (48000.0, 512, 2);

            Project project;
            auto& track = project.addTrack (TrackType::audio, "Sampler");
            const auto trackId = track.getId();

            mixer.setProject (&project);
            mixer.rebuild();

            SessionClip clip;
            clip.kind = SessionClip::Kind::audio;
            clip.sourceFile = toneFile;
            mixer.launchSessionClip (trackId, clip, 0);

            juce::AudioBuffer<float> out (2, 512);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> noInput;

            mixer.process (out, 0, midi, noInput, true, 0);
            expect (out.getMagnitude (0, 0, 512) > 0.3f,
                    "a looping audio session clip should be audible after wrapping mid-block");

            out.clear();
            mixer.process (out, 512, midi, noInput, true, 512);
            expect (out.getMagnitude (0, 0, 512) > 0.3f,
                    "the looped audio clip should still be audible many wraps later");

            mixer.setProject (nullptr);
            toneFile.deleteFile();
        }
    }
};

static MixerUnitTests mixerUnitTests;

}
