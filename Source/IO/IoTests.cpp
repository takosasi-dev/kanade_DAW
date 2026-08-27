#include "IO/ShiftJis.h"
#include "IO/UstFile.h"
#include "IO/OtoIni.h"
#include <juce_events/juce_events.h>

namespace ss
{

class IoUnitTests final : public juce::UnitTest
{
public:
    IoUnitTests() : juce::UnitTest ("ScoreSmith IO", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("decodeUstText passes through valid UTF-8 unchanged");
        {
            const juce::String original = juce::CharPointer_UTF8 ("\xe3\x81\x82\xe3\x81\x84"); // "あい"
            const auto utf8Bytes = original.toUTF8();
            const auto decoded = decodeUstText (utf8Bytes.getAddress(), (size_t) original.getNumBytesAsUTF8());
            expectEquals (decoded, original);
        }

       #if JUCE_WINDOWS
        beginTest ("decodeUstText falls back to Shift-JIS on this machine");
        {
            // "あい" (hiragana a, i) in Shift-JIS is the byte pair sequence 82 A0 82 A2.
            const juce::uint8 sjisBytes[] = { 0x82, 0xA0, 0x82, 0xA2 };
            const auto decoded = decodeUstText (sjisBytes, sizeof (sjisBytes));
            expectEquals (decoded, juce::String (juce::CharPointer_UTF8 ("\xe3\x81\x82\xe3\x81\x84")));
        }
       #endif

        beginTest ("parseUstFile reads two notes with correct positions and fields");
        {
            const juce::String ust =
                "[#VERSION]\r\n"
                "UST Version1.2\r\n"
                "[#SETTING]\r\n"
                "Tempo=120.00\r\n"
                "[#0000]\r\n"
                "Length=480\r\n"
                "Lyric=a\r\n"
                "NoteNum=60\r\n"
                "Velocity=100\r\n"
                "Intensity=100\r\n"
                "Modulation=0\r\n"
                "PreUtterance=\r\n"
                "Flags=\r\n"
                "PBS=0;0\r\n"
                "PBW=0\r\n"
                "PBY=\r\n"
                "[#0001]\r\n"
                "Length=240\r\n"
                "Lyric=i\r\n"
                "NoteNum=62\r\n"
                "Velocity=80\r\n"
                "Intensity=90\r\n"
                "Modulation=10\r\n"
                "PreUtterance=5.5\r\n"
                "Flags=g-5\r\n"
                "CustomTag=hello\r\n"
                "[#TRACKEND]\r\n";

            const auto notes = parseUstFile (ust.toRawUTF8(), (size_t) ust.getNumBytesAsUTF8());

            expectEquals ((int) notes.size(), 2);

            expectWithinAbsoluteError (notes[0].startBeats, 0.0, 1.0e-9);
            expectWithinAbsoluteError (notes[0].lengthBeats, 1.0, 1.0e-9); // 480 ticks == 1 beat
            expectEquals (notes[0].lyric, juce::String ("a"));
            expectEquals (notes[0].pitch, 60);
            expect (notes[0].preUtteranceMs < 0.0, "blank PreUtterance should mean \"use oto.ini's value\"");

            expectWithinAbsoluteError (notes[1].startBeats, 1.0, 1.0e-9); // right after note 0
            expectWithinAbsoluteError (notes[1].lengthBeats, 0.5, 1.0e-9); // 240 ticks == 0.5 beat
            expectEquals (notes[1].lyric, juce::String ("i"));
            expectEquals (notes[1].pitch, 62);
            expectEquals (notes[1].velocity, 80);
            expectWithinAbsoluteError (notes[1].preUtteranceMs, 5.5, 1.0e-9);
            expectEquals (notes[1].flags, juce::String ("g-5"));
            expectEquals (notes[1].extra["CustomTag"].toString(), juce::String ("hello"),
                         "an unrecognised key must be preserved, not dropped");
        }

        beginTest ("parseUstFile treats Lyric \"R\" as a rest");
        {
            const juce::String ust =
                "[#0000]\r\nLength=480\r\nLyric=R\r\nNoteNum=60\r\n[#TRACKEND]\r\n";
            const auto notes = parseUstFile (ust.toRawUTF8(), (size_t) ust.getNumBytesAsUTF8());
            expectEquals ((int) notes.size(), 1);
            expect (notes[0].isRest);
        }

        beginTest ("writeUstFile then parseUstFile round-trips the fields that matter");
        {
            UtauNote note;
            note.lyric = "ka";
            note.pitch = 67;
            note.lengthBeats = 2.0;
            note.velocity = 77;
            note.intensity = 88;
            note.modulation = 5;
            note.flags = "B10";
            note.extra.set ("CustomTag", "world");
            note.pitchBend.startMs = 12.5;
            note.pitchBend.startSemitones = -3.0;
            note.pitchBend.widthsMs = { 50.0, 100.0, 25.0 };
            note.pitchBend.heightsSemitones = { 1.5, -2.0, 0.5 };
            note.pitchBend.curveTypes = { "s", "r", "" };

            const auto text = writeUstFile ({ note });
            const auto roundTripped = parseUstFile (text.toRawUTF8(), (size_t) text.getNumBytesAsUTF8());

            expectEquals ((int) roundTripped.size(), 1);
            expectEquals (roundTripped[0].lyric, juce::String ("ka"));
            expectEquals (roundTripped[0].pitch, 67);
            expectWithinAbsoluteError (roundTripped[0].lengthBeats, 2.0, 1.0e-9);
            expectEquals (roundTripped[0].velocity, 77);
            expectEquals (roundTripped[0].flags, juce::String ("B10"));
            expectEquals (roundTripped[0].extra["CustomTag"].toString(), juce::String ("world"));
            expectWithinAbsoluteError (roundTripped[0].pitchBend.startMs, 12.5, 1.0e-9);
            expectWithinAbsoluteError (roundTripped[0].pitchBend.startSemitones, -3.0, 1.0e-9);
            expectEquals ((int) roundTripped[0].pitchBend.widthsMs.size(), 3);
            expectWithinAbsoluteError (roundTripped[0].pitchBend.widthsMs[1], 100.0, 1.0e-9);
            expectEquals ((int) roundTripped[0].pitchBend.heightsSemitones.size(), 3);
            expectWithinAbsoluteError (roundTripped[0].pitchBend.heightsSemitones[1], -2.0, 1.0e-9);
            expectEquals ((int) roundTripped[0].pitchBend.curveTypes.size(), 3);
            expectEquals (roundTripped[0].pitchBend.curveTypes[0], juce::String ("s"));
        }

        beginTest ("parseOtoIni reads alias, timings and resolves the sample path");
        {
            const juce::String oto =
                "a.wav=a,100.0,50.0,-200.0,30.0,10.0\r\n"
                "i.wav=,120.5,60.0,-150.0,25.0,5.0\r\n"; // no explicit alias -> falls back to the filename

            const juce::File folder ("C:/fake/voicebank");
            const auto entries = parseOtoIni (oto, folder);

            expectEquals ((int) entries.size(), 2);

            const auto aIt = entries.find ("a");
            expect (aIt != entries.end());
            expectEquals (aIt->second.sampleFile.getFullPathName(), folder.getChildFile ("a.wav").getFullPathName());
            expectWithinAbsoluteError (aIt->second.offset, 100.0, 1.0e-9);
            expectWithinAbsoluteError (aIt->second.consonant, 50.0, 1.0e-9);
            expectWithinAbsoluteError (aIt->second.cutoff, -200.0, 1.0e-9);
            expectWithinAbsoluteError (aIt->second.preUtterance, 30.0, 1.0e-9);
            expectWithinAbsoluteError (aIt->second.overlap, 10.0, 1.0e-9);

            const auto iIt = entries.find ("i");
            expect (iIt != entries.end(), "a blank alias field should fall back to the filename without extension");
        }

        beginTest ("parseOtoIni skips malformed lines instead of throwing");
        {
            const auto entries = parseOtoIni ("not a valid line\r\na.wav=a,1,2,3,4,5\r\n", juce::File ("C:/fake"));
            expectEquals ((int) entries.size(), 1);
        }
    }
};

static IoUnitTests ioUnitTests;

}
