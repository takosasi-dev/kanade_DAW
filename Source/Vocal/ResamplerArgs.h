#pragma once
#include "Core/UtauTypes.h"
#include "IO/OtoIni.h"
#include <juce_core/juce_core.h>

namespace ss
{
    /** Converts a MIDI note number to the note-name string the classic UTAU
        resampler CLI expects (e.g. 60 -> "C4"). */
    juce::String midiNoteToUtauPitch (int midiNoteNumber);

    /** Builds the argv for one note's resampler invocation, following the
        classic UTAU resampler CLI protocol:
            resampler.exe <in> <out> <pitch> <velocity> <flags> <offsetMs>
                          <lengthMs> <consonantMs> <cutoffMs> <intensity>
                          <modulation> <tempo> <pitchbend>
        `lengthMs` is precomputed by the caller (UtauRenderer, Task 10) from
        the project's real tempo map - this function has no tempo dependency
        of its own, which keeps it a pure, easily-tested string builder.
        Phase 1 always passes an empty pitchbend argument (flat pitch - see
        UtauPitchBend's doc comment for why). */
    juce::StringArray buildResamplerArgs (const juce::File& resamplerExe, const juce::File& inputWav,
                                          const juce::File& outputWav, const UtauNote& note,
                                          const OtoEntry& oto, double lengthMs);
}
