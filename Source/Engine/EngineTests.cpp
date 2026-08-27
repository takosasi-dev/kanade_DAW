#include "Engine/Transport.h"
#include "Engine/Recorder.h"
#include "Engine/SessionClock.h"

namespace ss
{
    /** Pure logic only - no audio device, no mixer.  Covers the two places where a
        mistake would silently drift the whole timeline: the tempo-map conversion and
        the loop wrap in Transport::advance. */
    class TransportTests : public juce::UnitTest
    {
    public:
        TransportTests() : juce::UnitTest ("Transport", "ScoreSmith") {}

        void runTest() override
        {
            Project project;
            Transport transport;
            transport.setProject (&project);
            transport.setSampleRate (48000.0);

            beginTest ("beat <-> sample round trip at a constant tempo");
            {
                project.tempo.setEvents ({ { 0.0, 120.0 } });

                for (const auto beats : { 0.0, 1.0, 3.5, 64.0, 127.75 })
                {
                    transport.setPositionBeats (beats);
                    expectWithinAbsoluteError (transport.getPositionBeats(), beats, 1.0e-4);
                }

                transport.setPositionBeats (4.0);   // 120 bpm -> 2 s -> 96000 samples
                expectEquals (transport.getPositionSamples(), (juce::int64) 96000);
                expectWithinAbsoluteError (transport.getPositionSeconds(), 2.0, 1.0e-9);
            }

            beginTest ("beat <-> sample round trip across a tempo change");
            {
                project.tempo.setEvents ({ { 0.0, 120.0 }, { 8.0, 60.0 } });

                // 8 beats at 120 bpm = 4 s, then 4 beats at 60 bpm = 4 s -> 8 s.
                transport.setPositionBeats (12.0);
                expectEquals (transport.getPositionSamples(), (juce::int64) 384000);
                expectWithinAbsoluteError (transport.getPositionBeats(), 12.0, 1.0e-4);

                for (const auto beats : { 0.5, 7.999, 8.0, 20.0 })
                {
                    transport.setPositionBeats (beats);
                    expectWithinAbsoluteError (transport.getPositionBeats(), beats, 1.0e-4);
                }
            }

            beginTest ("advance only moves while playing");
            {
                project.tempo.setEvents ({ { 0.0, 120.0 } });
                project.loopEnabled = false;
                transport.stop();
                transport.setPositionSamples (1000);

                expectEquals (transport.advance (512), (juce::int64) 1000);
                expectEquals (transport.getPositionSamples(), (juce::int64) 1000);

                transport.play();
                expectEquals (transport.advance (512), (juce::int64) 1000);
                expectEquals (transport.getPositionSamples(), (juce::int64) 1512);
            }

            beginTest ("loop wrap-around");
            {
                project.tempo.setEvents ({ { 0.0, 120.0 } });
                project.loopEnabled    = true;
                project.loopStartBeats = 4.0;    //  96000 samples
                project.loopEndBeats   = 8.0;    // 192000 samples
                transport.play();

                // a block that stops short of the loop end must not wrap
                transport.setPositionSamples (191000);
                expectEquals (transport.advance (512), (juce::int64) 191000);
                expectEquals (transport.getPositionSamples(), (juce::int64) 191512);

                // a block that straddles the loop end keeps its overshoot
                transport.setPositionSamples (192000 - 100);
                expectEquals (transport.advance (512), (juce::int64) 191900);
                expectEquals (transport.getPositionSamples(), (juce::int64) (96000 + 412));

                // playing before the loop is untouched by it
                transport.setPositionSamples (0);
                transport.advance (512);
                expectEquals (transport.getPositionSamples(), (juce::int64) 512);

                // a degenerate loop is ignored rather than dividing by zero
                project.loopEndBeats = project.loopStartBeats;
                transport.setPositionSamples (192000);
                transport.advance (512);
                expectEquals (transport.getPositionSamples(), (juce::int64) 192512);
            }

            beginTest ("looping never leaves the loop");
            {
                project.loopEnabled    = true;
                project.loopStartBeats = 0.0;
                project.loopEndBeats   = 2.0;    // 48000 samples
                transport.setPositionSamples (0);
                transport.play();

                for (int block = 0; block < 300; ++block)
                {
                    const auto blockStart = transport.advance (441);
                    expect (blockStart >= 0 && blockStart < 48000);
                }
            }

            transport.stop();
            transport.setProject (nullptr);
        }
    };

    static TransportTests transportTests;

    /** Take lengths.  The recorder is the one place where "how long was that?" has
        two different answers - what the transport covered and what reached the
        disk - and only the second one describes the file the clip points at. */
    class RecorderTests : public juce::UnitTest
    {
    public:
        RecorderTests() : juce::UnitTest ("Recorder", "ScoreSmith") {}

