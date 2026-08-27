#include "AI/Generator.h"

#include <algorithm>
#include <cmath>

/*  MIDI -> music (spec 8.1).

    A seeded, rule-and-template arranger: analyse the input's key and harmony,
    look up a per-genre rhythm/voicing template for each part, then walk away
    from that template by an amount the `creativity` slider controls.  Every
    draw comes from a juce::Random seeded off Options::seed, so a candidate is
    fully reproducible - that is what makes the A/B compare (9.5) and the
    per-part re-roll (9.4) work at all.

    >>> Where the Transformer goes (spec 10.2): generatePart() is the seam.
        Replace the `switch (part)` template lookup below with model sampling
        conditioned on (key, chord segments, part, genre, complexity) and
        everything above it - candidate gallery, novelty scoring, re-roll,
        humanise - keeps working unchanged.  Nothing outside generatePart()
        knows that the notes came from a table.                                */

namespace ss
{

namespace
{
    //==========================================================================
    //  Deterministic seeding
    //==========================================================================

    /** splitmix64-style mix so related seeds (candidate 1, candidate 2) do not
        produce related music - juce::Random with adjacent seeds does. */
    juce::int64 mixSeed (juce::int64 a, juce::int64 b) noexcept
    {
        auto x = (juce::uint64) a + 0x9e3779b97f4a7c15ULL * ((juce::uint64) b + 1);
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return (juce::int64) (x & 0x7fffffffffffffffULL);
    }

    struct Rng
    {
        explicit Rng (juce::int64 seed) : random (seed) {}

        double next()                  { return random.nextDouble(); }
        bool   chance (double p)       { return random.nextDouble() < p; }
        int    below (int n)           { return n <= 1 ? 0 : random.nextInt (n); }
        int    range (int lo, int hi)  { return hi <= lo ? lo : lo + random.nextInt (hi - lo + 1); }

        juce::Random random;
    };

    //==========================================================================
    //  Genre templates
    //==========================================================================
    enum class BassPattern { rootWhole, rootEighths, walking, syncopated, offbeat, tumbao };
    enum class CompPattern { sustain, quarterStabs, offbeatSkank, charleston, arpeggiated, swellPad };

    /*  Drum patterns are 16 characters, one sixteenth each, 'x' == hit.  Written
        as strings on purpose: a bit mask is unreadable in a diff, and these get
        tweaked by ear far more often than they get read by code.  */
    struct GenreProfile
    {
        const char* kick;
        const char* kickExtra;      // folded in as complexity rises
        const char* snare;
        const char* snareGhost;
        const char* hat;
        const char* hatExtra;       // fills the hats out to sixteenths
        const char* ride;           // used instead of hats when useRide

        int    hatNote      = 42;   // 42 closed / 44 pedal / 46 open
        int    snareNote    = 38;   // 38 snare / 37 side stick / 39 clap
        bool   useRide      = false;
        double swing        = 0.5;

        BassPattern bass    = BassPattern::rootEighths;
        CompPattern comp    = CompPattern::quarterStabs;
        bool   sevenths     = false;

        int    drumVelocity = 100;
        int    chordCentre  = 60;
        int    bassCentre   = 40;
        double density      = 1.0;
    };

    const GenreProfile& profileFor (Genre genre)
    {
        static const GenreProfile pop
        {
            "x.......x.......", "......x.........", "....x.......x...", "..x........x....",
            "x.x.x.x.x.x.x.x.", ".x.x.x.x.x.x.x.x", "................",
            42, 38, false, 0.5, BassPattern::rootEighths, CompPattern::quarterStabs, false, 100, 60, 40, 1.0
        };

        static const GenreProfile rock
        {
            "x.....x.x.......", "..x........x....", "....x.......x...", "..............x.",
            "x.x.x.x.x.x.x.x.", ".x.x.x.x.x.x.x.x", "................",
            42, 38, false, 0.5, BassPattern::rootEighths, CompPattern::quarterStabs, false, 108, 57, 40, 1.05
        };

        static const GenreProfile jazz
        {
            "x...............", "........x.......", "............x...", "....x......x....",
            "....x.......x...", "................", "x...xx..x...xx..",
            44, 38, true, 0.66, BassPattern::walking, CompPattern::charleston, true, 82, 63, 40, 0.9
        };

        static const GenreProfile lofi
        {
            "x.........x.....", "......x.........", "....x.......x...", "..x.............",
            "x.x.x.x.x.x.x.x.", "................", "................",
            42, 38, false, 0.58, BassPattern::rootWhole, CompPattern::sustain, true, 78, 60, 38, 0.75
        };

        static const GenreProfile edm
        {
            "x...x...x...x...", "..............x.", "....x.......x...", "................",
            "..x...x...x...x.", "x.x.x.x.x.x.x.x.", "................",
            46, 39, false, 0.5, BassPattern::offbeat, CompPattern::offbeatSkank, false, 112, 60, 36, 1.15
        };

        static const GenreProfile orchestral
        {
            "x.......x.......", "............x...", "............x...", "................",
            "................", "................", "................",
            42, 38, false, 0.5, BassPattern::rootWhole, CompPattern::swellPad, false, 74, 60, 40, 0.7
        };

        static const GenreProfile cityPop
        {
            "x.........x.....", "......x.........", "....x.......x...", "..x....x..x....x",
            "x.x.x.x.x.x.x.x.", ".x.x.x.x.x.x.x.x", "................",
            42, 38, false, 0.53, BassPattern::syncopated, CompPattern::arpeggiated, true, 96, 62, 40, 1.0
        };

        static const GenreProfile ballad
        {
            "x.......x.......", "..........x.....", "........x.......", "................",
            "x...x...x...x...", "x.x.x.x.x.x.x.x.", "................",
            42, 38, false, 0.5, BassPattern::rootWhole, CompPattern::sustain, false, 76, 60, 40, 0.65
        };

        static const GenreProfile funk
        {
            "x..x....x.x.....", "......x.......x.", "....x.......x...", "..x..x.x..x..x.x",
            "x.x.x.x.x.x.x.x.", ".x.x.x.x.x.x.x.x", "................",
            42, 38, false, 0.54, BassPattern::syncopated, CompPattern::offbeatSkank, true, 104, 60, 38, 1.2
        };

        static const GenreProfile bossaNova
        {
            "x.....x.x.....x.", "................", "..x...x...x...x.", "................",
            "x.x.x.x.x.x.x.x.", "................", "................",
            42, 37, false, 0.54, BassPattern::tumbao, CompPattern::charleston, true, 78, 62, 40, 0.8
        };

        switch (genre)
        {
            case Genre::pop:        return pop;
            case Genre::rock:       return rock;
            case Genre::jazz:       return jazz;
            case Genre::lofi:       return lofi;
            case Genre::edm:        return edm;
            case Genre::orchestral: return orchestral;
            case Genre::cityPop:    return cityPop;
            case Genre::ballad:     return ballad;
            case Genre::funk:       return funk;
            case Genre::bossaNova:  return bossaNova;
        }

        return pop;
    }

