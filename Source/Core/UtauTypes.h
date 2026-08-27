#pragma once
#include "Core/Types.h"
#include <juce_core/juce_core.h>
#include <vector>

/*  UTAU integration Phase 1 (docs/superpowers/specs/2026-08-26-utau-integration-
    phase1-design.md). These types mirror a classic .ust file closely enough to
    round-trip losslessly - see UstFile.h for the parser that produces them. */

namespace ss
{
    /** UST's PBS/PBW/PBY/PBM, kept as raw parsed values. Phase 1 does not apply
        this curve when rendering (every note renders at its flat pitch) - it is
        parsed and preserved purely so re-exporting a .ust does not lose data.
        ponytail: applying this curve for real means encoding the resampler's
        proprietary RLE pitch-string format, which needs a verified reference
        implementation to get bit-exact rather than reconstructing from memory -
        deferred past Phase 1 rather than risking silently wrong pitch. */
    struct UtauPitchBend
    {
        double startMs        = 0.0;   // PBS x
        double startSemitones = 0.0;   // PBS y
        std::vector<double>       widthsMs;          // PBW
        std::vector<double>       heightsSemitones;  // PBY
        std::vector<juce::String> curveTypes;        // PBM ("" | "s" | "r" | "j")
    };

    /** One UST note. `startBeats`/`lengthBeats` are RELATIVE to the owning
        UtauClip's startBeats, exactly like Note/MidiClip already work. */
    struct UtauNote
    {
        double startBeats  = 0.0;
        double lengthBeats = 1.0;
        int    pitch       = 60;      // NoteNum - MIDI note number
        juce::String lyric = "a";
        bool   isRest      = false;   // Lyric == "R"
        int    velocity    = 100;     // consonant speed, 0-200
        int    intensity   = 100;     // volume, 0-200
        int    modulation  = 0;       // pitch wobble depth, 0-100
        double preUtteranceMs = -1.0; // -1 == use the oto.ini value
        double voiceOverlapMs = -1.0; // -1 == use the oto.ini value
        juce::String flags;           // passed to the resampler verbatim
        UtauPitchBend pitchBend;
        juce::String envelope;        // raw "p1,p2,..." string, not edited in Phase 1
        /** Any .ust key not modelled above (future extensions, other tools'
            custom tags), so writeUstFile() can write it straight back out. */
        juce::NamedValueSet extra;

        double endBeats() const noexcept { return startBeats + lengthBeats; }
    };

    /** One UTAU clip on a TrackType::utau track. */
    struct UtauClip
    {
        ClipId id = invalidClipId;
        juce::String name;
        double startBeats  = 0.0;
        double lengthBeats = 4.0;
        std::vector<UtauNote> notes;

        juce::String voicebankId;          // VoicebankLibrary's identifier
        juce::File   renderedFile;         // cached render; empty == not rendered
        juce::int64  notesHashAtRender = 0; // currentContentHash() at the time renderedFile was produced

        double endBeats() const noexcept { return startBeats + lengthBeats; }

        /** Hash of everything that affects the rendered sound (notes + which
            voicebank). Compare against notesHashAtRender to know whether
            renderedFile is stale. */
        juce::int64 currentContentHash() const noexcept;
    };
}
