#include "IO/FileIO.h"
#include "AI/MusicTheory.h"

/*  MusicXML 4.0 partwise import/export (spec 10.4, 8.5).

    Scope note: this writes correct, openable notation - pitches, durations,
    rests, key/time signature, chord symbols and multiple parts - but it does not
    attempt engraving decisions (beaming groups, voice splitting, cross-staff,
    tuplet nesting). Finale/MuseScore re-engrave on import anyway, so spending
    effort there before Phase 2's notation editor would be wasted.
    ponytail: quantised-to-divisions durations only, no tuplets or voices;
    revisit when the Phase 2 notation editor needs round-trip fidelity.        */

namespace ss::io
{
    namespace
    {
        constexpr int divisionsPerQuarter = 480;

        struct TypeName { double quarters; const char* name; int dots; };

        // Longest first, so the search below picks the largest value that fits.
        const TypeName noteTypes[] = {
            { 6.0,   "whole",    1 }, { 4.0,   "whole",   0 },
            { 3.0,   "half",     1 }, { 2.0,   "half",    0 },
            { 1.5,   "quarter",  1 }, { 1.0,   "quarter", 0 },
            { 0.75,  "eighth",   1 }, { 0.5,   "eighth",  0 },
            { 0.375, "16th",     1 }, { 0.25,  "16th",    0 },
            { 0.125, "32nd",     0 }, { 0.0625, "64th",   0 }
        };

        const TypeName& closestNoteType (double quarters)
        {
            const TypeName* best = &noteTypes[0];
            auto bestError = std::abs (quarters - best->quarters);

            for (const auto& t : noteTypes)
            {
                const auto error = std::abs (quarters - t.quarters);

                if (error < bestError)
                {
                    bestError = error;
                    best = &t;
                }
            }

            return *best;
        }

        struct Pitch { juce::String step; int alter; int octave; };

        Pitch pitchFor (int midiNote, bool preferFlats)
        {
            static const char* sharpSteps[] = { "C", "C", "D", "D", "E", "F", "F", "G", "G", "A", "A", "B" };
            static const int   sharpAlter[] = {  0,   1,   0,   1,   0,   0,   1,   0,   1,   0,   1,   0  };
            static const char* flatSteps[]  = { "C", "D", "D", "E", "E", "F", "G", "G", "A", "A", "B", "B" };
            static const int   flatAlter[]  = {  0,  -1,   0,  -1,   0,   0,  -1,   0,  -1,   0,  -1,   0  };

            const auto pc = ((midiNote % 12) + 12) % 12;
            const auto octave = midiNote / 12 - 1;

            return preferFlats ? Pitch { flatSteps[pc],  flatAlter[pc],  octave }
                               : Pitch { sharpSteps[pc], sharpAlter[pc], octave };
        }

        /** Number of sharps (+) or flats (-) in the key signature. */
        int fifthsFor (const theory::Key& key)
        {
            // Circle of fifths from C, indexed by pitch class.
            static const int majorFifths[] = { 0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5 };
            const auto pc = ((key.tonic % 12) + 12) % 12;
            const auto isMinor = (key.scale == theory::ScaleType::naturalMinor
                                  || key.scale == theory::ScaleType::harmonicMinor);
            // A minor key shares its signature with the major a minor third up.
            return isMinor ? majorFifths[(pc + 3) % 12] : majorFifths[pc];
        }

        juce::XmlElement* addChild (juce::XmlElement& parent, const juce::String& tag,
                                    const juce::String& text = {})
        {
            auto* child = parent.createNewChildElement (tag);

            if (text.isNotEmpty())
                child->addTextElement (text);

            return child;
        }

        /** Flattens every clip on a track onto one absolute-beat note list. */
        std::vector<Note> flattenTrack (const Track& track)
        {
            std::vector<Note> notes;

            for (const auto& clip : track.midiClips)
                for (auto n : clip.notes)
                {
                    n.startBeats += clip.startBeats;
                    notes.push_back (n);
                }

            std::stable_sort (notes.begin(), notes.end(),
                              [] (const Note& a, const Note& b)
                              {
                                  return a.startBeats != b.startBeats ? a.startBeats < b.startBeats
                                                                      : a.pitch < b.pitch;
                              });
            return notes;
        }
    }

