#include "Core/Project.h"
#include <juce_core/juce_core.h>

namespace ss
{

class ProjectSessionUnitTests final : public juce::UnitTest
{
public:
    ProjectSessionUnitTests() : juce::UnitTest ("Session data model", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("addScene assigns increasing ids and keeps insertion order");
        {
            Project project;
            const auto aId = project.addScene ("Intro").id;
            const auto bId = project.addScene ("Verse").id;

            expect (aId != invalidSceneId);
            expect (bId != aId);
            expectEquals ((int) project.scenes.size(), 2);
            expectEquals (project.scenes[0].name, juce::String ("Intro"));
            expectEquals (project.scenes[1].name, juce::String ("Verse"));
        }

        beginTest ("findScene returns nullptr for an id that was never added");
        {
            Project project;
            expect (project.findScene ((SceneId) 999) == nullptr);
        }

        beginTest ("removeScene erases the scene and every track's slot for it");
        {
            Project project;
            const auto sceneId = project.addScene ("Verse").id;
            auto& track = project.addTrack (TrackType::midi, "Lead");

            SessionClip clip;
            clip.kind = SessionClip::Kind::midi;
            clip.name = "Riff";
            track.setSessionClip (sceneId, clip);

            expect (track.findSessionClip (sceneId) != nullptr);

            project.removeScene (sceneId);

            expect (project.findScene (sceneId) == nullptr);
            expect (track.findSessionClip (sceneId) == nullptr);
        }

        beginTest ("removeScene on an unknown id is a harmless no-op");
        {
            Project project;
            project.addScene ("Verse");
            project.removeScene ((SceneId) 999);
            expectEquals ((int) project.scenes.size(), 1);
        }

        beginTest ("Track::setSessionClip / findSessionClip / clearSessionClip round-trip");
        {
            Track track (1, TrackType::midi, "Bass");

            expect (track.findSessionClip ((SceneId) 1) == nullptr);

            SessionClip clip;
            clip.kind        = SessionClip::Kind::midi;
            clip.name        = "Bass loop";
            clip.lengthBeats = 8.0;
            clip.notes.push_back ({ 36, 0.0, 1.0, 100, 1, 1.0f });

            track.setSessionClip ((SceneId) 1, clip);

            auto* found = track.findSessionClip ((SceneId) 1);
            expect (found != nullptr);
            expectEquals (found->name, juce::String ("Bass loop"));
            expectWithinAbsoluteError (found->lengthBeats, 8.0, 1.0e-9);
            expectEquals ((int) found->notes.size(), 1);

            track.clearSessionClip ((SceneId) 1);
            expect (track.findSessionClip ((SceneId) 1) == nullptr);
        }

        beginTest ("SessionClip defaults to an empty midi-kind clip");
        {
            SessionClip clip;
            expect (clip.kind == SessionClip::Kind::midi);
            expectWithinAbsoluteError (clip.lengthBeats, 4.0, 1.0e-9);
            expectWithinAbsoluteError ((double) clip.playbackRate, 1.0, 1.0e-9);
            expect (! clip.reversed);
            expect (clip.notes.empty());
        }

        beginTest ("scenes and session slots round-trip through toVar/loadFromVar");
        {
            Project project;
            const auto scene1Id = project.addScene ("Intro").id;
            const auto scene2Id = project.addScene ("Drop").id;
            auto& track  = project.addTrack (TrackType::midi, "Lead");

            SessionClip midiClip;
            midiClip.kind        = SessionClip::Kind::midi;
            midiClip.name        = "Lead riff";
            midiClip.lengthBeats = 8.0;
            midiClip.notes.push_back ({ 64, 0.0, 2.0, 110, 1, 1.0f });
            track.setSessionClip (scene1Id, midiClip);

            SessionClip audioClip;
            audioClip.kind          = SessionClip::Kind::audio;
            audioClip.name          = "Vocal chop";
            audioClip.lengthBeats   = 4.0;
            audioClip.sourceFile    = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                          .getChildFile ("chop.wav");
            audioClip.offsetSeconds = 0.5;
            audioClip.gainDb        = -2.5f;
            audioClip.reversed      = true;
            audioClip.playbackRate  = 1.25;
            track.setSessionClip (scene2Id, audioClip);

            const auto trackId  = track.getId();

            Project reloaded;
            expect (reloaded.loadFromVar (project.toVar()));

            expectEquals ((int) reloaded.scenes.size(), 2);
            expectEquals (reloaded.scenes[0].name, juce::String ("Intro"));
            expectEquals (reloaded.scenes[1].name, juce::String ("Drop"));

            auto* reloadedTrack = reloaded.findTrack (trackId);
            expect (reloadedTrack != nullptr);

            if (reloadedTrack != nullptr)
            {
                auto* m = reloadedTrack->findSessionClip (scene1Id);
                expect (m != nullptr);
                if (m != nullptr)
                {
                    expect (m->kind == SessionClip::Kind::midi);
                    expectEquals (m->name, juce::String ("Lead riff"));
                    expectWithinAbsoluteError (m->lengthBeats, 8.0, 1.0e-9);
                    expectEquals ((int) m->notes.size(), 1);
                    expectEquals (m->notes[0].pitch, 64);
                }

                auto* a = reloadedTrack->findSessionClip (scene2Id);
                expect (a != nullptr);
                if (a != nullptr)
                {
                    expect (a->kind == SessionClip::Kind::audio);
                    expectEquals (a->name, juce::String ("Vocal chop"));
                    expectWithinAbsoluteError (a->offsetSeconds, 0.5, 1.0e-9);
                    expectWithinAbsoluteError ((double) a->gainDb, -2.5, 1.0e-4);
                    expect (a->reversed);
                    expectWithinAbsoluteError (a->playbackRate, 1.25, 1.0e-9);
                    expectEquals (a->sourceFile.getFileName(), juce::String ("chop.wav"));
                }
            }

            // A new scene added after reload must not collide with the ids that came from disk.
            auto& scene3 = reloaded.addScene ("Outro");
            expect (scene3.id != scene1Id && scene3.id != scene2Id);
        }
    }
};

static ProjectSessionUnitTests projectSessionUnitTests;

}
