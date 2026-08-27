#include "AI/Transcriber.h"
#include "AI/Generator.h"

#include <algorithm>
#include <cmath>

/*  Guards for the pure-logic half of the AI module: the pitch tracker against
    synthesised tones of known frequency, the chord-symbol round-trip, key
    estimation, and the seeded reproducibility that the whole candidate gallery
    and per-part re-roll depend on.

    Run with juce::UnitTestRunner (category "AI").                             */

namespace ss
{

class AITests final : public juce::UnitTest
{
public:
    AITests() : juce::UnitTest ("ScoreSmith AI", "AI") {}

    void runTest() override
    {
        testPitchTracking();
        testChordSymbols();
        testKeyEstimation();
        testGeneratorDeterminism();
        testGrooveAccentsFollowTheMeter();
        testCandidateNotes();
        testSuggestionSplices();
        testProgressCallbackAborts();
    }

private:
    //==========================================================================
    /** Sine sweep from startHz to endHz; pass the same value twice for a
        steady tone.  Short fades keep the ends from clicking. */
    static juce::AudioBuffer<float> makeTone (double startHz, double endHz,
                                              double seconds, double sampleRate)
    {
        const auto numSamples = juce::jmax (2, (int) std::round (seconds * sampleRate));
        const auto fade = juce::jmax (1, (int) (sampleRate * 0.005));

        juce::AudioBuffer<float> buffer (1, numSamples);
        auto* out = buffer.getWritePointer (0);

        double phase = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto t = (double) i / (double) numSamples;
            const auto hz = startHz * std::pow (endHz / startHz, t);   // constant semitones/second

            phase += juce::MathConstants<double>::twoPi * hz / sampleRate;

            auto gain = 0.5;

            if (i < fade)                  gain *= (double) i / (double) fade;
            if (i > numSamples - fade)     gain *= (double) (numSamples - i) / (double) fade;

            out[i] = (float) (gain * std::sin (phase));
        }

        return buffer;
    }

    static Transcriber::Options monoOptions()
    {
        Transcriber::Options o;
        o.mode             = Transcriber::Mode::monophonic;
        o.quantise         = Quantise::off;
        o.detectTempo      = false;
        o.detectSwing      = false;
        o.confidenceFloor  = 0.05f;
        o.minNoteLengthMs  = 40.0;
        return o;
    }

    //==========================================================================
    void testPitchTracking()
    {
        beginTest ("YIN recovers known pitches");

        constexpr double sampleRate = 44100.0;

        Transcriber transcriber;
        const auto options = monoOptions();

        struct TestCase { const char* name; double hz; int expectedPitch; };

        const TestCase cases[]
        {
            { "A440", 440.0,    69 },   // A4
            { "C3",   130.8128, 48 }    // C3, well below the STFT's bin resolution
        };

        for (const auto& c : cases)
        {
            const auto tone = makeTone (c.hz, c.hz, 1.5, sampleRate);
            const auto result = transcriber.transcribe (tone, sampleRate, options);

            expect (! result.notes.empty(), juce::String (c.name) + ": nothing detected");

            if (result.notes.empty())
                continue;

            const auto longest = *std::max_element (result.notes.begin(), result.notes.end(),
                                                    [] (const Note& a, const Note& b)
                                                    { return a.lengthBeats < b.lengthBeats; });

            expectEquals (longest.pitch, c.expectedPitch, juce::String (c.name) + ": wrong pitch");
            expect (longest.confidence > 0.6f,
                    juce::String (c.name) + ": a pure tone should be tracked confidently");
        }

        // A glide has to break into several notes - that is pitchBendTolerance
        // doing its job rather than one smeared note across an octave.
        const auto glide = makeTone (220.0, 440.0, 2.0, sampleRate);
        const auto result = transcriber.transcribe (glide, sampleRate, options);

        expect (result.notes.size() >= 3, "a one-octave glide should segment into several notes");

        if (result.notes.size() >= 2)
        {
            expect (result.notes.front().pitch >= 55 && result.notes.front().pitch <= 59,
                    "glide should start around A3 (57), got "
                        + juce::String (result.notes.front().pitch));
            expect (result.notes.back().pitch >= 67 && result.notes.back().pitch <= 71,
                    "glide should end around A4 (69), got "
                        + juce::String (result.notes.back().pitch));
        }
    }

