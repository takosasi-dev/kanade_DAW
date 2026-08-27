#pragma once
#include "Core/Project.h"
#include "Core/Settings.h"        // StemSeparator holds a Settings&
#include "AI/MusicTheory.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>

namespace ss
{
    /** Audio -> MIDI (spec 8.2).

        The shipped engine is a DSP pipeline (onset detection + pYIN-style pitch
        tracking + note segmentation), which is what actually runs today for
        monophonic material.  Polyphonic material goes through the same
        segmentation fed by a multi-F0 salience function; its confidence scores
        come out lower on purpose, and 15.2 in the spec is honest about the
        ceiling here.                                                          */
    class Transcriber
    {
    public:
        Transcriber();
        ~Transcriber();

        enum class Mode { monophonic, polyphonic, drums, automatic };

        struct Options
        {
            Mode     mode = Mode::automatic;
            double   sensitivity   = 0.5;   // 0..1 onset threshold, higher = more notes
            double   minNoteLengthMs = 60.0;
            int      minPitch = 21, maxPitch = 108;
            Quantise quantise = Quantise::sixteenth;
            double   quantiseStrength = 0.6;
            bool     detectSwing = true;
            bool     detectTempo = true;
            bool     detectKey   = true;
            double   pitchBendTolerance = 0.5;   // semitones before a new note starts
            float    confidenceFloor = 0.15f;    // notes below this are dropped
        };

        struct Result
        {
            std::vector<Note> notes;
            double estimatedBpm = 0.0;
            double swingRatio   = 0.5;      // 0.5 == straight
            theory::Key key;
            float  meanConfidence = 0.0f;
            juce::String message;           // warnings, e.g. "polyphonic - review pitches"
            bool   cancelled = false;       // the progress callback asked to stop
        };

        /** Progress callback: gets 0..1, returns false to abort.  Aborting is
            cooperative - the analysis loops check it at their existing progress
            checkpoints and return early with `Result::cancelled` set, instead of
            burning minutes of CPU on a result nobody is waiting for.          */
        using ProgressFn = std::function<bool (float)>;

        /** Blocking; call from a background thread.

            NOT re-entrant and not safe to call concurrently on one instance:
            each run stashes its onset analysis in the Transcriber so that
            suggestFixes() can answer from the real signal.  A second call while
            the first is running overwrites that state.  One Transcriber per
            concurrent job.                                                     */
        Result transcribe (const juce::AudioBuffer<float>& audio, double sampleRate,
                           const Options&, ProgressFn progress = {});

        /** Same threading rules as transcribe() - it is the same analysis with a
            decode in front. */
        Result transcribeFile (const juce::File&, juce::AudioFormatManager&,
                               const Options&, ProgressFn progress = {});

        /** Per-note alternatives for the one-click fix UI (spec 8.2, 9.3):
            octave errors, neighbouring semitones, split/merge suggestions.

            A suggestion is a splice: it replaces the `count` notes starting at
            `firstIndex` in Result::notes with `replacements`.  That is what lets
            "split this note" be 1 -> 2 and "merge with the next" 2 -> 1 without
            the UI having to reverse-engineer the intent from `label`.

            Reads the analysis state left by the last transcribe() on this
            instance, so call it on the same Transcriber that produced `result`. */
        struct Suggestion
        {
            juce::String      label;
            int               firstIndex = 0;
            int               count      = 1;
            std::vector<Note> replacements;
            float             score = 0.0f;
        };
        std::vector<Suggestion> suggestFixes (const Result&, int noteIndex) const;

        /** Bulk clean-ups offered in the accuracy panel (spec 8.2). */
        static void removeBelowConfidence (std::vector<Note>&, float threshold);
        static void removeShorterThan (std::vector<Note>&, double beats);
        static void mergeRepeatedPitches (std::vector<Note>&, double gapBeats);
        static void snapAllToScale (std::vector<Note>&, const theory::Key&);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    /** Source separation (spec 8.2-1).  Runs an external local model process
        (Demucs or compatible) configured in Preferences; there is no bundled
        network and no cloud call - v0.6 fixed inference as local-only.        */
    class StemSeparator
    {
    public:
        explicit StemSeparator (Settings&);
        ~StemSeparator();

        enum class Stem { vocals, drums, bass, piano, guitar, other };
        static juce::String toString (Stem);

        bool isAvailable() const;
        juce::String getUnavailableReason() const;

        struct Output { Stem stem; juce::File file; };

        /** Blocking; call from a background thread. */
        juce::Result separate (const juce::File& source, const juce::File& outputFolder,
                               std::vector<Output>& out,
                               std::function<void (float, const juce::String&)> progress = {});
        void cancel();

    private:
        Settings& settings;
        std::unique_ptr<juce::ChildProcess> process;
        std::atomic<bool> cancelled { false };
    };
}
