#include "UI/GenerateView.h"
#include "Engine/AudioEngine.h"
#include <algorithm>
#include <cmath>

namespace ss
{
    namespace
    {
        constexpr int panelWidth   = 320;
        constexpr int galleryHeight = 186;
        constexpr int cardWidth    = 218;
        constexpr int rowHeight    = 24;

        const Generator::Part allParts[] =
        {
            Generator::Part::drums, Generator::Part::bass, Generator::Part::chords,
            Generator::Part::pad, Generator::Part::arpeggio, Generator::Part::counterMelody,
            Generator::Part::strings, Generator::Part::lead
        };

        const Generator::Mode allModes[] =
        {
            Generator::Mode::autoArrange, Generator::Mode::continuation, Generator::Mode::styleTransfer,
            Generator::Mode::harmonise, Generator::Mode::humanise, Generator::Mode::accompaniment,
            Generator::Mode::drumPattern
        };

        const char* moodTagKeys[] = { "bright", "melancholic", "driving", "dreamy", "aggressive" };

        /** Mini piano roll used by the candidate thumbnails. */
        void paintMiniRoll (juce::Graphics& g, juce::Rectangle<float> area,
                            const std::vector<Note>& notes, double lengthBeats, juce::Colour colour)
        {
            if (notes.empty() || lengthBeats <= 0.0)
                return;

            int lowest = 127, highest = 0;
            for (const auto& n : notes)
            {
                lowest  = juce::jmin (lowest, n.pitch);
                highest = juce::jmax (highest, n.pitch);
            }
            const int span = juce::jmax (8, highest - lowest + 1);
            const float noteHeight = juce::jmax (1.5f, area.getHeight() / (float) span);

            for (const auto& n : notes)
            {
                const float x = area.getX() + (float) (n.startBeats / lengthBeats) * area.getWidth();
                const float w = juce::jmax (1.5f, (float) (n.lengthBeats / lengthBeats) * area.getWidth());
                const float y = area.getBottom() - (float) (n.pitch - lowest + 1) * noteHeight;
                g.setColour (colour.withMultipliedAlpha (juce::jmap ((float) n.velocity, 1.0f, 127.0f, 0.5f, 1.0f)));
                g.fillRect (juce::Rectangle<float> (x, y, w, noteHeight));
            }
        }
    }

