#pragma once
#include "Core/Project.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace ss::io
{
    /** Standard MIDI File in/out (spec 10.4).  Import maps each SMF track to one
        MidiClip; export writes one track per project MIDI track plus a tempo
        map track. */
    bool importMidiFile (const juce::File&, Project&, juce::String& errorOut);
    std::vector<Note> readMidiFileNotes (const juce::File&, double& bpmOut);
    bool exportMidiFile (const juce::File&, const Project&, juce::String& errorOut);
    bool exportMidiClip (const juce::File&, const MidiClip&, double bpm, juce::String& errorOut);

    /** Audio file import: returns the created clip, converting sample rate if
        it differs from the project's. */
    bool importAudioFile (const juce::File&, Project&, TrackId, double atBeat,
                          juce::AudioFormatManager&, juce::String& errorOut);

    /** Reads a whole file into memory (used by the transcriber and the editor). */
    bool readAudioFile (const juce::File&, juce::AudioFormatManager&,
                        juce::AudioBuffer<float>& out, double& sampleRateOut);

    /** Writes a buffer using the format implied by the extension
        (.wav/.aiff/.flac).  bitDepth 32 means 32-bit float (spec 10.3, v0.8). */
    bool writeAudioFile (const juce::File&, const juce::AudioBuffer<float>&,
                         double sampleRate, int bitDepth, juce::String& errorOut);

    /** MusicXML export/import (spec 10.4, 8.5). */
    bool exportMusicXml (const juce::File&, const Project&, juce::String& errorOut);
    bool importMusicXml (const juce::File&, Project&, juce::String& errorOut);

    juce::StringArray getSupportedAudioExtensions();
}