    bool hitAt (const char* pattern, int step) noexcept
    {
        return pattern != nullptr && step >= 0 && step < 16 && pattern[step] == 'x';
    }

    //==========================================================================
    //  Analysis
    //==========================================================================
    struct Segment
    {
        double start = 0.0;
        double length = 4.0;
        ChordEvent chord;

        double end() const noexcept { return start + length; }
    };

    struct Analysis
    {
        theory::Key key;
        std::vector<Segment> segments;      // harmonic rhythm, covering the whole output
        std::vector<Note> melody;
        TimeSignatureEvent timeSignature;   // humanise's accent grid needs the meter, not just beatsPerBar
        double beatsPerBar = 4.0;
        double lengthBeats = 32.0;
        double bpm = 120.0;
        int    numBars = 8;

        const Segment& segmentAt (double beat) const
        {
            static const Segment fallback;

            if (segments.empty())
                return fallback;

            const Segment* best = &segments.front();

            for (const auto& s : segments)
            {
                if (s.start > beat + 1.0e-6)
                    break;

                best = &s;
            }

            return *best;
        }
    };

    /** Applies the genre's swing to a position, so drums, bass and comping all
        push the off-beats by the same amount. */
    double swingPosition (double beat, double swing)
    {
        if (swing <= 0.505)
            return beat;

        const auto quarter = std::floor (beat);
        const auto within  = beat - quarter;
        const auto shift   = swing - 0.5;

        if (std::abs (within - 0.5)  < 1.0e-6) return quarter + 0.5  + shift;
        if (std::abs (within - 0.25) < 1.0e-6) return quarter + 0.25 + shift * 0.5;
        if (std::abs (within - 0.75) < 1.0e-6) return quarter + 0.75 + shift * 0.5;

        return beat;
    }

    Note makeNote (int pitch, double start, double length, int velocity, int channel = 1)
    {
        Note n;
        n.pitch       = juce::jlimit (0, 127, pitch);
        n.startBeats  = juce::jmax (0.0, start);
        n.lengthBeats = juce::jmax (0.03125, length);
        n.velocity    = juce::jlimit (1, 127, velocity);
        n.channel     = channel;
        n.confidence  = 1.0f;      // we wrote it; there is nothing to be unsure about
        return n;
    }

    /** Nearest octave placement of a pitch class to a target register. */
    int placeNear (int pitchClass, int centre)
    {
        auto p = ((pitchClass % 12) + 12) % 12;
        p += 12 * juce::roundToInt ((centre - p) / 12.0);
        return juce::jlimit (0, 127, p);
    }

    /** The chord's tones as absolute pitches around `centre`, ignoring a slash bass. */
    std::vector<int> chordTonesNear (const ChordEvent& c, int centre)
    {
        std::vector<int> tones;
        const auto root = placeNear (c.root, centre);

        for (auto iv : c.intervals)
            if (iv >= 0)
                tones.push_back (juce::jlimit (0, 127, root + iv));

        if (tones.empty())
            tones.push_back (root);

        std::sort (tones.begin(), tones.end());
        tones.erase (std::unique (tones.begin(), tones.end()), tones.end());
        return tones;
    }

    /** Adds the diatonic seventh to a triad, for the genres that expect one. */
    ChordEvent withSeventh (const ChordEvent& c, const theory::Key& key)
    {
        if (c.intervals.size() > 3)
            return c;

        auto extended = c;
        const auto major3rd = std::find (c.intervals.begin(), c.intervals.end(), 4) != c.intervals.end();

        // Diatonic seventh: major-7th over a I/IV-shaped chord, minor-7th otherwise.
        const auto seventh = theory::isInScale (c.root + 11, key) && major3rd ? 11 : 10;
        extended.intervals.push_back (seventh);
        extended.symbol = theory::chordSymbolFor (extended.root, extended.intervals,
                                                  theory::keyPrefersFlats (key));
        return extended;
    }

