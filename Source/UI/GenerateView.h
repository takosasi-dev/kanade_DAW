#pragma once
#include "AI/Generator.h"
#include "UI/UiSupport.h"

namespace ss
{
    class GenerateView;

    /** Turns one generation batch into Session scenes: one scene per
        candidate, one track per part shared across every candidate in the
        batch (not one new track per candidate - the whole point is that
        candidates land in the same track column so Session's per-track
        exclusivity becomes an instant A/B switch between them).  A free
        function, not a GenerateView method, so it is testable without a
        live view (spec: Session view, Phase 3). */
    void sendCandidatesToSession (Project&, const std::vector<Generator::Candidate>&);

    /** One entry in the candidate gallery (spec 9.4): a thumbnail of the
        generated material, click to audition, drag to adopt. */
    class CandidateCard final : public juce::Component,
                                public juce::TooltipClient
    {
    public:
        CandidateCard (GenerateView&, int index);

        void setSelected (bool);

        /** The candidate's notes in full - the card only has room for the first
            line or two of them. */
        juce::String getTooltip() override;
        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;

    private:
        GenerateView& owner;
        int index;
        bool selected = false;

        juce::TextButton adoptButton, rerollButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CandidateCard)
    };

    //==============================================================================
    /** MIDI generation screen (spec 9.4 / 9.5).

        Source preview on the left, parameters on the right, candidate gallery
        along the bottom.  Candidates audition instantly through
        AudioEngine::previewMidiClip, can be re-rolled a part at a time, dragged
        onto the timeline, and A/B'd against the source material. */
    class GenerateView final : public ProjectView
    {
    public:
        GenerateView (AppContext&, UiState&);
        ~GenerateView() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;

        // --- used by CandidateCard -------------------------------------------
        const Generator::Candidate* candidateAt (int index) const;
        void selectCandidate (int index);
        void auditionCandidate (int index);
        void adoptCandidate (int index);
        void sendCandidatesToSession();
        void showRerollMenu (int index, juce::Component* target);
        MidiClip mergeCandidate (int index) const;
        UiState& getUiState() noexcept { return ui; }

    private:
        void startGeneration();
        void rebuildGallery();
        Generator::Input buildInput() const;
        Generator::Options buildOptions() const;
        const MidiClip* sourceClip() const;
        void toggleAB();

        juce::Rectangle<int> sourceArea() const;

        std::vector<Generator::Candidate> candidates;
        int selectedCandidate = -1;
        bool playingOriginal = false;

        // --- widgets ----------------------------------------------------------
        juce::Label      sourceTitle, sourceInfo, parametersTitle, galleryTitle;
        juce::ComboBox   modeBox, genreBox, keyRootBox, keyScaleBox;
        juce::Label      modeLabel, genreLabel, keyLabel, complexityLabel, creativityLabel,
                         lengthLabel, candidatesLabel, timingLabel, velocityLabel, moodLabel;
        juce::Slider     complexitySlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Slider     creativitySlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Slider     humaniseTiming   { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Slider     humaniseVelocity { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Slider     lengthSlider     { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Slider     candidateCount   { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::ToggleButton useDetectedKey;
        juce::TextEditor moodEditor;
        juce::OwnedArray<juce::ToggleButton> moodButtons, partButtons;

        juce::TextButton generateButton, abButton, stopButton, useChordTrackButton, sendToSessionButton;
        bool useChordTrack = false;

        juce::Component  galleryHolder;
        juce::Viewport   gallery;
        juce::OwnedArray<CandidateCard> cards;
        TaskPanel        task;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GenerateView)
    };
}
