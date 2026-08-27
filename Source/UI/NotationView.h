#pragma once
#include "UI/UiSupport.h"

namespace ss
{
    /** Read-only staff view of the current MIDI clip (spec 8.5 / 9.8).

        The spec puts real notation EDITING in Phase 2, so this renders the same
        note data the piano roll edits - grand staff, chord row above, lyric row
        below - and says plainly that editing is not here yet.  Nothing in this
        file writes to the document. */
    class NotationView final : public ProjectView,
                               private juce::ScrollBar::Listener
    {
    public:
        NotationView (AppContext&, UiState&);
        ~NotationView() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    private:
        void scrollBarMoved (juce::ScrollBar*, double) override;

        MidiClip* currentClip() const;
        void paintStaff (juce::Graphics&, juce::Rectangle<int> area, int bottomLinePitch,
                         const juce::String& clefGlyph) const;
        void paintNotes (juce::Graphics&, juce::Rectangle<int> treble, juce::Rectangle<int> bass,
                         const MidiClip&) const;

        float xForBeat (double beat) const;

        double pixelsPerBeat = 46.0;
        double scrollBeats   = 0.0;

        juce::Label       bannerLabel, titleLabel;
        juce::TextButton  openPianoRollButton, exportMusicXmlButton;
        juce::ToggleButton showChordsToggle, showLyricsToggle;
        juce::Label       phase2Label;
        juce::ScrollBar   hScroll { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NotationView)
    };
}
