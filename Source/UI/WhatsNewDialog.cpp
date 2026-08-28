#include "UI/WhatsNewDialog.h"
#include "Core/Localisation.h"

namespace ss
{
    namespace WhatsNew
    {
        std::vector<std::pair<juce::String, juce::StringArray>> releaseNotes()
        {
            return {
                { "0.2.0", juce::StringArray {
                    TRANS ("Fixed a crash that could occur when opening a VST3 plugin's editor window."),
                    TRANS ("You can now save the current dock layout as the one KANADE DAW starts with."),
                    TRANS ("This What's New dialog.")
                } },
                { "0.1.0", juce::StringArray {
                    TRANS ("Initial public release."),
                    TRANS ("Audio and MIDI recording, editing, and mixing with VST3/AU plugin hosting."),
                    TRANS ("AI-assisted transcription (audio to MIDI) and arrangement generation."),
                    TRANS ("Piano roll editing, notation view, and MIDI/audio/MusicXML export."),
                    TRANS ("UTAU voicebank integration for vocal synthesis.")
                } }
            };
        }

        bool shouldShow (const juce::String& lastSeenVersion, const juce::String& currentVersion)
        {
            return lastSeenVersion.isNotEmpty() && lastSeenVersion != currentVersion;
        }
    }

    //==============================================================================
    WhatsNewDialog::WhatsNewDialog (const juce::String& version)
    {
        titleLabel.setText (TRANS ("What's new") + " - " + version, juce::dontSendNotification);
        titleLabel.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        titleLabel.setColour (juce::Label::textColourId, palette().text);
        addAndMakeVisible (titleLabel);

        juce::String text;
        for (const auto& entry : WhatsNew::releaseNotes())
            if (entry.first == version)
                for (const auto& bullet : entry.second)
                    text += juce::String ("- ") + bullet + "\n";

        notes.setMultiLine (true, true);
        notes.setReadOnly (true);
        notes.setCaretVisible (false);
        notes.setScrollbarsShown (true);
        notes.setColour (juce::TextEditor::backgroundColourId, palette().panelAltBg);
        notes.setColour (juce::TextEditor::textColourId, palette().text);
        notes.setColour (juce::TextEditor::outlineColourId, palette().outline);
        notes.setText (text.trimEnd(), false);
        addAndMakeVisible (notes);

        okButton.setButtonText (TRANS ("OK"));
        okButton.onClick = [this]
        {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState (0);
        };
        addAndMakeVisible (okButton);

        setSize (480, 340);
    }

    void WhatsNewDialog::paint (juce::Graphics& g)
    {
        g.fillAll (palette().windowBg);
    }

    void WhatsNewDialog::resized()
    {
        auto area = getLocalBounds().reduced (14, 12);
        titleLabel.setBounds (area.removeFromTop (28));
        area.removeFromTop (8);

        auto buttonRow = area.removeFromBottom (30);
        okButton.setBounds (buttonRow.removeFromRight (90));
        area.removeFromBottom (8);

        notes.setBounds (area);
    }

    void WhatsNewDialog::launchForVersion (const juce::String& version)
    {
        for (const auto& entry : WhatsNew::releaseNotes())
        {
            if (entry.first != version)
                continue;

            juce::DialogWindow::LaunchOptions options;
            options.content.setOwned (new WhatsNewDialog (version));
            options.dialogTitle            = TRANS ("What's new");
            options.dialogBackgroundColour = palette().windowBg;
            options.escapeKeyTriggersCloseButton = true;
            options.useNativeTitleBar      = true;
            options.resizable              = true;
            options.launchAsync();
            return;
        }
    }
}
