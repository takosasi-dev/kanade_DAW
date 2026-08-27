#include "IO/FileIO.h"

namespace ss::io
{
    juce::StringArray getSupportedAudioExtensions()
    {
        return { "wav", "aiff", "aif", "flac", "mp3", "ogg" };
    }

    //==============================================================================
    // MIDI
    //==============================================================================
    namespace
    {
        /*  A JUCE MidiFile stores times in ticks; we convert once, up front, using
            the file's own time format so imported material lands on the right
            beats regardless of what PPQ the exporting program used. */
        std::vector<Note> notesFromSequence (const juce::MidiMessageSequence& seq, double ticksPerQuarter)
        {
            std::vector<Note> notes;

            for (int i = 0; i < seq.getNumEvents(); ++i)
            {
                auto* event = seq.getEventPointer (i);
                const auto& msg = event->message;

                if (! msg.isNoteOn())
                    continue;

                const auto startBeats = msg.getTimeStamp() / ticksPerQuarter;
                double endBeats = startBeats + 1.0;

                if (event->noteOffObject != nullptr)
                    endBeats = event->noteOffObject->message.getTimeStamp() / ticksPerQuarter;

                Note n;
                n.pitch       = msg.getNoteNumber();
                n.velocity    = juce::jmax (1, (int) msg.getVelocity());
                n.channel     = msg.getChannel();
                n.startBeats  = startBeats;
                n.lengthBeats = juce::jmax (1.0 / 32.0, endBeats - startBeats);
                n.confidence  = 1.0f;      // authored data, not a guess
                notes.push_back (n);
            }

            std::stable_sort (notes.begin(), notes.end(),
                              [] (const Note& a, const Note& b) { return a.startBeats < b.startBeats; });
            return notes;
        }
    }

    std::vector<Note> readMidiFileNotes (const juce::File& source, double& bpmOut)
    {
        bpmOut = 120.0;
        juce::FileInputStream stream (source);

        if (! stream.openedOk())
            return {};

        juce::MidiFile midi;

        if (! midi.readFrom (stream))
            return {};

        const auto timeFormat = midi.getTimeFormat();
        const auto ticksPerQuarter = timeFormat > 0 ? (double) timeFormat : 960.0;

        juce::MidiMessageSequence tempoEvents;
        midi.findAllTempoEvents (tempoEvents);

        for (int i = 0; i < tempoEvents.getNumEvents(); ++i)
        {
            const auto& msg = tempoEvents.getEventPointer (i)->message;

            if (msg.isTempoMetaEvent())
            {
                bpmOut = 60.0 / (msg.getTempoSecondsPerQuarterNote());
                break;
            }
        }

        juce::MidiMessageSequence merged;

        for (int t = 0; t < midi.getNumTracks(); ++t)
        {
            auto track = *midi.getTrack (t);
            track.updateMatchedPairs();
            merged.addSequence (track, 0.0);
        }

        merged.updateMatchedPairs();
        return notesFromSequence (merged, ticksPerQuarter);
    }

