#include "UI/WhatsNewDialog.h"
#include "Core/Localisation.h"

namespace ss
{
    namespace WhatsNew
    {
        std::vector<std::pair<juce::String, juce::StringArray>> releaseNotes()
        {
            return {
                { "0.5.2", juce::StringArray {
                    TRANS ("Adding, removing or reordering a plugin no longer reloads every other plugin on the same track - just the slot that actually changed, so playback glitches less.")
                } },
                { "0.5.1", juce::StringArray {
                    TRANS ("Fixed effects that come after an instrument in a track's chain sometimes failing to open, or only playing the left channel."),
                    TRANS ("Fixed a crash when replacing or reordering a plugin whose editor was still open."),
                    TRANS ("Reparented plugin editors are nudged harder to fix a few more cases of a solid black window (e.g. some ZamAudio plugins).")
                } },
                { "0.5.0", juce::StringArray {
                    TRANS ("You can now reorder plugins and built-in effects within a track (Move up/down in the slot's right-click menu)."),
                    TRANS ("Pin your favourite plugins to the top of the Add plugin menu."),
                    TRANS ("Generate/Modular/Notation/Piano Roll/Session/Transcribe no longer clutter the default startup layout - open them from the View menu when you need them."),
                    TRANS ("Fixed some out-of-process plugin editors (e.g. ChowDSP plugins) rendering solid black.")
                } },
                { "0.4.0", juce::StringArray {
                    TRANS ("Third-party format extensions: add new import/export file formats with a manifest.json and an executable of your choice. See Help > \"How to build an extension\" or the README."),
                    TRANS ("Mixer channel strips can now show a numeric pan value."),
                    TRANS ("New performance settings: adjust the UI redraw rate and the undo history limit."),
                    TRANS ("Quickly switch the audio output device from the transport bar.")
                } },
                { "0.3.0", juce::StringArray {
                    TRANS ("Export and import DAWproject files, so you can move projects to and from Studio One, Bitwig, Cubase and other DAWs."),
                    TRANS ("Tracks, buses, clips, VST3 plugin state, built-in effects, automation, and Session view scenes all carry over.")
                } },
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
