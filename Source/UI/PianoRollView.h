#pragma once
#include "AI/Transcriber.h"      // Transcriber::Suggestion, applied by the fix menu
#include "UI/UiSupport.h"

namespace ss
{
    /** Piano roll editor (spec 8.3 / 9.3).

        Edits whatever MIDI clip UiState currently points at: note draw / move /
        resize / velocity, scale highlighting and chord hints from the key,
        quantise controls, ghosted notes from the other MIDI tracks, and the
        per-note confidence shading plus low-confidence walk-through that the
        transcription flow depends on. */
    class PianoRollView final : public ProjectView,
                                private juce::Timer,
                                private juce::ScrollBar::Listener
    {
    public:
        PianoRollView (AppContext&, UiState&);
        ~PianoRollView() override;

        /** Embedded in the transcribe view the toolbar is redundant - the
            accuracy panel already owns those controls. */
        void setCompact (bool shouldBeCompact);

        /** Selects and scrolls to the next note under the review threshold.
            Returns false when there are none left after the current one. */
        bool jumpToNextLowConfidenceNote();
        int  countLowConfidenceNotes() const;

        /** Suggestion menu for the currently selected note (spec 9.3). */
        void showFixSuggestionsForSelection();

        void paint (juce::Graphics&) override;
        void resized() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;

        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        bool keyPressed (const juce::KeyPress&) override;

    private:
        enum class DragMode { none, move, resizeStart, resizeEnd, velocity, marquee };

        void timerCallback() override;
        void scrollBarMoved (juce::ScrollBar*, double) override;

        MidiClip* currentClip() const;
        void      refreshFromProject();
        void      rebuildChordHints();
        void      updateScrollRanges();

        juce::Rectangle<int> toolbarArea() const;
        juce::Rectangle<int> rulerArea() const;
        juce::Rectangle<int> keyboardArea() const;
        juce::Rectangle<int> noteArea() const;
        juce::Rectangle<int> velocityArea() const;

        double beatForX (int x) const;
        float  xForBeat (double beat) const;
        int    pitchForY (int y) const;
        float  yForPitch (int pitch) const;

        int  noteIndexAt (juce::Point<int>) const;
        juce::Rectangle<float> rectForNote (const Note&, double clipStart) const;

        void commitDrag (const juce::String& transactionName);
        void applyQuantiseToSelection (bool everything);
        void showNoteContextMenu (int noteIndex, juce::Point<int> where);
        /** Splices in a suggestion's replacement notes - N notes out, M in, so
            split and merge apply as they are rather than as a length edit. */
        void applySuggestion (const Transcriber::Suggestion&);
        void selectNoteAndScrollTo (int index);
        void reselectByValue (const std::vector<Note>& wanted);
        void deleteSelection();
        void paintToolbarBackground (juce::Graphics&);

        // --- view state -----------------------------------------------------
        double pixelsPerBeat = 70.0;
        double scrollBeats   = 0.0;
        double keyHeight     = 11.0;
        int    lowestVisiblePitch = 48;
        bool   compact = false;
        bool   overlayOtherTracks = false;

        // --- editing state --------------------------------------------------
        std::vector<int>  selection;
        std::vector<Note> notesBeforeDrag;
        DragMode          dragMode = DragMode::none;
        bool              dragMoved = false;
        juce::Point<int>  dragStartPoint;
        double            dragStartBeat = 0.0;
        int               dragStartPitch = 0;
        int               hoverNote = -1;
        int               reviewCursor = -1;
        juce::Rectangle<int> marquee;
        int               lastNoteCount = -1;

        std::vector<ChordEvent> chordHints;

        // --- widgets --------------------------------------------------------
        juce::Label      titleLabel;
        juce::ComboBox   keyRootBox, keyScaleBox, quantiseBox;
        juce::Slider     quantiseStrength { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::TextButton quantiseButton, humaniseButton, snapToScaleButton;
        juce::ToggleButton overlayToggle, snapToggle;
        juce::TextButton nextLowConfidenceButton, fixButton;
        juce::Label      lowConfidenceLabel;
        juce::ScrollBar  hScroll { false }, vScroll { true };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollView)
    };
}
