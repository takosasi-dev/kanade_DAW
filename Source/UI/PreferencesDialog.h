#pragma once
#include "UI/UiSupport.h"

namespace ss
{
    /** Preferences (spec 9.6): eight categories as tabs down the left, settings
        on the right, exactly like every other DAW's settings dialog.

        Everything writes straight through to ss::Settings as it is changed -
        there is no OK/Cancel model, because a settings dialog that can silently
        lose a change is worse than one that applies immediately. */
    class PreferencesDialog final : public juce::Component
    {
    public:
        PreferencesDialog (AppContext&, juce::ApplicationCommandManager&, DarkLookAndFeel&,
                           std::function<void()> onSettingsChanged);
        ~PreferencesDialog() override;

        /** Opens the dialog in its own non-modal window. */
        static void launch (AppContext&, juce::ApplicationCommandManager&, DarkLookAndFeel&,
                            std::function<void()> onSettingsChanged);

        void resized() override;
        void paint (juce::Graphics&) override;

    private:
        AppContext& ctx;
        juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtLeft };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreferencesDialog)
    };
}
