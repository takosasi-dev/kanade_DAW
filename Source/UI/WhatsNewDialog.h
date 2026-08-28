#pragma once
#include "UI/UiSupport.h"

namespace ss
{
    namespace WhatsNew
    {
        /** Release notes for each shipped version, newest first. Add a new entry
            here by hand when a version ships - there is no dynamic fetching. */
        std::vector<std::pair<juce::String, juce::StringArray>> releaseNotes();

        /** True if the What's New dialog should be shown for `currentVersion`,
            given the last version the user has seen. `lastSeenVersion` empty means
            "never run before" - a fresh install is skipped rather than shown a
            "what's new" before ever using version 1 of anything. Factored out of
            Main.cpp's initialise() so the decision is unit-testable without
            constructing a GUI. */
        bool shouldShow (const juce::String& lastSeenVersion, const juce::String& currentVersion);
    }

    /** Shows one version's release notes as a short bullet list, the first time
        the app is opened after an update. Not a changelog browser - only ever
        shows the current version's entry, once. */
    class WhatsNewDialog final : public juce::Component
    {
    public:
        explicit WhatsNewDialog (const juce::String& version);

        void resized() override;
        void paint (juce::Graphics&) override;

        /** Opens the dialog for `version`'s entry in its own non-modal window.
            Does nothing if there is no release-notes entry for that version. */
        static void launchForVersion (const juce::String& version);

    private:
        juce::Label titleLabel;
        juce::TextEditor notes;
        juce::TextButton okButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WhatsNewDialog)
    };
}