    bool importMidiFile (const juce::File& source, Project& project, juce::String& errorOut)
    {
        juce::FileInputStream stream (source);

        if (! stream.openedOk())
        {
            errorOut = "Could not open " + source.getFullPathName();
            return false;
        }

        juce::MidiFile midi;

        if (! midi.readFrom (stream))
        {
            errorOut = source.getFileName() + " is not a valid MIDI file";
            return false;
        }

        const auto timeFormat = midi.getTimeFormat();

        if (timeFormat <= 0)
        {
            // SMPTE-timed files exist but are vanishingly rare in music tooling.
            errorOut = "SMPTE-timed MIDI files are not supported";
            return false;
        }

        const auto ticksPerQuarter = (double) timeFormat;

        // Tempo map first, so the clips that follow land at the right seconds.
        juce::MidiMessageSequence tempoEvents;
        midi.findAllTempoEvents (tempoEvents);
        std::vector<TempoEvent> tempoMap;

        for (int i = 0; i < tempoEvents.getNumEvents(); ++i)
        {
            const auto& msg = tempoEvents.getEventPointer (i)->message;

            if (msg.isTempoMetaEvent())
                tempoMap.push_back ({ msg.getTimeStamp() / ticksPerQuarter,
                                      60.0 / msg.getTempoSecondsPerQuarterNote() });
        }

        if (! tempoMap.empty())
            project.tempo.setEvents (std::move (tempoMap));

        juce::MidiMessageSequence timeSigEvents;
        midi.findAllTimeSigEvents (timeSigEvents);
        std::vector<TimeSignatureEvent> sigs;

        for (int i = 0; i < timeSigEvents.getNumEvents(); ++i)
        {
            const auto& msg = timeSigEvents.getEventPointer (i)->message;

            if (msg.isTimeSignatureMetaEvent())
            {
                int numerator = 4, denominator = 4;
                msg.getTimeSignatureInfo (numerator, denominator);
                sigs.push_back ({ msg.getTimeStamp() / ticksPerQuarter, numerator, denominator });
            }
        }

        if (! sigs.empty())
            project.tempo.setTimeSignatures (std::move (sigs));

        int imported = 0;

        for (int t = 0; t < midi.getNumTracks(); ++t)
        {
            auto sequence = *midi.getTrack (t);
            sequence.updateMatchedPairs();
            auto notes = notesFromSequence (sequence, ticksPerQuarter);

            if (notes.empty())
                continue;      // tempo/marker-only tracks

            juce::String trackName = "MIDI " + juce::String (t + 1);

            for (int i = 0; i < sequence.getNumEvents(); ++i)
            {
                const auto& msg = sequence.getEventPointer (i)->message;

                if (msg.isTrackNameEvent() && msg.getTextFromTextMetaEvent().isNotEmpty())
                {
                    trackName = msg.getTextFromTextMetaEvent();
                    break;
                }
            }

            auto& track = project.addTrack (TrackType::midi, trackName);

            const auto firstBeat = notes.front().startBeats;
            double lastBeat = 0.0;

            for (auto& n : notes)
            {
                n.startBeats -= firstBeat;
                lastBeat = juce::jmax (lastBeat, n.endBeats());
            }

            MidiClip clip;
            clip.id          = project.nextClipId();
            clip.name        = trackName;
            clip.startBeats  = firstBeat;
            clip.lengthBeats = juce::jmax (4.0, lastBeat);
            clip.notes       = std::move (notes);
            track.midiClips.push_back (std::move (clip));
            ++imported;
        }

        if (imported == 0)
        {
            errorOut = source.getFileName() + " contains no notes";
            return false;
        }

        project.markDirty();
        return true;
    }

    //==============================================================================
    namespace
    {
        constexpr int exportTicksPerQuarter = 960;

        void addNotesToSequence (juce::MidiMessageSequence& seq, const std::vector<Note>& notes,
                                 double offsetBeats)
        {
            for (const auto& n : notes)
            {
                const auto start = (n.startBeats + offsetBeats) * exportTicksPerQuarter;
                const auto end   = (n.endBeats()  + offsetBeats) * exportTicksPerQuarter;

                seq.addEvent (juce::MidiMessage::noteOn  (n.channel, n.pitch, (juce::uint8) n.velocity), start);
                seq.addEvent (juce::MidiMessage::noteOff (n.channel, n.pitch), juce::jmax (start + 1.0, end));
            }

            seq.updateMatchedPairs();
            seq.sort();
        }
    }

    bool exportMidiFile (const juce::File& destination, const Project& project, juce::String& errorOut)
    {
        juce::MidiFile midi;
        midi.setTicksPerQuarterNote (exportTicksPerQuarter);

        // Track 0 carries tempo and time signature, per the SMF type-1 convention.
        {
            juce::MidiMessageSequence conductor;

            for (const auto& t : project.tempo.getEvents())
                conductor.addEvent (juce::MidiMessage::tempoMetaEvent ((int) (60'000'000.0 / t.bpm)),
                                    t.beat * exportTicksPerQuarter);

            for (const auto& t : project.tempo.getTimeSignatures())
                conductor.addEvent (juce::MidiMessage::timeSignatureMetaEvent (t.numerator, t.denominator),
                                    t.beat * exportTicksPerQuarter);

            for (const auto& m : project.markers)
                conductor.addEvent (juce::MidiMessage::textMetaEvent (6, m.name), m.beat * exportTicksPerQuarter);

            conductor.addEvent (juce::MidiMessage::endOfTrack(),
                                juce::jmax (1.0, project.endBeats() * exportTicksPerQuarter));
            midi.addTrack (conductor);
        }

        int exported = 0;

        for (const auto& track : project.getTracks())
        {
            if (track->getType() != TrackType::midi || track->midiClips.empty())
                continue;

            juce::MidiMessageSequence seq;
            seq.addEvent (juce::MidiMessage::textMetaEvent (3, track->name), 0.0);

            for (const auto& clip : track->midiClips)
                addNotesToSequence (seq, clip.notes, clip.startBeats);

            seq.addEvent (juce::MidiMessage::endOfTrack(),
                          juce::jmax (1.0, track->endBeats() * exportTicksPerQuarter));
            midi.addTrack (seq);
            ++exported;
        }

        if (exported == 0)
        {
            errorOut = "There are no MIDI tracks to export";
            return false;
        }

        destination.deleteFile();
        juce::FileOutputStream stream (destination);

        if (! stream.openedOk())
        {
            errorOut = "Could not write to " + destination.getFullPathName();
            return false;
        }

        if (! midi.writeTo (stream))
        {
            errorOut = "Failed to write MIDI data";
            return false;
        }

        return true;
    }

