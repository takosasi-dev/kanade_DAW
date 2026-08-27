#include "UI/PianoRollView.h"
#include "AI/Generator.h"
#include "AI/Transcriber.h"
#include "Engine/AudioEngine.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace ss
{
    namespace
    {
        constexpr int keyboardWidth  = 56;
        constexpr int rulerHeight    = 22;
        constexpr int toolbarHeight  = 32;
        constexpr int velocityHeight = 66;
        constexpr int scrollThickness = 12;
        constexpr int chordStripHeight = 16;
        constexpr float edgeGrab     = 5.0f;

        bool isBlackKey (int pitch) noexcept
        {
            switch (((pitch % 12) + 12) % 12)
            {
                case 1: case 3: case 6: case 8: case 10: return true;
                default: return false;
            }
        }
    }

    //==============================================================================
    PianoRollView::PianoRollView (AppContext& c, UiState& s)
        : ProjectView (c, s)
    {
        setWantsKeyboardFocus (true);

        titleLabel.setText (TRANS ("Piano Roll"), juce::dontSendNotification);
        titleLabel.setColour (juce::Label::textColourId, palette().textDim);
        addAndMakeVisible (titleLabel);

        fillKeyComboBoxes (keyRootBox, keyScaleBox, ui.editKey);
        keyRootBox.setTooltip (TRANS ("Key for scale highlighting and chord hints"));
        auto keyChanged = [this]
        {
            ui.editKey = keyFromComboBoxes (keyRootBox, keyScaleBox);
            rebuildChordHints();
            repaint();
        };
        keyRootBox.onChange  = keyChanged;
        keyScaleBox.onChange = keyChanged;
        addAndMakeVisible (keyRootBox);
        addAndMakeVisible (keyScaleBox);

        fillQuantiseComboBox (quantiseBox, ui.grid);
        quantiseBox.setTooltip (TRANS ("Grid"));
        quantiseBox.onChange = [this]
        {
            ui.grid = quantiseFromComboBox (quantiseBox);
            ui.sendChangeMessage();
            repaint();
        };
        addAndMakeVisible (quantiseBox);

        quantiseStrength.setRange (0.0, 1.0, 0.01);
        quantiseStrength.setValue (1.0, juce::dontSendNotification);
        quantiseStrength.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
        quantiseStrength.setTooltip (TRANS ("Quantise strength"));
        addAndMakeVisible (quantiseStrength);

        quantiseButton.setButtonText (TRANS ("Quantise"));
        quantiseButton.onClick = [this] { applyQuantiseToSelection (selection.empty()); };
        addAndMakeVisible (quantiseButton);

        humaniseButton.setButtonText (TRANS ("Humanise"));
        humaniseButton.onClick = [this]
        {
            auto* clip = currentClip();
            if (clip == nullptr) return;
            const double bpm = project().tempo.bpmAt (clip->startBeats);
            const auto timeSig = project().tempo.timeSignatureAt (clip->startBeats);
            performProjectEdit (project(), TRANS ("Humanise"), [this, bpm, timeSig]
            {
                if (auto* c = currentClip())
                    Generator::humanise (c->notes, 0.35, 0.35, bpm, timeSig,
                                         juce::Random::getSystemRandom().nextInt64());
            });
        };
        addAndMakeVisible (humaniseButton);

        snapToScaleButton.setButtonText (TRANS ("Snap to scale"));
        snapToScaleButton.onClick = [this]
        {
            if (currentClip() == nullptr) return;
            const auto key = ui.editKey;
            performProjectEdit (project(), TRANS ("Snap to scale"), [this, key]
            {
                if (auto* c = currentClip())
                    for (auto& n : c->notes)
                        n.pitch = theory::snapToScale (n.pitch, key);
            });
        };
        addAndMakeVisible (snapToScaleButton);

        overlayToggle.setButtonText (TRANS ("Overlay tracks"));
        overlayToggle.setTooltip (TRANS ("Show notes from the other MIDI tracks behind this clip"));
        overlayToggle.onClick = [this] { overlayOtherTracks = overlayToggle.getToggleState(); repaint(); };
        addAndMakeVisible (overlayToggle);

        snapToggle.setButtonText (TRANS ("Snap"));
        snapToggle.setToggleState (ui.snapEnabled, juce::dontSendNotification);
        snapToggle.onClick = [this] { ui.snapEnabled = snapToggle.getToggleState(); ui.sendChangeMessage(); };
        addAndMakeVisible (snapToggle);

        nextLowConfidenceButton.setButtonText (TRANS ("Next low-confidence"));
        nextLowConfidenceButton.setTooltip (TRANS ("Jump to the next note the transcriber was unsure about"));
        nextLowConfidenceButton.onClick = [this] { jumpToNextLowConfidenceNote(); };
        addAndMakeVisible (nextLowConfidenceButton);

        fixButton.setButtonText (TRANS ("Fix..."));
        fixButton.setTooltip (TRANS ("One-click corrections for the selected note"));
        fixButton.onClick = [this] { showFixSuggestionsForSelection(); };
        addAndMakeVisible (fixButton);

        lowConfidenceLabel.setColour (juce::Label::textColourId, palette().warning);
        lowConfidenceLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (lowConfidenceLabel);

        hScroll.addListener (this);
        vScroll.addListener (this);
        addAndMakeVisible (hScroll);
        addAndMakeVisible (vScroll);

        refreshFromProject();
        startTimerHz (24);
    }

    PianoRollView::~PianoRollView()
    {
        stopTimer();
        hScroll.removeListener (this);
        vScroll.removeListener (this);
    }

    void PianoRollView::setCompact (bool shouldBeCompact)
    {
        compact = shouldBeCompact;

        auto show = [this] (juce::Component& c) { c.setVisible (! compact); };
        show (titleLabel);       show (keyRootBox);      show (keyScaleBox);
        show (quantiseBox);      show (quantiseStrength); show (quantiseButton);
        show (humaniseButton);   show (snapToScaleButton); show (overlayToggle);
        show (snapToggle);       show (nextLowConfidenceButton);
        show (fixButton);        show (lowConfidenceLabel);

        resized();
    }

    //==============================================================================
    MidiClip* PianoRollView::currentClip() const
    {
        if (ctx.project == nullptr || ui.selectedClipIsAudio)
            return nullptr;

        if (auto* t = project().findTrack (ui.selectedTrack))
            return t->findMidiClip (ui.selectedClip);

        return nullptr;
    }

    void PianoRollView::changeListenerCallback (juce::ChangeBroadcaster*)
    {
        refreshFromProject();
    }

    void PianoRollView::refreshFromProject()
    {
        auto* clip = currentClip();
        const int count = clip != nullptr ? (int) clip->notes.size() : -1;

        if (count != lastNoteCount)
        {
            // The note list changed underneath us (undo, transcription, another
            // view) - index-based selection can no longer be trusted.
            selection.clear();
            reviewCursor = -1;
            lastNoteCount = count;
        }

        snapToggle.setToggleState (ui.snapEnabled, juce::dontSendNotification);

        // Another view may have changed the key or the grid (the transcriber
        // detects a key, the timeline changes the grid) - follow it.
        keyRootBox.setSelectedId (juce::jlimit (0, 11, ui.editKey.tonic) + 1, juce::dontSendNotification);
        {
            const auto& scales = scaleMenuValues();
            for (int i = 0; i < (int) scales.size(); ++i)
                if (scales[(size_t) i] == ui.editKey.scale)
                    keyScaleBox.setSelectedId (i + 1, juce::dontSendNotification);

            const auto& grids = quantiseMenuValues();
            for (int i = 0; i < (int) grids.size(); ++i)
                if (grids[(size_t) i] == ui.grid)
                    quantiseBox.setSelectedId (i + 1, juce::dontSendNotification);
        }

        rebuildChordHints();
        updateScrollRanges();

        const int low = countLowConfidenceNotes();
        lowConfidenceLabel.setText (low > 0 ? juce::String (low) + " " + TRANS ("to review")
                                            : TRANS ("All notes confident"),
                                    juce::dontSendNotification);
        lowConfidenceLabel.setColour (juce::Label::textColourId,
                                      low > 0 ? palette().warning : palette().textDim);
        nextLowConfidenceButton.setEnabled (low > 0);
        repaint();
    }

    void PianoRollView::rebuildChordHints()
    {
        chordHints.clear();
        auto* clip = currentClip();
        if (clip == nullptr || ctx.project == nullptr)
            return;

        // The chord track wins when the project has one; otherwise the hints are
        // analysed per bar so the roll still tells you what you are looking at.
        if (! project().chords.empty())
        {
            chordHints = project().chords;
            return;
        }

        const auto ts = project().tempo.timeSignatureAt (clip->startBeats);
        const double barLength = juce::jmax (1.0, ts.numerator * (4.0 / juce::jmax (1, ts.denominator)));

        for (double b = 0.0; b < clip->lengthBeats; b += barLength)
        {
            auto chord = theory::detectChord (clip->notes, b, b + barLength, ui.editKey);
            if (chord.symbol.isNotEmpty())
            {
                chord.beat = clip->startBeats + b;
                chord.lengthBeats = barLength;
                chordHints.push_back (chord);
            }
        }
    }

    int PianoRollView::countLowConfidenceNotes() const
    {
        auto* clip = currentClip();
        if (clip == nullptr) return 0;

        int n = 0;
        for (const auto& note : clip->notes)
            if (note.isLowConfidence (ui.confidenceThreshold))
                ++n;
        return n;
    }

    //==============================================================================
    juce::Rectangle<int> PianoRollView::toolbarArea() const
    {
        return compact ? juce::Rectangle<int>() : getLocalBounds().removeFromTop (toolbarHeight);
    }

    juce::Rectangle<int> PianoRollView::rulerArea() const
    {
        auto r = getLocalBounds();
        r.removeFromTop (compact ? 0 : toolbarHeight);
        r.removeFromRight (scrollThickness);
        r.removeFromLeft (keyboardWidth);
        return r.removeFromTop (rulerHeight);
    }

    juce::Rectangle<int> PianoRollView::keyboardArea() const
    {
        auto r = getLocalBounds();
        r.removeFromTop ((compact ? 0 : toolbarHeight) + rulerHeight);
        r.removeFromBottom (velocityHeight + scrollThickness);
        return r.removeFromLeft (keyboardWidth);
    }

    juce::Rectangle<int> PianoRollView::noteArea() const
    {
        auto r = getLocalBounds();
        r.removeFromTop ((compact ? 0 : toolbarHeight) + rulerHeight);
        r.removeFromBottom (velocityHeight + scrollThickness);
        r.removeFromLeft (keyboardWidth);
        r.removeFromRight (scrollThickness);
        return r;
    }

    juce::Rectangle<int> PianoRollView::velocityArea() const
    {
        auto r = getLocalBounds();
        r.removeFromBottom (scrollThickness);
        r.removeFromLeft (keyboardWidth);
        r.removeFromRight (scrollThickness);
        return r.removeFromBottom (velocityHeight);
    }

    double PianoRollView::beatForX (int x) const
    {
        return scrollBeats + (x - noteArea().getX()) / juce::jmax (1.0, pixelsPerBeat);
    }

    float PianoRollView::xForBeat (double beat) const
    {
        return (float) (noteArea().getX() + (beat - scrollBeats) * pixelsPerBeat);
    }

    int PianoRollView::pitchForY (int y) const
    {
        const auto area = noteArea();
        return juce::jlimit (0, 127, lowestVisiblePitch
                                       + (int) std::floor ((area.getBottom() - y) / keyHeight));
    }

    float PianoRollView::yForPitch (int pitch) const
    {
        const auto area = noteArea();
        return (float) area.getBottom() - (float) ((pitch - lowestVisiblePitch + 1) * keyHeight);
    }

    juce::Rectangle<float> PianoRollView::rectForNote (const Note& n, double clipStart) const
    {
        const float x1 = xForBeat (clipStart + n.startBeats);
        const float x2 = xForBeat (clipStart + n.endBeats());
        return { x1, yForPitch (n.pitch), juce::jmax (3.0f, x2 - x1), (float) keyHeight };
    }

    int PianoRollView::noteIndexAt (juce::Point<int> p) const
    {
        auto* clip = currentClip();
        if (clip == nullptr) return -1;

        // Back to front so the topmost (last drawn) note wins.
        for (int i = (int) clip->notes.size(); --i >= 0;)
            if (rectForNote (clip->notes[(size_t) i], clip->startBeats).contains (p.toFloat()))
                return i;

        return -1;
    }

    //==============================================================================
    void PianoRollView::updateScrollRanges()
    {
        const auto area = noteArea();
        if (area.isEmpty())
            return;

        const double visibleBeats = area.getWidth() / juce::jmax (1.0, pixelsPerBeat);
        double contentEnd = 32.0;
        if (ctx.project != nullptr)
            contentEnd = juce::jmax (contentEnd, project().endBeats() + 8.0);
        if (auto* clip = currentClip())
            contentEnd = juce::jmax (contentEnd, clip->endBeats() + 4.0);

        hScroll.setRangeLimits (0.0, juce::jmax (contentEnd, scrollBeats + visibleBeats), juce::dontSendNotification);
        hScroll.setCurrentRange (scrollBeats, visibleBeats, juce::dontSendNotification);

        const double visiblePitches = juce::jmax (1.0, area.getHeight() / keyHeight);
        const double topValue = 128.0 - (lowestVisiblePitch + visiblePitches);
        vScroll.setRangeLimits (0.0, 128.0, juce::dontSendNotification);
        vScroll.setCurrentRange (juce::jlimit (0.0, 128.0 - visiblePitches, topValue),
                                 visiblePitches, juce::dontSendNotification);
    }

    void PianoRollView::scrollBarMoved (juce::ScrollBar* bar, double newRangeStart)
    {
        if (bar == &hScroll)
        {
            scrollBeats = juce::jmax (0.0, newRangeStart);
        }
        else
        {
            const double visiblePitches = juce::jmax (1.0, noteArea().getHeight() / keyHeight);
            lowestVisiblePitch = juce::jlimit (0, 127, (int) std::round (128.0 - newRangeStart - visiblePitches));
        }

        repaint();
    }

    void PianoRollView::timerCallback()
    {
        // Only the playhead moves at this rate; repaint the strip it lives in.
        if (ctx.engine != nullptr && ctx.engine->getTransport().isPlaying())
            repaint (noteArea().withTop (rulerArea().getY()));
    }

    //==============================================================================
    void PianoRollView::paintToolbarBackground (juce::Graphics& g)
    {
        const auto& p = palette();
        auto area = toolbarArea();
        g.setColour (p.panelBg);
        g.fillRect (area);
        g.setColour (p.divider);
        g.drawHorizontalLine (area.getBottom() - 1, (float) area.getX(), (float) area.getRight());
    }

    void PianoRollView::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        g.fillAll (p.windowBg);

        if (ctx.project == nullptr)
            return;

        if (! compact)
            paintToolbarBackground (g);

        const auto notes  = noteArea();
        const auto keys   = keyboardArea();
        const auto vel    = velocityArea();
        auto* clip = currentClip();

        // --- pitch lanes, with the current scale highlighted (spec 8.3) -------
        g.setColour (p.laneBg);
        g.fillRect (notes);

        const int topPitch = pitchForY (notes.getY());
        for (int pitch = lowestVisiblePitch; pitch <= topPitch + 1; ++pitch)
        {
            const float y = yForPitch (pitch);
            juce::Rectangle<float> row ((float) notes.getX(), y, (float) notes.getWidth(), (float) keyHeight);

            const bool inScale = theory::isInScale (pitch, ui.editKey);
            const bool isTonic = (((pitch - ui.editKey.tonic) % 12) + 12) % 12 == 0;

            if (isTonic)        g.setColour (p.accent.withAlpha (0.12f));
            else if (! inScale) g.setColour (p.isDark ? juce::Colours::black.withAlpha (0.22f)
                                                      : juce::Colours::black.withAlpha (0.05f));
            else if (isBlackKey (pitch)) g.setColour (p.laneAltBg.withAlpha (0.6f));
            else                continue;

            g.fillRect (row);
        }

        paintBeatGrid (g, notes, project().tempo, scrollBeats, pixelsPerBeat, ui.grid);

        // --- loop region ------------------------------------------------------
        if (ctx.project != nullptr && project().loopEnabled)
        {
            const float x1 = xForBeat (project().loopStartBeats);
            const float x2 = xForBeat (project().loopEndBeats);
            g.setColour (p.loopRegion);
            g.fillRect (juce::Rectangle<float> (x1, (float) notes.getY(),
                                                juce::jmax (0.0f, x2 - x1), (float) notes.getHeight()));
        }

        // --- everything outside the edited clip is dimmed ---------------------
        if (clip != nullptr)
        {
            g.setColour (p.windowBg.withAlpha (0.55f));
            const float left  = xForBeat (clip->startBeats);
            const float right = xForBeat (clip->endBeats());
            if (left > notes.getX())
                g.fillRect (juce::Rectangle<float> ((float) notes.getX(), (float) notes.getY(),
                                                    left - notes.getX(), (float) notes.getHeight()));
            if (right < notes.getRight())
                g.fillRect (juce::Rectangle<float> (right, (float) notes.getY(),
                                                    notes.getRight() - right, (float) notes.getHeight()));
        }

        // --- ghosted notes from the other MIDI tracks (spec 8.3) --------------
        if (overlayOtherTracks && ctx.project != nullptr)
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (notes);
            g.setColour (p.noteGhost);

            for (const auto& track : project().getTracks())
            {
                if (track->getType() != TrackType::midi || track->getId() == ui.selectedTrack)
                    continue;

                for (const auto& other : track->midiClips)
                    for (const auto& n : other.notes)
                        g.fillRect (rectForNote (n, other.startBeats).reduced (0.0f, 1.0f));
            }
        }

        // --- chord hints ------------------------------------------------------
        if (! chordHints.empty())
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (notes.withHeight (chordStripHeight));
            g.setColour (p.panelAltBg.withAlpha (0.85f));
            g.fillRect (notes.withHeight (chordStripHeight));
            g.setFont (juce::Font (juce::FontOptions (12.0f)));

            for (const auto& chord : chordHints)
            {
                const float x = xForBeat (chord.beat);
                const float w = (float) (chord.lengthBeats * pixelsPerBeat);
                if (x + w < notes.getX() || x > notes.getRight()) continue;

                g.setColour (p.outline);
                g.drawVerticalLine (juce::roundToInt (x), (float) notes.getY(),
                                    (float) notes.getY() + chordStripHeight);
                g.setColour (p.textDim);
                g.drawText (chord.symbol, juce::roundToInt (x) + 4, notes.getY(),
                            juce::jmax (20, juce::roundToInt (w) - 6), chordStripHeight,
                            juce::Justification::centredLeft, false);
            }
        }

        // --- the notes themselves --------------------------------------------
        if (clip != nullptr)
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (notes);

            for (int i = 0; i < (int) clip->notes.size(); ++i)
            {
                const auto& n = clip->notes[(size_t) i];
                auto r = rectForNote (n, clip->startBeats);
                if (r.getRight() < notes.getX() || r.getX() > notes.getRight()
                     || r.getBottom() < notes.getY() || r.getY() > notes.getBottom())
                    continue;

                const bool isSelected = std::find (selection.begin(), selection.end(), i) != selection.end();

                // Hand-entered notes stay on-brand; transcribed ones are shaded
                // by confidence (spec 8.2-5 / 9.3).
                auto fill = n.confidence >= 0.999f ? p.note : confidenceColour (n.confidence);
                fill = fill.withMultipliedAlpha (juce::jmap ((float) n.velocity, 1.0f, 127.0f, 0.55f, 1.0f));

                g.setColour (isSelected ? p.noteSelected : fill);
                g.fillRoundedRectangle (r.reduced (0.0f, 0.5f), 2.0f);

                paintConfidenceHatch (g, r.reduced (0.0f, 0.5f), n.confidence, ui.confidenceThreshold);

                g.setColour (isSelected ? p.textBright : p.windowBg.withAlpha (0.7f));
                g.drawRoundedRectangle (r.reduced (0.5f, 1.0f), 2.0f, 1.0f);

                if (i == reviewCursor)
                {
                    g.setColour (p.warning);
                    g.drawRoundedRectangle (r.expanded (2.0f, 2.0f), 3.0f, 2.0f);
                }
            }
        }

        if (! marquee.isEmpty())
        {
            g.setColour (p.accent.withAlpha (0.2f));
            g.fillRect (marquee);
            g.setColour (p.accent);
            g.drawRect (marquee, 1);
        }

        // --- keyboard ---------------------------------------------------------
        g.setColour (p.panelAltBg);
        g.fillRect (keys);
        g.setFont (juce::Font (juce::FontOptions (9.5f)));

        for (int pitch = lowestVisiblePitch; pitch <= topPitch + 1; ++pitch)
        {
            const float y = yForPitch (pitch);
            juce::Rectangle<float> key ((float) keys.getX(), y, (float) keys.getWidth(), (float) keyHeight);

            g.setColour (isBlackKey (pitch) ? (p.isDark ? juce::Colour (0xff15171c) : juce::Colour (0xff32363d))
                                            : (p.isDark ? juce::Colour (0xffcfd4dc) : juce::Colours::white));
            g.fillRect (key.reduced (0.0f, 0.5f));

            if (pitch % 12 == 0)
            {
                g.setColour (juce::Colour (0xff2b2f36));
                g.drawText (theory::midiNoteName (pitch), key.reduced (3.0f, 0.0f).toNearestInt(),
                            juce::Justification::centredRight, false);
            }
        }

        g.setColour (p.outline);
        g.drawVerticalLine (keys.getRight() - 1, (float) keys.getY(), (float) keys.getBottom());

        // --- ruler ------------------------------------------------------------
        paintBeatRuler (g, rulerArea(), project().tempo, scrollBeats, pixelsPerBeat);

        // --- velocity lane ----------------------------------------------------
        g.setColour (p.panelBg);
        g.fillRect (vel);
        g.setColour (p.divider);
        g.drawHorizontalLine (vel.getY(), (float) vel.getX(), (float) vel.getRight());
        g.setColour (p.textDim);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (TRANS ("Velocity"), vel.getX() + 4, vel.getY() + 2, 90, 12,
                    juce::Justification::topLeft, false);

        if (clip != nullptr)
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (vel);

            for (int i = 0; i < (int) clip->notes.size(); ++i)
            {
                const auto& n = clip->notes[(size_t) i];
                const float x = xForBeat (clip->startBeats + n.startBeats);
                if (x < vel.getX() - 4 || x > vel.getRight()) continue;

                const float h = (float) vel.getHeight() * (n.velocity / 127.0f);
                const bool isSelected = std::find (selection.begin(), selection.end(), i) != selection.end();
                g.setColour (isSelected ? p.noteSelected : p.accent);
                g.fillRect (juce::Rectangle<float> (x, vel.getBottom() - h, 3.0f, h));
            }
        }

        // --- playhead ---------------------------------------------------------
        if (ctx.engine != nullptr)
        {
            const float x = xForBeat (ctx.engine->getTransport().getPositionBeats());
            if (x >= notes.getX() && x <= notes.getRight())
            {
                g.setColour (p.playhead);
                g.drawVerticalLine (juce::roundToInt (x), (float) rulerArea().getY(), (float) vel.getBottom());
            }
        }

        if (clip == nullptr)
        {
            g.setColour (p.textDim);
            g.setFont (juce::Font (juce::FontOptions (15.0f)));
            g.drawText (TRANS ("Select a MIDI clip in the timeline to edit it here"),
                        notes, juce::Justification::centred, false);
        }
    }

    void PianoRollView::resized()
    {
        if (! compact)
        {
            auto bar = toolbarArea().reduced (6, 4);
            titleLabel.setBounds (bar.removeFromLeft (86));
            keyRootBox.setBounds (bar.removeFromLeft (66).reduced (2, 0));
            keyScaleBox.setBounds (bar.removeFromLeft (116).reduced (2, 0));
            bar.removeFromLeft (6);
            quantiseBox.setBounds (bar.removeFromLeft (76).reduced (2, 0));
            quantiseStrength.setBounds (bar.removeFromLeft (120).reduced (2, 0));
            quantiseButton.setBounds (bar.removeFromLeft (78).reduced (2, 0));
            bar.removeFromLeft (4);
            humaniseButton.setBounds (bar.removeFromLeft (84).reduced (2, 0));
            snapToScaleButton.setBounds (bar.removeFromLeft (98).reduced (2, 0));
            bar.removeFromLeft (6);
            snapToggle.setBounds (bar.removeFromLeft (66));
            overlayToggle.setBounds (bar.removeFromLeft (126));

            fixButton.setBounds (bar.removeFromRight (66).reduced (2, 0));
            nextLowConfidenceButton.setBounds (bar.removeFromRight (150).reduced (2, 0));
            lowConfidenceLabel.setBounds (bar.removeFromRight (juce::jmax (0, juce::jmin (140, bar.getWidth()))));
        }

        auto r = getLocalBounds();
        hScroll.setBounds (r.getX() + keyboardWidth, r.getBottom() - scrollThickness,
                           juce::jmax (0, r.getWidth() - keyboardWidth - scrollThickness), scrollThickness);
        const auto notes = noteArea();
        vScroll.setBounds (notes.getRight(), notes.getY(), scrollThickness, notes.getHeight());

        updateScrollRanges();
    }

    //==============================================================================
    void PianoRollView::mouseMove (const juce::MouseEvent& e)
    {
        const int index = noteIndexAt (e.getPosition());
        if (index != hoverNote)
        {
            hoverNote = index;
            repaint();
        }

        auto* clip = currentClip();
        auto cursor = juce::MouseCursor::NormalCursor;
        if (clip != nullptr && index >= 0)
        {
            const auto r = rectForNote (clip->notes[(size_t) index], clip->startBeats);
            if (e.position.x > r.getRight() - edgeGrab || e.position.x < r.getX() + edgeGrab)
                cursor = juce::MouseCursor::LeftRightResizeCursor;
        }
        setMouseCursor (cursor);
    }

    void PianoRollView::mouseDown (const juce::MouseEvent& e)
    {
        grabKeyboardFocus();

        auto* clip = currentClip();
        if (clip == nullptr)
            return;

        const auto pos = e.getPosition();

        if (rulerArea().contains (pos))
        {
            if (ctx.engine != nullptr)
                ctx.engine->getTransport().setPositionBeats (juce::jmax (0.0, ui.snap (beatForX (pos.x))));
            return;
        }

        if (velocityArea().contains (pos))
        {
            notesBeforeDrag = clip->notes;
            dragMode  = DragMode::velocity;
            dragMoved = true;
            mouseDrag (e);
            return;
        }

        if (! noteArea().contains (pos))
            return;

        const int index = noteIndexAt (pos);

        if (e.mods.isPopupMenu())
        {
            if (index >= 0)
            {
                if (std::find (selection.begin(), selection.end(), index) == selection.end())
                    selection = { index };
                showNoteContextMenu (index, pos);
            }
            return;
        }

        if (index < 0)
        {
            if (! e.mods.isShiftDown())
                selection.clear();
            dragMode = DragMode::marquee;
            dragStartPoint = pos;
            marquee = {};
            repaint();
            return;
        }

        if (e.mods.isShiftDown())
        {
            if (std::find (selection.begin(), selection.end(), index) == selection.end())
                selection.push_back (index);
        }
        else if (std::find (selection.begin(), selection.end(), index) == selection.end())
        {
            selection = { index };
        }

        const auto r = rectForNote (clip->notes[(size_t) index], clip->startBeats);
        dragMode = e.position.x < r.getX() + edgeGrab      ? DragMode::resizeStart
                 : e.position.x > r.getRight() - edgeGrab  ? DragMode::resizeEnd
                                                           : DragMode::move;

        notesBeforeDrag = clip->notes;
        dragStartPoint  = pos;
        dragStartBeat   = beatForX (pos.x);
        dragStartPitch  = pitchForY (pos.y);
        dragMoved       = false;
        repaint();
    }

    void PianoRollView::mouseDrag (const juce::MouseEvent& e)
    {
        auto* clip = currentClip();
        if (clip == nullptr || dragMode == DragMode::none)
            return;

        if (dragMode == DragMode::marquee)
        {
            marquee = juce::Rectangle<int> (dragStartPoint, e.getPosition());
            selection.clear();
            for (int i = 0; i < (int) clip->notes.size(); ++i)
                if (rectForNote (clip->notes[(size_t) i], clip->startBeats)
                        .toNearestInt().intersects (marquee))
                    selection.push_back (i);
            repaint();
            return;
        }

        dragMoved = true;

        if (dragMode == DragMode::velocity)
        {
            const auto vel = velocityArea();
            const int  v   = juce::jlimit (1, 127,
                                           (int) std::round ((vel.getBottom() - e.position.y)
                                                              / juce::jmax (1, vel.getHeight()) * 127.0f));
            const double beatUnderMouse = beatForX (e.x) - clip->startBeats;

            for (int i = 0; i < (int) clip->notes.size(); ++i)
            {
                auto& n = clip->notes[(size_t) i];
                const bool inSelection = ! selection.empty()
                                       && std::find (selection.begin(), selection.end(), i) != selection.end();
                const bool underMouse  = selection.empty()
                                       && beatUnderMouse >= n.startBeats - 0.02 && beatUnderMouse <= n.endBeats();
                if (inSelection || underMouse)
                    n.velocity = v;
            }
            repaint();
            return;
        }

        if (selection.empty())
            return;

        const int primary = selection.front();
        if (primary >= (int) notesBeforeDrag.size())
            return;

        const double rawDelta  = beatForX (e.x) - dragStartBeat;
        const int    deltaPitch = pitchForY (e.y) - dragStartPitch;
        const double step      = juce::jmax (0.0625, quantiseStepInBeats (ui.grid));

        // Snap the note the gesture started on and move the rest by the same
        // offset - dragging a chord must not smear it across the grid.
        const double anchorAbs = clip->startBeats + notesBeforeDrag[(size_t) primary].startBeats;
        const double anchorEnd = clip->startBeats + notesBeforeDrag[(size_t) primary].endBeats();

        double deltaBeats = rawDelta;
        if (ui.snapEnabled)
        {
            const double reference = dragMode == DragMode::resizeEnd ? anchorEnd : anchorAbs;
            deltaBeats = ui.snap (reference + rawDelta) - reference;
        }

        for (int index : selection)
        {
            if (index < 0 || index >= (int) notesBeforeDrag.size())
                continue;

            const auto& orig = notesBeforeDrag[(size_t) index];
            auto& n = clip->notes[(size_t) index];
            n = orig;

            switch (dragMode)
            {
                case DragMode::move:
                    n.startBeats = juce::jmax (0.0, orig.startBeats + deltaBeats);
                    n.pitch      = juce::jlimit (0, 127, orig.pitch + deltaPitch);
                    break;

                case DragMode::resizeEnd:
                    n.lengthBeats = juce::jmax (step * 0.25, orig.lengthBeats + deltaBeats);
                    break;

                case DragMode::resizeStart:
                {
                    const double newStart = juce::jlimit (0.0, orig.endBeats() - step * 0.25,
                                                          orig.startBeats + deltaBeats);
                    n.lengthBeats = orig.endBeats() - newStart;
                    n.startBeats  = newStart;
                    break;
                }

                default: break;
            }
        }

        repaint();
    }

    void PianoRollView::mouseUp (const juce::MouseEvent&)
    {
        if (dragMode == DragMode::marquee)
        {
            marquee = {};
            dragMode = DragMode::none;
            repaint();
            return;
        }

        if (dragMode == DragMode::none)
            return;

        const juce::String name = dragMode == DragMode::velocity ? TRANS ("Edit velocity")
                                : dragMode == DragMode::move     ? TRANS ("Move notes")
                                                                 : TRANS ("Resize notes");
        commitDrag (name);
        dragMode = DragMode::none;
    }

    void PianoRollView::commitDrag (const juce::String& transactionName)
    {
        auto* clip = currentClip();
        if (clip == nullptr)
            return;

        if (! dragMoved)
            return;

        auto finalNotes = clip->notes;

        // Rewind so the undo snapshot is taken on the pre-drag document, then
        // re-apply the result as one undoable step.
        clip->notes = notesBeforeDrag;

        performProjectEdit (project(), transactionName, [this, finalNotes]
        {
            if (auto* c = currentClip())
            {
                c->notes = finalNotes;
                c->sortNotes();
            }
        });

        reselectByValue (finalNotes);
    }

    void PianoRollView::reselectByValue (const std::vector<Note>& wanted)
    {
        auto* clip = currentClip();
        if (clip == nullptr)
            return;

        std::vector<Note> picked;
        for (int i : selection)
            if (i >= 0 && i < (int) wanted.size())
                picked.push_back (wanted[(size_t) i]);

        selection.clear();
        for (const auto& target : picked)
            for (int i = 0; i < (int) clip->notes.size(); ++i)
            {
                const auto& n = clip->notes[(size_t) i];
                if (n.pitch == target.pitch && std::abs (n.startBeats - target.startBeats) < 1.0e-6)
                {
                    if (std::find (selection.begin(), selection.end(), i) == selection.end())
                        selection.push_back (i);
                    break;
                }
            }

        lastNoteCount = (int) clip->notes.size();
    }

    void PianoRollView::mouseDoubleClick (const juce::MouseEvent& e)
    {
        auto* clip = currentClip();
        if (clip == nullptr || ! noteArea().contains (e.getPosition()))
            return;

        const int index = noteIndexAt (e.getPosition());

        if (index >= 0)
        {
            selection = { index };
            deleteSelection();
            return;
        }

        const double step  = juce::jmax (0.25, quantiseStepInBeats (ui.grid));
        const double start = juce::jmax (0.0, ui.snap (beatForX (e.x)) - clip->startBeats);
        const int    pitch = pitchForY (e.y);

        performProjectEdit (project(), TRANS ("Add note"), [this, start, pitch, step]
        {
            if (auto* c = currentClip())
            {
                Note n;
                n.pitch = pitch;
                n.startBeats = start;
                n.lengthBeats = step;
                n.velocity = 100;
                c->notes.push_back (n);
                c->sortNotes();
                c->lengthBeats = juce::jmax (c->lengthBeats, start + step);
            }
        });
    }

    void PianoRollView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        if (e.mods.isCommandDown())
        {
            // Zoom around the mouse so the note under the cursor stays put.
            const double beatUnderMouse = beatForX (e.x);
            pixelsPerBeat = juce::jlimit (4.0, 600.0, pixelsPerBeat * (1.0 + wheel.deltaY * 0.6));
            scrollBeats   = juce::jmax (0.0, beatUnderMouse - (e.x - noteArea().getX()) / pixelsPerBeat);
        }
        else if (e.mods.isShiftDown())
        {
            scrollBeats = juce::jmax (0.0, scrollBeats - wheel.deltaY * 8.0);
        }
        else if (e.mods.isAltDown())
        {
            keyHeight = juce::jlimit (5.0, 30.0, keyHeight * (1.0 + wheel.deltaY * 0.5));
        }
        else
        {
            lowestVisiblePitch = juce::jlimit (0, 127, lowestVisiblePitch + (int) std::round (wheel.deltaY * -6.0f));
        }

        updateScrollRanges();
        repaint();
    }

    bool PianoRollView::keyPressed (const juce::KeyPress& key)
    {
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            deleteSelection();
            return true;
        }

        auto* clip = currentClip();
        if (clip == nullptr || selection.empty())
            return false;

        int   pitchDelta = 0;
        double beatDelta = 0.0;
        const double step = juce::jmax (0.0625, quantiseStepInBeats (ui.grid));

        if (key == juce::KeyPress::upKey)         pitchDelta = key.getModifiers().isShiftDown() ? 12 : 1;
        else if (key == juce::KeyPress::downKey)  pitchDelta = key.getModifiers().isShiftDown() ? -12 : -1;
        else if (key == juce::KeyPress::leftKey)  beatDelta = -step;
        else if (key == juce::KeyPress::rightKey) beatDelta = step;
        else return false;

        auto indices = selection;
        performProjectEdit (project(), TRANS ("Nudge notes"), [this, indices, pitchDelta, beatDelta]
        {
            if (auto* c = currentClip())
            {
                for (int i : indices)
                    if (i >= 0 && i < (int) c->notes.size())
                    {
                        auto& n = c->notes[(size_t) i];
                        n.pitch = juce::jlimit (0, 127, n.pitch + pitchDelta);
                        n.startBeats = juce::jmax (0.0, n.startBeats + beatDelta);
                    }
                c->sortNotes();
            }
        });

        return true;
    }

    void PianoRollView::deleteSelection()
    {
        if (currentClip() == nullptr || selection.empty())
            return;

        auto indices = selection;
        std::sort (indices.begin(), indices.end(), std::greater<int>());

        performProjectEdit (project(), TRANS ("Delete notes"), [this, indices]
        {
            if (auto* c = currentClip())
                for (int i : indices)
                    if (i >= 0 && i < (int) c->notes.size())
                        c->notes.erase (c->notes.begin() + i);
        });

        selection.clear();
    }

    //==============================================================================
    void PianoRollView::applyQuantiseToSelection (bool everything)
    {
        auto* clip = currentClip();
        if (clip == nullptr || ui.grid == Quantise::off)
            return;

        const auto grid     = ui.grid;
        const double amount = quantiseStrength.getValue();
        auto indices        = selection;

        performProjectEdit (project(), TRANS ("Quantise"), [this, everything, indices, grid, amount]
        {
            auto* c = currentClip();
            if (c == nullptr) return;

            auto quantiseOne = [grid, amount] (Note& n)
            {
                n.startBeats = juce::jmax (0.0, applyQuantise (n.startBeats, grid, amount));
            };

            if (everything)
                for (auto& n : c->notes) quantiseOne (n);
            else
                for (int i : indices)
                    if (i >= 0 && i < (int) c->notes.size()) quantiseOne (c->notes[(size_t) i]);

            c->sortNotes();
        });
    }

    //==============================================================================
    bool PianoRollView::jumpToNextLowConfidenceNote()
    {
        auto* clip = currentClip();
        if (clip == nullptr)
            return false;

        const int count = (int) clip->notes.size();
        for (int offset = 1; offset <= count; ++offset)
        {
            const int i = ((reviewCursor + offset) % juce::jmax (1, count) + count) % juce::jmax (1, count);
            if (clip->notes[(size_t) i].isLowConfidence (ui.confidenceThreshold))
            {
                reviewCursor = i;
                selectNoteAndScrollTo (i);
                return true;
            }
        }

        reviewCursor = -1;
        return false;
    }

    void PianoRollView::selectNoteAndScrollTo (int index)
    {
        auto* clip = currentClip();
        if (clip == nullptr || index < 0 || index >= (int) clip->notes.size())
            return;

        selection = { index };
        const auto& n = clip->notes[(size_t) index];

        const auto area = noteArea();
        const double visibleBeats = area.getWidth() / juce::jmax (1.0, pixelsPerBeat);
        const double abs = clip->startBeats + n.startBeats;
        if (abs < scrollBeats + visibleBeats * 0.15 || abs > scrollBeats + visibleBeats * 0.85)
            scrollBeats = juce::jmax (0.0, abs - visibleBeats * 0.35);

        const int visiblePitches = juce::jmax (1, (int) (area.getHeight() / keyHeight));
        if (n.pitch < lowestVisiblePitch + 2 || n.pitch > lowestVisiblePitch + visiblePitches - 2)
            lowestVisiblePitch = juce::jlimit (0, 127, n.pitch - visiblePitches / 2);

        updateScrollRanges();
        repaint();
    }

    void PianoRollView::showFixSuggestionsForSelection()
    {
        if (selection.empty())
        {
            if (! jumpToNextLowConfidenceNote())
                return;
        }

        showNoteContextMenu (selection.front(), noteArea().getCentre());
    }

    void PianoRollView::showNoteContextMenu (int noteIndex, juce::Point<int> where)
    {
        auto* clip = currentClip();
        if (clip == nullptr || noteIndex < 0 || noteIndex >= (int) clip->notes.size())
            return;

        juce::PopupMenu menu;
        menu.addSectionHeader (theory::midiNoteName (clip->notes[(size_t) noteIndex].pitch)
                               + "  " + juce::String (juce::roundToInt (
                                   clip->notes[(size_t) noteIndex].confidence * 100.0f)) + "%");

        // The one-click fixes the spec asks for, straight from the transcriber.
        std::vector<Transcriber::Suggestion> suggestions;
        if (ctx.transcriber != nullptr)
        {
            Transcriber::Result asResult;
            asResult.notes = clip->notes;
            asResult.key   = ui.editKey;
            suggestions = ctx.transcriber->suggestFixes (asResult, noteIndex);
        }

        for (int i = 0; i < (int) suggestions.size(); ++i)
            menu.addItem (1000 + i, suggestions[(size_t) i].label
                                      + "  (" + juce::String (suggestions[(size_t) i].score, 2) + ")");

        if (suggestions.empty())
            menu.addItem (99, TRANS ("No suggestions"), false);

        menu.addSeparator();
        menu.addItem (1, TRANS ("Snap to scale"));
        menu.addItem (2, TRANS ("Octave up"));
        menu.addItem (3, TRANS ("Octave down"));
        menu.addItem (4, TRANS ("Set confidence to 100%"));
        menu.addSeparator();
        menu.addItem (5, TRANS ("Delete"));

        const auto screenPos = localPointToGlobal (where);

        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetComponent (this)
                                .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                            [this, noteIndex, suggestions] (int result)
        {
            if (result <= 0) return;

            if (result >= 1000)
            {
                const int index = result - 1000;
                if (index < (int) suggestions.size())
                    applySuggestion (suggestions[(size_t) index]);
                return;
            }

            if (result == 5)
            {
                selection = { noteIndex };
                deleteSelection();
                return;
            }

            const auto key = ui.editKey;
            performProjectEdit (project(), TRANS ("Fix note"), [this, noteIndex, result, key]
            {
                auto* c = currentClip();
                if (c == nullptr || noteIndex >= (int) c->notes.size()) return;

                auto& n = c->notes[(size_t) noteIndex];
                if (result == 1) n.pitch = theory::snapToScale (n.pitch, key);
                if (result == 2) n.pitch = juce::jlimit (0, 127, n.pitch + 12);
                if (result == 3) n.pitch = juce::jlimit (0, 127, n.pitch - 12);
                n.confidence = 1.0f;
            });
        });
    }

    void PianoRollView::applySuggestion (const Transcriber::Suggestion& suggestion)
    {
        auto* clip = currentClip();

        if (clip == nullptr || suggestion.replacements.empty())
            return;

        const auto first = suggestion.firstIndex;
        const auto count = juce::jmax (0, suggestion.count);

        if (first < 0 || first + count > (int) clip->notes.size())
            return;                       // the clip moved on since the menu opened

        auto replacements = suggestion.replacements;

        for (auto& n : replacements)
            n.confidence = 1.0f;          // reviewed by a human now

        performProjectEdit (project(), TRANS ("Apply suggestion"), [this, first, count, replacements]
        {
            auto* c = currentClip();
            if (c == nullptr || first + count > (int) c->notes.size()) return;

            // A splice, not an overwrite: the replacement list can be longer or
            // shorter than what it replaces (split 1 -> 2, merge 2 -> 1).
            c->notes.erase (c->notes.begin() + first, c->notes.begin() + first + count);
            c->notes.insert (c->notes.begin() + first, replacements.begin(), replacements.end());
            c->sortNotes();
        });

        // Indices shifted under the selection; re-find the notes we just made.
        selection.clear();

        if (auto* c = currentClip())
            for (const auto& target : replacements)
                for (int i = 0; i < (int) c->notes.size(); ++i)
                    if (c->notes[(size_t) i].pitch == target.pitch
                         && std::abs (c->notes[(size_t) i].startBeats - target.startBeats) < 1.0e-6)
                    {
                        selection.push_back (i);
                        break;
                    }

        lastNoteCount = currentClip() != nullptr ? (int) currentClip()->notes.size() : -1;
        repaint();
    }
}
