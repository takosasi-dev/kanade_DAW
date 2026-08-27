#include "UI/WaveformEditorView.h"
#include "Engine/AudioEngine.h"
#include <algorithm>
#include <cmath>

namespace ss
{
    namespace
    {
        constexpr int toolbarHeight = 76;
        constexpr int infoHeight    = 22;
    }

    WaveformEditorView::WaveformEditorView (AppContext& c, UiState& s)
        : ProjectView (c, s)
    {
        infoLabel.setColour (juce::Label::textColourId, palette().textDim);
        infoLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        addAndMakeVisible (infoLabel);

        selectionLabel.setColour (juce::Label::textColourId, palette().textDim);
        selectionLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        selectionLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (selectionLabel);

        auto addButton = [this] (juce::TextButton& b, const juce::String& text, const juce::String& tip)
        {
            b.setButtonText (text);
            b.setTooltip (tip);
            addAndMakeVisible (b);
        };

        addButton (trimButton,     TRANS ("Trim to selection"), TRANS ("Keep only the selected range"));
        addButton (deleteButton,   TRANS ("Delete selection"),  TRANS ("Remove the selected range"));
        addButton (fadeInButton,   TRANS ("Fade in"),           TRANS ("Fade in up to the selection end"));
        addButton (fadeOutButton,  TRANS ("Fade out"),          TRANS ("Fade out from the selection start"));
        addButton (normalizeButton, TRANS ("Normalize"),        TRANS ("Set the clip gain so the loudest peak hits 0 dB"));
        addButton (reverseButton,  TRANS ("Reverse"),           TRANS ("Play the clip backwards"));
        addButton (resetButton,    TRANS ("Reset edits"),       TRANS ("Clear gain, fades and reverse"));
        addButton (transcribeButton, TRANS ("Transcribe this range"),
                   TRANS ("Send the selected range to the transcriber"));

        gainLabel.setText (TRANS ("Clip gain"), juce::dontSendNotification);
        gainLabel.setColour (juce::Label::textColourId, palette().textDim);
        addAndMakeVisible (gainLabel);

        gainSlider.setRange (-24.0, 24.0, 0.1);
        gainSlider.setTextValueSuffix (" dB");
        gainSlider.setDoubleClickReturnValue (true, 0.0);
        gainSlider.onDragEnd = [this]
        {
            const float v = (float) gainSlider.getValue();
            edit (TRANS ("Clip gain"), [v] (AudioClip& c) { c.gainDb = v; });
        };
        addAndMakeVisible (gainSlider);

        trimButton.onClick = [this]
        {
            auto* clip = currentClip();
            if (clip == nullptr || selectionEnd - selectionStart < 0.01) return;

            const double start = selectionStart, length = selectionEnd - selectionStart;
            const auto& tempo = project().tempo;
            const double clipStartSec = tempo.beatsToSeconds (clip->startBeats);
            const double deltaSec = start - clip->offsetSeconds;
            const double newStartBeats = tempo.secondsToBeats (clipStartSec + deltaSec);
            const double newEndBeats   = tempo.secondsToBeats (clipStartSec + deltaSec + length);

            edit (TRANS ("Trim clip"), [start, newStartBeats, newEndBeats] (AudioClip& c)
            {
                c.offsetSeconds = start;
                c.startBeats    = newStartBeats;
                c.lengthBeats   = juce::jmax (0.0625, newEndBeats - newStartBeats);
            });
            selectionStart = selectionEnd = 0.0;
        };

        deleteButton.onClick = [this]
        {
            auto* clip = currentClip();
            if (clip == nullptr || selectionEnd - selectionStart < 0.01) return;

            const auto& tempo = project().tempo;
            const double clipStartSec = tempo.beatsToSeconds (clip->startBeats);
            const double clipEndSec   = clipStartSec + clipDurationSeconds();
            const double selStartSec  = clipStartSec + (selectionStart - clip->offsetSeconds);
            const double selEndSec    = clipStartSec + (selectionEnd   - clip->offsetSeconds);

            const bool touchesStart = selStartSec <= clipStartSec + 0.005;
            const bool touchesEnd   = selEndSec   >= clipEndSec - 0.005;
            const auto trackId = ui.selectedTrack;
            const auto clipId  = ui.selectedClip;

            performProjectEdit (project(), TRANS ("Delete range"),
                                [this, trackId, clipId, touchesStart, touchesEnd,
                                 selStartSec, selEndSec, clipEndSec]
            {
                auto* track = project().findTrack (trackId);
                if (track == nullptr) return;
                auto* c = track->findAudioClip (clipId);
                if (c == nullptr) return;

                const auto& tm = project().tempo;

                if (touchesStart && touchesEnd)
                {
                    track->audioClips.erase (std::remove_if (track->audioClips.begin(), track->audioClips.end(),
                                                             [clipId] (const AudioClip& a) { return a.id == clipId; }),
                                             track->audioClips.end());
                    return;
                }

                if (touchesStart)
                {
                    c->offsetSeconds += selEndSec - tm.beatsToSeconds (c->startBeats);
                    const double newStart = tm.secondsToBeats (selEndSec);
                    c->lengthBeats = c->endBeats() - newStart;
                    c->startBeats  = newStart;
                    return;
                }

                if (touchesEnd)
                {
                    c->lengthBeats = tm.secondsToBeats (selStartSec) - c->startBeats;
                    return;
                }

                // A hole in the middle: keep the head, add the tail as a new clip.
                AudioClip tail = *c;
                tail.id = project().nextClipId();
                tail.offsetSeconds = c->offsetSeconds + (selEndSec - tm.beatsToSeconds (c->startBeats));
                tail.startBeats  = tm.secondsToBeats (selEndSec);
                tail.lengthBeats = tm.secondsToBeats (clipEndSec) - tail.startBeats;
                tail.fadeInSec   = 0.0;

                c->lengthBeats = tm.secondsToBeats (selStartSec) - c->startBeats;
                c->fadeOutSec  = 0.0;
                track->audioClips.push_back (tail);
            });

            selectionStart = selectionEnd = 0.0;
        };

        fadeInButton.onClick = [this]
        {
            auto* clip = currentClip();
            if (clip == nullptr) return;
            const double length = juce::jmax (0.01, selectionEnd - clip->offsetSeconds);
            edit (TRANS ("Fade in"), [length] (AudioClip& c) { c.fadeInSec = length; });
        };

        fadeOutButton.onClick = [this]
        {
            auto* clip = currentClip();
            if (clip == nullptr) return;
            const double clipEnd = clip->offsetSeconds + clipDurationSeconds();
            const double length  = juce::jmax (0.01, clipEnd - selectionStart);
            edit (TRANS ("Fade out"), [length] (AudioClip& c) { c.fadeOutSec = length; });
        };

        normalizeButton.onClick = [this]
        {
            // ponytail: peak taken from the thumbnail, so it is accurate to the
            // thumbnail's resolution rather than sample-exact.  Read the file with
            // io::readAudioFile on a background thread if that ever matters.
            if (thumbnail == nullptr) return;
            const float peak = thumbnail->getApproximatePeak();
            if (peak <= 0.0f) return;

            const float db = juce::jlimit (-24.0f, 24.0f,
                                           juce::Decibels::gainToDecibels (1.0f / peak));
            edit (TRANS ("Normalize"), [db] (AudioClip& c) { c.gainDb = db; });
        };

        reverseButton.onClick = [this]
        {
            edit (TRANS ("Reverse"), [] (AudioClip& c) { c.reversed = ! c.reversed; });
        };

        resetButton.onClick = [this]
        {
            edit (TRANS ("Reset edits"), [] (AudioClip& c)
            {
                c.gainDb = 0.0f; c.fadeInSec = 0.0; c.fadeOutSec = 0.0; c.reversed = false;
            });
        };

        transcribeButton.onClick = [this]
        {
            auto* clip = currentClip();
            if (clip == nullptr) return;

            UiState::TranscribeRequest request;
            request.valid = true;
            request.file  = clip->sourceFile;
            request.offsetSeconds = selectionEnd > selectionStart ? selectionStart : clip->offsetSeconds;
            request.lengthSeconds = selectionEnd > selectionStart ? selectionEnd - selectionStart
                                                                  : clipDurationSeconds();
            request.placeAtBeat   = clip->startBeats;
            request.clipName      = clip->name.isNotEmpty() ? clip->name
                                                            : clip->sourceFile.getFileNameWithoutExtension();
            ui.transcribeRequest = request;
            ui.sendChangeMessage();
            ui.goTo (MainComponent::View::transcribe);
        };

        refresh();
        startTimer (400);
    }

