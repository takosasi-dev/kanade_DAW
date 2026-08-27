#include "UI/GenerateView.h"
#include "Core/Project.h"

namespace ss
{

class GenerateViewUnitTests final : public juce::UnitTest
{
public:
    GenerateViewUnitTests() : juce::UnitTest ("GenerateView", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("sendCandidatesToSession creates one scene per candidate, one track per part, shared across candidates");
        {
            Project project;

            Generator::Candidate candidateA;
            candidateA.name = "Candidate A";
            MidiClip bassA;
            bassA.name = "Bass A";
            bassA.lengthBeats = 8.0;
            bassA.notes.push_back ({ 36, 0.0, 1.0, 100, 1, 1.0f });
            candidateA.parts.push_back ({ Generator::Part::bass, bassA });

            Generator::Candidate candidateB;
            candidateB.name = "Candidate B";
            MidiClip bassB;
            bassB.name = "Bass B";
            bassB.lengthBeats = 8.0;
            bassB.notes.push_back ({ 38, 0.0, 1.0, 100, 1, 1.0f });
            candidateB.parts.push_back ({ Generator::Part::bass, bassB });

            sendCandidatesToSession (project, { candidateA, candidateB });

            expectEquals ((int) project.scenes.size(), 2);
            expectEquals (project.scenes[0].name, juce::String ("Candidate A"));
            expectEquals (project.scenes[1].name, juce::String ("Candidate B"));

            // Exactly one bass track, shared by both candidates - not one each.
            int bassTracks = 0;
            for (const auto& t : project.getTracks())
                if (t->name.startsWith ("Bass"))
                    ++bassTracks;
            expectEquals (bassTracks, 1);

            auto& track = project.getTrack (0);
            auto* clipA = track.findSessionClip (project.scenes[0].id);
            auto* clipB = track.findSessionClip (project.scenes[1].id);

            expect (clipA != nullptr);
            expect (clipB != nullptr);

            if (clipA != nullptr) expectEquals (clipA->name, juce::String ("Bass A"));
            if (clipB != nullptr) expectEquals (clipB->name, juce::String ("Bass B"));
        }

        beginTest ("sendCandidatesToSession on an empty candidate list is a no-op");
        {
            Project project;
            sendCandidatesToSession (project, {});
            expectEquals ((int) project.scenes.size(), 0);
        }
    }
};

static GenerateViewUnitTests generateViewUnitTests;

}
