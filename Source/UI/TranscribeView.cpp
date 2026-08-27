#include "UI/TranscribeView.h"
#include "Engine/AudioEngine.h"
#include "IO/FileIO.h"
#include <algorithm>
#include <cmath>

namespace ss
{
    namespace
    {
        constexpr int panelWidth  = 306;
        constexpr int rowHeight   = 24;
        constexpr int reviewHeight = 32;
    }

    TranscribeView::TranscribeView (AppContext& c, UiState& s)
        : ProjectView (c, s), pianoRoll (c, s)
    {
        sourceLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
        sourceLabel.setColour (juce::Label::textColourId, palette().text);
        addAndMakeVisible (sourceLabel);

        summaryLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        summaryLabel.setColour (juce::Label::textColourId, palette().textDim);
        addAndMakeVisible (summaryLabel);

        reviewLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        addAndMakeVisible (reviewLabel);

        stemTabs.addChangeListener (this);
        addAndMakeVisible (stemTabs);
        stemTabs.setVisible (false);

        // --- accuracy panel (spec 8.2 "精度チューニングUI") -------------------
        auto addLabel = [this] (juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (12.0f)));
            l.setColour (juce::Label::textColourId, palette().textDim);
            addAndMakeVisible (l);
        };

        addLabel (modeLabel, TRANS ("Material"));
        modeBox.addItem (TRANS ("Monophonic"), 1);
        modeBox.addItem (TRANS ("Polyphonic"), 2);
        modeBox.addItem (TRANS ("Drums"), 3);
        modeBox.addItem (TRANS ("Detect automatically"), 4);
        modeBox.setSelectedId (4, juce::dontSendNotification);
        addAndMakeVisible (modeBox);

        addLabel (sensitivityLabel, TRANS ("Sensitivity"));
        sensitivitySlider.setRange (0.0, 1.0, 0.01);
        sensitivitySlider.setValue (0.5, juce::dontSendNotification);
        sensitivitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
        sensitivitySlider.setTooltip (TRANS ("Higher finds more notes, and more false ones"));
        addAndMakeVisible (sensitivitySlider);

        addLabel (minLengthLabel, TRANS ("Min. note length"));
        minLengthSlider.setRange (10.0, 500.0, 1.0);
        minLengthSlider.setValue (60.0, juce::dontSendNotification);
        minLengthSlider.setTextValueSuffix (" ms");
        minLengthSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 20);
        addAndMakeVisible (minLengthSlider);

        addLabel (pitchRangeLabel, TRANS ("Pitch range"));
        pitchRangeSlider.setRange (0.0, 127.0, 1.0);
        pitchRangeSlider.setMinAndMaxValues (21.0, 108.0, juce::dontSendNotification);
        pitchRangeSlider.setTooltip (TRANS ("Notes outside this range are discarded"));
        pitchRangeSlider.onValueChange = [this]
        {
            pitchRangeLabel.setText (TRANS ("Pitch range") + "  "
                                       + theory::midiNoteName ((int) pitchRangeSlider.getMinValue())
                                       + " - " + theory::midiNoteName ((int) pitchRangeSlider.getMaxValue()),
                                     juce::dontSendNotification);
        };
        addAndMakeVisible (pitchRangeSlider);

        addLabel (quantiseLabel, TRANS ("Quantise"));
        fillQuantiseComboBox (quantiseBox, Quantise::sixteenth);
        addAndMakeVisible (quantiseBox);

        addLabel (strengthLabel, TRANS ("Quantise strength"));
        strengthSlider.setRange (0.0, 1.0, 0.01);
        strengthSlider.setValue (0.6, juce::dontSendNotification);
        strengthSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
        addAndMakeVisible (strengthSlider);

        addLabel (confidenceLabel, TRANS ("Confidence floor"));
        confidenceFloorSlider.setRange (0.0, 0.9, 0.01);
        confidenceFloorSlider.setValue (0.15, juce::dontSendNotification);
        confidenceFloorSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
        confidenceFloorSlider.setTooltip (TRANS ("Notes the transcriber is less sure of than this are dropped"));
        addAndMakeVisible (confidenceFloorSlider);

        addLabel (reviewThresholdLabel, TRANS ("Review below"));
        reviewThresholdSlider.setRange (0.0, 1.0, 0.01);
        reviewThresholdSlider.setValue (ui.confidenceThreshold, juce::dontSendNotification);
        reviewThresholdSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
        reviewThresholdSlider.onValueChange = [this]
        {
            ui.confidenceThreshold = (float) reviewThresholdSlider.getValue();
            ui.sendChangeMessage();
            refreshReviewRow();
        };
        addAndMakeVisible (reviewThresholdSlider);

        swingToggle.setButtonText (TRANS ("Detect swing"));
        swingToggle.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (swingToggle);

        tempoToggle.setButtonText (TRANS ("Detect tempo"));
        tempoToggle.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (tempoToggle);

        keyToggle.setButtonText (TRANS ("Detect key"));
        keyToggle.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (keyToggle);

        chooseFileButton.setButtonText (TRANS ("Choose audio file..."));
        chooseFileButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> (TRANS ("Choose audio file"), juce::File(),
                                                           io::getSupportedAudioExtensions().joinIntoString (";"));
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file.existsAsFile())
                {
                    stems.clear();
                    rebuildStemTabs();
                    setSource (file, 0.0, 0.0, file.getFileNameWithoutExtension());
                }
            });
        };
        addAndMakeVisible (chooseFileButton);

        transcribeButton.setButtonText (TRANS ("Transcribe"));
        transcribeButton.setColour (juce::TextButton::buttonColourId, palette().accentDim);
        transcribeButton.onClick = [this] { startTranscription(); };
        addAndMakeVisible (transcribeButton);

        separateButton.setButtonText (TRANS ("Separate stems"));
        separateButton.onClick = [this] { startStemSeparation(); };
        addAndMakeVisible (separateButton);

        applyTempoButton.setButtonText (TRANS ("Use detected tempo"));
        applyTempoButton.setEnabled (false);
        applyTempoButton.onClick = [this]
        {
            const double bpm = lastResult.estimatedBpm;
            if (bpm <= 1.0 || ctx.project == nullptr) return;

            performProjectEdit (project(), TRANS ("Use detected tempo"), [this, bpm]
            {
                auto events = project().tempo.getEvents();
                if (events.empty()) events.push_back ({ 0.0, bpm });
                else                events.front().bpm = bpm;
                project().tempo.setEvents (std::move (events));
            });
        };
        addAndMakeVisible (applyTempoButton);

        // --- bulk clean-ups ---------------------------------------------------
        auto addCleanup = [this] (juce::TextButton& b, const juce::String& text, int which)
        {
            b.setButtonText (text);
            b.onClick = [this, which] { applyBulkCleanup (which); };
            addAndMakeVisible (b);
        };
        addCleanup (removeLowConfidenceButton, TRANS ("Remove unsure notes"), 0);
        addCleanup (removeShortButton,         TRANS ("Remove very short notes"), 1);
        addCleanup (mergeRepeatsButton,        TRANS ("Merge repeated pitches"), 2);
        addCleanup (snapScaleButton,           TRANS ("Snap all to key"), 3);

        // --- review row -------------------------------------------------------
        previousReviewButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x80"))
                                              + " " + TRANS ("Previous"));
        previousReviewButton.onClick = [this]
        {
            // Walking backwards is "next" applied (count - 1) times; the
            // transcriber's own ordering is what the user sees.
            for (int i = 0; i < juce::jmax (0, pianoRoll.countLowConfidenceNotes() - 1); ++i)
                pianoRoll.jumpToNextLowConfidenceNote();
            refreshReviewRow();
        };
        addAndMakeVisible (previousReviewButton);

        nextReviewButton.setButtonText (TRANS ("Next low-confidence") + " "
                                          + juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6")));
        nextReviewButton.onClick = [this] { pianoRoll.jumpToNextLowConfidenceNote(); refreshReviewRow(); };
        addAndMakeVisible (nextReviewButton);

        fixButton.setButtonText (TRANS ("Fix..."));
        fixButton.onClick = [this] { pianoRoll.showFixSuggestionsForSelection(); };
        addAndMakeVisible (fixButton);

        pianoRoll.setCompact (true);
        addAndMakeVisible (pianoRoll);
        addChildComponent (task);

        setSource ({}, 0.0, 0.0, {});
        refreshReviewRow();
    }

    TranscribeView::~TranscribeView()
    {
        stemTabs.removeChangeListener (this);
        if (thumbnail != nullptr)
            thumbnail->removeChangeListener (this);
    }

    void TranscribeView::detachFromProject()
    {
        pianoRoll.detachFromProject();
        ProjectView::detachFromProject();
    }

    void TranscribeView::attachToProject()
    {
        ProjectView::attachToProject();
        pianoRoll.attachToProject();

        resultTrack  = invalidTrackId;
        resultClipId = invalidClipId;
        hasResult    = false;
    }

    //==============================================================================
    void TranscribeView::consumePendingRequest()
    {
        if (! ui.transcribeRequest.valid)
            return;

        const auto request = ui.transcribeRequest;
        ui.transcribeRequest = {};

        placeAtBeat = request.placeAtBeat;
        stems.clear();
        rebuildStemTabs();
        setSource (request.file, request.offsetSeconds, request.lengthSeconds, request.clipName);
    }

    void TranscribeView::setSource (const juce::File& file, double offsetSeconds, double lengthSeconds,
                                    const juce::String& displayName)
    {
        sourceFile          = file;
        sourceOffsetSeconds = offsetSeconds;
        sourceLengthSeconds = lengthSeconds;
        sourceName          = displayName;

        if (thumbnail != nullptr)
            thumbnail->removeChangeListener (this);
        thumbnail.reset();

        if (file.existsAsFile() && ctx.engine != nullptr)
        {
            thumbnail = std::make_unique<juce::AudioThumbnail> (256, ctx.engine->getFormatManager(), cache);
            thumbnail->addChangeListener (this);
            thumbnail->setSource (new juce::FileInputSource (file));
        }

        sourceLabel.setText (file.existsAsFile()
                               ? TRANS ("Source") + ": " + file.getFileName()
                                   + (sourceLengthSeconds > 0.0
                                        ? "   " + juce::String (sourceOffsetSeconds, 2) + " - "
                                              + juce::String (sourceOffsetSeconds + sourceLengthSeconds, 2) + " s"
                                        : juce::String())
                               : TRANS ("Right-click an audio clip in the timeline and choose "
                                        "\"Transcribe this range\", or choose a file here"),
                             juce::dontSendNotification);

        transcribeButton.setEnabled (file.existsAsFile());
        separateButton.setEnabled (file.existsAsFile()
                                     && ctx.stemSeparator != nullptr && ctx.stemSeparator->isAvailable());
        separateButton.setTooltip (ctx.stemSeparator != nullptr && ! ctx.stemSeparator->isAvailable()
                                     ? ctx.stemSeparator->getUnavailableReason()
                                     : TRANS ("Split the file into vocals / drums / bass / other"));
        repaint();
    }

    void TranscribeView::changeListenerCallback (juce::ChangeBroadcaster* source)
    {
        if (source == &stemTabs)
        {
            const int index = stemTabs.getCurrentTabIndex();
            selectedStem = index - 1;

            if (selectedStem >= 0 && selectedStem < (int) stems.size())
                setSource (stems[(size_t) selectedStem].file, 0.0, 0.0,
                           StemSeparator::toString (stems[(size_t) selectedStem].stem));
            return;
        }

        if (dynamic_cast<juce::AudioThumbnail*> (source) != nullptr)
        {
            repaint (waveArea());
            return;
        }

        refreshReviewRow();
        repaint();
    }

    //==============================================================================
    Transcriber::Options TranscribeView::currentOptions() const
    {
        Transcriber::Options o;
        o.mode = (Transcriber::Mode) juce::jlimit (0, 3, modeBox.getSelectedId() - 1);
        o.sensitivity      = sensitivitySlider.getValue();
        o.minNoteLengthMs  = minLengthSlider.getValue();
        o.minPitch         = (int) pitchRangeSlider.getMinValue();
        o.maxPitch         = (int) pitchRangeSlider.getMaxValue();
        o.quantise         = quantiseFromComboBox (quantiseBox);
        o.quantiseStrength = strengthSlider.getValue();
        o.detectSwing      = swingToggle.getToggleState();
        o.detectTempo      = tempoToggle.getToggleState();
        o.detectKey        = keyToggle.getToggleState();
        o.confidenceFloor  = (float) confidenceFloorSlider.getValue();
        return o;
    }

    void TranscribeView::startTranscription()
    {
        if (! sourceFile.existsAsFile() || ctx.transcriber == nullptr || ctx.engine == nullptr)
            return;

        task.onCancel = nullptr;   // the stem job may have left its own hook here

        auto result  = std::make_shared<Transcriber::Result>();
        const auto options = currentOptions();
        const auto file = sourceFile;
        const double offset = sourceOffsetSeconds, length = sourceLengthSeconds;

        task.run (TRANS ("Transcribing..."), [this, result, options, file, offset, length] (TaskPanel& t)
        {
            t.setStatus (TRANS ("Reading audio..."));

            juce::AudioBuffer<float> buffer;
            double sampleRate = 0.0;
            if (! io::readAudioFile (file, ctx.engine->getFormatManager(), buffer, sampleRate)
                 || sampleRate <= 0.0 || buffer.getNumSamples() == 0)
            {
                result->message = TRANS ("Could not read the audio file");
                return;
            }

            const int startSample = juce::jlimit (0, buffer.getNumSamples() - 1,
                                                  (int) (offset * sampleRate));
            const int available   = buffer.getNumSamples() - startSample;
            const int numSamples  = length > 0.0
                                      ? juce::jlimit (1, available, (int) (length * sampleRate))
                                      : available;

            juce::AudioBuffer<float> slice (buffer.getNumChannels(), numSamples);
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                slice.copyFrom (ch, 0, buffer, ch, startSample, numSamples);

            if (t.isCancelled())
                return;

            t.setStatus (TRANS ("Detecting notes..."));

            // Returning false aborts the analysis at its next checkpoint, so
            // Cancel stops the DSP instead of only discarding what it produces.
            *result = ctx.transcriber->transcribe (slice, sampleRate, options,
                                                   [&t] (float p)
                                                   {
                                                       t.setProgress (p);
                                                       return ! t.isCancelled();
                                                   });
        },
        [this, result]
        {
            if (! task.isCancelled() && ! result->cancelled)
                applyResult (*result);
        });
    }

    void TranscribeView::startStemSeparation()
    {
        if (ctx.stemSeparator == nullptr || ! sourceFile.existsAsFile() || ctx.project == nullptr)
            return;

        auto output = std::make_shared<std::vector<StemSeparator::Output>>();
        auto message = std::make_shared<juce::String>();
        const auto file = sourceFile;
        const auto folder = project().getMediaFolder().getChildFile ("Stems");
        folder.createDirectory();

        task.run (TRANS ("Separating stems..."), [this, output, message, file, folder] (TaskPanel& t)
        {
            const auto result = ctx.stemSeparator->separate (file, folder, *output,
                                                             [&t] (float p, const juce::String& s)
                                                             {
                                                                 t.setProgress (p);
                                                                 t.setStatus (s);
                                                             });
            if (result.failed())
                *message = result.getErrorMessage();
        },
        [this, output, message]
        {
            if (message->isNotEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                        TRANS ("Stem separation failed"), *message,
                                                        TRANS ("OK"), this);
                return;
            }

            stems = *output;
            rebuildStemTabs();
        });

        // Cancelling the panel has to reach the separator's child process too.
        task.onCancel = [this] { if (ctx.stemSeparator != nullptr) ctx.stemSeparator->cancel(); };
        task.setStatus (TRANS ("Starting..."));
    }

    void TranscribeView::rebuildStemTabs()
    {
        stemTabs.clearTabs();

        if (stems.empty())
        {
            stemTabs.setVisible (false);
            resized();
            return;
        }

        stemTabs.addTab (TRANS ("Original"), palette().panelAltBg, 0);
        for (int i = 0; i < (int) stems.size(); ++i)
            stemTabs.addTab (StemSeparator::toString (stems[(size_t) i].stem), palette().panelAltBg, i + 1);

        stemTabs.setCurrentTabIndex (0, false);
        stemTabs.setVisible (true);
        resized();
    }

    //==============================================================================
    void TranscribeView::applyResult (const Transcriber::Result& r)
    {
        lastResult = r;
        hasResult  = true;

        applyTempoButton.setEnabled (r.estimatedBpm > 1.0);

        summaryLabel.setText (r.notes.empty()
                                ? (r.message.isNotEmpty() ? r.message : TRANS ("No notes were detected"))
                                : juce::String (r.notes.size()) + " " + TRANS ("notes")
                                    + "   " + juce::String (r.estimatedBpm, 1) + " BPM"
                                    + "   " + theory::toString (r.key)
                                    + "   " + TRANS ("mean confidence") + " "
                                    + juce::String (juce::roundToInt (r.meanConfidence * 100.0f)) + "%"
                                    + (r.message.isNotEmpty() ? "   -   " + r.message : juce::String()),
                              juce::dontSendNotification);

        if (r.notes.empty() || ctx.project == nullptr)
            return;

        if (keyToggle.getToggleState())
            ui.editKey = r.key;

        double lastEnd = 0.0;
        for (const auto& n : r.notes)
            lastEnd = juce::jmax (lastEnd, n.endBeats());

        const auto name  = sourceName.isNotEmpty() ? sourceName : TRANS ("Transcribed");
        const auto notes = r.notes;
        const double at  = placeAtBeat;

        performProjectEdit (project(), TRANS ("Transcribe"), [this, notes, name, at, lastEnd]
        {
            Track* track = resultTrack != invalidTrackId ? project().findTrack (resultTrack) : nullptr;
            if (track == nullptr)
            {
                track = &project().addTrack (TrackType::midi, TRANS ("Transcribed") + ": " + name);
                resultTrack = track->getId();
                resultClipId = invalidClipId;
            }

            MidiClip* clip = resultClipId != invalidClipId ? track->findMidiClip (resultClipId) : nullptr;
            if (clip == nullptr)
            {
                MidiClip fresh;
                fresh.id   = project().nextClipId();
                fresh.name = name;
                track->midiClips.push_back (fresh);
                clip = &track->midiClips.back();
                resultClipId = clip->id;
            }

            clip->startBeats  = at;
            clip->lengthBeats = juce::jmax (4.0, std::ceil (lastEnd));
            clip->notes       = notes;
            clip->sortNotes();
        });

        ui.select (resultTrack, resultClipId, false);
        refreshReviewRow();
    }

    MidiClip* TranscribeView::resultClip() const
    {
        if (ctx.project == nullptr || resultTrack == invalidTrackId)
            return nullptr;

        if (auto* t = project().findTrack (resultTrack))
            return t->findMidiClip (resultClipId);

        return nullptr;
    }

    void TranscribeView::applyBulkCleanup (int which)
    {
        if (resultClip() == nullptr || ctx.project == nullptr)
            return;

        const float threshold = ui.confidenceThreshold;
        const double shortest = juce::jmax (0.03125, quantiseStepInBeats (quantiseFromComboBox (quantiseBox)) * 0.5);
        const auto key = ui.editKey;

        performProjectEdit (project(), TRANS ("Clean up notes"), [this, which, threshold, shortest, key]
        {
            auto* clip = resultClip();
            if (clip == nullptr) return;

            switch (which)
            {
                case 0: Transcriber::removeBelowConfidence (clip->notes, threshold); break;
                case 1: Transcriber::removeShorterThan (clip->notes, shortest); break;
                case 2: Transcriber::mergeRepeatedPitches (clip->notes, shortest); break;
                case 3: Transcriber::snapAllToScale (clip->notes, key); break;
                default: break;
            }

            clip->sortNotes();
        });

        refreshReviewRow();
    }

    void TranscribeView::refreshReviewRow()
    {
        const int low = pianoRoll.countLowConfidenceNotes();
        const bool any = low > 0;

        reviewLabel.setText (any ? juce::String (low) + " " + TRANS ("notes below the review threshold")
                                 : (hasResult ? TRANS ("Nothing left to review")
                                              : TRANS ("Transcribe a clip to review its notes")),
                             juce::dontSendNotification);
        reviewLabel.setColour (juce::Label::textColourId, any ? palette().warning : palette().textDim);

        previousReviewButton.setEnabled (any);
        nextReviewButton.setEnabled (any);
        fixButton.setEnabled (any);

        const bool haveClip = resultClip() != nullptr;
        removeLowConfidenceButton.setEnabled (haveClip);
        removeShortButton.setEnabled (haveClip);
        mergeRepeatsButton.setEnabled (haveClip);
        snapScaleButton.setEnabled (haveClip);
    }

    //==============================================================================
    juce::Rectangle<int> TranscribeView::waveArea() const
    {
        auto area = getLocalBounds();
        area.removeFromRight (panelWidth);
        area.removeFromTop (rowHeight + (stemTabs.isVisible() ? 26 : 0));
        area.removeFromBottom (reviewHeight + 18);
        return area.removeFromTop (juce::jmax (60, (int) (area.getHeight() * 0.38))).reduced (8, 4);
    }

    void TranscribeView::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        g.fillAll (p.windowBg);

        auto panel = getLocalBounds().removeFromRight (panelWidth);
        g.setColour (p.panelBg);
        g.fillRect (panel);
        g.setColour (p.divider);
        g.drawVerticalLine (panel.getX(), 0.0f, (float) getHeight());

        const auto wave = waveArea();
        g.setColour (p.laneBg);
        g.fillRect (wave);
        g.setColour (p.outline);
        g.drawRect (wave, 1);

        if (thumbnail != nullptr && thumbnail->getTotalLength() > 0.0)
        {
            const double start = sourceOffsetSeconds;
            const double end   = sourceLengthSeconds > 0.0 ? start + sourceLengthSeconds
                                                           : thumbnail->getTotalLength();
            g.setColour (p.waveform);
            thumbnail->drawChannels (g, wave.reduced (2), start, juce::jmax (start + 0.01, end), 0.95f);
        }
        else if (sourceFile.existsAsFile())
        {
            g.setColour (p.textDim);
            g.drawText (TRANS ("Loading..."), wave, juce::Justification::centred, false);
        }
        else
        {
            g.setColour (p.textDim);
            g.drawText (TRANS ("No source selected"), wave, juce::Justification::centred, false);
        }

        // Section heading over the accuracy panel.
        g.setColour (p.textBright);
        g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        g.drawText (TRANS ("Accuracy"), panel.getX() + 12, 8, panel.getWidth() - 24, 20,
                    juce::Justification::centredLeft, false);
    }

    void TranscribeView::resized()
    {
        auto area = getLocalBounds();

        // --- right-hand accuracy panel ---------------------------------------
        auto panel = area.removeFromRight (panelWidth).reduced (12, 8);
        panel.removeFromTop (24);

        auto row = [&panel] (int h = rowHeight) { auto r = panel.removeFromTop (h); panel.removeFromTop (4); return r; };

        modeLabel.setBounds (row (18));
        modeBox.setBounds (row());
        sensitivityLabel.setBounds (row (18));
        sensitivitySlider.setBounds (row());
        minLengthLabel.setBounds (row (18));
        minLengthSlider.setBounds (row());
        pitchRangeLabel.setBounds (row (18));
        pitchRangeSlider.setBounds (row());
        quantiseLabel.setBounds (row (18));
        quantiseBox.setBounds (row());
        strengthLabel.setBounds (row (18));
        strengthSlider.setBounds (row());
        confidenceLabel.setBounds (row (18));
        confidenceFloorSlider.setBounds (row());
        reviewThresholdLabel.setBounds (row (18));
        reviewThresholdSlider.setBounds (row());

        auto toggles = row (22);
        swingToggle.setBounds (toggles.removeFromLeft (100));
        tempoToggle.setBounds (toggles.removeFromLeft (100));
        keyToggle.setBounds (toggles);

        panel.removeFromTop (6);
        removeLowConfidenceButton.setBounds (row (22));
        removeShortButton.setBounds (row (22));
        mergeRepeatsButton.setBounds (row (22));
        snapScaleButton.setBounds (row (22));

        // Actions pinned to the bottom of the panel.
        auto actions = panel.removeFromBottom (96);
        chooseFileButton.setBounds (actions.removeFromTop (26));
        actions.removeFromTop (5);
        separateButton.setBounds (actions.removeFromTop (26));
        actions.removeFromTop (5);
        applyTempoButton.setBounds (actions.removeFromTop (26));
        panel.removeFromBottom (6);
        transcribeButton.setBounds (panel.removeFromBottom (32));

        // --- left-hand dual display ------------------------------------------
        auto top = area.removeFromTop (rowHeight).reduced (8, 2);
        summaryLabel.setBounds (top.removeFromRight (top.getWidth() * 3 / 5));
        sourceLabel.setBounds (top);

        if (stemTabs.isVisible())
            stemTabs.setBounds (area.removeFromTop (26).reduced (8, 1));

        auto review = area.removeFromBottom (reviewHeight).reduced (8, 3);
        previousReviewButton.setBounds (review.removeFromLeft (110));
        review.removeFromLeft (4);
        nextReviewButton.setBounds (review.removeFromLeft (190));
        review.removeFromLeft (4);
        fixButton.setBounds (review.removeFromLeft (66));
        review.removeFromLeft (10);
        reviewLabel.setBounds (review);

        area.removeFromBottom (18);
        area.removeFromTop (juce::jmax (60, (int) (area.getHeight() * 0.38)));
        pianoRoll.setBounds (area);

        task.setBounds (getLocalBounds().withSizeKeepingCentre (460, 72));
    }
}