    //==========================================================================
    //  Novelty
    //==========================================================================
    float noveltyBetween (const std::vector<Note>& candidate, const std::vector<Note>& source,
                          double beatsPerBar)
    {
        if (candidate.empty() || source.empty())
            return 1.0f;

        double a[12] {}, b[12] {};

        for (const auto& n : candidate) a[((n.pitch % 12) + 12) % 12] += juce::jmax (0.0, n.lengthBeats);
        for (const auto& n : source)    b[((n.pitch % 12) + 12) % 12] += juce::jmax (0.0, n.lengthBeats);

        double dot = 0.0, na = 0.0, nb = 0.0;

        for (int i = 0; i < 12; ++i) { dot += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; }

        const auto pitchSimilarity = (na > 0.0 && nb > 0.0) ? dot / std::sqrt (na * nb) : 0.0;

        // Rhythmic overlap: which sixteenth slots of the bar are used at all.
        const auto slots = juce::jmax (1, (int) std::round (beatsPerBar * 4.0));
        std::vector<char> usedA ((size_t) slots, 0), usedB ((size_t) slots, 0);

        for (const auto& n : candidate)
            usedA[(size_t) (((int) std::llround (n.startBeats * 4.0) % slots + slots) % slots)] = 1;

        for (const auto& n : source)
            usedB[(size_t) (((int) std::llround (n.startBeats * 4.0) % slots + slots) % slots)] = 1;

        int intersection = 0, unionCount = 0;

        for (int i = 0; i < slots; ++i)
        {
            if (usedA[(size_t) i] != 0 && usedB[(size_t) i] != 0) ++intersection;
            if (usedA[(size_t) i] != 0 || usedB[(size_t) i] != 0) ++unionCount;
        }

        const auto rhythmSimilarity = unionCount > 0 ? (double) intersection / (double) unionCount : 0.0;

        return (float) juce::jlimit (0.0, 1.0, 0.5 * (1.0 - pitchSimilarity) + 0.5 * (1.0 - rhythmSimilarity));
    }
}

//==============================================================================
//  Impl - a placeholder today.  When spec 10.2's Transformer lands, the loaded
//  session/weights live here rather than being reloaded per generate() call.
//==============================================================================
struct Generator::Impl {};

Generator::Generator() : impl (std::make_unique<Impl>()) {}
Generator::~Generator() = default;

juce::String Generator::toString (Part p)
{
    switch (p)
    {
        case Part::drums:         return "Drums";
        case Part::bass:          return "Bass";
        case Part::chords:        return "Chords";
        case Part::pad:           return "Pad";
        case Part::arpeggio:      return "Arpeggio";
        case Part::counterMelody: return "Counter melody";
        case Part::strings:       return "Strings";
        case Part::lead:          return "Lead";
    }

    return "Part";
}

juce::String Generator::toString (Mode m)
{
    switch (m)
    {
        case Mode::autoArrange:   return "Auto arrange";
        case Mode::continuation:  return "Continuation";
        case Mode::styleTransfer: return "Style transfer";
        case Mode::harmonise:     return "Harmonise";
        case Mode::humanise:      return "Humanise";
        case Mode::accompaniment: return "Accompaniment";
        case Mode::drumPattern:   return "Drum pattern";
    }

    return "Generate";
}

//==============================================================================
namespace
{
    Analysis analyseInput (const Generator::Input& input, const Generator::Options& options)
    {
        Analysis a;

        a.bpm    = input.bpm > 20.0 ? input.bpm : 120.0;
        a.melody = input.melody;

        const auto numerator   = juce::jmax (1, input.timeSignature.numerator);
        const auto denominator = juce::jmax (1, input.timeSignature.denominator);
        a.timeSignature = input.timeSignature;
        a.timeSignature.numerator   = numerator;
        a.timeSignature.denominator = denominator;
        a.beatsPerBar = juce::jmax (1.0, (double) numerator * 4.0 / (double) denominator);

        a.lengthBeats = options.lengthBeats > 0.0 ? options.lengthBeats : 32.0;
        a.numBars     = juce::jmax (1, (int) std::ceil (a.lengthBeats / a.beatsPerBar - 1.0e-6));

        a.key = (options.useDetectedKey && ! input.melody.empty())
              ? theory::estimateKey (input.melody)
              : options.key;

        auto chords = input.chords;

        if (chords.empty())
            chords = Generator::harmonise (input.melody, a.key, options.complexity, a.beatsPerBar);

        std::sort (chords.begin(), chords.end(),
                   [] (const ChordEvent& x, const ChordEvent& y) { return x.beat < y.beat; });

        if (chords.empty())
        {
            ChordEvent tonic;
            tonic.root      = a.key.tonic;
            tonic.intervals = a.key.scale == theory::ScaleType::major ? std::vector<int> { 0, 4, 7 }
                                                                     : std::vector<int> { 0, 3, 7 };
            tonic.lengthBeats = a.beatsPerBar;
            tonic.symbol = theory::chordSymbolFor (tonic.root, tonic.intervals,
                                                   theory::keyPrefersFlats (a.key));
            chords.push_back (tonic);
        }

        // ponytail: the progression is looped verbatim rather than developed -
        // no modulation, no turnaround on the last bar.  A section model (A/B/
        // chorus) is where this goes next.
        // Loop the progression until it covers the requested length.  Repeating
        // the whole phrase keeps the half-bar chords that harmonise() may have
        // inserted, which tiling one-chord-per-bar would flatten.
        const auto sourceLength = juce::jmax (a.beatsPerBar,
                                              chords.back().beat + juce::jmax (0.0, chords.back().lengthBeats));

        for (double offset = 0.0; offset < a.lengthBeats; offset += sourceLength)
        {
            for (const auto& c : chords)
            {
                const auto start = offset + c.beat;

                if (start >= a.lengthBeats - 1.0e-6)
                    break;

                Segment s;
                s.start  = start;
                s.chord  = c;
                s.length = juce::jmax (0.25, c.lengthBeats);
                a.segments.push_back (s);
            }
        }

        // Trim each segment at the next one and at the end of the output, so the
        // parts below can trust `Segment::length` without re-checking.
        for (size_t i = 0; i < a.segments.size(); ++i)
        {
            const auto limit = i + 1 < a.segments.size() ? a.segments[i + 1].start : a.lengthBeats;
            a.segments[i].length = juce::jmax (0.25, juce::jmin (a.segments[i].end(), limit) - a.segments[i].start);
        }

        return a;
    }

    //--------------------------------------------------------------------------
    //  Parts
    //--------------------------------------------------------------------------
    void buildDrums (MidiClip& clip, const Analysis& a, const GenreProfile& g,
                     double complexity, double creativity, Rng& rng)
    {
        constexpr double sixteenth = 0.25;

        const auto stepsPerBar = juce::jmax (1, (int) std::round (a.beatsPerBar / sixteenth));
        const auto fillEvery   = complexity > 0.7 ? 2 : (complexity > 0.35 ? 4 : 8);
        const auto useRide     = g.useRide && complexity > 0.45;

        const auto add = [&] (int pitch, double beat, int velocity)
        {
            if (beat >= a.lengthBeats - 1.0e-6)
                return;

            clip.notes.push_back (makeNote (pitch, beat, sixteenth, velocity, 10));
        };

        for (int bar = 0; bar < a.numBars; ++bar)
        {
            const auto barStart  = (double) bar * a.beatsPerBar;
            const auto isFillBar = fillEvery > 0 && ((bar + 1) % fillEvery) == 0 && bar + 1 < a.numBars;
            const auto fillStart = stepsPerBar - 4;

            for (int step = 0; step < stepsPerBar; ++step)
            {
                if (isFillBar && step >= fillStart)
                    continue;                       // the fill takes over the last beat

                const auto index = step % 16;
                const auto beat  = barStart + swingPosition ((double) step * sixteenth, g.swing);

                // Creativity occasionally drops a templated hit; that is what
                // stops four candidates sounding like four copies.
                const auto keep = ! rng.chance (creativity * 0.10);

                if (hitAt (g.kick, index) && keep)
                    add (36, beat, g.drumVelocity + rng.range (-6, 4));

                if (complexity > 0.4 && hitAt (g.kickExtra, index)
                    && rng.chance (0.4 + complexity * 0.6))
                    add (36, beat, g.drumVelocity - 12 + rng.range (-4, 4));

                if (hitAt (g.snare, index) && keep)
                    add (g.snareNote, beat, g.drumVelocity + rng.range (-5, 5));

                if (complexity > 0.55 && hitAt (g.snareGhost, index)
                    && rng.chance (0.3 + complexity * 0.5))
                    add (38, beat, 30 + rng.range (0, 12));      // ghost notes stay quiet

                if (useRide)
                {
                    if (hitAt (g.ride, index))
                        add (51, beat, g.drumVelocity - 10 + rng.range (-5, 5));
                }
                else if (hitAt (g.hat, index) && keep)
                {
                    add (g.hatNote, beat, g.drumVelocity - 18 + rng.range (-6, 6));
                }

                if (complexity > 0.6 && ! useRide && hitAt (g.hatExtra, index)
                    && rng.chance (0.35 + complexity * 0.5))
                    add (g.hatNote, beat, g.drumVelocity - 30 + rng.range (-4, 4));
            }

            if (isFillBar)
            {
                // Descending tom fill over the last beat, velocity rising into
                // the downbeat - the standard way to signal a section change.
                static const int fillVoices[] { 38, 50, 47, 43 };
                const auto notes = 2 + (int) std::round (complexity * 2.0);

                for (int i = 0; i < notes; ++i)
                {
                    const auto beat = barStart + (double) (fillStart + i) * sixteenth;
                    const auto voice = fillVoices[(size_t) juce::jmin (i, 3)];
                    add (rng.chance (creativity * 0.3) ? 38 : voice,
                         beat, g.drumVelocity - 14 + i * 8);
                }

                add (49, barStart + a.beatsPerBar, g.drumVelocity);   // crash on the downbeat
            }
        }
    }

