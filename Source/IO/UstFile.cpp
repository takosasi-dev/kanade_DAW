#include "IO/UstFile.h"
#include "IO/ShiftJis.h"

namespace ss
{
namespace
{
    std::vector<double> parseCsvDoubles (const juce::String& csv)
    {
        std::vector<double> result;

        for (const auto& token : juce::StringArray::fromTokens (csv, ",", ""))
            result.push_back (token.trim().getDoubleValue());

        return result;
    }

    juce::String joinCsvDoubles (const std::vector<double>& values)
    {
        juce::StringArray tokens;

        for (auto v : values)
            tokens.add (juce::String (v, 3));

        return tokens.joinIntoString (",");
    }

    UtauNote parseNoteSection (const juce::StringArray& lines)
    {
        UtauNote note;
        double lengthTicks = (double) ustTicksPerBeat;

        for (const auto& line : lines)
        {
            const auto eq = line.indexOfChar ('=');
            if (eq < 0) continue;

            const auto key   = line.substring (0, eq).trim();
            const auto value = line.substring (eq + 1);

            if      (key == "Length")       lengthTicks = value.getDoubleValue();
            else if (key == "Lyric")        { note.lyric = value; note.isRest = value.trim().equalsIgnoreCase ("R"); }
            else if (key == "NoteNum")      note.pitch = value.getIntValue();
            else if (key == "Velocity")     note.velocity = value.getIntValue();
            else if (key == "Intensity")    note.intensity = value.getIntValue();
            else if (key == "Modulation")   note.modulation = value.getIntValue();
            else if (key == "PreUtterance") note.preUtteranceMs = value.trim().isEmpty() ? -1.0 : value.getDoubleValue();
            else if (key == "VoiceOverlap") note.voiceOverlapMs = value.trim().isEmpty() ? -1.0 : value.getDoubleValue();
            else if (key == "Flags")        note.flags = value;
            else if (key == "Envelope")     note.envelope = value;
            else if (key == "PBS")
            {
                const auto parts = juce::StringArray::fromTokens (value, ";", "");
                note.pitchBend.startMs        = parts.size() > 0 ? parts[0].getDoubleValue() : 0.0;
                note.pitchBend.startSemitones = parts.size() > 1 ? parts[1].getDoubleValue() : 0.0;
            }
            else if (key == "PBW") note.pitchBend.widthsMs = parseCsvDoubles (value);
            else if (key == "PBY") note.pitchBend.heightsSemitones = parseCsvDoubles (value);
            else if (key == "PBM")
            {
                note.pitchBend.curveTypes.clear();
                for (const auto& t : juce::StringArray::fromTokens (value, ",", ""))
                    note.pitchBend.curveTypes.push_back (t);
            }
            else
                note.extra.set (juce::Identifier (key.removeCharacters (" \t")), value);
        }

        lengthTicks = juce::jlimit (0.0, 1.0e7, lengthTicks);  // reject negative/absurd note lengths from untrusted .ust input
        note.lengthBeats = lengthTicks / (double) ustTicksPerBeat;
        return note;
    }
}

std::vector<UtauNote> parseUstFile (const void* data, size_t numBytes)
{
    const auto text = decodeUstText (data, numBytes);
    std::vector<UtauNote> notes;

    juce::StringArray currentSection;
    bool inNoteSection = false;
    double cursorBeats = 0.0;

    auto flushSection = [&]
    {
        if (! inNoteSection || currentSection.isEmpty())
            return;

        auto note = parseNoteSection (currentSection);
        note.startBeats = cursorBeats;
        cursorBeats += note.lengthBeats;
        notes.push_back (note);
    };

    for (const auto& rawLine : juce::StringArray::fromLines (text))
    {
        const auto line = rawLine.trim();

        if (line.startsWithChar ('[') && line.endsWithChar (']'))
        {
            flushSection();
            currentSection.clear();

            const auto tag = line.substring (1, line.length() - 1);
            inNoteSection = tag.startsWithChar ('#')
                           && tag != "#VERSION" && tag != "#SETTING" && tag != "#TRACKEND"
                           && tag != "#PREV" && tag != "#NEXT" && tag != "#DELETE";
            continue;
        }

        if (inNoteSection && line.isNotEmpty())
            currentSection.add (line);
    }

    flushSection();
    return notes;
}

juce::String writeUstFile (const std::vector<UtauNote>& notes)
{
    juce::String out;
    out << "[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\nTempo=120.00\r\nTracks=1\r\nMode2=True\r\n";

    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& n = notes[i];
        out << "[#" << juce::String (i).paddedLeft ('0', 4) << "]\r\n";
        out << "Length=" << juce::String (juce::roundToInt (n.lengthBeats * (double) ustTicksPerBeat)) << "\r\n";
        out << "Lyric=" << (n.isRest ? juce::String ("R") : n.lyric) << "\r\n";
        out << "NoteNum=" << n.pitch << "\r\n";
        out << "Velocity=" << n.velocity << "\r\n";
        out << "Intensity=" << n.intensity << "\r\n";
        out << "Modulation=" << n.modulation << "\r\n";
        out << "PreUtterance=" << (n.preUtteranceMs >= 0.0 ? juce::String (n.preUtteranceMs, 3) : juce::String()) << "\r\n";
        out << "VoiceOverlap=" << (n.voiceOverlapMs >= 0.0 ? juce::String (n.voiceOverlapMs, 3) : juce::String()) << "\r\n";
        out << "Flags=" << n.flags << "\r\n";
        out << "PBS=" << juce::String (n.pitchBend.startMs, 3) << ";" << juce::String (n.pitchBend.startSemitones, 3) << "\r\n";
        out << "PBW=" << joinCsvDoubles (n.pitchBend.widthsMs) << "\r\n";
        out << "PBY=" << joinCsvDoubles (n.pitchBend.heightsSemitones) << "\r\n";

        {
            juce::StringArray pbm;
            for (auto& t : n.pitchBend.curveTypes) pbm.add (t);
            out << "PBM=" << pbm.joinIntoString (",") << "\r\n";
        }

        if (n.envelope.isNotEmpty())
            out << "Envelope=" << n.envelope << "\r\n";

        for (auto& kv : n.extra)
            out << kv.name.toString() << "=" << kv.value.toString() << "\r\n";
    }

    out << "[#TRACKEND]\r\n";
    return out;
}
}