    WaveformEditorView::~WaveformEditorView()
    {
        stopTimer();
        if (thumbnail != nullptr)
            thumbnail->removeChangeListener (this);
    }

    void WaveformEditorView::launch (AppContext& ctx, UiState& ui)
    {
        if (! ui.selectedClipIsAudio || ui.selectedClip == invalidClipId)
            return;

        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (new WaveformEditorView (ctx, ui));
        options.content->setSize (900, 460);
        options.dialogTitle            = TRANS ("Waveform Editor");
        options.dialogBackgroundColour = palette().windowBg;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar      = true;
        options.resizable              = true;
        options.launchAsync();          // the window owns and deletes itself
    }

    //==============================================================================
    AudioClip* WaveformEditorView::currentClip() const
    {
        if (ctx.project == nullptr || ! ui.selectedClipIsAudio)
            return nullptr;

        if (auto* t = project().findTrack (ui.selectedTrack))
            return t->findAudioClip (ui.selectedClip);

        return nullptr;
    }

    double WaveformEditorView::clipDurationSeconds() const
    {
        auto* clip = currentClip();
        if (clip == nullptr || ctx.project == nullptr)
            return 0.0;

        const auto& tempo = project().tempo;
        return juce::jmax (0.01, tempo.beatsToSeconds (clip->endBeats())
                                   - tempo.beatsToSeconds (clip->startBeats));
    }