    void buildBass (MidiClip& clip, const Analysis& a, const GenreProfile& g,
                    double complexity, double creativity, Rng& rng)
    {
        for (size_t i = 0; i < a.segments.size(); ++i)
        {
            const auto& seg  = a.segments[i];
            const auto& next = a.segments[(i + 1) % a.segments.size()];

            const auto root      = placeNear (seg.chord.root, g.bassCentre);
            const auto nextRoot  = placeNear (next.chord.root, g.bassCentre);
            const auto tones     = chordTonesNear (seg.chord, g.bassCentre);
            const auto velocity  = 88 + (int) std::round (complexity * 12.0);

            const auto add = [&] (int pitch, double start, double length, int vel)
            {
                if (start >= seg.end() - 1.0e-6 || start >= a.lengthBeats - 1.0e-6)
                    return;

                clip.notes.push_back (makeNote (pitch, start,
                                                juce::jmin (length, seg.end() - start), vel));
            };

            switch (g.bass)
            {
                case BassPattern::rootWhole:
                    add (root, seg.start, seg.length * 0.95, velocity);
                    break;

                case BassPattern::rootEighths:
                {
                    const auto step = complexity > 0.55 ? 0.5 : 1.0;

                    for (double b = 0.0; b < seg.length - 1.0e-6; b += step)
                    {
                        const auto onBeat = std::abs (b - std::floor (b)) < 1.0e-6;
                        auto pitch = root;

                        if (! onBeat && rng.chance (0.25 + creativity * 0.4))
                            pitch = root + 12;

                        add (pitch, seg.start + swingPosition (b, g.swing), step * 0.9,
                             onBeat ? velocity : velocity - 14);
                    }
                    break;
                }

                case BassPattern::walking:
                {
                    // Quarter notes: root, chord tones, then a chromatic approach
                    // into the next chord - the whole point of a walking line.
                    const auto quarters = juce::jmax (1, (int) std::round (seg.length));

                    for (int q = 0; q < quarters; ++q)
                    {
                        int pitch = root;

                        if (q == quarters - 1 && quarters > 1)
                        {
                            const auto direction = nextRoot >= root ? -1 : 1;
                            pitch = nextRoot + direction;         // leading tone into the next root
                        }
                        else if (q > 0)
                        {
                            pitch = tones[(size_t) rng.below ((int) tones.size())];

                            if (rng.chance (creativity * 0.3))
                                pitch += 12;
                        }

                        add (pitch, seg.start + q, 0.95, velocity - (q == 0 ? 0 : 6));
                    }
                    break;
                }

                case BassPattern::syncopated:
                {
                    static const char* const pattern = "x..x..x...x.x...";

                    for (int step = 0; (double) step * 0.25 < seg.length - 1.0e-6; ++step)
                    {
                        const auto index = step % 16;

                        if (! hitAt (pattern, index) && ! (complexity > 0.65 && rng.chance (0.12)))
                            continue;

                        const auto octaveUp = rng.chance (0.18 + creativity * 0.25);
                        add (root + (octaveUp ? 12 : 0),
                             seg.start + swingPosition ((double) step * 0.25, g.swing),
                             0.22, velocity - (index == 0 ? 0 : 10));
                    }
                    break;
                }

                case BassPattern::offbeat:
                {
                    for (double b = 0.5; b < seg.length - 1.0e-6; b += 1.0)
                        add (root, seg.start + b, 0.45, velocity);

                    if (complexity > 0.5)
                        add (root, seg.start, 0.45, velocity);
                    break;
                }

                case BassPattern::tumbao:
                {
                    // Bossa: root on the downbeat, fifth on the "and of 2".
                    for (double b = 0.0; b < seg.length - 1.0e-6; b += 2.0)
                    {
                        add (root, seg.start + b, 1.4, velocity);

                        if (b + 1.5 < seg.length)
                            add (root + 7, seg.start + b + 1.5, 0.55, velocity - 8);
                    }
                    break;
                }
            }
        }
    }

    void buildComp (MidiClip& clip, const Analysis& a, const GenreProfile& g, CompPattern pattern,
                    int centre, int numVoices, int baseVelocity,
                    double complexity, double creativity, Rng& rng)
    {
        for (const auto& seg : a.segments)
        {
            auto chord = seg.chord;

            if (g.sevenths && complexity > 0.3)
                chord = withSeventh (chord, a.key);

            auto voicing = theory::voiceChord (chord, centre, numVoices);

            // Creativity rotates the voicing; the harmony is identical, the
            // colour is not, and it is the cheapest real variation available.
            if (! voicing.empty() && rng.chance (creativity * 0.5))
            {
                const auto lowest = voicing.front();
                voicing.erase (voicing.begin());
                voicing.push_back (lowest + 12);
            }

            const auto strike = [&] (double start, double length, int velocity)
            {
                if (start >= seg.end() - 1.0e-6 || start >= a.lengthBeats - 1.0e-6)
                    return;

                const auto clipped = juce::jmin (length, seg.end() - start);

                for (size_t v = 0; v < voicing.size(); ++v)
                {
                    // A tiny per-voice spread: a strummed or rolled chord, not a
                    // block of perfectly simultaneous MIDI.
                    const auto offset = (double) v * 0.006 * (1.0 + creativity);
                    clip.notes.push_back (makeNote (voicing[v], start + offset, clipped,
                                                    velocity - (int) v * 2));
                }
            };

            switch (pattern)
            {
                case CompPattern::sustain:
                    strike (seg.start, seg.length * 0.98, baseVelocity);
                    break;

                case CompPattern::swellPad:
                    strike (seg.start, seg.length * 0.99, baseVelocity - 18);
                    break;

                case CompPattern::quarterStabs:
                {
                    const auto step = complexity > 0.5 ? 1.0 : 2.0;

                    for (double b = 0.0; b < seg.length - 1.0e-6; b += step)
                        strike (seg.start + b, step * 0.6,
                                baseVelocity - (std::abs (b) < 1.0e-6 ? 0 : 8));
                    break;
                }

                case CompPattern::offbeatSkank:
                {
                    for (double b = 0.5; b < seg.length - 1.0e-6; b += 1.0)
                        strike (seg.start + swingPosition (b, g.swing), 0.4, baseVelocity - 6);
                    break;
                }

                case CompPattern::charleston:
                {
                    // Charleston: beat 1 and the "and of 2", the backbone of jazz
                    // and bossa comping.
                    for (double b = 0.0; b < seg.length - 1.0e-6; b += 2.0)
                    {
                        if (rng.chance (0.85 - creativity * 0.25))
                            strike (seg.start + b, 0.9, baseVelocity);

                        if (b + 1.5 < seg.length)
                            strike (seg.start + swingPosition (b + 1.5, g.swing), 0.9, baseVelocity - 7);
                    }
                    break;
                }

                case CompPattern::arpeggiated:
                {
                    const auto step = complexity > 0.5 ? 0.25 : 0.5;
                    int index = 0;

                    for (double b = 0.0; b < seg.length - 1.0e-6; b += step, ++index)
                    {
                        if (voicing.empty())
                            break;

                        const auto pitch = voicing[(size_t) (index % (int) voicing.size())];
                        clip.notes.push_back (makeNote (pitch, seg.start + swingPosition (b, g.swing),
                                                        step * 0.9,
                                                        baseVelocity - 10 - (index % 2) * 6));
                    }
                    break;
                }
            }
        }
    }

