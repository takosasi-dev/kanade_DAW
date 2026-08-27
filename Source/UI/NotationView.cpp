#include "UI/NotationView.h"
#include <cmath>

namespace ss
{
    namespace
    {
        constexpr int bannerHeight  = 26;
        constexpr int toolbarHeight = 30;
        constexpr int chordRowHeight = 24;
        constexpr int lyricRowHeight = 26;
        constexpr int marginLeft    = 64;
        constexpr int scrollThickness = 12;
        constexpr float lineSpacing = 9.0f;   // distance between staff lines

        /** Diatonic step (0..6) within the octave and whether the pitch needs a
            sharp when written from the natural below it. */
        struct StaffStep { int step; bool sharp; };

        StaffStep staffStepFor (int pitch) noexcept
        {
            static const StaffStep table[12]
            {
                { 0, false }, { 0, true }, { 1, false }, { 1, true }, { 2, false }, { 3, false },
                { 3, true },  { 4, false }, { 4, true }, { 5, false }, { 5, true }, { 6, false }
            };
            const int pc = ((pitch % 12) + 12) % 12;
            const int octave = pitch / 12;
            auto entry = table[pc];
            entry.step += octave * 7;
            return entry;
        }

        /** y for a note whose diatonic step index is `step`, on a staff whose
            bottom line sits at `bottomLineY` and holds `bottomStep`. */
        float yForStep (float bottomLineY, int bottomStep, int step) noexcept
        {
            return bottomLineY - (step - bottomStep) * (lineSpacing * 0.5f);
        }
    }

    //==============================================================================
    NotationView::NotationView (AppContext& c, UiState& s)
        : ProjectView (c, s)
    {
        titleLabel.setText (TRANS ("Notation"), juce::dontSendNotification);
        titleLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        titleLabel.setColour (juce::Label::textColourId, palette().textBright);
        addAndMakeVisible (titleLabel);

        bannerLabel.setText (TRANS ("Read-only preview. Notation editing arrives in Phase 2 - "
                                    "edit the notes in the piano roll for now."),
                             juce::dontSendNotification);
        bannerLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        bannerLabel.setColour (juce::Label::textColourId, palette().warning);
        addAndMakeVisible (bannerLabel);

        openPianoRollButton.setButtonText (TRANS ("Edit in piano roll"));
        openPianoRollButton.onClick = [this] { ui.goTo (MainComponent::View::pianoRoll); };
        addAndMakeVisible (openPianoRollButton);

        exportMusicXmlButton.setButtonText (TRANS ("Export MusicXML..."));
        exportMusicXmlButton.setTooltip (TRANS ("Uses File > Export > MusicXML"));
        // Routed through the command so there is one exporter and one file chooser.
        exportMusicXmlButton.onClick = [this] { ui.invoke (CommandIDs::exportMusicXml); };
        addAndMakeVisible (exportMusicXmlButton);

        showChordsToggle.setButtonText (TRANS ("Chord row"));
        showChordsToggle.setToggleState (true, juce::dontSendNotification);
        showChordsToggle.onClick = [this] { repaint(); };
        addAndMakeVisible (showChordsToggle);

        showLyricsToggle.setButtonText (TRANS ("Lyric row"));
        showLyricsToggle.setToggleState (true, juce::dontSendNotification);
        showLyricsToggle.onClick = [this] { repaint(); };
        addAndMakeVisible (showLyricsToggle);

        phase2Label.setText (TRANS ("Part extraction and engraving settings: Phase 2"),
                             juce::dontSendNotification);
        phase2Label.setFont (juce::Font (juce::FontOptions (11.0f)));
        phase2Label.setColour (juce::Label::textColourId, palette().textDim);
        phase2Label.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (phase2Label);

        hScroll.addListener (this);
        addAndMakeVisible (hScroll);
    }

    NotationView::~NotationView()
    {
        hScroll.removeListener (this);
    }

    MidiClip* NotationView::currentClip() const
    {
        if (ctx.project == nullptr || ui.selectedClipIsAudio)
            return nullptr;

        if (auto* t = project().findTrack (ui.selectedTrack))
            return t->findMidiClip (ui.selectedClip);

        return nullptr;
    }

    void NotationView::changeListenerCallback (juce::ChangeBroadcaster*)
    {
        auto* clip = currentClip();
        const double length = clip != nullptr ? clip->lengthBeats : 32.0;
        const double visible = juce::jmax (1.0, (getWidth() - marginLeft) / juce::jmax (1.0, pixelsPerBeat));

        hScroll.setRangeLimits (0.0, juce::jmax (length, visible), juce::dontSendNotification);
        hScroll.setCurrentRange (scrollBeats, visible, juce::dontSendNotification);
        repaint();
    }

    void NotationView::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
    {
        scrollBeats = juce::jmax (0.0, newRangeStart);
        repaint();
    }

