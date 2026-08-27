#pragma once
#include "UI/UiSupport.h"

namespace ss
{
    /** A plain container whose owner supplies the layout.  Tab content gets
        resized by the TabbedComponent, not by us, so it needs its own hook. */
    struct LaidOutPanel : juce::Component
    {
        std::function<void()> onResized;
        void resized() override { if (onResized) onResized(); }
    };

    /** Left-hand browser (spec 9.1): the project's tracks and clips, and a file
        tree over the projects folder and every configured sample library. */
    class BrowserPanel final : public ProjectView,
                               private juce::ListBoxModel,
                               private juce::FileBrowserListener
    {
    public:
        BrowserPanel (AppContext&, UiState&);
        ~BrowserPanel() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;

    private:
        struct Entry { TrackId track; ClipId clip; bool isAudio; bool isTrackRow; juce::String text; };

        void rebuildEntries();
        void rebuildRoots();

        // ListBoxModel
        int  getNumRows() override;
        void paintListBoxItem (int, juce::Graphics&, int, int, bool) override;
        void listBoxItemClicked (int, const juce::MouseEvent&) override;
        void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override;

        // FileBrowserListener
        void selectionChanged() override {}
        void fileClicked (const juce::File&, const juce::MouseEvent&) override {}
        void fileDoubleClicked (const juce::File&) override;
        void browserRootChanged (const juce::File&) override {}

        std::vector<Entry> entries;

        juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
        LaidOutPanel          projectTab, filesTab;
        juce::ListBox         projectList;
        juce::ComboBox        rootBox;
        juce::Label           hintLabel;

        juce::TimeSliceThread          directoryThread { "ScoreSmith Browser" };
        juce::WildcardFileFilter       fileFilter;
        std::unique_ptr<juce::DirectoryContentsList> directoryList;
        std::unique_ptr<juce::FileTreeComponent>     fileTree;
        juce::StringArray roots;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrowserPanel)
    };

    //==========================================================================
    /** Right-hand AI panel (spec 9.1): the entry points into transcription and
        generation, plus the confidence readout for whatever clip is selected. */
    class AiPanel final : public ProjectView
    {
    public:
        AiPanel (AppContext&, UiState&);

        void paint (juce::Graphics&) override;
        void resized() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;

    private:
        MidiClip*  selectedMidiClip() const;
        AudioClip* selectedAudioClip() const;
        void refresh();

        juce::Label      titleLabel, statusLabel, confidenceTitle, confidenceLabel, thresholdLabel;
        juce::TextButton transcribeButton, generateButton, reviewButton, waveformButton;
        juce::Slider     thresholdSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

        /** Histogram of note confidences, ten buckets. */
        int histogram[10] {};
        int lowConfidenceCount = 0, noteCount = 0;
        juce::Rectangle<int> histogramArea;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AiPanel)
    };
}