    //==============================================================================
    bool exportMusicXml (const juce::File& destination, const Project& project, juce::String& errorOut)
    {
        std::vector<const Track*> parts;

        for (const auto& t : project.getTracks())
            if (t->getType() == TrackType::midi && ! t->midiClips.empty())
                parts.push_back (t.get());

        if (parts.empty())
        {
            errorOut = "There are no MIDI tracks to export as notation";
            return false;
        }

        juce::XmlElement root ("score-partwise");
        root.setAttribute ("version", "4.0");

        {
            auto* work = addChild (root, "work");
            addChild (*work, "work-title", project.name);

            auto* identification = addChild (root, "identification");
            auto* encoding = addChild (*identification, "encoding");
            addChild (*encoding, "software", "KANADE DAW");
            addChild (*encoding, "encoding-date",
                      juce::Time::getCurrentTime().formatted ("%Y-%m-%d"));
        }

        auto* partList = addChild (root, "part-list");

        for (size_t i = 0; i < parts.size(); ++i)
        {
            auto* scorePart = addChild (*partList, "score-part");
            scorePart->setAttribute ("id", "P" + juce::String ((int) i + 1));
            addChild (*scorePart, "part-name", parts[i]->name);
        }

        // Key is estimated once across the whole project so every part agrees.
        std::vector<Note> allNotes;

        for (auto* part : parts)
        {
            auto notes = flattenTrack (*part);
            allNotes.insert (allNotes.end(), notes.begin(), notes.end());
        }

        const auto key = theory::estimateKey (allNotes);
        const auto fifths = fifthsFor (key);
        const auto preferFlats = (fifths < 0);
        const auto timeSig = project.tempo.timeSignatureAt (0.0);
        const auto beatsPerBar = timeSig.numerator * 4.0 / timeSig.denominator;
        const auto totalBars = juce::jmax (1, (int) std::ceil (project.endBeats() / beatsPerBar));

        for (size_t p = 0; p < parts.size(); ++p)
        {
            auto* partElement = addChild (root, "part");
            partElement->setAttribute ("id", "P" + juce::String ((int) p + 1));

            const auto notes = flattenTrack (*parts[p]);
            size_t noteIndex = 0;

            for (int bar = 0; bar < totalBars; ++bar)
            {
                auto* measure = addChild (*partElement, "measure");
                measure->setAttribute ("number", bar + 1);

                if (bar == 0)
                {
                    auto* attributes = addChild (*measure, "attributes");
                    addChild (*attributes, "divisions", juce::String (divisionsPerQuarter));

                    auto* keyElement = addChild (*attributes, "key");
                    addChild (*keyElement, "fifths", juce::String (fifths));
                    addChild (*keyElement, "mode",
                              key.scale == theory::ScaleType::major ? "major" : "minor");

                    auto* timeElement = addChild (*attributes, "time");
                    addChild (*timeElement, "beats", juce::String (timeSig.numerator));
                    addChild (*timeElement, "beat-type", juce::String (timeSig.denominator));

                    auto* clef = addChild (*attributes, "clef");
                    // Pick the clef from the part's median pitch rather than always
                    // treble - a bass part in treble clef is unreadable.
                    const auto useBass = ! notes.empty()
                                       && notes[notes.size() / 2].pitch < 55;
                    addChild (*clef, "sign", useBass ? "F" : "G");
                    addChild (*clef, "line", useBass ? "4" : "2");

                    auto* direction = addChild (*measure, "direction");
                    direction->setAttribute ("placement", "above");
                    auto* directionType = addChild (*direction, "direction-type");
                    auto* metronome = addChild (*directionType, "metronome");
                    addChild (*metronome, "beat-unit", "quarter");
                    addChild (*metronome, "per-minute", juce::String ((int) project.tempo.bpmAt (0.0)));
                }

                const auto barStart = bar * beatsPerBar;
                const auto barEnd   = barStart + beatsPerBar;

                // Chord symbols that begin inside this bar (spec 8.4.7 / 9.8).
                if (p == 0)
                {
                    for (const auto& chord : project.chords)
                    {
                        if (chord.beat < barStart || chord.beat >= barEnd || chord.symbol.isEmpty())
                            continue;

                        auto* harmony = addChild (*measure, "harmony");
                        auto* rootElement = addChild (*harmony, "root");
                        const auto rootPitch = pitchFor (60 + chord.root, preferFlats);
                        addChild (*rootElement, "root-step", rootPitch.step);

                        if (rootPitch.alter != 0)
                            addChild (*rootElement, "root-alter", juce::String (rootPitch.alter));

                        addChild (*harmony, "kind", chord.symbol);
                    }
                }

                double cursor = barStart;
                bool wroteAnything = false;

                while (noteIndex < notes.size() && notes[noteIndex].startBeats < barEnd)
                {
                    const auto& note = notes[noteIndex];

                    if (note.startBeats < barStart)     // spills in from an earlier bar
                    {
                        ++noteIndex;
                        continue;
                    }

                    if (note.startBeats > cursor + 1.0e-6)
                    {
                        auto* rest = addChild (*measure, "note");
                        addChild (*rest, "rest");
                        const auto restQuarters = note.startBeats - cursor;
                        addChild (*rest, "duration",
                                  juce::String (juce::roundToInt (restQuarters * divisionsPerQuarter)));
                        addChild (*rest, "type", closestNoteType (restQuarters).name);
                        cursor = note.startBeats;
                        wroteAnything = true;
                    }

                    // Simultaneous notes become a <chord> stack.
                    const auto stackStart = note.startBeats;
                    bool firstOfStack = true;
                    double stackLength = note.lengthBeats;

                    while (noteIndex < notes.size()
                           && std::abs (notes[noteIndex].startBeats - stackStart) < 1.0e-6)
                    {
                        const auto& stacked = notes[noteIndex];
                        const auto length = juce::jmin (stacked.lengthBeats, barEnd - stackStart);
                        auto* noteElement = addChild (*measure, "note");

                        if (! firstOfStack)
                            addChild (*noteElement, "chord");

                        const auto pitch = pitchFor (stacked.pitch, preferFlats);
                        auto* pitchElement = addChild (*noteElement, "pitch");
                        addChild (*pitchElement, "step", pitch.step);

                        if (pitch.alter != 0)
                            addChild (*pitchElement, "alter", juce::String (pitch.alter));

                        addChild (*pitchElement, "octave", juce::String (pitch.octave));
                        addChild (*noteElement, "duration",
                                  juce::String (juce::jmax (1, juce::roundToInt (length * divisionsPerQuarter))));

                        const auto& type = closestNoteType (length);
                        addChild (*noteElement, "type", type.name);

                        for (int d = 0; d < type.dots; ++d)
                            addChild (*noteElement, "dot");

                        stackLength = juce::jmax (stackLength, length);
                        firstOfStack = false;
                        wroteAnything = true;
                        ++noteIndex;
                    }

                    cursor = juce::jmin (barEnd, stackStart + stackLength);
                }

                if (! wroteAnything)
                {
                    auto* rest = addChild (*measure, "note");
                    addChild (*rest, "rest")->setAttribute ("measure", "yes");
                    addChild (*rest, "duration",
                              juce::String (juce::roundToInt (beatsPerBar * divisionsPerQuarter)));
                }
                else if (cursor < barEnd - 1.0e-6)
                {
                    auto* rest = addChild (*measure, "note");
                    addChild (*rest, "rest");
                    const auto restQuarters = barEnd - cursor;
                    addChild (*rest, "duration",
                              juce::String (juce::roundToInt (restQuarters * divisionsPerQuarter)));
                    addChild (*rest, "type", closestNoteType (restQuarters).name);
                }
            }
        }

        juce::XmlElement::TextFormat format;
        format.dtd = "<!DOCTYPE score-partwise PUBLIC "
                     "\"-//Recordare//DTD MusicXML 4.0 Partwise//EN\" "
                     "\"http://www.musicxml.org/dtds/partwise.dtd\">";

        if (! destination.replaceWithText (root.toString (format)))
        {
            errorOut = "Could not write to " + destination.getFullPathName();
            return false;
        }

        return true;
    }

