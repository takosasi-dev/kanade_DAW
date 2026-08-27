#include "Vocal/ResamplerArgs.h"

namespace ss
{
    juce::String midiNoteToUtauPitch (int midiNoteNumber)
    {
        static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const auto octave = midiNoteNumber / 12 - 1;
        const auto name = names[juce::jlimit (0, 11, midiNoteNumber % 12)];
        return juce::String (name) + juce::String (octave);
    }

    juce::StringArray buildResamplerArgs (const juce::File& resamplerExe, const juce::File& inputWav,
                                          const juce::File& outputWav, const UtauNote& note,
                                          const OtoEntry& oto, double lengthMs)
    {
        const auto preUtterance = note.preUtteranceMs >= 0.0 ? note.preUtteranceMs : oto.preUtterance;

        juce::StringArray args;
        args.add (resamplerExe.getFullPathName());
        args.add (inputWav.getFullPathName());
        args.add (outputWav.getFullPathName());
        args.add (midiNoteToUtauPitch (note.pitch));
        args.add (juce::String (note.velocity));
        args.add (note.flags);
        args.add (juce::String (oto.offset + preUtterance, 3));
        args.add (juce::String (lengthMs, 3));
        args.add (juce::String (oto.consonant, 3));
        args.add (juce::String (oto.cutoff, 3));
        args.add (juce::String (note.intensity));
        args.add (juce::String (note.modulation));
        args.add ("100");   // tempo - unused by most resamplers without a pitchbend curve, kept for CLI shape compatibility
        args.add ({});      // pitchbend - Phase 1 scope cut, see UtauPitchBend's doc comment
        return args;
    }
}
