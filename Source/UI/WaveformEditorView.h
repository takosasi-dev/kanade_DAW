#pragma once
#include "UI/UiSupport.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace ss
{
    /** Non-destructive audio clip editor (spec 8.4.4): trim, cut, fade in/out,
        gain, normalize and reverse on the clip UiState points at.

        Every operation is a change to the AudioClip's fields, never to the file
        on disk, and every one of them goes through the undo manager.

        MainComponent::View has no entry for this editor (the header is frozen),
        so it opens as its own window from the timeline's clip menu. */
    class WaveformEditorView final : public ProjectView,
                                     private juce::Timer
    {
    public:
        WaveformEditorView (AppContext&, UiState&);
        ~WaveformEditorView() override;

        /** Opens the editor for the currently selected audio clip. */
        static void launch (AppContext&, UiState&);

        void paint (juce::Graphics&) override;
        void resized() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;

        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;

        AudioClip* currentClip() const;
        void refresh();
        void edit (const juce::String& name, std::function<void (AudioClip&)>);

        juce::Rectangle<int> waveArea() const;
        double timeForX (int x) const;
        float  xForTime (double seconds) const;
        double clipDurationSeconds() const;

        juce::AudioThumbnailCache cache { 8 };
        std::unique_ptr<juce::AudioThumbnail> thumbnail;
        juce::File loadedFile;

        double selectionStart = 0.0, selectionEnd = 0.0;   // seconds into the source file
        double dragAnchor = 0.0;

        juce::Label      infoLabel, selectionLabel;
        juce::TextButton trimButton, deleteButton, fadeInButton, fadeOutButton,
                         normalizeButton, reverseButton, resetButton, transcribeButton;
        juce::Slider     gainSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Label      gainLabel;
        bool             updating = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformEditorView)
    };
}