    //==============================================================================
    bool importMusicXml (const juce::File& source, Project& project, juce::String& errorOut)
    {
        auto xml = juce::XmlDocument::parse (source);

        if (xml == nullptr)
        {
            errorOut = source.getFileName() + " is not readable XML";
            return false;
        }

        if (! xml->hasTagName ("score-partwise"))
        {
            // score-timewise exists in the standard but essentially nothing emits it.
            errorOut = "Only partwise MusicXML is supported";
            return false;
        }

        juce::StringPairArray partNames;

        if (auto* partList = xml->getChildByName ("part-list"))
            for (auto* scorePart : partList->getChildWithTagNameIterator ("score-part"))
                partNames.set (scorePart->getStringAttribute ("id"),
                               scorePart->getChildElementAllSubText ("part-name", "Part"));

        int imported = 0;

        for (auto* part : xml->getChildWithTagNameIterator ("part"))
        {
            const auto id = part->getStringAttribute ("id");
            auto& track = project.addTrack (TrackType::midi,
                                            partNames.getValue (id, "Imported " + id));

            std::vector<Note> notes;
            double divisions = divisionsPerQuarter;
            double cursorBeats = 0.0;
            double lastNoteStart = 0.0;

            for (auto* measure : part->getChildWithTagNameIterator ("measure"))
            {
                if (auto* attributes = measure->getChildByName ("attributes"))
                    if (auto* d = attributes->getChildByName ("divisions"))
                        divisions = juce::jmax (1.0, d->getAllSubText().getDoubleValue());

                for (auto* noteElement : measure->getChildWithTagNameIterator ("note"))
                {
                    const auto durationBeats =
                        noteElement->getChildElementAllSubText ("duration", "0").getDoubleValue() / divisions;

                    const auto isChordNote = noteElement->getChildByName ("chord") != nullptr;
                    const auto isRest      = noteElement->getChildByName ("rest") != nullptr;

                    if (isChordNote)
                        cursorBeats = lastNoteStart;    // stack on the previous note

                    if (! isRest)
                    {
                        if (auto* pitch = noteElement->getChildByName ("pitch"))
                        {
                            static const juce::StringArray steps { "C", "D", "E", "F", "G", "A", "B" };
                            static const int stepSemitones[] = { 0, 2, 4, 5, 7, 9, 11 };

                            const auto step   = pitch->getChildElementAllSubText ("step", "C");
                            const auto alter  = pitch->getChildElementAllSubText ("alter", "0").getIntValue();
                            const auto octave = pitch->getChildElementAllSubText ("octave", "4").getIntValue();
                            const auto stepIndex = juce::jmax (0, steps.indexOf (step));

                            Note n;
                            n.pitch       = (octave + 1) * 12 + stepSemitones[stepIndex] + alter;
                            n.startBeats  = cursorBeats;
                            n.lengthBeats = juce::jmax (1.0 / 32.0, durationBeats);
                            n.velocity    = 96;
                            n.confidence  = 1.0f;

                            if (juce::isPositiveAndBelow (n.pitch, 128))
                                notes.push_back (n);
                        }
                    }

                    if (! isChordNote)
                        lastNoteStart = cursorBeats;

                    cursorBeats = lastNoteStart + durationBeats;
                }
            }

            if (notes.empty())
            {
                project.removeTrack (track.getId());
                continue;
            }

            double end = 0.0;

            for (const auto& n : notes)
                end = juce::jmax (end, n.endBeats());

            MidiClip clip;
            clip.id          = project.nextClipId();
            clip.name        = track.name;
            clip.startBeats  = 0.0;
            clip.lengthBeats = juce::jmax (4.0, end);
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
}