    void WaveformEditorView::changeListenerCallback (juce::ChangeBroadcaster*)
    {
        refresh();
    }

    void WaveformEditorView::timerCallback()
    {
        // The thumbnail loads in the background; keep drawing until it is there.
        if (thumbnail != nullptr && ! thumbnail->isFullyLoaded())
            repaint (waveArea());
    }

    void WaveformEditorView::refresh()
    {
        auto* clip = currentClip();
        const bool hasClip = clip != nullptr;

        trimButton.setEnabled (hasClip);
        deleteButton.setEnabled (hasClip);
        fadeInButton.setEnabled (hasClip);
        fadeOutButton.setEnabled (hasClip);
        normalizeButton.setEnabled (hasClip);
        reverseButton.setEnabled (hasClip);
        resetButton.setEnabled (hasClip);
        transcribeButton.setEnabled (hasClip);
        gainSlider.setEnabled (hasClip);

        if (! hasClip)
        {
            infoLabel.setText (TRANS ("No audio clip selected"), juce::dontSendNotification);
            repaint();
            return;
        }

        if (clip->sourceFile != loadedFile && ctx.engine != nullptr)
        {
            if (thumbnail != nullptr)
                thumbnail->removeChangeListener (this);

            loadedFile = clip->sourceFile;
            thumbnail = std::make_unique<juce::AudioThumbnail> (256, ctx.engine->getFormatManager(), cache);
            thumbnail->addChangeListener (this);
            if (loadedFile.existsAsFile())
                thumbnail->setSource (new juce::FileInputSource (loadedFile));
        }

        {
            const juce::ScopedValueSetter<bool> guard (updating, true);
            gainSlider.setValue (clip->gainDb, juce::dontSendNotification);
        }

        infoLabel.setText (loadedFile.getFileName()
                             + "   " + juce::String (clipDurationSeconds(), 2) + " s"
                             + (clip->reversed ? "   " + TRANS ("(reversed)") : juce::String()),
                           juce::dontSendNotification);

        selectionLabel.setText (selectionEnd > selectionStart
                                  ? TRANS ("Selection") + ": " + juce::String (selectionStart, 3) + " - "
                                        + juce::String (selectionEnd, 3) + " s"
                                  : TRANS ("Drag on the waveform to select a range"),
                                juce::dontSendNotification);
        repaint();
    }

    void WaveformEditorView::edit (const juce::String& name, std::function<void (AudioClip&)> fn)
    {
        if (ctx.project == nullptr)
            return;

        const auto trackId = ui.selectedTrack;
        const auto clipId  = ui.selectedClip;

        performProjectEdit (project(), name, [this, trackId, clipId, fn]
        {
            if (auto* t = project().findTrack (trackId))
                if (auto* c = t->findAudioClip (clipId))
                    fn (*c);
        });
    }

    //==============================================================================
    juce::Rectangle<int> WaveformEditorView::waveArea() const
    {
        auto r = getLocalBounds();
        r.removeFromTop (infoHeight);
        r.removeFromBottom (toolbarHeight);
        return r.reduced (8, 4);
    }

    double WaveformEditorView::timeForX (int x) const
    {
        auto* clip = currentClip();
        const auto area = waveArea();
        if (clip == nullptr || area.getWidth() <= 0)
            return 0.0;

        const double t = (x - area.getX()) / (double) area.getWidth();
        return clip->offsetSeconds + juce::jlimit (0.0, 1.0, t) * clipDurationSeconds();
    }