    void buildArpeggio (MidiClip& clip, const Analysis& a, const GenreProfile& g,
                        double complexity, double creativity, Rng& rng)
    {
        const auto step = complexity > 0.5 ? 0.25 : 0.5;
        const auto direction = rng.below (3);       // 0 up, 1 down, 2 up-down

        for (const auto& seg : a.segments)
        {
            auto chord = seg.chord;

            if (g.sevenths && complexity > 0.4)
                chord = withSeventh (chord, a.key);

            const auto voicing = theory::voiceChord (chord, g.chordCentre + 12, 4);

            if (voicing.empty())
                continue;

            const auto size = (int) voicing.size();
            int index = 0;

            for (double b = 0.0; b < seg.length - 1.0e-6; b += step, ++index)
            {
                int voice = 0;

                switch (direction)
                {
                    case 0:  voice = index % size; break;
                    case 1:  voice = size - 1 - (index % size); break;
                    default:
                    {
                        const auto cycle = juce::jmax (1, size * 2 - 2);
                        const auto p = index % cycle;
                        voice = p < size ? p : cycle - p;
                        break;
                    }
                }

                auto pitch = voicing[(size_t) juce::jlimit (0, size - 1, voice)];

                if (rng.chance (creativity * 0.15))
                    pitch += 12;

                clip.notes.push_back (makeNote (pitch, seg.start + swingPosition (b, g.swing),
                                                step * 0.95, 68 + rng.range (-6, 6)));
            }
        }
    }

    /** Constrained random walk over chord and scale tones.  Shared by the
        counter melody and by the lead when there is no melody to work from. */
    void buildMelodicLine (MidiClip& clip, const Analysis& a, const GenreProfile& g,
                           int centre, int velocity, bool avoidMelody,
                           double complexity, double creativity, Rng& rng)
    {
        int current = centre;
        int direction = rng.chance (0.5) ? 1 : -1;

        const auto& scale = theory::scaleIntervals (a.key.scale);

        for (const auto& seg : a.segments)
        {
            const auto tones = chordTonesNear (seg.chord, centre);

            // Denser rhythms as complexity rises; the slider has to do something
            // audible or it is decoration.
            const auto step = complexity > 0.7 ? 0.5 : (complexity > 0.35 ? 1.0 : 2.0);

            for (double b = 0.0; b < seg.length - 1.0e-6; b += step)
            {
                if (rng.chance (0.18 * (1.0 - complexity)))
                    continue;                                    // rests keep it from droning

                const auto strongBeat = std::abs (b - std::floor (b)) < 1.0e-6
                                     && ((int) std::llround (b) % 2) == 0;

                int target;

                if (strongBeat || ! rng.chance (creativity * 0.6))
                {
                    // Nearest chord tone to where the line already is.
                    target = tones.front();

                    for (auto t : tones)
                        if (std::abs (t - current) < std::abs (target - current))
                            target = t;

                    if (rng.chance (creativity * 0.35))
                        target += direction * 12;
                }
                else
                {
                    // Step through the scale instead - passing motion.
                    const auto degree = scale[(size_t) rng.below ((int) scale.size())];
                    target = placeNear (a.key.tonic + degree, current);
                }

                // Bias towards small intervals, and turn round at the extremes.
                if (std::abs (target - current) > 9 && rng.chance (0.7))
                    target = current + direction * rng.range (1, 4);

                if (target > centre + 14) direction = -1;
                if (target < centre - 14) direction = 1;

                current = juce::jlimit (36, 96, target);

                const auto start = seg.start + swingPosition (b, g.swing);

                if (avoidMelody)
                {
                    // Do not put a semitone rub right under the tune.
                    const auto clashes = std::any_of (a.melody.begin(), a.melody.end(),
                                                      [&] (const Note& m)
                                                      {
                                                          return m.startBeats < start + step
                                                              && m.endBeats() > start
                                                              && std::abs (m.pitch - current) == 1;
                                                      });

                    if (clashes)
                        continue;
                }

                clip.notes.push_back (makeNote (current, start, step * 0.9,
                                                velocity + rng.range (-8, 8)));
            }
        }
    }

    void buildLead (MidiClip& clip, const Analysis& a, const GenreProfile& g,
                    double complexity, double creativity, Rng& rng)
    {
        if (a.melody.empty())
        {
            buildMelodicLine (clip, a, g, 72, 96, false, complexity, creativity, rng);
            return;
        }

        // Restate the melody in the target feel: swing applied, and at higher
        // creativity the occasional neighbour-tone embellishment.
        for (const auto& source : a.melody)
        {
            if (source.startBeats >= a.lengthBeats)
                continue;

            auto n = source;
            n.startBeats  = swingPosition (source.startBeats, g.swing);
            n.lengthBeats = juce::jmin (source.lengthBeats, a.lengthBeats - n.startBeats);
            n.confidence  = 1.0f;
            n.velocity    = juce::jlimit (1, 127, source.velocity + rng.range (-4, 4));

            if (n.lengthBeats <= 0.0)
                continue;

            if (n.lengthBeats > 0.75 && rng.chance (creativity * 0.3))
            {
                // Split into a neighbour-tone pair rather than one long note.
                const auto half = n.lengthBeats * 0.5;
                auto neighbour = n;
                neighbour.startBeats  = n.startBeats + half;
                neighbour.lengthBeats = half;
                neighbour.pitch       = theory::snapToScale (n.pitch + (rng.chance (0.5) ? 2 : -2), a.key);
                n.lengthBeats = half;

                clip.notes.push_back (n);
                clip.notes.push_back (neighbour);
                continue;
            }

            clip.notes.push_back (n);
        }
    }

