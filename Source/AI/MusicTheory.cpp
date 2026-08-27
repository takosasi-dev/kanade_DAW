#include "AI/MusicTheory.h"

#include <algorithm>
#include <cmath>

/*  Pure music theory - no JUCE GUI, no I/O, no state.  Transcriber and
    Generator both lean on this, and the unit tests pin the round-trips.       */

namespace ss::theory
{

//==============================================================================
//  Scales
//==============================================================================

juce::String toString (ScaleType s)
{
    switch (s)
    {
        case ScaleType::major:           return "major";
        case ScaleType::naturalMinor:    return "minor";
        case ScaleType::harmonicMinor:   return "harmonic minor";
        case ScaleType::dorian:          return "dorian";
        case ScaleType::mixolydian:      return "mixolydian";
        case ScaleType::lydian:          return "lydian";
        case ScaleType::phrygian:        return "phrygian";
        case ScaleType::locrian:         return "locrian";
        case ScaleType::majorPentatonic: return "major pentatonic";
        case ScaleType::minorPentatonic: return "minor pentatonic";
        case ScaleType::blues:           return "blues";
        case ScaleType::chromatic:       return "chromatic";
    }

    return "major";
}

const std::vector<int>& scaleIntervals (ScaleType s)
{
    // Indexed by ScaleType - keep in the same order as the enum.
    static const std::vector<int> table[]
    {
        { 0, 2, 4, 5, 7, 9, 11 },        // major
        { 0, 2, 3, 5, 7, 8, 10 },        // natural minor
        { 0, 2, 3, 5, 7, 8, 11 },        // harmonic minor
        { 0, 2, 3, 5, 7, 9, 10 },        // dorian
        { 0, 2, 4, 5, 7, 9, 10 },        // mixolydian
        { 0, 2, 4, 6, 7, 9, 11 },        // lydian
        { 0, 1, 3, 5, 7, 8, 10 },        // phrygian
        { 0, 1, 3, 5, 6, 8, 10 },        // locrian
        { 0, 2, 4, 7, 9 },               // major pentatonic
        { 0, 3, 5, 7, 10 },              // minor pentatonic
        { 0, 3, 5, 6, 7, 10 },           // blues
        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }   // chromatic
    };

    constexpr auto numScales = sizeof (table) / sizeof (table[0]);
    const auto index = (size_t) s;
    return table[index < numScales ? index : 0];
}