    void NotationView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        if (e.mods.isCommandDown())
            pixelsPerBeat = juce::jlimit (12.0, 200.0, pixelsPerBeat * (1.0 + wheel.deltaY * 0.5));
        else
            scrollBeats = juce::jmax (0.0, scrollBeats - wheel.deltaY * 6.0);

        changeListenerCallback (nullptr);
    }

    float NotationView::xForBeat (double beat) const
    {
        return (float) (marginLeft + (beat - scrollBeats) * pixelsPerBeat);
    }

    //==============================================================================
    void NotationView::paintStaff (juce::Graphics& g, juce::Rectangle<int> area, int bottomLinePitch,
                                   const juce::String& clefGlyph) const
    {
        const auto& p = palette();
        const float bottomLineY = (float) area.getBottom() - lineSpacing;

        g.setColour (p.text.withAlpha (0.7f));
        for (int line = 0; line < 5; ++line)
        {
            const float y = bottomLineY - line * lineSpacing;
            g.drawHorizontalLine (juce::roundToInt (y), (float) area.getX(), (float) area.getRight());
        }

        // ponytail: the clefs are the Unicode musical symbols, which rely on the
        // platform's font fallback.  Swap for a bundled SMuFL font (Bravura) when
        // notation graduates out of read-only in Phase 2.
        g.setFont (juce::Font (juce::FontOptions (34.0f)));
        g.setColour (p.text);
        g.drawText (clefGlyph, area.getX() - 46, juce::roundToInt (bottomLineY - 4 * lineSpacing) - 10,
                    40, juce::roundToInt (4 * lineSpacing) + 20, juce::Justification::centredRight, false);

        juce::ignoreUnused (bottomLinePitch);
    }

    void NotationView::paintNotes (juce::Graphics& g, juce::Rectangle<int> treble, juce::Rectangle<int> bass,
                                   const MidiClip& clip) const
    {
        const auto& p = palette();

        const int trebleBottomStep = staffStepFor (64).step;   // E4
        const int bassBottomStep   = staffStepFor (43).step;   // G2
        const float trebleBottomY  = (float) treble.getBottom() - lineSpacing;
        const float bassBottomY    = (float) bass.getBottom() - lineSpacing;

        const double visibleEnd = scrollBeats + (getWidth() - marginLeft) / juce::jmax (1.0, pixelsPerBeat);

        for (const auto& note : clip.notes)
        {
            if (note.endBeats() < scrollBeats || note.startBeats > visibleEnd)
                continue;

            const auto info = staffStepFor (note.pitch);
            const bool onTreble = note.pitch >= 60;

            const float bottomY    = onTreble ? trebleBottomY : bassBottomY;
            const int   bottomStep = onTreble ? trebleBottomStep : bassBottomStep;
            const auto  staffArea  = onTreble ? treble : bass;

            const float x = xForBeat (note.startBeats);
            const float y = yForStep (bottomY, bottomStep, info.step);

            // Ledger lines above and below the five staff lines.
            g.setColour (p.text.withAlpha (0.7f));
            for (int step = bottomStep + 10; step <= info.step; step += 2)
                g.drawHorizontalLine (juce::roundToInt (yForStep (bottomY, bottomStep, step)),
                                      x - 8.0f, x + 8.0f);
            for (int step = bottomStep - 2; step >= info.step; step -= 2)
                g.drawHorizontalLine (juce::roundToInt (yForStep (bottomY, bottomStep, step)),
                                      x - 8.0f, x + 8.0f);

            // Confidence shading carries through from the piano roll (spec 9.3).
            const auto colour = note.confidence >= 0.999f ? p.text : confidenceColour (note.confidence);
            const bool hollow = note.lengthBeats >= 2.0;

            juce::Rectangle<float> head (x - 5.5f, y - lineSpacing * 0.5f + 0.5f, 11.0f, lineSpacing - 1.0f);
            g.setColour (colour);
            if (hollow) g.drawEllipse (head, 1.6f);
            else        g.fillEllipse (head);

            if (info.sharp)
            {
                g.setFont (juce::Font (juce::FontOptions (13.0f)));
                g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\x99\xaf")),
                            juce::roundToInt (x) - 22, juce::roundToInt (y - 8.0f), 16, 16,
                            juce::Justification::centredRight, false);
            }

            // Stems, except on whole notes.
            if (note.lengthBeats < 4.0)
            {
                const bool stemUp = y > staffArea.getCentreY();
                g.drawLine (stemUp ? head.getRight() : head.getX(), y,
                            stemUp ? head.getRight() : head.getX(), y + (stemUp ? -26.0f : 26.0f), 1.4f);

                // Beam-less flags: one bar per halving below a quarter note.
                int flags = 0;
                for (double len = note.lengthBeats; len < 0.75 && flags < 3; len *= 2.0)
                    ++flags;

                for (int f = 0; f < flags; ++f)
                {
                    const float fy = y + (stemUp ? -26.0f + f * 5.0f : 26.0f - f * 5.0f);
                    const float fx = stemUp ? head.getRight() : head.getX();
                    g.drawLine (fx, fy, fx + 7.0f, fy + (stemUp ? 7.0f : -7.0f), 1.4f);
                }
            }
        }
    }

    void NotationView::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        g.fillAll (p.windowBg);

        auto area = getLocalBounds();
        area.removeFromTop (toolbarHeight);

        g.setColour (p.warning.withAlpha (0.12f));
        g.fillRect (area.removeFromTop (bannerHeight));

        area.removeFromBottom (scrollThickness);

        // The page: a light sheet in both themes, because staff notation on a
        // dark ground is genuinely harder to read.
        auto page = area.reduced (12, 8);
        g.setColour (p.isDark ? juce::Colour (0xff1b1e24) : juce::Colours::white);
        g.fillRect (page);
        g.setColour (p.outline);
        g.drawRect (page, 1);

        auto* clip = currentClip();
        if (clip == nullptr)
        {
            g.setColour (p.textDim);
            g.setFont (juce::Font (juce::FontOptions (15.0f)));
            g.drawText (TRANS ("Select a MIDI clip in the timeline to see it as notation"),
                        page, juce::Justification::centred, false);
            return;
        }

        auto content = page.reduced (8, 6);
        auto chordRow = showChordsToggle.getToggleState() ? content.removeFromTop (chordRowHeight)
                                                          : juce::Rectangle<int>();
        auto lyricRow = showLyricsToggle.getToggleState() ? content.removeFromBottom (lyricRowHeight)
                                                          : juce::Rectangle<int>();

        const int staffHeight = juce::jmax (52, content.getHeight() / 2 - 8);
        auto treble = content.removeFromTop (staffHeight);
        auto bass   = content.removeFromBottom (staffHeight);

        // Bar lines across the whole system.
        const auto ts = project().tempo.timeSignatureAt (clip->startBeats);
        const double barLength = juce::jmax (1.0, ts.numerator * (4.0 / juce::jmax (1, ts.denominator)));
        const double visibleEnd = scrollBeats + (getWidth() - marginLeft) / juce::jmax (1.0, pixelsPerBeat);

        g.setColour (p.text.withAlpha (0.5f));
        for (double b = std::floor (scrollBeats / barLength) * barLength; b <= visibleEnd; b += barLength)
        {
            const float x = xForBeat (b);
            if (x < page.getX() || x > page.getRight()) continue;
            g.drawVerticalLine (juce::roundToInt (x), (float) treble.getBottom() - 5 * lineSpacing,
                                (float) bass.getBottom() - lineSpacing);
        }

        paintStaff (g, treble, 64, juce::String (juce::CharPointer_UTF8 ("\xf0\x9d\x84\x9e")));  // treble clef
        paintStaff (g, bass,   43, juce::String (juce::CharPointer_UTF8 ("\xf0\x9d\x84\xa2"))); // bass clef
        paintNotes (g, treble, bass, *clip);

        if (! chordRow.isEmpty())
        {
            g.setColour (p.textDim);
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            for (const auto& chord : project().chords)
            {
                const float x = xForBeat (chord.beat - clip->startBeats);
                if (x < page.getX() || x > page.getRight()) continue;
                g.drawText (chord.symbol, juce::roundToInt (x), chordRow.getY(), 80, chordRow.getHeight(),
                            juce::Justification::centredLeft, false);
            }
        }

        if (! lyricRow.isEmpty())
        {
            g.setColour (p.textDim.withAlpha (0.7f));
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (TRANS ("Lyrics: Phase 2"), lyricRow, juce::Justification::centredLeft, false);
        }
    }

    void NotationView::resized()
    {
        auto area = getLocalBounds();

        auto toolbar = area.removeFromTop (toolbarHeight).reduced (8, 3);
        titleLabel.setBounds (toolbar.removeFromLeft (90));
        openPianoRollButton.setBounds (toolbar.removeFromLeft (140).reduced (2, 0));
        exportMusicXmlButton.setBounds (toolbar.removeFromLeft (150).reduced (2, 0));
        toolbar.removeFromLeft (10);
        showChordsToggle.setBounds (toolbar.removeFromLeft (104));
        showLyricsToggle.setBounds (toolbar.removeFromLeft (104));
        phase2Label.setBounds (toolbar);

        bannerLabel.setBounds (area.removeFromTop (bannerHeight).reduced (12, 2));
        hScroll.setBounds (area.removeFromBottom (scrollThickness).reduced (12, 0));

        changeListenerCallback (nullptr);
    }
}