    //==============================================================================
    CandidateCard::CandidateCard (GenerateView& o, int i)
        : owner (o), index (i)
    {
        adoptButton.setButtonText (TRANS ("Adopt"));
        adoptButton.setTooltip (TRANS ("Add this candidate to the project"));
        adoptButton.onClick = [this] { owner.adoptCandidate (index); };
        addAndMakeVisible (adoptButton);

        rerollButton.setButtonText (TRANS ("Re-roll"));
        rerollButton.setTooltip (TRANS ("Regenerate one part, leaving the rest alone"));
        rerollButton.onClick = [this] { owner.showRerollMenu (index, &rerollButton); };
        addAndMakeVisible (rerollButton);

        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    juce::String CandidateCard::getTooltip()
    {
        const auto* candidate = owner.candidateAt (index);
        return candidate != nullptr ? candidate->notes : juce::String();
    }

    void CandidateCard::setSelected (bool s)
    {
        if (selected == s) return;
        selected = s;
        repaint();
    }

    void CandidateCard::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        auto area = getLocalBounds().toFloat().reduced (2.0f);

        g.setColour (selected ? p.panelAltBg : p.panelBg);
        g.fillRoundedRectangle (area, 4.0f);
        g.setColour (selected ? p.accent : p.outline);
        g.drawRoundedRectangle (area, 4.0f, selected ? 2.0f : 1.0f);

        const auto* candidate = owner.candidateAt (index);
        if (candidate == nullptr)
            return;

        g.setColour (p.text);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (candidate->name.isNotEmpty() ? candidate->name
                                                 : TRANS ("Candidate") + " " + juce::String (index + 1),
                    area.reduced (8.0f, 4.0f).withHeight (16.0f).toNearestInt(),
                    juce::Justification::centredLeft, false);

        g.setColour (p.textDim);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (TRANS ("Novelty") + " " + juce::String (juce::roundToInt (candidate->noveltyScore * 100.0f)) + "%",
                    area.reduced (8.0f, 4.0f).withHeight (16.0f).toNearestInt(),
                    juce::Justification::centredRight, false);

        // Spec 8.1 wants the accuracy caveat visible, so the notes get their own
        // strip under the thumbnail; the tooltip carries the rest.
        constexpr float notesHeight = 26.0f;

        auto notesArea = area.reduced (8.0f, 4.0f).withTrimmedBottom (28.0f)
                             .removeFromBottom (notesHeight);

        g.setColour (p.textDim);
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        g.drawFittedText (candidate->notes, notesArea.toNearestInt(),
                          juce::Justification::topLeft, 2, 0.9f);

        // One lane per generated part, so the thumbnail says what is in it.
        auto rollArea = area.reduced (8.0f, 4.0f).withTrimmedTop (20.0f)
                            .withTrimmedBottom (28.0f + notesHeight);
        g.setColour (p.laneBg);
        g.fillRect (rollArea);

        const int numParts = juce::jmax (1, (int) candidate->parts.size());
        const float laneHeight = rollArea.getHeight() / (float) numParts;

        for (int i = 0; i < (int) candidate->parts.size(); ++i)
        {
            auto lane = rollArea.withHeight (laneHeight).withY (rollArea.getY() + i * laneHeight);
            const auto& clip = candidate->parts[(size_t) i].second;

            g.setColour (p.divider);
            g.drawHorizontalLine (juce::roundToInt (lane.getBottom()), lane.getX(), lane.getRight());

            paintMiniRoll (g, lane.reduced (1.0f), clip.notes,
                           juce::jmax (1.0, clip.lengthBeats), p.note);

            g.setColour (p.textDim);
            g.setFont (juce::Font (juce::FontOptions (9.0f)));
            g.drawText (Generator::toString (candidate->parts[(size_t) i].first),
                        lane.reduced (2.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, false);
        }
    }

    void CandidateCard::resized()
    {
        auto buttons = getLocalBounds().reduced (8, 6).removeFromBottom (22);
        adoptButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 2));
        buttons.removeFromLeft (4);
        rerollButton.setBounds (buttons);
    }

    void CandidateCard::mouseDown (const juce::MouseEvent&)
    {
        owner.selectCandidate (index);
        owner.auditionCandidate (index);
    }

    void CandidateCard::mouseDrag (const juce::MouseEvent& e)
    {
        if (e.getDistanceFromDragStart() < 8)
            return;

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            // The note list is too big for a juce::var, so the payload is parked
            // on UiState and the description is just a tag.
            owner.getUiState().dragPayload = std::make_shared<MidiClip> (owner.mergeCandidate (index));
            container->startDragging ("ss.candidate", this);
        }
    }

    //==============================================================================
    GenerateView::GenerateView (AppContext& c, UiState& s)
        : ProjectView (c, s)
    {
        auto title = [this] (juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
            l.setColour (juce::Label::textColourId, palette().textBright);
            addAndMakeVisible (l);
        };
        title (sourceTitle, TRANS ("Source"));
        title (parametersTitle, TRANS ("Parameters"));
        title (galleryTitle, TRANS ("Candidates"));

        sourceInfo.setFont (juce::Font (juce::FontOptions (12.0f)));
        sourceInfo.setColour (juce::Label::textColourId, palette().textDim);
        addAndMakeVisible (sourceInfo);

        auto label = [this] (juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (12.0f)));
            l.setColour (juce::Label::textColourId, palette().textDim);
            addAndMakeVisible (l);
        };

        label (modeLabel, TRANS ("Mode"));
        for (int i = 0; i < (int) juce::numElementsInArray (allModes); ++i)
            modeBox.addItem (Generator::toString (allModes[i]), i + 1);
        modeBox.setSelectedId (1, juce::dontSendNotification);
        addAndMakeVisible (modeBox);

        label (genreLabel, TRANS ("Genre"));
        {
            const auto& genres = allGenres();
            for (int i = 0; i < (int) genres.size(); ++i)
                genreBox.addItem (toString (genres[(size_t) i]), i + 1);
            genreBox.setSelectedId (1, juce::dontSendNotification);
        }
        addAndMakeVisible (genreBox);

        label (keyLabel, TRANS ("Key"));
        fillKeyComboBoxes (keyRootBox, keyScaleBox, ui.editKey);
        addAndMakeVisible (keyRootBox);
        addAndMakeVisible (keyScaleBox);

        useDetectedKey.setButtonText (TRANS ("Use detected key"));
        useDetectedKey.setToggleState (true, juce::dontSendNotification);
        useDetectedKey.onClick = [this]
        {
            keyRootBox.setEnabled (! useDetectedKey.getToggleState());
            keyScaleBox.setEnabled (! useDetectedKey.getToggleState());
        };
        keyRootBox.setEnabled (false);
        keyScaleBox.setEnabled (false);
        addAndMakeVisible (useDetectedKey);

        auto zeroToOne = [this] (juce::Slider& sl, double initial, const juce::String& tip)
        {
            sl.setRange (0.0, 1.0, 0.01);
            sl.setValue (initial, juce::dontSendNotification);
            sl.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
            sl.setTooltip (tip);
            addAndMakeVisible (sl);
        };
        label (complexityLabel, TRANS ("Complexity"));
        zeroToOne (complexitySlider, 0.5, TRANS ("Density and syncopation"));
        label (creativityLabel, TRANS ("Creativity"));
        zeroToOne (creativitySlider, 0.5, TRANS ("How far the generator strays from the genre template"));
        label (timingLabel, TRANS ("Humanise timing"));
        zeroToOne (humaniseTiming, 0.0, TRANS ("Timing jitter"));
        label (velocityLabel, TRANS ("Humanise velocity"));
        zeroToOne (humaniseVelocity, 0.0, TRANS ("Velocity variation"));

        label (lengthLabel, TRANS ("Length"));
        lengthSlider.setRange (4.0, 256.0, 1.0);
        lengthSlider.setValue (32.0, juce::dontSendNotification);
        lengthSlider.setTextValueSuffix (" " + TRANS ("beats"));
        lengthSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 20);
        addAndMakeVisible (lengthSlider);

        label (candidatesLabel, TRANS ("Candidates"));
        candidateCount.setRange (1.0, 8.0, 1.0);
        candidateCount.setValue (4.0, juce::dontSendNotification);
        candidateCount.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
        addAndMakeVisible (candidateCount);

        label (moodLabel, TRANS ("Mood"));
        for (auto* key : moodTagKeys)
        {
            auto* b = moodButtons.add (new juce::ToggleButton (TRANS (key)));
            addAndMakeVisible (b);
        }
        moodEditor.setTextToShowWhenEmpty (TRANS ("extra tags, comma separated"), palette().textDim);
        moodEditor.setMultiLine (false);
        addAndMakeVisible (moodEditor);

        for (auto part : allParts)
        {
            auto* b = partButtons.add (new juce::ToggleButton (Generator::toString (part)));
            b->setToggleState (part == Generator::Part::drums || part == Generator::Part::bass
                                 || part == Generator::Part::chords, juce::dontSendNotification);
            addAndMakeVisible (b);
        }

        generateButton.setButtonText (TRANS ("Generate"));
        generateButton.setColour (juce::TextButton::buttonColourId, palette().accentDim);
        generateButton.onClick = [this] { startGeneration(); };
        addAndMakeVisible (generateButton);

        abButton.setButtonText (TRANS ("A/B against original"));
        abButton.setTooltip (TRANS ("Alternate between the source material and the selected candidate"));
        abButton.onClick = [this] { toggleAB(); };
        addAndMakeVisible (abButton);

        stopButton.setButtonText (TRANS ("Stop preview"));
        stopButton.onClick = [this] { if (ctx.engine != nullptr) ctx.engine->stopPreview(); };
        addAndMakeVisible (stopButton);

        sendToSessionButton.setButtonText (TRANS ("Send to Session"));
        sendToSessionButton.onClick = [this] { sendCandidatesToSession(); };
        addAndMakeVisible (sendToSessionButton);

        useChordTrackButton.setButtonText (TRANS ("Use chord track"));
        useChordTrackButton.setClickingTogglesState (true);
        useChordTrackButton.setTooltip (TRANS ("Generate from the project's chord progression instead of the "
                                               "selected clip"));
        useChordTrackButton.onClick = [this]
        {
            useChordTrack = useChordTrackButton.getToggleState();
            repaint();
            changeListenerCallback (nullptr);
        };
        addAndMakeVisible (useChordTrackButton);

        gallery.setViewedComponent (&galleryHolder, false);
        gallery.setScrollBarsShown (false, true);
        addAndMakeVisible (gallery);
        addChildComponent (task);

        changeListenerCallback (nullptr);
    }

    GenerateView::~GenerateView()
    {
        if (ctx.engine != nullptr)
            ctx.engine->stopPreview();
    }

    //==============================================================================
    const MidiClip* GenerateView::sourceClip() const
    {
        if (ctx.project == nullptr || ui.selectedClipIsAudio)
            return nullptr;

        if (auto* t = project().findTrack (ui.selectedTrack))
            return t->findMidiClip (ui.selectedClip);

        return nullptr;
    }

    void GenerateView::changeListenerCallback (juce::ChangeBroadcaster*)
    {
        const auto* clip = sourceClip();

        if (useChordTrack)
        {
            sourceInfo.setText (ctx.project != nullptr && ! project().chords.empty()
                                  ? juce::String ((int) project().chords.size()) + " " + TRANS ("chords")
                                  : TRANS ("The chord track is empty"),
                                juce::dontSendNotification);
        }
        else
        {
            sourceInfo.setText (clip != nullptr
                                  ? (clip->name.isNotEmpty() ? clip->name : TRANS ("MIDI"))
                                        + "   " + juce::String ((int) clip->notes.size()) + " " + TRANS ("notes")
                                  : TRANS ("Select a MIDI clip in the timeline, or switch to the chord track"),
                                juce::dontSendNotification);
        }

        generateButton.setEnabled (ctx.generator != nullptr
                                     && (clip != nullptr
                                         || (useChordTrack && ctx.project != nullptr && ! project().chords.empty())));
        abButton.setEnabled (selectedCandidate >= 0 && clip != nullptr);
        repaint (sourceArea());
    }

    Generator::Input GenerateView::buildInput() const
    {
        Generator::Input input;

        if (ctx.project != nullptr)
        {
            input.bpm = project().tempo.bpmAt (0.0);
            input.timeSignature = project().tempo.timeSignatureAt (0.0);
            input.chords = project().chords;
        }

        if (auto* clip = sourceClip())
            input.melody = clip->notes;

        return input;
    }

    Generator::Options GenerateView::buildOptions() const
    {
        Generator::Options options;
        options.mode = allModes[juce::jlimit (0, (int) juce::numElementsInArray (allModes) - 1,
                                              modeBox.getSelectedId() - 1)];

        const auto& genres = allGenres();
        options.genre = genres[(size_t) juce::jlimit (0, (int) genres.size() - 1, genreBox.getSelectedId() - 1)];

        options.complexity       = complexitySlider.getValue();
        options.creativity       = creativitySlider.getValue();
        options.humanizeTiming   = humaniseTiming.getValue();
        options.humanizeVelocity = humaniseVelocity.getValue();
        options.lengthBeats      = lengthSlider.getValue();
        options.numCandidates    = (int) candidateCount.getValue();
        options.useDetectedKey   = useDetectedKey.getToggleState();
        options.key              = keyFromComboBoxes (keyRootBox, keyScaleBox);

        options.parts.clear();
        for (int i = 0; i < partButtons.size() && i < (int) juce::numElementsInArray (allParts); ++i)
            if (partButtons[i]->getToggleState())
                options.parts.push_back (allParts[i]);

        for (int i = 0; i < moodButtons.size(); ++i)
            if (moodButtons[i]->getToggleState())
                options.moodTags.add (moodTagKeys[i]);

        options.moodTags.addTokens (moodEditor.getText(), ",", "");
        options.moodTags.trim();
        options.moodTags.removeEmptyStrings();

        return options;
    }

    void GenerateView::startGeneration()
    {
        if (ctx.generator == nullptr)
            return;

        auto results = std::make_shared<std::vector<Generator::Candidate>>();
        const auto input = buildInput();
        const auto options = buildOptions();

        task.onCancel = nullptr;
        task.run (TRANS ("Generating..."), [this, results, input, options] (TaskPanel& t)
        {
            // Returning false stops the arranger at its next part boundary
            // rather than letting it run on for a result we are about to drop.
            *results = ctx.generator->generate (input, options,
                                                [&t] (float p)
                                                {
                                                    t.setProgress (p);
                                                    return ! t.isCancelled();
                                                });
        },
        [this, results]
        {
            if (task.isCancelled())
                return;

            candidates = *results;
            selectedCandidate = candidates.empty() ? -1 : 0;
            rebuildGallery();
        });
    }

    void GenerateView::rebuildGallery()
    {
        cards.clear();

        for (int i = 0; i < (int) candidates.size(); ++i)
        {
            auto* card = cards.add (new CandidateCard (*this, i));
            card->setSelected (i == selectedCandidate);
            galleryHolder.addAndMakeVisible (card);
        }

        resized();
        changeListenerCallback (nullptr);
    }

    const Generator::Candidate* GenerateView::candidateAt (int index) const
    {
        return index >= 0 && index < (int) candidates.size() ? &candidates[(size_t) index] : nullptr;
    }

    void GenerateView::selectCandidate (int index)
    {
        selectedCandidate = index;
        for (int i = 0; i < cards.size(); ++i)
            cards[i]->setSelected (i == index);

        abButton.setEnabled (index >= 0 && sourceClip() != nullptr);
    }

    MidiClip GenerateView::mergeCandidate (int index) const
    {
        MidiClip merged;
        const auto* candidate = candidateAt (index);
        if (candidate == nullptr)
            return merged;

        merged.name = candidate->name.isNotEmpty() ? candidate->name
                                                   : TRANS ("Candidate") + " " + juce::String (index + 1);

        for (const auto& part : candidate->parts)
        {
            merged.lengthBeats = juce::jmax (merged.lengthBeats, part.second.lengthBeats);
            for (const auto& n : part.second.notes)
                merged.notes.push_back (n);
        }

        merged.sortNotes();
        return merged;
    }

    void GenerateView::auditionCandidate (int index)
    {
        if (ctx.engine == nullptr || ctx.project == nullptr)
            return;

        // Route the preview through whichever MIDI track is current so it plays
        // on that track's instrument; fall back to the first MIDI track.
        TrackId target = ui.selectedTrack;
        if (auto* t = project().findTrack (target))
        {
            if (t->getType() != TrackType::midi)
                target = invalidTrackId;
        }
        else
        {
            target = invalidTrackId;
        }

        if (target == invalidTrackId)
            for (const auto& track : project().getTracks())
                if (track->getType() == TrackType::midi)
                {
                    target = track->getId();
                    break;
                }

        const auto clip = mergeCandidate (index);
        ctx.engine->previewMidiClip (target, clip);
        playingOriginal = false;
    }

    void GenerateView::toggleAB()
    {
        if (ctx.engine == nullptr)
            return;

        const auto* original = sourceClip();
        if (original == nullptr)
            return;

        playingOriginal = ! playingOriginal;
        abButton.setButtonText (playingOriginal ? TRANS ("Playing: original")
                                                : TRANS ("Playing: candidate"));

        if (playingOriginal)
            ctx.engine->previewMidiClip (ui.selectedTrack, *original);
        else if (selectedCandidate >= 0)
            ctx.engine->previewMidiClip (ui.selectedTrack, mergeCandidate (selectedCandidate));
    }

    void GenerateView::adoptCandidate (int index)
    {
        const auto* candidate = candidateAt (index);
        if (candidate == nullptr || ctx.project == nullptr)
            return;

        const auto parts = candidate->parts;
        const auto name  = candidate->name;
        const double at  = sourceClip() != nullptr ? sourceClip()->startBeats : 0.0;

        // One track per generated part - the spec's output is multi-track MIDI.
        performProjectEdit (project(), TRANS ("Adopt candidate"), [this, parts, name, at]
        {
            for (const auto& part : parts)
            {
                auto& track = project().addTrack (TrackType::midi,
                                                  Generator::toString (part.first)
                                                    + (name.isNotEmpty() ? " - " + name : juce::String()));
                auto clip = part.second;
                clip.id = project().nextClipId();
                clip.startBeats = at;
                if (clip.name.isEmpty())
                    clip.name = Generator::toString (part.first);
                track.midiClips.push_back (clip);
            }
        });

        ui.goTo (MainComponent::View::timeline);
    }

    void sendCandidatesToSession (Project& project, const std::vector<Generator::Candidate>& candidates)
    {
        std::map<Generator::Part, Track*> partTracks;   // one track per part, shared across every candidate in this batch

        for (const auto& candidate : candidates)
        {
            auto& scene = project.addScene (candidate.name);

            for (const auto& part : candidate.parts)
            {
                Track* track = nullptr;
                auto it = partTracks.find (part.first);

                if (it != partTracks.end())
                {
                    track = it->second;
                }
                else
                {
                    track = &project.addTrack (TrackType::midi, Generator::toString (part.first));
                    partTracks[part.first] = track;
                }

                SessionClip clip;
                clip.kind        = SessionClip::Kind::midi;
                clip.name        = part.second.name.isNotEmpty() ? part.second.name : Generator::toString (part.first);
                clip.lengthBeats = part.second.lengthBeats;
                clip.notes       = part.second.notes;
                track->setSessionClip (scene.id, clip);
            }
        }
    }

    void GenerateView::sendCandidatesToSession()
    {
        if (ctx.project == nullptr || candidates.empty())
            return;

        performProjectEdit (project(), TRANS ("Send candidates to Session"),
                            [this] { ss::sendCandidatesToSession (project(), candidates); });

        ui.goTo (MainComponent::View::session);
    }

    void GenerateView::showRerollMenu (int index, juce::Component* target)
    {
        const auto* candidate = candidateAt (index);
        if (candidate == nullptr || ctx.generator == nullptr)
            return;

        juce::PopupMenu menu;
        menu.addSectionHeader (TRANS ("Regenerate part"));
        for (int i = 0; i < (int) candidate->parts.size(); ++i)
            menu.addItem (i + 1, Generator::toString (candidate->parts[(size_t) i].first));

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
                            [this, index] (int result)
        {
            if (result <= 0) return;

            auto* candidate = index >= 0 && index < (int) candidates.size() ? &candidates[(size_t) index] : nullptr;
            if (candidate == nullptr || result - 1 >= (int) candidate->parts.size())
                return;

            const int partIndex = result - 1;
            const auto part = candidate->parts[(size_t) partIndex].first;
            const auto seed = juce::Random::getSystemRandom().nextInt64();
            const auto input = buildInput();
            const auto options = buildOptions();
            auto regenerated = std::make_shared<MidiClip>();

            // Off the message thread like every other generator call.
            task.onCancel = nullptr;
            task.run (TRANS ("Regenerating part..."),
                      [this, regenerated, input, options, part, seed] (TaskPanel&)
            {
                *regenerated = ctx.generator->regeneratePart (input, options, part, seed);
            },
                      [this, index, partIndex, regenerated]
            {
                if (task.isCancelled())
                    return;

                if (index < (int) candidates.size()
                     && partIndex < (int) candidates[(size_t) index].parts.size())
                    candidates[(size_t) index].parts[(size_t) partIndex].second = *regenerated;

                for (auto* card : cards)
                    card->repaint();
            });
        });
    }

    //==============================================================================
    juce::Rectangle<int> GenerateView::sourceArea() const
    {
        auto area = getLocalBounds();
        area.removeFromRight (panelWidth);
        area.removeFromBottom (galleryHeight);
        area.removeFromTop (26);
        return area.reduced (10, 6);
    }

    void GenerateView::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        g.fillAll (p.windowBg);

        auto panel = getLocalBounds().removeFromRight (panelWidth);
        g.setColour (p.panelBg);
        g.fillRect (panel);
        g.setColour (p.divider);
        g.drawVerticalLine (panel.getX(), 0.0f, (float) getHeight());

        auto galleryStrip = getLocalBounds().removeFromBottom (galleryHeight);
        galleryStrip.removeFromRight (panelWidth);
        g.setColour (p.panelBg);
        g.fillRect (galleryStrip);
        g.setColour (p.divider);
        g.drawHorizontalLine (galleryStrip.getY(), 0.0f, (float) (getWidth() - panelWidth));

        // --- source preview ---------------------------------------------------
        const auto area = sourceArea();
        g.setColour (p.laneBg);
        g.fillRect (area);
        g.setColour (p.outline);
        g.drawRect (area, 1);

        if (useChordTrack)
        {
            if (ctx.project == nullptr || project().chords.empty())
            {
                g.setColour (p.textDim);
                g.drawText (TRANS ("The chord track is empty"), area, juce::Justification::centred, false);
                return;
            }

            double total = 0.0;
            for (const auto& chord : project().chords)
                total = juce::jmax (total, chord.beat + chord.lengthBeats);

            g.setFont (juce::Font (juce::FontOptions (14.0f)));
            for (const auto& chord : project().chords)
            {
                const float x = area.getX() + (float) (chord.beat / juce::jmax (1.0, total)) * area.getWidth();
                const float w = (float) (chord.lengthBeats / juce::jmax (1.0, total)) * area.getWidth();

                g.setColour (p.panelAltBg);
                g.fillRect (juce::Rectangle<float> (x + 1.0f, (float) area.getY() + 2.0f,
                                                    juce::jmax (2.0f, w - 2.0f), (float) area.getHeight() - 4.0f));
                g.setColour (p.text);
                g.drawText (chord.symbol, juce::Rectangle<float> (x, (float) area.getY(), w,
                                                                 (float) area.getHeight()).toNearestInt(),
                            juce::Justification::centred, false);
            }
            return;
        }

        if (const auto* clip = sourceClip())
        {
            paintMiniRoll (g, area.reduced (4).toFloat(), clip->notes,
                           juce::jmax (1.0, clip->lengthBeats), p.note);
        }
        else
        {
            g.setColour (p.textDim);
            g.drawText (TRANS ("Select a MIDI clip in the timeline, or switch to the chord track"),
                        area, juce::Justification::centred, false);
        }
    }

    void GenerateView::resized()
    {
        auto area = getLocalBounds();

        // --- parameter panel --------------------------------------------------
        auto panel = area.removeFromRight (panelWidth).reduced (12, 8);
        parametersTitle.setBounds (panel.removeFromTop (20));
        panel.removeFromTop (6);

        auto row = [&panel] (int h = rowHeight) { auto r = panel.removeFromTop (h); panel.removeFromTop (4); return r; };

        modeLabel.setBounds (row (18));
        modeBox.setBounds (row());
        genreLabel.setBounds (row (18));
        genreBox.setBounds (row());

        keyLabel.setBounds (row (18));
        auto keyRow = row();
        keyRootBox.setBounds (keyRow.removeFromLeft (72));
        keyRow.removeFromLeft (4);
        keyScaleBox.setBounds (keyRow.removeFromLeft (128));
        useDetectedKey.setBounds (row (20));

        complexityLabel.setBounds (row (18));
        complexitySlider.setBounds (row());
        creativityLabel.setBounds (row (18));
        creativitySlider.setBounds (row());
        timingLabel.setBounds (row (18));
        humaniseTiming.setBounds (row());
        velocityLabel.setBounds (row (18));
        humaniseVelocity.setBounds (row());
        lengthLabel.setBounds (row (18));
        lengthSlider.setBounds (row());
        candidatesLabel.setBounds (row (18));
        candidateCount.setBounds (row());

        moodLabel.setBounds (row (18));
        {
            auto moodRow = row (20);
            const int w = juce::jmax (40, moodRow.getWidth() / juce::jmax (1, moodButtons.size()));
            for (auto* b : moodButtons)
                b->setBounds (moodRow.removeFromLeft (w));
            moodEditor.setBounds (row (22));
        }

        // Instrumentation, two per row.
        for (int i = 0; i < partButtons.size(); i += 2)
        {
            auto partRow = row (20);
            partButtons[i]->setBounds (partRow.removeFromLeft (partRow.getWidth() / 2));
            if (i + 1 < partButtons.size())
                partButtons[i + 1]->setBounds (partRow);
        }

        auto actions = panel.removeFromBottom (126);
        useChordTrackButton.setBounds (actions.removeFromTop (24));
        actions.removeFromTop (4);
        abButton.setBounds (actions.removeFromTop (24));
        actions.removeFromTop (4);
        stopButton.setBounds (actions.removeFromTop (24));
        actions.removeFromTop (4);
        sendToSessionButton.setBounds (actions.removeFromTop (24));
        panel.removeFromBottom (6);
        generateButton.setBounds (panel.removeFromBottom (32));

        // --- gallery ----------------------------------------------------------
        auto galleryStrip = area.removeFromBottom (galleryHeight);
        galleryTitle.setBounds (galleryStrip.removeFromTop (22).reduced (10, 2));
        gallery.setBounds (galleryStrip.reduced (6, 2));
        galleryHolder.setBounds (0, 0, juce::jmax (gallery.getWidth(), cards.size() * cardWidth),
                                 juce::jmax (0, gallery.getHeight() - 12));
        for (int i = 0; i < cards.size(); ++i)
            cards[i]->setBounds (i * cardWidth, 0, cardWidth, galleryHolder.getHeight());

        // --- source header ----------------------------------------------------
        auto header = area.removeFromTop (26).reduced (10, 3);
        sourceTitle.setBounds (header.removeFromLeft (90));
        sourceInfo.setBounds (header);

        task.setBounds (getLocalBounds().withSizeKeepingCentre (460, 72));
    }
}
