#include "UI/WhatsNewDialog.h"

namespace ss
{

class WhatsNewDialogUnitTests final : public juce::UnitTest
{
public:
    WhatsNewDialogUnitTests() : juce::UnitTest ("WhatsNewDialog", "ScoreSmith") {}

    void runTest() override
    {
        beginTest ("shouldShow is false on a fresh install (empty last-seen version)");
        {
            expect (! WhatsNew::shouldShow ({}, "0.1.0"),
                    "a never-run-before install must not see What's New before ever using the app");
        }

        beginTest ("shouldShow is false when the last-seen version matches the current version");
        {
            expect (! WhatsNew::shouldShow ("0.1.0", "0.1.0"));
        }

        beginTest ("shouldShow is true when the last-seen version differs from the current version");
        {
            expect (WhatsNew::shouldShow ("0.1.0", "0.2.0"));
            expect (WhatsNew::shouldShow ("0.2.0", "0.1.0"), "a downgrade should still surface that version's notes");
        }

        beginTest ("releaseNotes has a well-formed entry for the current 0.2.0 release");
        {
            const auto notes = WhatsNew::releaseNotes();
            expect (! notes.empty());

            bool found = false;
            for (const auto& entry : notes)
            {
                if (entry.first != "0.2.0")
                    continue;

                found = true;
                expect (! entry.second.isEmpty(), "0.2.0 must ship with at least one bullet point");
            }
            expect (found, "the current shipped version must have a release-notes entry");
        }
    }
};

static WhatsNewDialogUnitTests whatsNewDialogUnitTests;

}