    /** Continuation (spec 8.1): take the tail of the phrase as a motif and
        develop it - diatonic transposition, retrograde, rhythmic displacement -
        rather than inventing something unrelated. */
    void buildContinuation (MidiClip& clip, const Analysis& a, const GenreProfile& g,
                            double complexity, double creativity, Rng& rng)
    {
        if (a.melody.empty())
        {
            buildMelodicLine (clip, a, g, 72, 96, false, complexity, creativity, rng);
            return;
        }

        double melodyEnd = 0.0;

        for (const auto& n : a.melody)
            melodyEnd = juce::jmax (melodyEnd, n.endBeats());

        const auto motifLength = a.beatsPerBar * 2.0;
        const auto motifStart  = juce::jmax (0.0, melodyEnd - motifLength);

        std::vector<Note> motif;

        for (const auto& n : a.melody)
            if (n.startBeats >= motifStart - 1.0e-6)
            {
                auto copy = n;
                copy.startBeats -= motifStart;
                motif.push_back (copy);
            }

        if (motif.empty())
        {
            buildMelodicLine (clip, a, g, 72, 96, false, complexity, creativity, rng);
            return;
        }

        const auto& scale = theory::scaleIntervals (a.key.scale);
        const auto degrees = (int) scale.size();

        for (double offset = 0.0; offset < a.lengthBeats - 1.0e-6; offset += motifLength)
        {
            const auto shift     = rng.range (-2, 2);                        // scale degrees
            const auto retrograde = creativity > 0.5 && rng.chance (creativity * 0.35);

            for (const auto& source : motif)
            {
                auto start = source.startBeats;

                if (retrograde)
                    start = motifLength - source.startBeats - source.lengthBeats;

                start += offset;

                if (start >= a.lengthBeats - 1.0e-6 || start < 0.0)
                    continue;

                // Transpose along the scale, not chromatically, so the motif
                // still belongs to the key after it moves.
                auto pitch = source.pitch;

                if (shift != 0 && degrees > 0)
                {
                    const auto snapped = theory::snapToScale (pitch, a.key);
                    auto moved = snapped;

                    for (int s = 0; s < std::abs (shift); ++s)
                        moved = theory::snapToScale (moved + (shift > 0 ? 2 : -2), a.key);

                    pitch = moved;
                }

                // Land the strong beats on chord tones so the development still
                // fits the harmony underneath it.
                const auto& seg = a.segmentAt (start);
                const auto onBeat = std::abs (start - std::round (start)) < 1.0e-6;

                if (onBeat && rng.chance (0.6))
                {
                    const auto tones = chordTonesNear (seg.chord, pitch);
                    auto best = tones.front();

                    for (auto t : tones)
                        if (std::abs (t - pitch) < std::abs (best - pitch))
                            best = t;

                    pitch = best;
                }

                clip.notes.push_back (makeNote (pitch, swingPosition (start, g.swing),
                                                juce::jmin (source.lengthBeats, a.lengthBeats - start),
                                                juce::jlimit (1, 127, source.velocity + rng.range (-6, 6))));
            }
        }
    }

    //--------------------------------------------------------------------------
    MidiClip generatePartImpl (Generator::Part part, const Analysis& analysis,
                               const Generator::Options& options, juce::int64 seed)
    {
        // >>> spec 10.2: this switch is the Transformer seam.  Everything before
        //     it (key, chord segments, bar grid) is the conditioning signal a
        //     model would take; everything after it is unchanged either way.
        Rng rng (seed);

        // Variation is drawn from the seed itself, not from a candidate index,
        // so regeneratePart() with a candidate's seed reproduces it exactly.
        const auto variation = rng.next();

        auto complexity = juce::jlimit (0.0, 1.0, options.complexity + (variation - 0.5) * 0.18);
        auto creativity = juce::jlimit (0.0, 1.0, options.creativity);

        const auto& g = profileFor (options.genre);

        // Genre baseline density: "complexity 0.5" has to mean something
        // different in a ballad than it does in funk.
        complexity = juce::jlimit (0.0, 1.0, complexity * g.density);

        // Mood tags nudge register and density; they are hints, not a language.
        int velocityTrim = 0, registerTrim = 0;

        for (const auto& tag : options.moodTags)
        {
            const auto t = tag.trim().toLowerCase();

            if (t == "bright")                    { velocityTrim += 6;  registerTrim += 12; }
            else if (t == "melancholic" || t == "dark") { velocityTrim -= 8; registerTrim -= 12; }
            else if (t == "driving")              { complexity = juce::jmin (1.0, complexity + 0.15); velocityTrim += 4; }
            else if (t == "calm" || t == "soft")  { complexity = juce::jmax (0.0, complexity - 0.2); velocityTrim -= 10; }
        }

        MidiClip clip;
        clip.name        = Generator::toString (part);
        clip.startBeats  = 0.0;
        clip.lengthBeats = analysis.lengthBeats;

        switch (part)
        {
            case Generator::Part::drums:
                buildDrums (clip, analysis, g, complexity, creativity, rng);
                break;

            case Generator::Part::bass:
                buildBass (clip, analysis, g, complexity, creativity, rng);
                break;

            case Generator::Part::chords:
                buildComp (clip, analysis, g, g.comp, g.chordCentre + registerTrim / 2,
                           complexity > 0.5 ? 4 : 3, 84 + velocityTrim,
                           complexity, creativity, rng);
                break;

            case Generator::Part::pad:
                buildComp (clip, analysis, g, CompPattern::swellPad, g.chordCentre - 12 + registerTrim / 2,
                           4, 58 + velocityTrim, complexity, creativity, rng);
                break;

            case Generator::Part::strings:
                buildComp (clip, analysis, g, CompPattern::sustain, g.chordCentre + 12 + registerTrim / 2,
                           complexity > 0.5 ? 5 : 4, 70 + velocityTrim, complexity, creativity, rng);
                break;

            case Generator::Part::arpeggio:
                buildArpeggio (clip, analysis, g, complexity, creativity, rng);
                break;

            case Generator::Part::counterMelody:
                buildMelodicLine (clip, analysis, g, g.chordCentre + 5 + registerTrim / 2,
                                  74 + velocityTrim, true, complexity, creativity, rng);
                break;

            case Generator::Part::lead:
                if (options.mode == Generator::Mode::continuation)
                    buildContinuation (clip, analysis, g, complexity, creativity, rng);
                else
                    buildLead (clip, analysis, g, complexity, creativity, rng);
                break;
        }

        // A touch of the genre's own feel before any user humanisation on top.
        // Straight drum machines stay on the grid; everything else breathes.
        const auto baselineTiming = (part == Generator::Part::drums && g.swing <= 0.505)
                                  ? 0.0 : 0.15 * creativity;

        Generator::humanise (clip.notes,
                             juce::jmax (options.humanizeTiming, baselineTiming),
                             juce::jmax (options.humanizeVelocity, 0.22),
                             analysis.bpm, analysis.timeSignature, mixSeed (seed, 977));

        clip.sortNotes();
        return clip;
    }