    //==========================================================================
    void testChordSymbols()
    {
        beginTest ("chord symbols round-trip");

        const char* const symbols[]
        {
            "C", "Cm", "Cmaj7", "Am", "G7", "F#m7b5", "Ddim7", "Esus4",
            "Bb", "A#m9", "C/E", "Gsus2", "D13", "Faug", "C7sus4", "Emmaj7"
        };

        for (const auto* text : symbols)
        {
            ChordEvent chord;
            expect (theory::parseChordSymbol (text, chord), juce::String ("could not parse ") + text);

            const auto formatted = theory::chordSymbolFor (chord.root, chord.intervals);

            ChordEvent again;
            expect (theory::parseChordSymbol (formatted, again), "could not re-parse " + formatted);
            expectEquals (again.root, chord.root, "root changed: " + juce::String (text) + " -> " + formatted);
            expect (again.intervals == chord.intervals,
                    "intervals changed: " + juce::String (text) + " -> " + formatted);
        }

        // Flat spellings come back as flats when the caller asks for them - the
        // whole point of the preferFlats parameter (gap 8).
        struct FlatCase { const char* text; int root; };

        const FlatCase flatCases[] { { "Bb", 10 }, { "Ebm7", 3 }, { "Abmaj7", 8 }, { "Dbsus4", 1 } };

        for (const auto& f : flatCases)
        {
            ChordEvent parsed;
            expect (theory::parseChordSymbol (f.text, parsed), juce::String ("could not parse ") + f.text);
            expectEquals (parsed.root, f.root);

            const auto flat = theory::chordSymbolFor (parsed.root, parsed.intervals, true);
            expectEquals (flat, juce::String (f.text), "flat spelling was not preserved");

            // ...and it has to parse back to exactly the same chord.
            ChordEvent again;
            expect (theory::parseChordSymbol (flat, again), "could not re-parse " + flat);
            expectEquals (again.root, parsed.root);
            expect (again.intervals == parsed.intervals, "intervals changed round-tripping " + flat);

            // Sharps stay the default when nobody says otherwise.
            expect (theory::chordSymbolFor (parsed.root, parsed.intervals) != flat,
                    juce::String (f.text) + " should spell with sharps by default");
        }

        // A slash bass is spelled from the same setting as the root.
        ChordEvent slash;
        expect (theory::parseChordSymbol ("Bb/D", slash));
        expectEquals (theory::chordSymbolFor (slash.root, slash.intervals, true), juce::String ("Bb/D"));

        // Which spelling a key wants (flat keys: F, Bb, Eb, Ab, Db and modes).
        expect (theory::keyPrefersFlats ({ 10, theory::ScaleType::major }), "Bb major is a flat key");
        expect (theory::keyPrefersFlats ({ 5,  theory::ScaleType::major }), "F major is a flat key");
        expect (theory::keyPrefersFlats ({ 7,  theory::ScaleType::naturalMinor }), "G minor is a flat key");
        expect (theory::keyPrefersFlats ({ 7,  theory::ScaleType::dorian }), "G dorian folds onto F major");
        expect (! theory::keyPrefersFlats ({ 2, theory::ScaleType::dorian }), "D dorian folds onto C major");
        expect (! theory::keyPrefersFlats ({ 7, theory::ScaleType::major }), "G major is a sharp key");
        expect (! theory::keyPrefersFlats ({ 0, theory::ScaleType::major }), "C major has no accidentals");

        // Generated chords - the case the parse-time spelling could never cover.
        const auto inBbMajor = theory::diatonicChords ({ 10, theory::ScaleType::major }, false);
        expectEquals ((int) inBbMajor.size(), 7, "expected seven diatonic chords");
        expectEquals (inBbMajor[0].symbol, juce::String ("Bb"), "I of Bb major");
        expectEquals (inBbMajor[3].symbol, juce::String ("Eb"), "IV of Bb major");

        // Sharp spellings survive verbatim.
        ChordEvent chord;
        expect (theory::parseChordSymbol ("F#m7b5", chord));
        expectEquals (chord.root, 6);
        expectEquals (theory::chordSymbolFor (chord.root, chord.intervals), juce::String ("F#m7b5"));

        // A slash bass survives as a negative interval below the root.
        expect (theory::parseChordSymbol ("C/E", chord));
        expectEquals (theory::chordSymbolFor (chord.root, chord.intervals), juce::String ("C/E"));

        expect (! theory::parseChordSymbol ("Hmm", chord), "nonsense should not parse as a chord");
        expect (! theory::parseChordSymbol ("", chord), "an empty string should not parse as a chord");
    }

