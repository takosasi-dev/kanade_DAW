#include "Core/UtauTypes.h"
#include "Core/Types.h"
#include "Core/Project.h"
#include <juce_events/juce_events.h>

namespace ss
{

class UtauTypesUnitTests final : public juce::UnitTest
{
public:
    UtauTypesUnitTests() : juce::UnitTest ("ScoreSmith UTAU types", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("TrackType::utau round-trips through its string form");
        {
            expectEquals (toString (TrackType::utau), juce::String ("utau"));
            expect (trackTypeFromString ("utau") == TrackType::utau);
            expect (trackTypeFromString ("audio") == TrackType::audio);
            expect (trackTypeFromString ("midi") == TrackType::midi);
            expect (trackTypeFromString ("nonsense") == TrackType::midi,
                    "an unrecognised string should fall back to midi, matching the existing convention");
        }

        beginTest ("UtauClip::endBeats adds startBeats and lengthBeats");
        {
            UtauClip clip;
            clip.startBeats = 4.0;
            clip.lengthBeats = 8.0;
            expectWithinAbsoluteError (clip.endBeats(), 12.0, 1.0e-9);
        }

        beginTest ("UtauClip::currentContentHash changes when a note changes, stable otherwise");
        {
            UtauClip clip;
            clip.voicebankId = "TestBank";
            UtauNote note;
            note.lyric = "a";
            note.pitch = 60;
            clip.notes.push_back (note);

            const auto hash1 = clip.currentContentHash();
            const auto hash2 = clip.currentContentHash();
            expectEquals (hash1, hash2, "the same content must hash the same way every time");

            clip.notes[0].lyric = "i";
            expect (clip.currentContentHash() != hash1, "changing a note's lyric must change the hash");
        }

        beginTest ("Track::endBeats considers utauClips too");
        {
            Track track (1, TrackType::utau, "Vocal");
            UtauClip clip;
            clip.startBeats = 10.0;
            clip.lengthBeats = 5.0;
            track.utauClips.push_back (clip);

            expectWithinAbsoluteError (track.endBeats(), 15.0, 1.0e-9);
        }

        beginTest ("Track::findUtauClip finds by id, returns nullptr otherwise");
        {
            Track track (1, TrackType::utau, "Vocal");
            UtauClip clip;
            clip.id = 42;
            track.utauClips.push_back (clip);

            expect (track.findUtauClip (42) != nullptr);
            expect (track.findUtauClip (99) == nullptr);
        }

        beginTest ("Project save/load round-trips a UtauClip");
        {
            Project project;
            auto& track = project.addTrack (TrackType::utau, "Vocal");

            UtauClip clip;
            clip.id = 5;
            clip.name = "Verse 1";
            clip.startBeats = 4.0;
            clip.lengthBeats = 8.0;
            clip.voicebankId = "TestBank";
            clip.renderedFile = juce::File ("C:/fake/rendered.wav");
            clip.notesHashAtRender = 12345;

            UtauNote note;
            note.lyric = "ka";
            note.pitch = 64;
            note.startBeats = 0.0;
            note.lengthBeats = 1.0;
            note.velocity = 90;
            note.flags = "B10";
            note.isRest = false;
            note.intensity = 77;
            note.modulation = 12;
            note.preUtteranceMs = 8.5;
            note.voiceOverlapMs = 3.2;
            note.envelope = "0,5,35,0,100,100,0";
            note.pitchBend.startMs = 1.0;
            note.pitchBend.startSemitones = 2.0;
            note.pitchBend.widthsMs = { 10.0, 20.0 };
            note.pitchBend.heightsSemitones = { 0.5, -1.5 };
            note.pitchBend.curveTypes = { "s", "" };
            note.extra.set ("CustomTag", "roundtrip-value");
            clip.notes.push_back (note);

            track.utauClips.push_back (clip);

            const auto saved = project.toVar();

            Project reloaded;
            reloaded.loadFromVar (saved);

            expectEquals (reloaded.getNumTracks(), 1);
            expect (reloaded.getTrack (0).getType() == TrackType::utau);
            expectEquals ((int) reloaded.getTrack (0).utauClips.size(), 1);

            const auto& rc = reloaded.getTrack (0).utauClips[0];
            expectEquals (rc.name, juce::String ("Verse 1"));
            expectWithinAbsoluteError (rc.startBeats, 4.0, 1.0e-9);
            expectWithinAbsoluteError (rc.lengthBeats, 8.0, 1.0e-9);
            expectEquals (rc.voicebankId, juce::String ("TestBank"));
            expectEquals (rc.notesHashAtRender, (juce::int64) 12345);
            expectEquals (rc.renderedFile.getFullPathName(), juce::String ("C:\\fake\\rendered.wav"));
            expectEquals ((int) rc.notes.size(), 1);

            const auto& rn = rc.notes[0];
            expectEquals (rn.lyric, juce::String ("ka"));
            expectEquals (rn.pitch, 64);
            expectEquals (rn.velocity, 90);
            expectEquals (rn.flags, juce::String ("B10"));
            expect (! rn.isRest);
            expectEquals (rn.intensity, 77);
            expectEquals (rn.modulation, 12);
            expectWithinAbsoluteError (rn.preUtteranceMs, 8.5, 1.0e-9);
            expectWithinAbsoluteError (rn.voiceOverlapMs, 3.2, 1.0e-9);
            expectEquals (rn.envelope, juce::String ("0,5,35,0,100,100,0"));
            expectEquals (rn.extra["CustomTag"].toString(), juce::String ("roundtrip-value"));
            expectWithinAbsoluteError (rn.pitchBend.startMs, 1.0, 1.0e-9);
            expectWithinAbsoluteError (rn.pitchBend.startSemitones, 2.0, 1.0e-9);
            expectEquals ((int) rn.pitchBend.widthsMs.size(), 2);
            expectEquals ((int) rn.pitchBend.heightsSemitones.size(), 2);
            expectWithinAbsoluteError (rn.pitchBend.heightsSemitones[1], -1.5, 1.0e-9);
            expectEquals ((int) rn.pitchBend.curveTypes.size(), 2);
            expectEquals (rn.pitchBend.curveTypes[0], juce::String ("s"));
        }
    }
};

static UtauTypesUnitTests utauTypesUnitTests;

}
