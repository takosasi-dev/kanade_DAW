#pragma once
#include "Core/Project.h"
#include "AI/MusicTheory.h"
#include <functional>

namespace ss
{
    /** MIDI -> music (spec 8.1).

        Generation is a seeded, rule-and-corpus driven arranger: harmonic
        analysis of the input, genre-specific rhythm/voicing templates, then a
        constrained random walk.  Every run is reproducible from `seed`, so the
        A/B compare and "regenerate this part only" flows in 9.4/9.5 work.

        The spec's Transformer plan (10.2) plugs in at generatePart(): swap the
        template lookup for model sampling and the surrounding UI is unchanged. */
    class Generator
    {
    public:
        Generator();
        ~Generator();

        enum class Mode { autoArrange, continuation, styleTransfer, harmonise,
                          humanise, accompaniment, drumPattern };

        enum class Part { drums, bass, chords, pad, arpeggio, counterMelody, strings, lead };
        static juce::String toString (Part);
        static juce::String toString (Mode);

        struct Options
        {
            Mode   mode = Mode::autoArrange;
            Genre  genre = Genre::pop;
            double complexity  = 0.5;   // 0..1  density / syncopation
            double creativity  = 0.5;   // 0..1  how far from the template
            double humanizeTiming   = 0.0;   // 0..1
            double humanizeVelocity = 0.0;   // 0..1
            std::vector<Part> parts { Part::drums, Part::bass, Part::chords };
            theory::Key key;
            bool   useDetectedKey = true;
            double lengthBeats = 32.0;
            int    numCandidates = 4;
            juce::int64 seed = 0;       // 0 == random
            juce::StringArray moodTags; // "bright", "melancholic", "driving"...
        };

        /** One generated candidate for the gallery (spec 9.4). */
        struct Candidate
        {
            juce::String name;
            /** Free text for the gallery to show beside the thumbnail: which
                parts were generated, the seed, and any accuracy caveat the mode
                carries (spec 8.1 wants style transfer's limits visible).  The
                name stays a name - do not smuggle warnings into it.           */
            juce::String notes;
            juce::int64  seed = 0;
            std::vector<std::pair<Part, MidiClip>> parts;
            float noveltyScore = 0.0f;   // distance from the source material
        };

        struct Input
        {
            std::vector<Note>       melody;
            std::vector<ChordEvent> chords;
            double bpm = 120.0;
            TimeSignatureEvent timeSignature;
        };

        /** Progress callback: gets 0..1, returns false to abort.  Aborting is
            cooperative - the DSP loops check it and bail out at the next
            checkpoint rather than running to completion.                      */
        using ProgressFn = std::function<bool (float)>;

        /** Blocking; call from a background thread.  An aborted run returns the
            candidates that were already finished, so the caller can tell a
            cancellation from a run that legitimately produced nothing.        */
        std::vector<Candidate> generate (const Input&, const Options&, ProgressFn progress = {});

        /** Re-rolls a single part of an existing candidate, leaving the rest. */
        MidiClip regeneratePart (const Input&, const Options&, Part, juce::int64 seed);

        /** In-place transforms - used by both the generator and the piano roll's
            right-click menu, so they take plain note lists. */
        static void humanise (std::vector<Note>&, double timingAmount, double velocityAmount,
                              double bpm, const TimeSignatureEvent&, juce::int64 seed);

        /** Groove accent multiplier (0..1) for a position in the bar, following
            the meter: bar downbeat strongest, then the mid-bar pulse, then the
            other pulses, then the subdivisions.  Public so the humanise grid can
            be pinned by a test without going through the random jitter.       */
        static double grooveAccent (double beat, const TimeSignatureEvent&);
        static std::vector<ChordEvent> harmonise (const std::vector<Note>& melody,
                                                  const theory::Key&, double complexity,
                                                  double barLengthBeats);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