    //==========================================================================
    void testKeyEstimation()
    {
        beginTest ("estimateKey correlates against the K-S profiles");

        const auto scaleFrom = [] (int tonic)
        {
            std::vector<Note> notes;
            const int degrees[] { 0, 2, 4, 5, 7, 9, 11, 12 };
            double beat = 0.0;

            for (auto d : degrees)
            {
                Note n;
                n.pitch       = 60 + tonic + d;
                n.startBeats  = beat;
                n.lengthBeats = 1.0;
                notes.push_back (n);
                beat += 1.0;
            }

            notes.back().lengthBeats = 4.0;     // land on the tonic
            return notes;
        };

        const auto c = theory::estimateKey (scaleFrom (0));
        expectEquals (c.tonic, 0, "expected C, got " + theory::toString (c));
        expect (c.scale == theory::ScaleType::major, "expected major, got " + theory::toString (c));

        // Transposing the collection must transpose the answer - this is what
        // catches an implementation that always says C major.
        const auto g = theory::estimateKey (scaleFrom (7));
        expectEquals (g.tonic, 7, "expected G, got " + theory::toString (g));
        expect (g.scale == theory::ScaleType::major, "expected major, got " + theory::toString (g));

        expectEquals (theory::estimateKey ({}).tonic, 0, "an empty note list should not crash");
    }

    //==========================================================================
    static bool sameClip (const MidiClip& a, const MidiClip& b)
    {
        if (a.notes.size() != b.notes.size())
            return false;

        for (size_t i = 0; i < a.notes.size(); ++i)
        {
            const auto& x = a.notes[i];
            const auto& y = b.notes[i];

            if (x.pitch != y.pitch || x.velocity != y.velocity
                || std::abs (x.startBeats  - y.startBeats)  > 1.0e-9
                || std::abs (x.lengthBeats - y.lengthBeats) > 1.0e-9)
                return false;
        }

        return true;
    }

    static bool sameCandidate (const Generator::Candidate& a, const Generator::Candidate& b)
    {
        if (a.parts.size() != b.parts.size())
            return false;

        for (size_t i = 0; i < a.parts.size(); ++i)
            if (a.parts[i].first != b.parts[i].first || ! sameClip (a.parts[i].second, b.parts[i].second))
                return false;

        return true;
    }

    static bool sameRun (const std::vector<Generator::Candidate>& a,
                         const std::vector<Generator::Candidate>& b)
    {
        if (a.size() != b.size())
            return false;

        for (size_t i = 0; i < a.size(); ++i)
            if (! sameCandidate (a[i], b[i]))
                return false;

        return true;
    }

