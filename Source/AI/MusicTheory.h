#pragma once
#include "Core/Types.h"

namespace ss::theory
{
    enum class ScaleType { major, naturalMinor, harmonicMinor, dorian, mixolydian, lydian, phrygian, locrian,
                           majorPentatonic, minorPentatonic, blues, chromatic };

    juce::String toString (ScaleType);
    const std::vector<int>& scaleIntervals (ScaleType);
    juce::StringArray noteNames();

    /** "C", "F#", "Bb" -> pitch class 0..11. */
    int  pitchClassFromName (const juce::String&);
    juce::String pitchClassName (int pitchClass, bool preferFlats = false);
    juce::String midiNoteName (int midiNote, bool preferFlats = false);

    struct Key { int tonic = 0; ScaleType scale = ScaleType::major; };
    juce::String toString (const Key&);

    /** True when the key signature is written with flats (F, Bb, Eb, Ab, Db and
        their modes).  Callers that know the key hand this to `chordSymbolFor`
        so a generated chord in Bb major comes out "Eb" rather than "D#".      */
    bool keyPrefersFlats (const Key&);

    /** Krumhansl-Schmuckler style key estimate from a pitch histogram. */
    Key estimateKey (const std::vector<Note>&);

    bool isInScale (int midiNote, const Key&);
    /** Nearest in-scale pitch, preferring the original when already in key. */
    int  snapToScale (int midiNote, const Key&);

    /** Parse "Cmaj7", "Am", "G7", "F#m7b5", "Bb/D" -> ChordEvent (beat unset). */
    bool parseChordSymbol (const juce::String&, ChordEvent& out);
    /** `preferFlats` picks the accidental spelling; use `keyPrefersFlats` when
        the caller has a key, otherwise sharps.  Parsing keeps whatever the user
        typed, so this only decides how *generated* chords are spelled.        */
    juce::String chordSymbolFor (int root, const std::vector<int>& intervals,
                                 bool preferFlats = false);

    /** Diatonic chords of a key, as scale-degree triads/sevenths. */
    std::vector<ChordEvent> diatonicChords (const Key&, bool sevenths);

    /** Best-fit chord for the notes sounding in [startBeat, endBeat). */
    ChordEvent detectChord (const std::vector<Note>&, double startBeat, double endBeat, const Key&);

    /** Voice a chord into MIDI pitches near `centrePitch`, `numVoices` notes. */
    std::vector<int> voiceChord (const ChordEvent&, int centrePitch, int numVoices);
}
