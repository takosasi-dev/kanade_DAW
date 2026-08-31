#pragma once
#include "UI/UiSupport.h"

namespace ss
{
    /** Static reference for third-party developers: the format extension
        manifest schema and CLI contract (spec:
        docs/superpowers/specs/2026-08-28-format-extension-api-design.md).
        Opened from Help > "How to build a format extension...". Unlike
        WhatsNewDialog this never changes per version - one fixed body. */
    class ExtensionHelpDialog final : public juce::Component
    {
    public:
        ExtensionHelpDialog();

        void resized() override;
        void paint (juce::Graphics&) override;

        static void launch();

    private:
        juce::Label titleLabel;
        juce::TextEditor body;
        juce::TextButton okButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExtensionHelpDialog)
    };
}