    void testGeneratorDeterminism()
    {
        beginTest ("generate is reproducible from its seed");

        Generator::Input input;
        input.bpm = 120.0;

        const int melody[] { 60, 62, 64, 65, 67, 65, 64, 62 };
        double beat = 0.0;

        for (auto p : melody)
        {
            Note n;
            n.pitch       = p;
            n.startBeats  = beat;
            n.lengthBeats = 1.0;
            n.velocity    = 90;
            input.melody.push_back (n);
            beat += 1.0;
        }

        Generator::Options options;
        options.genre         = Genre::pop;
        options.seed          = 12345;
        options.numCandidates = 3;
        options.lengthBeats   = 16.0;
        options.parts         = { Generator::Part::drums, Generator::Part::bass, Generator::Part::chords };

        Generator generator;

        const auto first  = generator.generate (input, options);
        const auto repeat = generator.generate (input, options);

        expectEquals ((int) first.size(), 3);
        expect (sameRun (first, repeat), "the same seed produced different music");

        auto otherSeed = options;
        otherSeed.seed = 999;

        expect (! sameRun (first, generator.generate (input, otherSeed)),
                "different seeds produced identical music");

        expect (first.size() < 2 || ! sameCandidate (first[0], first[1]),
                "candidates within one run must differ from each other");

        // Everything we wrote ourselves is certain by definition (spec 9.3).
        for (const auto& candidate : first)
            for (const auto& part : candidate.parts)
            {
                expect (! part.second.notes.empty(),
                        Generator::toString (part.first) + " generated nothing");

                for (const auto& n : part.second.notes)
                    expect (n.confidence >= 1.0f, "generated notes must carry full confidence");
            }

        // Re-rolling one part has to reproduce exactly what generate() made, or
        // the "regenerate this part only" flow in 9.4 silently changes the rest.
        const auto rerolled = generator.regeneratePart (input, options, Generator::Part::bass, first[0].seed);

        const MidiClip* originalBass = nullptr;

        for (const auto& part : first[0].parts)
            if (part.first == Generator::Part::bass)
                originalBass = &part.second;

        expect (originalBass != nullptr, "no bass part was generated");

        if (originalBass != nullptr)
            expect (sameClip (*originalBass, rerolled),
                    "regeneratePart did not reproduce the part generate() made");
    }
    //==========================================================================
    void testGrooveAccentsFollowTheMeter()
    {
        beginTest ("humanise accents follow the time signature");

        const auto accent = [] (double beat, int num, int den)
        {
            TimeSignatureEvent ts;
            ts.numerator   = num;
            ts.denominator = den;
            return Generator::grooveAccent (beat, ts);
        };

        // 4/4: bar downbeat strongest, beat 3 next, then the offbeats.
        expectWithinAbsoluteError (accent (0.0, 4, 4), 1.0, 1.0e-9);
        expect (accent (2.0, 4, 4) > accent (1.0, 4, 4), "beat 3 outranks beat 2 in 4/4");
        expect (accent (1.0, 4, 4) > accent (0.5, 4, 4), "a beat outranks the 'and' in 4/4");
        expect (accent (0.5, 4, 4) > accent (0.25, 4, 4), "the 'and' outranks the 'e' in 4/4");

        // 3/4 is the case the 4/4-only grid got wrong: every bar downbeat must
        // score 1.0, not just the first one, and nothing in the bar may beat it.
        for (int bar = 0; bar < 4; ++bar)
        {
            const auto barStart = 3.0 * bar;

            expectWithinAbsoluteError (accent (barStart, 3, 4), 1.0, 1.0e-9,
                                       "bar " + juce::String (bar + 1) + " of 3/4 lost its downbeat");

            expect (accent (barStart + 1.0, 3, 4) < accent (barStart, 3, 4),
                    "beat 2 must not outrank the downbeat in 3/4");
            expect (accent (barStart + 2.0, 3, 4) < accent (barStart, 3, 4),
                    "beat 3 must not outrank the downbeat in 3/4");
            expect (accent (barStart + 0.5, 3, 4) < accent (barStart + 1.0, 3, 4),
                    "the 'and' must not outrank a beat in 3/4");
        }

        // The two beats of a 3/4 bar that are not the downbeat are peers - the
        // old fixed table weighted them differently depending on the bar.
        expectWithinAbsoluteError (accent (1.0, 3, 4), accent (2.0, 3, 4), 1.0e-9);
        expectWithinAbsoluteError (accent (1.0, 3, 4), accent (4.0, 3, 4), 1.0e-9);

        // 6/8 pulses in dotted quarters: beats 0 and 1.5, not every eighth.
        expectWithinAbsoluteError (accent (0.0, 6, 8), 1.0, 1.0e-9);
        expect (accent (1.5, 6, 8) > accent (1.0, 6, 8), "6/8 pulses on the dotted quarter");
        expect (accent (1.5, 6, 8) > accent (0.5, 6, 8), "6/8 pulses on the dotted quarter");
        expectWithinAbsoluteError (accent (3.0, 6, 8), 1.0, 1.0e-9, "6/8 bar 2 lost its downbeat");

        // End to end: a downbeat still comes out louder than the sixteenth after
        // it once the random jitter is piled on top.
        std::vector<Note> notes;

        for (int i = 0; i < 12; ++i)
        {
            Note n;
            n.pitch       = 60;
            n.startBeats  = 0.25 * i;
            n.lengthBeats = 0.25;
            n.velocity    = 100;
            notes.push_back (n);
        }

        TimeSignatureEvent threeFour;
        threeFour.numerator = 3;

        auto humanised = notes;
        Generator::humanise (humanised, 0.0, 1.0, 120.0, threeFour, 4242);

        expectEquals ((int) humanised.size(), 12, "humanise must not add or drop notes");
        expect (humanised[0].velocity > humanised[1].velocity,
                "the bar downbeat should be louder than the sixteenth after it");
        expect (humanised[4].velocity > humanised[5].velocity,
                "beat 2 should be louder than the sixteenth after it");

        // Timing 0 must leave positions alone, or "velocity only" is a lie.
        for (size_t i = 0; i < humanised.size(); ++i)
            expectWithinAbsoluteError (humanised[i].startBeats, notes[i].startBeats, 1.0e-12);
    }

