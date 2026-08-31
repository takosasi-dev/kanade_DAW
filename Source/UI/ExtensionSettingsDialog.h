#pragma once
#include "Extensions/FormatExtension.h"
#include "UI/UiSupport.h"
#include <functional>
#include <map>
#include <optional>

namespace ss
{
    /** Auto-generated from a FormatExtension's `settings` array (see
        docs/superpowers/specs/2026-08-31-extension-settings-ui-design.md) -
        one row per setting (slider/checkbox/dropdown), shown immediately
        before invoking the extension. Same juce::DialogWindow::LaunchOptions
        shape as ExtensionHelpDialog, but returns its result via callback
        instead of showing static text. */
    class ExtensionSettingsDialog final : public juce::Component
    {
    public:
        /** nullopt means the user cancelled (Cancel button, Escape, or the
            window's own close button) - every other case is a map keyed by
            each setting's own envVar, holding the value convention
            FormatExtensionRunner.h documents (a slider's number as a
            string, "1"/"0" for a checkbox, the selected option string for
            a dropdown). */
        using Result = std::optional<std::map<juce::String, juce::String>>;

        ExtensionSettingsDialog (const std::vector<ExtensionSetting>& settings,
                                 std::function<void (Result)> onComplete);
        ~ExtensionSettingsDialog() override;

        void resized() override;
        void paint (juce::Graphics&) override;

        /** Opens the dialog in its own modal window titled `extensionName`. */
        static void launch (const juce::String& extensionName,
                            const std::vector<ExtensionSetting>& settings,
                            std::function<void (Result)> onComplete);

    private:
        void finish (Result);

        struct Row
        {
            juce::String envVar;
            ExtensionSettingType type;
            std::unique_ptr<juce::Label> label;
            std::unique_ptr<juce::Component> control;   // Slider, ToggleButton, or ComboBox
        };

        std::function<void (Result)> onComplete;
        bool completed = false;

        juce::OwnedArray<Row> rows;
        juce::TextButton okButton, cancelButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExtensionSettingsDialog)
    };
}