    float WaveformEditorView::xForTime (double seconds) const
    {
        auto* clip = currentClip();
        const auto area = waveArea();
        if (clip == nullptr)
            return (float) area.getX();

        const double t = (seconds - clip->offsetSeconds) / juce::jmax (0.001, clipDurationSeconds());
        return (float) (area.getX() + juce::jlimit (0.0, 1.0, t) * area.getWidth());
    }

    void WaveformEditorView::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        g.fillAll (p.windowBg);

        const auto area = waveArea();
        g.setColour (p.laneBg);
        g.fillRect (area);
        g.setColour (p.outline);
        g.drawRect (area, 1);

        auto* clip = currentClip();
        if (clip == nullptr)
        {
            g.setColour (p.textDim);
            g.drawText (TRANS ("No audio clip selected"), area, juce::Justification::centred, false);
            return;
        }

        // Centre line, then the waveform for exactly the clip's source range.
        g.setColour (p.outline.withAlpha (0.5f));
        g.drawHorizontalLine (area.getCentreY(), (float) area.getX(), (float) area.getRight());

        if (thumbnail != nullptr && thumbnail->getTotalLength() > 0.0)
        {
            g.setColour (p.waveform);
            thumbnail->drawChannels (g, area.reduced (2), clip->offsetSeconds,
                                     clip->offsetSeconds + clipDurationSeconds(), 0.95f);
        }
        else
        {
            g.setColour (p.textDim);
            g.drawText (TRANS ("Loading..."), area, juce::Justification::centred, false);
        }

        // Fades, as the wedges they will actually apply.
        g.setColour (p.windowBg.withAlpha (0.65f));
        if (clip->fadeInSec > 0.0)
        {
            const float x = xForTime (clip->offsetSeconds + clip->fadeInSec);
            juce::Path fade;
            fade.addTriangle ((float) area.getX(), (float) area.getY(), x, (float) area.getY(),
                              (float) area.getX(), (float) area.getBottom());
            g.fillPath (fade);
        }
        if (clip->fadeOutSec > 0.0)
        {
            const float x = xForTime (clip->offsetSeconds + clipDurationSeconds() - clip->fadeOutSec);
            juce::Path fade;
            fade.addTriangle ((float) area.getRight(), (float) area.getY(), x, (float) area.getY(),
                              (float) area.getRight(), (float) area.getBottom());
            g.fillPath (fade);
        }

        if (selectionEnd > selectionStart)
        {
            const float x1 = xForTime (selectionStart), x2 = xForTime (selectionEnd);
            g.setColour (p.accent.withAlpha (0.22f));
            g.fillRect (juce::Rectangle<float> (x1, (float) area.getY(), x2 - x1, (float) area.getHeight()));
            g.setColour (p.accent);
            g.drawVerticalLine (juce::roundToInt (x1), (float) area.getY(), (float) area.getBottom());
            g.drawVerticalLine (juce::roundToInt (x2), (float) area.getY(), (float) area.getBottom());
        }
    }

    void WaveformEditorView::resized()
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (infoHeight).reduced (8, 2);
        selectionLabel.setBounds (top.removeFromRight (top.getWidth() / 2));
        infoLabel.setBounds (top);

        auto bottom = r.removeFromBottom (toolbarHeight).reduced (8, 6);

        auto row1 = bottom.removeFromTop (26);
        auto place = [] (juce::Rectangle<int>& row, juce::Component& c, int w)
        {
            c.setBounds (row.removeFromLeft (w).reduced (2, 0));
        };
        place (row1, trimButton, 140);
        place (row1, deleteButton, 130);
        place (row1, fadeInButton, 90);
        place (row1, fadeOutButton, 96);
        place (row1, normalizeButton, 100);
        place (row1, reverseButton, 90);
        place (row1, resetButton, 110);

        bottom.removeFromTop (6);
        auto row2 = bottom.removeFromTop (26);
        gainLabel.setBounds (row2.removeFromLeft (74));
        place (row2, gainSlider, 240);
        row2.removeFromLeft (12);
        transcribeButton.setBounds (row2.removeFromLeft (190).reduced (2, 0));
    }

    void WaveformEditorView::mouseDown (const juce::MouseEvent& e)
    {
        if (! waveArea().contains (e.getPosition()))
            return;

        dragAnchor = timeForX (e.x);
        selectionStart = selectionEnd = dragAnchor;
        refresh();
    }

    void WaveformEditorView::mouseDrag (const juce::MouseEvent& e)
    {
        if (currentClip() == nullptr)
            return;

        const double t = timeForX (e.x);
        selectionStart = juce::jmin (dragAnchor, t);
        selectionEnd   = juce::jmax (dragAnchor, t);
        refresh();
    }
}