    //==========================================================================
    static Generator::Input simpleInput()
    {
        Generator::Input input;
        input.bpm = 120.0;

        const int melody[] { 60, 62, 64, 65, 67, 65, 64, 62 };
        double beat = 0.0;

        for (auto p : melody)
        {
            Note n;
            n.pitch       = p;
            n.startBeats  = beat;
            n.lengthBeats = 1.0;
            n.velocity    = 90;
            input.melody.push_back (n);
            beat += 1.0;
        }

        return input;
    }

    void testCandidateNotes()
    {
        beginTest ("candidates carry their notes outside the name");

        Generator generator;
        const auto input = simpleInput();

        Generator::Options options;
        options.seed          = 7;
        options.numCandidates = 2;
        options.lengthBeats   = 16.0;
        options.parts         = { Generator::Part::drums, Generator::Part::bass };

        for (const auto& c : generator.generate (input, options))
        {
            expect (c.notes.isNotEmpty(), "a candidate with no notes text");
            expect (c.notes.contains (juce::String (c.seed)), "the notes must name the seed");
            expect (c.notes.contains (Generator::toString (Generator::Part::drums))
                     && c.notes.contains (Generator::toString (Generator::Part::bass)),
                    "the notes must list the generated parts");
        }

        // Style transfer's caveat belongs in the notes and must be out of the
        // name - that was the whole reason for the field (spec 8.1, gap 6).
        auto styleTransfer = options;
        styleTransfer.mode = Generator::Mode::styleTransfer;

        for (const auto& c : generator.generate (input, styleTransfer))
        {
            expect (c.notes.containsIgnoreCase ("experimental"),
                    "style transfer must warn about its accuracy");
            expect (! c.name.containsIgnoreCase ("experimental"),
                    "the warning must not be jammed into the candidate name");
        }
    }