    bool exportMidiClip (const juce::File& destination, const MidiClip& clip, double bpm,
                         juce::String& errorOut)
    {
        juce::MidiFile midi;
        midi.setTicksPerQuarterNote (exportTicksPerQuarter);

        juce::MidiMessageSequence seq;
        seq.addEvent (juce::MidiMessage::tempoMetaEvent ((int) (60'000'000.0 / juce::jmax (1.0, bpm))), 0.0);
        seq.addEvent (juce::MidiMessage::textMetaEvent (3, clip.name), 0.0);
        addNotesToSequence (seq, clip.notes, 0.0);
        seq.addEvent (juce::MidiMessage::endOfTrack(),
                      juce::jmax (1.0, clip.lengthBeats * exportTicksPerQuarter));
        midi.addTrack (seq);

        destination.deleteFile();
        juce::FileOutputStream stream (destination);

        if (! stream.openedOk())
        {
            errorOut = "Could not write to " + destination.getFullPathName();
            return false;
        }

        return midi.writeTo (stream);
    }

    //==============================================================================
    // Audio
    //==============================================================================
    bool readAudioFile (const juce::File& source, juce::AudioFormatManager& formats,
                        juce::AudioBuffer<float>& out, double& sampleRateOut)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));

        if (reader == nullptr)
            return false;

        sampleRateOut = reader->sampleRate;
        out.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&out, 0, (int) reader->lengthInSamples, 0, true, true);
        return true;
    }

    bool importAudioFile (const juce::File& source, Project& project, TrackId trackId,
                          double atBeat, juce::AudioFormatManager& formats, juce::String& errorOut)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));

        if (reader == nullptr)
        {
            errorOut = "Unsupported or unreadable audio file: " + source.getFileName();
            return false;
        }

        auto* track = project.findTrack (trackId);

        if (track == nullptr || track->getType() != TrackType::audio)
        {
            errorOut = "Audio can only be dropped onto an audio track";
            return false;
        }

        const auto lengthSeconds = reader->lengthInSamples / juce::jmax (1.0, reader->sampleRate);
        const auto startSeconds  = project.tempo.beatsToSeconds (atBeat);
        const auto lengthBeats   = project.tempo.secondsToBeats (startSeconds + lengthSeconds) - atBeat;

        AudioClip clip;
        clip.id          = project.nextClipId();
        clip.name        = source.getFileNameWithoutExtension();
        clip.sourceFile  = source;
        clip.startBeats  = atBeat;
        clip.lengthBeats = juce::jmax (0.25, lengthBeats);
        track->audioClips.push_back (std::move (clip));

        project.markDirty();
        return true;
    }

    bool writeAudioFile (const juce::File& destination, const juce::AudioBuffer<float>& buffer,
                         double sampleRate, int bitDepth, juce::String& errorOut)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        auto* format = formats.findFormatForFileExtension (destination.getFileExtension());

        if (format == nullptr)
        {
            errorOut = "Unsupported output format: " + destination.getFileExtension();
            return false;
        }

        destination.deleteFile();
        auto stream = std::make_unique<juce::FileOutputStream> (destination);

        if (! stream->openedOk())
        {
            errorOut = "Could not write to " + destination.getFullPathName();
            return false;
        }

        // 32 means float in ScoreSmith (spec 10.3, v0.8); JUCE wants the flag set
        // separately from the bit depth.
        const auto useFloat = (bitDepth >= 32);
        juce::StringPairArray metadata;

        std::unique_ptr<juce::AudioFormatWriter> writer (
            format->createWriterFor (stream.get(), sampleRate,
                                     (unsigned int) buffer.getNumChannels(),
                                     useFloat ? 32 : bitDepth, metadata, 0));

        if (writer == nullptr)
        {
            errorOut = "This format does not support "
                     + juce::String (bitDepth) + "-bit at " + juce::String (sampleRate, 0) + " Hz";
            return false;
        }

        // createWriterFor only takes ownership on success (it leaves the stream
        // alone when it returns nullptr), so release only once we have a writer.
        stream.release();

        if (! writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples()))
        {
            errorOut = "Failed while writing audio data";
            return false;
        }

        return true;
    }
}
