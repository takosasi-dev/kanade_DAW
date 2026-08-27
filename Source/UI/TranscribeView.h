#pragma once
#include "AI/Transcriber.h"
#include "UI/PianoRollView.h"
#include "UI/UiSupport.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace ss
{
    /** Audio -> MIDI screen (spec 9.3).

        Dual display: the source waveform on top, the transcribed notes in a
        piano roll underneath, sharing one time axis.  The accuracy panel on the
        right drives Transcriber::Options; stem tabs appear once the separator
        has produced stems; the review row at the bottom walks the low-confidence
        notes.  Transcription and separation both run on a background thread with
        an inline progress bar and a working cancel button. */
    class TranscribeView final : public ProjectView
    {
    public:
        TranscribeView (AppContext&, UiState&);
        ~TranscribeView() override;

        void detachFromProject() override;
        void attachToProject() override;

        /** Called by MainComponent when this view becomes visible, so a
            "Transcribe this range" request from the timeline is picked up. */
        void consumePendingRequest();

        void paint (juce::Graphics&) override;
        void resized() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;

    private:
        void setSource (const juce::File&, double offsetSeconds, double lengthSeconds,
                        const juce::String& displayName);
        void startTranscription();
        void startStemSeparation();
        void applyResult (const Transcriber::Result&);
        Transcriber::Options currentOptions() const;
        void applyBulkCleanup (int which);
        void refreshReviewRow();
        void rebuildStemTabs();

        juce::Rectangle<int> waveArea() const;
        MidiClip* resultClip() const;

        // --- source ---------------------------------------------------------
        juce::File   sourceFile;
        double       sourceOffsetSeconds = 0.0, sourceLengthSeconds = 0.0;
        juce::String sourceName;
        double       placeAtBeat = 0.0;

        juce::AudioThumbnailCache cache { 8 };
        std::unique_ptr<juce::AudioThumbnail> thumbnail;

        std::vector<StemSeparator::Output> stems;
        int selectedStem = -1;   // -1 == the original mix

        // --- result ---------------------------------------------------------
        TrackId resultTrack = invalidTrackId;
        ClipId  resultClipId = invalidClipId;
        Transcriber::Result lastResult;
        bool    hasResult = false;

        // --- widgets --------------------------------------------------------
        juce::Label       sourceLabel, summaryLabel, reviewLabel;
        juce::TabbedButtonBar stemTabs { juce::TabbedButtonBar::TabsAtTop };

        juce::Label    modeLabel, sensitivityLabel, minLengthLabel, pitchRangeLabel,
                       quantiseLabel, strengthLabel, confidenceLabel, reviewThresholdLabel;
        juce::ComboBox modeBox, quantiseBox;
        juce::Slider   sensitivitySlider  { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Slider   minLengthSlider    { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Slider   pitchRangeSlider   { juce::Slider::TwoValueHorizontal, juce::Slider::NoTextBox };
        juce::Slider   strengthSlider     { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Slider   confidenceFloorSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Slider   reviewThresholdSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::ToggleButton swingToggle, tempoToggle, keyToggle;

        juce::TextButton transcribeButton, separateButton, chooseFileButton, applyTempoButton;
        juce::TextButton removeLowConfidenceButton, removeShortButton, mergeRepeatsButton, snapScaleButton;
        juce::TextButton previousReviewButton, nextReviewButton, fixButton;

        PianoRollView pianoRoll;
        TaskPanel     task;

        std::unique_ptr<juce::FileChooser> chooser;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TranscribeView)
    };
}
