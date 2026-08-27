#pragma once
#include "Core/UtauTypes.h"
#include <juce_core/juce_core.h>
#include <vector>

namespace ss
{
    /** Ticks per quarter note in the classic UST format (same convention as a
        480-PPQ MIDI file). */
    inline constexpr int ustTicksPerBeat = 480;

    /** Parses a .ust file's raw bytes into a flat, file-order note list.
        A .ust stores notes as a strictly sequential list with no explicit
        start position - startBeats is computed here by accumulating each
        note's own length. Decodes text via decodeUstText() (ShiftJis.h)
        internally, so callers can pass raw file bytes as-is. */
    std::vector<UtauNote> parseUstFile (const void* data, size_t numBytes);

    /** Writes `notes` back out in one-section-per-note .ust text, preserving
        each note's `extra` keys verbatim. Always UTF-8 - Phase 1 does not
        write Shift-JIS. */
    juce::String writeUstFile (const std::vector<UtauNote>& notes);
}
