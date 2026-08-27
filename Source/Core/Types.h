#pragma once
#include <juce_core/juce_core.h>
#include <vector>

/*  ScoreSmith shared value types.
    Spec: 8.1 / 8.2 / 8.4.7 / 9.3.  Everything below is plain data - no behaviour,
    no ownership - so every subsystem (engine, mixer, AI, UI) can pass it around
    freely.  Musical positions are ALWAYS in beats (quarter notes); only clip
    source offsets are in seconds, because they index into a sample file.        */

namespace ss
{
    using TrackId = juce::int64;
    using ClipId  = juce::int64;
    using SceneId = juce::int64;

    inline constexpr TrackId invalidTrackId = -1;
    inline constexpr ClipId  invalidClipId  = -1;
    inline constexpr SceneId invalidSceneId = -1;

    enum class TrackType { audio, midi, utau };

    /** A note event.  `confidence` is the AI transcription certainty in 0..1;
        hand-entered / generated notes use 1.0f.  Spec 8.2-5, 9.3.             */
    struct Note
    {
        int    pitch       = 60;    // MIDI note number 0..127
        double startBeats  = 0.0;
        double lengthBeats = 1.0;
        int    velocity    = 100;   // 1..127
        int    channel     = 1;     // 1..16
        float  confidence  = 1.0f;  // 0..1

        double endBeats() const noexcept { return startBeats + lengthBeats; }
        bool   isLowConfidence (float threshold) const noexcept { return confidence < threshold; }
    };

    /** A launchable clip in the Session grid (spec: Session view, Phase 3).
        No startBeats - it is independent of the linear timeline.  Field
        shapes mirror MidiClip/AudioClip minus the position, so conversion
        either direction is a flat field copy. */
    struct SessionClip
    {
        enum class Kind { midi, audio };

        Kind         kind = Kind::midi;
        juce::String name;
        double       lengthBeats = 4.0;

        // kind == midi
        std::vector<Note> notes;      // relative to clip start, same convention as MidiClip::notes

        // kind == audio
        juce::File sourceFile;
        double     offsetSeconds  = 0.0;
        float      gainDb         = 0.0f;
        double     fadeInSec      = 0.0;
        double     fadeOutSec     = 0.0;
        bool       reversed       = false;
        double     playbackRate   = 1.0;
    };

    /** Tempo map entry (spec 8.4.7). */
    struct TempoEvent
    {
        double beat = 0.0;
        double bpm  = 120.0;
    };

    struct TimeSignatureEvent
    {
        double beat        = 0.0;
        int    numerator   = 4;
        int    denominator = 4;
    };

    /** Chord track entry (spec 8.4.7).  `symbol` is display text ("Cmaj7"),
        `root` is a pitch class 0..11, `intervals` are semitones from the root. */
    struct ChordEvent
    {
        double           beat = 0.0;
        double           lengthBeats = 4.0;
        juce::String     symbol;
        int              root = 0;
        std::vector<int> intervals { 0, 4, 7 };
    };

    /** Marker / cue point (spec 9.9). */
    struct Marker
    {
        double       beat = 0.0;
        juce::String name;
    };

    enum class Quantise
    {
        off, whole, half, quarter, eighth, sixteenth, thirtySecond,
        eighthTriplet, sixteenthTriplet
    };

    /** Length of one quantise step in beats.  0 == no quantisation. */
    double quantiseStepInBeats (Quantise q) noexcept;

    /** Snap `beats` to `q`, blended by `strength` (0 = untouched, 1 = hard snap).
        Spec 8.2-3 asks for an adjustable snap strength rather than a hard grid. */
    double applyQuantise (double beats, Quantise q, double strength) noexcept;

    /** Genre presets for the generator (spec 8.1). */
    enum class Genre { pop, rock, jazz, lofi, edm, orchestral, cityPop, ballad, funk, bossaNova };

    juce::String toString (Genre g);
    juce::String toString (TrackType t);
    juce::String toString (Quantise q);

    Genre     genreFromString     (const juce::String&);
    TrackType trackTypeFromString (const juce::String&);
    Quantise  quantiseFromString  (const juce::String&);

    /** All genres, in menu order. */
    const std::vector<Genre>& allGenres();
}