    std::vector<Generator::Part> partsForMode (const Generator::Options& options)
    {
        using Part = Generator::Part;

        switch (options.mode)
        {
            case Generator::Mode::drumPattern:   return { Part::drums };
            case Generator::Mode::harmonise:     return { Part::chords };
            case Generator::Mode::humanise:      return { Part::lead };
            case Generator::Mode::continuation:  return { Part::lead };

            case Generator::Mode::accompaniment:
            {
                std::vector<Part> parts { Part::chords, Part::bass };

                if (std::find (options.parts.begin(), options.parts.end(), Part::drums) != options.parts.end())
                    parts.push_back (Part::drums);

                return parts;
            }

            case Generator::Mode::styleTransfer:
            {
                auto parts = options.parts;

                if (std::find (parts.begin(), parts.end(), Part::lead) == parts.end())
                    parts.push_back (Part::lead);

                return parts;
            }

            case Generator::Mode::autoArrange:
            default:
                break;
        }

        return options.parts.empty() ? std::vector<Part> { Part::drums, Part::bass, Part::chords }
                                     : options.parts;
    }
}

//==============================================================================
std::vector<Generator::Candidate> Generator::generate (const Input& input, const Options& options,
                                                       ProgressFn progress)
{
    std::vector<Candidate> candidates;

    // Returns false once the caller has asked to stop; from then on it keeps
    // saying false without calling back again, so a bail-out on the way out of
    // the loop cannot resurrect the run.
    bool aborted = false;

    const auto report = [&progress, &aborted] (float p)
    {
        if (! aborted && progress && ! progress (juce::jlimit (0.0f, 1.0f, p)))
            aborted = true;

        return ! aborted;
    };

    if (! report (0.0f))
        return candidates;

    const auto analysis = analyseInput (input, options);
    const auto parts    = partsForMode (options);
    const auto numCandidates = juce::jlimit (1, 16, options.numCandidates);

    // A zero seed means "surprise me", but we still record the seed we used so
    // the candidate can be reproduced or re-rolled later.
    const auto baseSeed = options.seed != 0 ? options.seed
                                            : (juce::int64) juce::Time::getHighResolutionTicks();

    // Source material to measure novelty against.  With no melody, the chord
    // roots stand in for it - otherwise every candidate would score 1.0.
    std::vector<Note> source = input.melody;

    if (source.empty())
        for (const auto& seg : analysis.segments)
            source.push_back (makeNote (placeNear (seg.chord.root, 60), seg.start, seg.length, 90));

    for (int i = 0; i < numCandidates; ++i)
    {
        const auto candidateSeed = mixSeed (baseSeed, i + 1);

        Candidate candidate;
        candidate.seed = candidateSeed;

        std::vector<Note> allNotes;

        for (auto part : parts)
        {
            auto clip = generatePartImpl (part, analysis, options, mixSeed (candidateSeed, (juce::int64) part));

            if (options.mode == Mode::humanise && part == Part::lead)
            {
                // Humanise mode is the identity arrangement plus feel: keep the
                // user's notes, only move them.
                clip.notes = input.melody;
                humanise (clip.notes,
                          juce::jmax (0.35, options.humanizeTiming),
                          juce::jmax (0.45, options.humanizeVelocity),
                          analysis.bpm, analysis.timeSignature, candidateSeed);
                clip.sortNotes();
            }

            allNotes.insert (allNotes.end(), clip.notes.begin(), clip.notes.end());
            candidate.parts.emplace_back (part, std::move (clip));

            // Parts are the finest checkpoint the arranger has; a single part is
            // milliseconds, so Cancel feels instant from here.
            if (! report ((float) ((i + (double) candidate.parts.size() / (double) juce::jmax ((size_t) 1, parts.size()))
                                       / (double) numCandidates)))
                return candidates;
        }

        candidate.noveltyScore = noveltyBetween (allNotes, source, analysis.beatsPerBar);

        candidate.name = ss::toString (options.genre) + " " + toString (options.mode).toLowerCase()
                       + " " + juce::String (i + 1);

        juce::StringArray partNames;

        for (const auto& part : candidate.parts)
            partNames.add (toString (part.first));

        // Spec 8.1 asks for style transfer's limits to be visible in the UI, and
        // the gallery now has a place to put text that is not the name.
        candidate.notes = (options.mode == Mode::styleTransfer
                              ? juce::String ("Experimental: the arrangement is re-voiced by rule, "
                                              "so the harmony can drift from the source - check it. ")
                              : juce::String())
                        + partNames.joinIntoString (", ")
                        + " - seed " + juce::String (candidateSeed);

        candidates.push_back (std::move (candidate));

        if (! report ((float) (i + 1) / (float) numCandidates))
            return candidates;      // partial result: the caller asked to stop
    }

    report (1.0f);
    return candidates;
}

//==============================================================================
MidiClip Generator::regeneratePart (const Input& input, const Options& options,
                                    Part part, juce::int64 seed)
{
    // Same analysis, same per-part seed derivation as generate() - so every
    // other part of the candidate stays byte-identical.
    const auto analysis = analyseInput (input, options);
    return generatePartImpl (part, analysis, options, mixSeed (seed, (juce::int64) part));
}

//==============================================================================
double Generator::grooveAccent (double beat, const TimeSignatureEvent& timeSignature)
{
    const auto numerator   = juce::jmax (1, timeSignature.numerator);
    const auto denominator = juce::jmax (1, timeSignature.denominator);

    // Everything below counts in sixteenths of a quarter-note beat, which is the
    // grid the accents were tuned on.
    const auto sixteenthsPerBar = juce::jmax (1, juce::roundToInt (numerator * 16.0 / denominator));

    // Compound meters (6/8, 9/8, 12/8) pulse in dotted quarters - three notated
    // eighths, six sixteenths - not on every eighth.
    const auto compound      = denominator == 8 && numerator % 3 == 0 && numerator > 3;
    const auto perPulse      = compound ? 6 : juce::jmax (1, juce::roundToInt (16.0 / denominator));
    const auto step          = ((juce::roundToInt (beat * 4.0) % sixteenthsPerBar) + sixteenthsPerBar)
                                   % sixteenthsPerBar;

    if (step == 0)
        return 1.00;                                  // bar downbeat

    if (step % perPulse == 0)
        // The half-way pulse carries the secondary accent, the way beat 3 does
        // in 4/4.  Odd meters have no exact midpoint, so nobody gets the lift.
        return step * 2 == sixteenthsPerBar ? 0.96 : 0.91;

    if (perPulse % 2 == 0 && step % (perPulse / 2) == 0)
        return 0.86;                                  // the "and" of a pulse

    return 0.73;                                      // weak subdivisions
}

void Generator::humanise (std::vector<Note>& notes, double timingAmount, double velocityAmount,
                          double bpm, const TimeSignatureEvent& timeSignature, juce::int64 seed)
{
    if (notes.empty())
        return;

    juce::Random rng (seed);

    const auto timing   = juce::jlimit (0.0, 1.0, timingAmount);
    const auto velocity = juce::jlimit (0.0, 1.0, velocityAmount);

    if (timing <= 0.0 && velocity <= 0.0)
        return;

    // 25 ms at full strength.  Past that it stops sounding human and starts
    // sounding like a mistake, so the slider is scaled to a musical range.
    const auto beatsPerSecond  = juce::jmax (20.0, bpm) / 60.0;
    const auto maxJitterBeats  = 0.025 * timing * beatsPerSecond;

    // ponytail: the accent grid is a sixteenth-note quantisation of the bar, so
    // triplet and quintuplet figures land on the nearest sixteenth's accent, and
    // positions are read as bar-aligned - a clip that starts mid-bar accents
    // against its own start.  A tuplet-aware grid taking the clip offset is the
    // upgrade if swung triplet feels or off-bar clips ever matter.

    for (auto& n : notes)
    {
        // Accent comes off the written position, before any jitter moves it.
        const auto accent = grooveAccent (n.startBeats, timeSignature);

        if (velocity > 0.0)
        {
            const auto shaped   = (double) n.velocity * accent
                                * (1.0 + (rng.nextDouble() - 0.5) * 0.12);
            const auto blended  = (double) n.velocity + (shaped - (double) n.velocity) * velocity;
            n.velocity = juce::jlimit (1, 127, juce::roundToInt (blended));
        }

        if (maxJitterBeats > 0.0)
        {
            // Two uniforms averaged gives a rough bell: most notes stay near the
            // grid and only a few stray, which is how people actually play.
            const auto jitter = ((rng.nextDouble() + rng.nextDouble()) - 1.0) * maxJitterBeats;
            n.startBeats = juce::jmax (0.0, n.startBeats + jitter);
        }
    }
}

//==============================================================================
std::vector<ChordEvent> Generator::harmonise (const std::vector<Note>& melody, const theory::Key& key,
                                              double complexity, double barLengthBeats)
{
    const auto bar = barLengthBeats > 0.0 ? barLengthBeats : 4.0;
    const auto diatonicTriads   = theory::diatonicChords (key, false);
    const auto diatonicSevenths = theory::diatonicChords (key, true);

    std::vector<ChordEvent> chords;

    if (melody.empty())
    {
        // No melody to follow: the most useful default progression there is.
        static const int degrees[] { 0, 4, 5, 3 };   // I - V - vi - IV

        for (int i = 0; i < 4; ++i)
        {
            auto c = complexity > 0.34 ? diatonicSevenths[(size_t) degrees[i]]
                                       : diatonicTriads[(size_t) degrees[i]];
            c.beat        = (double) i * bar;
            c.lengthBeats = bar;
            chords.push_back (c);
        }

        return chords;
    }

    double end = 0.0;

    for (const auto& n : melody)
        end = juce::jmax (end, n.endBeats());

    const auto numBars = juce::jmax (1, (int) std::ceil (end / bar - 1.0e-6));

    for (int b = 0; b < numBars; ++b)
    {
        const auto start = (double) b * bar;
        auto detected = theory::detectChord (melody, start, start + bar, key);

        // Prefer the diatonic chord on the same root: detectChord works off a
        // handful of melody notes, and the key is the stronger prior.
        const auto& table = complexity > 0.34 ? diatonicSevenths : diatonicTriads;

        for (const auto& d : table)
            if (d.root == detected.root)
            {
                detected.intervals = d.intervals;
                break;
            }

        if (complexity <= 0.34 && detected.intervals.size() > 3)
            detected.intervals.resize (3);

        detected.beat        = start;
        detected.lengthBeats = bar;
        detected.symbol      = theory::chordSymbolFor (detected.root, detected.intervals,
                                                       theory::keyPrefersFlats (key));

        chords.push_back (detected);
    }

    if (complexity <= 0.67)
        return chords;

    //--- secondary dominants --------------------------------------------------
    // Split the second half of a bar into V7 of whatever comes next, but only
    // when the melody there does not rub against the new third or seventh.
    std::vector<ChordEvent> withDominants;
    withDominants.reserve (chords.size() * 2);

    for (size_t i = 0; i < chords.size(); ++i)
    {
        auto current = chords[i];

        if (i + 1 >= chords.size())
        {
            withDominants.push_back (current);
            break;
        }

        const auto nextRoot = chords[i + 1].root;
        const auto dominantRoot = ((nextRoot + 7) % 12 + 12) % 12;

        if (dominantRoot == current.root)
        {
            withDominants.push_back (current);
            continue;
        }

        const auto half = current.beat + current.lengthBeats * 0.5;

        ChordEvent dominant;
        dominant.root        = dominantRoot;
        dominant.intervals   = { 0, 4, 7, 10 };
        dominant.beat        = half;
        dominant.lengthBeats = current.lengthBeats * 0.5;
        dominant.symbol      = theory::chordSymbolFor (dominant.root, dominant.intervals,
                                                       theory::keyPrefersFlats (key));

        const auto clashes = std::any_of (melody.begin(), melody.end(),
            [&] (const Note& n)
            {
                if (n.endBeats() <= half || n.startBeats >= dominant.beat + dominant.lengthBeats)
                    return false;

                const auto degree = ((n.pitch - dominantRoot) % 12 + 12) % 12;
                return degree == 3 || degree == 5 || degree == 11;   // b3, 4 and maj7 against a V7
            });

        if (clashes)
        {
            withDominants.push_back (current);
            continue;
        }

        current.lengthBeats *= 0.5;
        withDominants.push_back (current);
        withDominants.push_back (dominant);
    }

    return withDominants;
}

} // namespace ss