        void runTest() override
        {
            auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("ScoreSmithRecorderTests");
            folder.createDirectory();

            /*  One second of input, written in blocks well inside the FIFO, so the
                writer thread cannot turn this into a dropped-block test by being
                slow.  Returns the take length in beats, or -1 for no take.       */
            const auto recordOneSecond = [&folder] (const TempoMap& tempo, double startBeat,
                                                    double endBeatToClaim)
            {
                Recorder recorder;
                const auto file = folder.getChildFile ("take.wav").getNonexistentSibling();

                if (! recorder.start ({ { 1, 0, file } }, 48000.0, 24, startBeat).wasOk())
                    return -1.0;

                juce::AudioBuffer<float> input (1, 500);
                input.clear();

                for (int block = 0; block < 96; ++block)     // 96 * 500 == 48000 == 1 s
                    recorder.processBlock (input, 500);

                const auto takes = recorder.finish (endBeatToClaim, tempo);
                file.deleteFile();

                if (takes.size() != 1)
                    return -1.0;

                return takes.front().lengthBeats;
            };

            beginTest ("a take is as long as what was written, not as long as the transport ran");
            {
                TempoMap tempo;
                tempo.setEvents ({ { 0.0, 120.0 } });

                // 1 s at 120 bpm is 2 beats, whatever endBeat claims.
                expectWithinAbsoluteError (recordOneSecond (tempo, 8.0, 999.0), 2.0, 1.0e-3);
                expectWithinAbsoluteError (recordOneSecond (tempo, 8.0, 8.0), 2.0, 1.0e-3);
            }

            beginTest ("take length goes through the tempo map");
            {
                TempoMap fast;
                fast.setEvents ({ { 0.0, 240.0 } });

                // The same second of audio is twice as many beats at twice the tempo.
                expectWithinAbsoluteError (recordOneSecond (fast, 0.0, 999.0), 4.0, 1.0e-3);

                // ...and a take that starts after a tempo change is measured there,
                // not at the start of the map.
                TempoMap changing;
                changing.setEvents ({ { 0.0, 120.0 }, { 4.0, 240.0 } });

                expectWithinAbsoluteError (recordOneSecond (changing, 0.0, 999.0), 2.0, 1.0e-3);
                expectWithinAbsoluteError (recordOneSecond (changing, 8.0, 999.0), 4.0, 1.0e-3);
            }

            beginTest ("finishing a recorder that never started yields nothing");
            {
                Recorder recorder;
                TempoMap tempo;
                expect (recorder.finish (4.0, tempo).empty());
                expect (! recorder.isRecording());
            }

            folder.deleteRecursively();
        }
    };

    static RecorderTests recorderTests;

    class SessionClockTests : public juce::UnitTest
    {
    public:
        SessionClockTests() : juce::UnitTest ("SessionClock", "ScoreSmith") {}

        void runTest() override
        {
            TempoMap tempo;

            beginTest ("nextBarBoundarySample returns the current sample when already on a boundary");
            {
                tempo.setEvents ({ { 0.0, 120.0 } });
                tempo.setTimeSignatures ({ { 0.0, 4, 4 } });

                SessionClock clock;
                clock.prepare (48000.0);

                expectEquals (clock.nextBarBoundarySample (tempo), (juce::int64) 0);
            }

            beginTest ("nextBarBoundarySample computes the next 1-bar boundary at 120bpm 4/4");
            {
                tempo.setEvents ({ { 0.0, 120.0 } });
                tempo.setTimeSignatures ({ { 0.0, 4, 4 } });

                SessionClock clock;
                clock.prepare (48000.0);

                // 120bpm -> 0.5s/beat, 4/4 -> 4 beats/bar -> 2s/bar -> 96000 samples/bar.
                clock.advance (48000);   // 1s in = halfway through the first bar

                expectEquals (clock.nextBarBoundarySample (tempo), (juce::int64) 96000);
            }

            beginTest ("nextBarBoundarySample respects a 3/4 time signature");
            {
                tempo.setEvents ({ { 0.0, 120.0 } });
                tempo.setTimeSignatures ({ { 0.0, 3, 4 } });

                SessionClock clock;
                clock.prepare (48000.0);

                // 3/4 at 120bpm -> 3 beats/bar -> 1.5s/bar -> 72000 samples/bar.
                clock.advance (36000);   // halfway through the first bar

                expectEquals (clock.nextBarBoundarySample (tempo), (juce::int64) 72000);
            }

            beginTest ("advance accumulates and reset returns to zero, independent of any transport");
            {
                SessionClock clock;
                clock.prepare (48000.0);

                clock.advance (100);
                clock.advance (200);
                expectEquals (clock.currentSample(), (juce::int64) 300);

                clock.reset();
                expectEquals (clock.currentSample(), (juce::int64) 0);
            }
        }
    };

    static SessionClockTests sessionClockTests;
}