    //==========================================================================
    void testSuggestionSplices()
    {
        beginTest ("suggestions replace N notes with M");

        // Two same-pitch notes with a hairline gap - the merge case.
        Transcriber::Result result;

        Note a;
        a.pitch = 64; a.startBeats = 0.0; a.lengthBeats = 1.0; a.confidence = 0.4f;

        Note b = a;
        b.startBeats  = 1.05;
        b.lengthBeats = 0.95;

        result.notes = { a, b };
        result.key   = { 0, theory::ScaleType::major };

        Transcriber transcriber;
        const auto suggestions = transcriber.suggestFixes (result, 0);

        const Transcriber::Suggestion* merge = nullptr;

        for (const auto& s : suggestions)
        {
            // Every suggestion must describe a splice that can actually be applied.
            expect (! s.replacements.empty(), "a suggestion with nothing to put back");
            expect (s.firstIndex >= 0 && s.firstIndex + s.count <= (int) result.notes.size(),
                    "a suggestion that would splice outside the note list");

            if (s.label.containsIgnoreCase ("merge"))
                merge = &s;
        }

        expect (merge != nullptr, "no merge suggestion for two abutting same-pitch notes");

        if (merge != nullptr)
        {
            expectEquals (merge->count, 2, "merge has to consume both notes");
            expectEquals ((int) merge->replacements.size(), 1, "merge produces one note");
            expectEquals (merge->replacements[0].pitch, 64);
            expectWithinAbsoluteError (merge->replacements[0].startBeats, 0.0, 1.0e-9);
            expectWithinAbsoluteError (merge->replacements[0].endBeats(), b.endBeats(), 1.0e-9);
        }

        // Pitch fixes stay 1 -> 1 and leave the timing alone.
        Transcriber::Result octaveError;
        Note stray;
        stray.pitch = 84; stray.startBeats = 1.0; stray.lengthBeats = 1.0; stray.confidence = 0.2f;

        octaveError.notes = { a, stray, a };
        octaveError.notes[2].startBeats = 2.0;
        octaveError.key = { 0, theory::ScaleType::major };

        bool sawOctaveFix = false;

        for (const auto& s : transcriber.suggestFixes (octaveError, 1))
        {
            expectEquals (s.firstIndex, 1, "a fix for note 1 must splice at note 1");

            if (s.label.containsIgnoreCase ("octave down"))
            {
                sawOctaveFix = true;
                expectEquals (s.count, 1);
                expectEquals ((int) s.replacements.size(), 1);
                expectEquals (s.replacements[0].pitch, 72);
                expectWithinAbsoluteError (s.replacements[0].startBeats, 1.0, 1.0e-9);
                expectWithinAbsoluteError (s.replacements[0].lengthBeats, 1.0, 1.0e-9);
            }
        }

        expect (sawOctaveFix, "an octave-high outlier should offer an octave-down fix");
        expect (transcriber.suggestFixes (result, 99).empty(), "an out-of-range index should be empty");
        expect (transcriber.suggestFixes (result, -1).empty(), "a negative index should be empty");
    }

    //==========================================================================
    void testProgressCallbackAborts()
    {
        beginTest ("returning false from the progress callback aborts");

        constexpr double sampleRate = 44100.0;

        Transcriber transcriber;
        const auto options = monoOptions();
        const auto tone = makeTone (440.0, 440.0, 6.0, sampleRate);

        int fullCalls = 0;
        const auto full = transcriber.transcribe (tone, sampleRate, options,
                                                  [&fullCalls] (float) { ++fullCalls; return true; });

        expect (! full.cancelled, "a run nobody cancelled must not report itself cancelled");
        expect (! full.notes.empty(), "the reference run found nothing to compare against");
        expect (fullCalls > 4, "the progress callback should be called throughout the run");

        // Stop at the first checkpoint past the start.
        int abortedCalls = 0;
        const auto stopped = transcriber.transcribe (tone, sampleRate, options,
                                                     [&abortedCalls] (float)
                                                     {
                                                         return ++abortedCalls < 2;
                                                     });

        expect (stopped.cancelled, "the aborted run did not report itself cancelled");
        expect (stopped.notes.empty(), "an aborted run must not hand back half a transcription");
        expectEquals (abortedCalls, 2, "the callback must not be asked again after it says stop");
        expect (abortedCalls < fullCalls, "aborting did not shorten the run");

        // An empty callback still means "carry on".
        expect (! transcriber.transcribe (tone, sampleRate, options).cancelled,
                "no callback at all must not read as a cancellation");

        //--- the generator side ------------------------------------------------
        Generator generator;
        const auto input = simpleInput();

        Generator::Options genOptions;
        genOptions.seed          = 31;
        genOptions.numCandidates = 3;
        genOptions.lengthBeats   = 16.0;
        genOptions.parts         = { Generator::Part::drums, Generator::Part::bass, Generator::Part::chords };

        int genCalls = 0;
        const auto everything = generator.generate (input, genOptions,
                                                    [&genCalls] (float) { ++genCalls; return true; });

        expectEquals ((int) everything.size(), 3);

        int stopAfter = 0;
        const auto partial = generator.generate (input, genOptions,
                                                 [&stopAfter] (float) { return ++stopAfter < 2; });

        expect ((int) partial.size() < (int) everything.size(),
                "aborting the generator still produced every candidate");
        expectEquals (stopAfter, 2, "the generator kept calling back after it was told to stop");
    }
};

static AITests aiTests;

} // namespace ss