juce::StringArray noteNames()
{
    return { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
}

static int wrapPitchClass (int pc) noexcept { return ((pc % 12) + 12) % 12; }

/** Reads a note letter plus any accidentals from the front of `text`.
    Returns -1 if it does not start with a note name; `charsUsed` gets the
    number of characters consumed so chord parsing can carry on from there. */
static int parseRootAt (const juce::String& text, int& charsUsed)
{
    charsUsed = 0;

    if (text.isEmpty())
        return -1;

    static const int letterToPc[] { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G

    const auto first = juce::CharacterFunctions::toUpperCase (text[0]);

    if (first < 'A' || first > 'G')
        return -1;

    int pc = letterToPc[(int) (first - 'A')];
    charsUsed = 1;

    // Unicode sharp/flat glyphs turn up in imported chord charts, so accept them.
    for (int i = 1; i < text.length(); ++i)
    {
        const auto c = text[i];

        if (c == '#' || c == (juce::juce_wchar) 0x266f)      { ++pc; ++charsUsed; }
        else if (c == 'b' || c == (juce::juce_wchar) 0x266d) { --pc; ++charsUsed; }
        else                                                  break;
    }

    return wrapPitchClass (pc);
}

int pitchClassFromName (const juce::String& name)
{
    int used = 0;
    return parseRootAt (name.trim(), used);
}

juce::String pitchClassName (int pitchClass, bool preferFlats)
{
    static const char* const sharps[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* const flats[]  { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"  };

    const auto pc = wrapPitchClass (pitchClass);
    return preferFlats ? flats[pc] : sharps[pc];
}

juce::String midiNoteName (int midiNote, bool preferFlats)
{
    const auto n = juce::jlimit (0, 127, midiNote);
    // Scientific pitch notation: middle C (60) is C4.
    return pitchClassName (n % 12, preferFlats) + juce::String (n / 12 - 1);
}

juce::String toString (const Key& k)
{
    return pitchClassName (k.tonic, keyPrefersFlats (k)) + " " + toString (k.scale);
}

bool keyPrefersFlats (const Key& k)
{
    // Fold the mode back onto its relative major, then read the key signature
    // off the circle of fifths.  C and F# sit on the fence; both are spelled
    // with sharps here so the default and the neutral case agree.
    const auto toRelativeMajor = [] (ScaleType s) -> int
    {
        switch (s)
        {
            case ScaleType::naturalMinor:
            case ScaleType::harmonicMinor:
            case ScaleType::minorPentatonic:
            case ScaleType::blues:           return 3;
            case ScaleType::dorian:          return 10;
            case ScaleType::phrygian:        return 8;
            case ScaleType::lydian:          return 7;
            case ScaleType::mixolydian:      return 5;
            case ScaleType::locrian:         return 1;
            case ScaleType::major:
            case ScaleType::majorPentatonic:
            case ScaleType::chromatic:
            default:                         return 0;
        }
    };

    //                             C      Db     D      Eb     E      F
    static const bool flatKeys[] { false, true,  false, true,  false, true,
    //                             F#     G      Ab     A      Bb     B
                                   false, false, true,  false, true,  false };

    return flatKeys[wrapPitchClass (k.tonic + toRelativeMajor (k.scale))];
}

//==============================================================================
//  Key estimation (Krumhansl-Schmuckler)
//==============================================================================

// Krumhansl & Kessler (1982) probe-tone profiles.
static const double kMajorProfile[12]
    { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
static const double kMinorProfile[12]
    { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

/** Pearson correlation of two 12-element vectors. */
static double correlate12 (const double* a, const double* b) noexcept
{
    double meanA = 0.0, meanB = 0.0;

    for (int i = 0; i < 12; ++i) { meanA += a[i]; meanB += b[i]; }

    meanA /= 12.0;
    meanB /= 12.0;

    double num = 0.0, varA = 0.0, varB = 0.0;

    for (int i = 0; i < 12; ++i)
    {
        const auto x = a[i] - meanA;
        const auto y = b[i] - meanB;

        num  += x * y;
        varA += x * x;
        varB += y * y;
    }

    const auto den = std::sqrt (varA * varB);
    return den > 1.0e-12 ? num / den : 0.0;
}

Key estimateKey (const std::vector<Note>& notes)
{
    Key best;

    if (notes.empty())
        return best;

    // Duration-weighted histogram: a whole note of C says more about the key
    // than a passing sixteenth.  Confidence is folded in as well so that a
    // shaky transcription does not get to vote at full strength - generated
    // notes are all 1.0f, so this is a no-op on anything we made ourselves.
    double hist[12] {};

    for (const auto& n : notes)
    {
        const auto weight = juce::jmax (0.0, n.lengthBeats)
                          * (double) juce::jlimit (0.05f, 1.0f, n.confidence);
        hist[wrapPitchClass (n.pitch)] += weight;
    }

    double total = 0.0;
    for (auto h : hist) total += h;

    if (total <= 0.0)
        return best;

    double bestScore = -2.0;
    double rotated[12];

    for (int tonic = 0; tonic < 12; ++tonic)
    {
        for (int i = 0; i < 12; ++i)
            rotated[i] = hist[wrapPitchClass (tonic + i)];

        const auto majScore = correlate12 (rotated, kMajorProfile);
        const auto minScore = correlate12 (rotated, kMinorProfile);

        if (majScore > bestScore) { bestScore = majScore; best = { tonic, ScaleType::major        }; }
        if (minScore > bestScore) { bestScore = minScore; best = { tonic, ScaleType::naturalMinor }; }
    }

    return best;
}

bool isInScale (int midiNote, const Key& key)
{
    const auto& ivs = scaleIntervals (key.scale);
    const auto degree = wrapPitchClass (midiNote - key.tonic);

    return std::find (ivs.begin(), ivs.end(), degree) != ivs.end();
}

int snapToScale (int midiNote, const Key& key)
{
    if (isInScale (midiNote, key))
        return midiNote;

    // Search outwards so a chromatic note moves by the smallest possible step;
    // ties resolve downwards, which is what leading-tone spellings usually want.
    for (int distance = 1; distance <= 6; ++distance)
    {
        if (isInScale (midiNote - distance, key)) return juce::jlimit (0, 127, midiNote - distance);
        if (isInScale (midiNote + distance, key)) return juce::jlimit (0, 127, midiNote + distance);
    }

    return midiNote;
}

//==============================================================================
//  Chord symbols
//==============================================================================

namespace
{
    struct ChordQuality
    {
        const char* suffix;
        std::vector<int> intervals;
    };

    /** Canonical qualities.  Both directions of the symbol conversion use this
        one table, so parse -> format round-trips exactly for every entry.
        Ordered most-common-first: `chordSymbolFor` returns the first exact
        interval match, so plain triads win over exotic spellings. */
    const std::vector<ChordQuality>& canonicalQualities()
    {
        static const std::vector<ChordQuality> table
        {
            { "",       { 0, 4, 7 } },
            { "m",      { 0, 3, 7 } },
            { "dim",    { 0, 3, 6 } },
            { "aug",    { 0, 4, 8 } },
            { "sus2",   { 0, 2, 7 } },
            { "sus4",   { 0, 5, 7 } },
            { "5",      { 0, 7 } },
            { "6",      { 0, 4, 7, 9 } },
            { "m6",     { 0, 3, 7, 9 } },
            { "7",      { 0, 4, 7, 10 } },
            { "maj7",   { 0, 4, 7, 11 } },
            { "m7",     { 0, 3, 7, 10 } },
            { "mmaj7",  { 0, 3, 7, 11 } },
            { "m7b5",   { 0, 3, 6, 10 } },
            { "dim7",   { 0, 3, 6, 9 } },
            { "7sus4",  { 0, 5, 7, 10 } },
            { "add9",   { 0, 4, 7, 14 } },
            { "9",      { 0, 4, 7, 10, 14 } },
            { "maj9",   { 0, 4, 7, 11, 14 } },
            { "m9",     { 0, 3, 7, 10, 14 } },
            { "69",     { 0, 4, 7, 9, 14 } },
            { "11",     { 0, 4, 7, 10, 14, 17 } },
            { "13",     { 0, 4, 7, 10, 14, 21 } },
            { "7b5",    { 0, 4, 6, 10 } },
            { "7#5",    { 0, 4, 8, 10 } },
            { "7b9",    { 0, 4, 7, 10, 13 } },
            { "7#9",    { 0, 4, 7, 10, 15 } },
            { "7#11",   { 0, 4, 7, 10, 18 } }
        };

        return table;
    }

    struct QualityAlias { const char* text; const char* canonical; };

    /** Spellings people actually type / that come out of other tools, longest
        first so prefix matching never stops early on "maj" inside "maj7". */
    const std::vector<QualityAlias>& qualityAliases()
    {
        static const std::vector<QualityAlias> table
        {
            { "minmaj7", "mmaj7" }, { "m(maj7)", "mmaj7" },
            { "7sus4", "7sus4" },   { "-7b5", "m7b5" },   { "m7-5", "m7b5" },
            { "mmaj7", "mmaj7" },   { "maj13", "13" },
            { "7#11", "7#11" },     { "m7b5", "m7b5" },   { "dim7", "dim7" },
            { "maj7", "maj7" },     { "maj9", "maj9" },   { "add9", "add9" },
            { "sus2", "sus2" },     { "sus4", "sus4" },   { "min7", "m7" },
            { "min9", "m9" },       { "min6", "m6" },     { "6/9", "69" },
            { "mM7", "mmaj7" },     { "7b9", "7b9" },     { "7#9", "7#9" },
            { "7b5", "7b5" },       { "7#5", "7#5" },
            { "dim", "dim" },       { "aug", "aug" },     { "sus", "sus4" },
            { "min", "m" },         { "maj", "" },
            { "m7", "m7" },         { "m9", "m9" },       { "m6", "m6" },
            { "M7", "maj7" },       { "M9", "maj9" },     { "-7", "m7" },
            { "69", "69" },         { "13", "13" },       { "11", "11" },
            { "m", "m" },           { "-", "m" },         { "M", "" },
            { "o", "dim" },         { "\xc3\xb8", "m7b5" },
            { "+", "aug" },         { "5", "5" },         { "6", "6" },
            { "7", "7" },           { "9", "9" }
        };

        return table;
    }

    const std::vector<int>* intervalsForCanonical (const juce::String& canonical)
    {
        for (const auto& q : canonicalQualities())
            if (canonical == q.suffix)
                return &q.intervals;

        return nullptr;
    }

    /** Chord tones as pitch classes, ignoring any slash bass. */
    std::vector<int> chordPitchClasses (const ChordEvent& c)
    {
        std::vector<int> pcs;

        for (auto iv : c.intervals)
            if (iv >= 0)
                pcs.push_back (wrapPitchClass (c.root + iv));

        std::sort (pcs.begin(), pcs.end());
        pcs.erase (std::unique (pcs.begin(), pcs.end()), pcs.end());
        return pcs;
    }
}

bool parseChordSymbol (const juce::String& symbol, ChordEvent& out)
{
    // Parentheses are decoration - "C7(b9)" and "C7b9" mean the same thing.
    auto text = symbol.trim().removeCharacters (" ()");

    if (text.isEmpty())
        return false;

    juce::String bassText;

    if (const auto slash = text.indexOfChar ('/'); slash >= 0)
    {
        bassText = text.substring (slash + 1);
        text     = text.substring (0, slash);
    }

    int used = 0;
    const auto root = parseRootAt (text, used);

    if (root < 0)
        return false;

    const auto remainder = text.substring (used);

    // Longest alias that starts the remainder wins; anything left over is
    // decoration we do not model (and we keep it in the display symbol).
    juce::String canonical;
    int matched = -1;

    for (const auto& alias : qualityAliases())
    {
        const juce::String aliasText (juce::CharPointer_UTF8 (alias.text));

        if (aliasText.length() > matched && remainder.startsWith (aliasText))
        {
            matched   = aliasText.length();
            canonical = alias.canonical;
        }
    }

    if (matched < 0)
    {
        if (remainder.isNotEmpty())
            return false;      // "Cwhatever" is not a chord

        canonical = "";
    }

    const auto* ivs = intervalsForCanonical (canonical);

    if (ivs == nullptr)
        return false;

    out.root      = root;
    out.intervals = *ivs;

    // A slash bass is carried as a negative interval below the root, which is
    // exactly what voiceChord wants and what chordSymbolFor reads back.
    if (bassText.isNotEmpty())
    {
        int bassUsed = 0;
        const auto bass = parseRootAt (bassText, bassUsed);

        if (bass >= 0 && bass != root)
            out.intervals.insert (out.intervals.begin(), wrapPitchClass (bass - root) - 12);
    }

    // Keep the spelling the user typed verbatim: even with `preferFlats` the
    // formatter only knows the key, not that this particular chord was written
    // "A#" in a flat key on purpose.
    out.symbol = symbol.trim();
    return true;
}

juce::String chordSymbolFor (int root, const std::vector<int>& intervals, bool preferFlats)
{
    const auto spell = [preferFlats] (int pc) { return pitchClassName (pc, preferFlats); };

    auto ivs = intervals;
    std::sort (ivs.begin(), ivs.end());

    juce::String bassSuffix;

    if (! ivs.empty() && ivs.front() < 0)
    {
        bassSuffix = "/" + spell (root + ivs.front());
        ivs.erase (ivs.begin(), std::find_if (ivs.begin(), ivs.end(),
                                              [] (int i) { return i >= 0; }));
    }

    ivs.erase (std::unique (ivs.begin(), ivs.end()), ivs.end());

    const auto lookup = [&ivs] () -> juce::String
    {
        for (const auto& q : canonicalQualities())
            if (q.intervals == ivs)
                return juce::String (q.suffix);

        return {};
    };

    if (! ivs.empty())
    {
        if (const auto exact = lookup(); exact.isNotEmpty() || ivs == canonicalQualities()[0].intervals)
            return spell (root) + exact + bassSuffix;

        // Second pass: fold octave-displaced voicings (0,4,7,12,16) back down.
        auto folded = ivs;
        for (auto& i : folded) i = wrapPitchClass (i);
        std::sort (folded.begin(), folded.end());
        folded.erase (std::unique (folded.begin(), folded.end()), folded.end());

        for (const auto& q : canonicalQualities())
            if (q.intervals == folded)
                return spell (root) + q.suffix + bassSuffix;
    }

    // Nothing in the table fits - spell the notes rather than lie about it.
    juce::StringArray names;
    for (auto iv : ivs) names.add (spell (root + iv));

    return spell (root) + "(" + names.joinIntoString (" ") + ")" + bassSuffix;
}

std::vector<ChordEvent> diatonicChords (const Key& key, bool sevenths)
{
    auto scale = scaleIntervals (key.scale);

    // Pentatonic / blues / chromatic have no seven-degree stack of thirds;
    // fall back to the parent major so the caller always gets seven chords.
    if (scale.size() != 7)
        scale = scaleIntervals (ScaleType::major);

    const auto n = (int) scale.size();

    const auto degreeInterval = [&scale, n] (int index)
    {
        return scale[(size_t) (index % n)] + 12 * (index / n);
    };

    std::vector<ChordEvent> chords;
    chords.reserve (7);

    for (int degree = 0; degree < n; ++degree)
    {
        const auto base = degreeInterval (degree);

        ChordEvent c;
        c.root      = wrapPitchClass (key.tonic + base);
        c.intervals = { 0,
                        degreeInterval (degree + 2) - base,
                        degreeInterval (degree + 4) - base };

        if (sevenths)
            c.intervals.push_back (degreeInterval (degree + 6) - base);

        c.symbol = chordSymbolFor (c.root, c.intervals, keyPrefersFlats (key));
        chords.push_back (c);
    }

    return chords;
}

//==============================================================================
//  Chord detection
//==============================================================================

ChordEvent detectChord (const std::vector<Note>& notes, double startBeat, double endBeat, const Key& key)
{
    ChordEvent result;
    result.beat        = startBeat;
    result.lengthBeats = juce::jmax (0.0, endBeat - startBeat);
    result.root        = key.tonic;
    result.intervals   = key.scale == ScaleType::major ? std::vector<int> { 0, 4, 7 }
                                                       : std::vector<int> { 0, 3, 7 };

    // Duration-weighted pitch-class weights for the notes sounding in the window.
    double weights[12] {};
    double total = 0.0;
    int    lowestPitch = 128;

    for (const auto& n : notes)
    {
        const auto overlap = juce::jmin (n.endBeats(), endBeat) - juce::jmax (n.startBeats, startBeat);

        if (overlap <= 0.0)
            continue;

        const auto w = overlap * (double) juce::jlimit (0.05f, 1.0f, n.confidence);
        weights[wrapPitchClass (n.pitch)] += w;
        total += w;

        if (n.pitch < lowestPitch)
            lowestPitch = n.pitch;
    }

    if (total <= 0.0)
    {
        result.symbol = chordSymbolFor (result.root, result.intervals, keyPrefersFlats (key));
        return result;
    }

    for (auto& w : weights)
        w /= total;

    // Candidate set: the qualities that actually turn up in the styles we
    // arrange for.  Anything rarer is better spelled by the user than guessed.
    static const char* const candidates[] { "", "m", "7", "maj7", "m7", "dim", "m7b5", "sus4", "6", "m6" };

    const auto diatonic = diatonicChords (key, true);

    double bestScore = -1.0e9;

    for (int root = 0; root < 12; ++root)
    {
        for (const auto* suffix : candidates)
        {
            const auto* ivs = intervalsForCanonical (suffix);

            if (ivs == nullptr)
                continue;

            ChordEvent candidate;
            candidate.root      = root;
            candidate.intervals = *ivs;

            const auto tones = chordPitchClasses (candidate);

            double inChord = 0.0;

            for (auto pc : tones)
                inChord += weights[pc];

            // Non-chord weight is penalised; unsounded chord tones cost a little
            // too, so a triad is not beaten by a 13th chord that "covers" more.
            const auto outOfChord = 1.0 - inChord;

            double missing = 0.0;
            for (auto pc : tones)
                if (weights[pc] <= 1.0e-6)
                    missing += 1.0;

            double score = inChord - 0.85 * outOfChord - 0.06 * missing;

            // Root in the bass is strong evidence; so is being in the key.
            if (lowestPitch < 128 && wrapPitchClass (lowestPitch) == root)
                score += 0.12;

            for (const auto& d : diatonic)
                if (d.root == root)
                {
                    score += 0.08;
                    break;
                }

            score += 0.05 * weights[root];   // the root itself sounding at all

            if (score > bestScore)
            {
                bestScore         = score;
                result.root       = root;
                result.intervals  = *ivs;
            }
        }
    }

    result.symbol = chordSymbolFor (result.root, result.intervals, keyPrefersFlats (key));
    return result;
}

//==============================================================================
//  Voicing
//==============================================================================

std::vector<int> voiceChord (const ChordEvent& chord, int centrePitch, int numVoices)
{
    auto ivs = chord.intervals;

    if (ivs.empty())
        ivs = { 0, 4, 7 };

    std::sort (ivs.begin(), ivs.end());
    ivs.erase (std::unique (ivs.begin(), ivs.end()), ivs.end());

    const auto voices = juce::jmax (1, numVoices);

    // Put the root in the octave nearest the requested centre, then stack.
    int root = wrapPitchClass (chord.root);
    root += 12 * juce::roundToInt ((centrePitch - root) / 12.0);

    std::vector<int> out;
    out.reserve ((size_t) juce::jmax (voices, (int) ivs.size()));

    for (auto iv : ivs)
        out.push_back (root + iv);

    // Not enough notes in the chord: spread upwards an octave at a time, which
    // gives an open voicing rather than a doubled cluster.
    for (size_t i = 0; (int) out.size() < voices && i < out.size(); ++i)
        out.push_back (out[i] + 12);

    // Too many: drop from the top so the bass and root always survive.
    while ((int) out.size() > voices)
        out.pop_back();

    for (auto& p : out)
        p = juce::jlimit (0, 127, p);

    std::sort (out.begin(), out.end());
    return out;
}

} // namespace ss::theory
