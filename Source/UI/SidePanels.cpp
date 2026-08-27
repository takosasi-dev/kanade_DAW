#include "UI/SidePanels.h"
#include "Core/Settings.h"
#include "Engine/AudioEngine.h"
#include "IO/FileIO.h"
#include "UI/WaveformEditorView.h"
#include <algorithm>

namespace ss
{
    namespace
    {
        juce::String browserWildcards()
        {
            auto extensions = io::getSupportedAudioExtensions();
            juce::StringArray patterns;

            for (auto e : extensions)
            {
                e = e.trim();
                if (e.startsWith ("*")) e = e.substring (1);
                if (! e.startsWith (".")) e = "." + e;
                patterns.add ("*" + e);
            }

            patterns.addArray (juce::StringArray { "*.mid", "*.midi", "*.ssproj", "*.sf2", "*.sfz" });
            patterns.removeDuplicates (true);
            return patterns.joinIntoString (";");
        }
    }

    //==========================================================================
    BrowserPanel::BrowserPanel (AppContext& c, UiState& s)
        : ProjectView (c, s), fileFilter (browserWildcards(), "*", TRANS ("Audio, MIDI and projects"))
    {
        projectList.setModel (this);
        projectList.setRowHeight (21);
        projectTab.addAndMakeVisible (projectList);
        projectTab.setInterceptsMouseClicks (false, true);

        rootBox.onChange = [this]
        {
            const int index = rootBox.getSelectedId() - 1;
            if (directoryList != nullptr && index >= 0 && index < roots.size())
                directoryList->setDirectory (juce::File (roots[index]), true, true);
        };
        filesTab.addAndMakeVisible (rootBox);

        directoryThread.startThread (juce::Thread::Priority::background);
        directoryList = std::make_unique<juce::DirectoryContentsList> (&fileFilter, directoryThread);
        fileTree = std::make_unique<juce::FileTreeComponent> (*directoryList);
        fileTree->addListener (this);
        fileTree->setDragAndDropDescription ("ss.browserFile");
        filesTab.addAndMakeVisible (*fileTree);

        hintLabel.setText (TRANS ("Double-click a file to import it at the playhead"),
                           juce::dontSendNotification);
        hintLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
        hintLabel.setColour (juce::Label::textColourId, palette().textDim);
        filesTab.addAndMakeVisible (hintLabel);

        projectTab.onResized = [this] { projectList.setBounds (projectTab.getLocalBounds()); };
        filesTab.onResized = [this]
        {
            auto files = filesTab.getLocalBounds();
            rootBox.setBounds (files.removeFromTop (26).reduced (4, 2));
            hintLabel.setBounds (files.removeFromBottom (18).reduced (6, 0));
            if (fileTree != nullptr)
                fileTree->setBounds (files.reduced (2));
        };

        tabs.addTab (TRANS ("Project"), palette().panelBg, &projectTab, false);
        tabs.addTab (TRANS ("Files"), palette().panelBg, &filesTab, false);
        addAndMakeVisible (tabs);

        rebuildRoots();
        rebuildEntries();
    }

    BrowserPanel::~BrowserPanel()
    {
        if (fileTree != nullptr)
            fileTree->removeListener (this);

        fileTree.reset();
        directoryList.reset();
        directoryThread.stopThread (2000);
    }

    void BrowserPanel::rebuildRoots()
    {
        roots.clear();

        if (ctx.settings != nullptr)
        {
            roots.add (ctx.settings->getProjectsFolder().getFullPathName());
            roots.addArray (ctx.settings->getSampleLibraryFolders());
        }

        roots.add (juce::File::getSpecialLocation (juce::File::userMusicDirectory).getFullPathName());
        roots.removeEmptyStrings();
        roots.removeDuplicates (true);

        rootBox.clear (juce::dontSendNotification);
        for (int i = 0; i < roots.size(); ++i)
            rootBox.addItem (juce::File (roots[i]).getFileName().isNotEmpty()
                               ? juce::File (roots[i]).getFileName() : roots[i], i + 1);

        if (! roots.isEmpty())
        {
            rootBox.setSelectedId (1, juce::dontSendNotification);
            directoryList->setDirectory (juce::File (roots[0]), true, true);
        }
    }

    void BrowserPanel::changeListenerCallback (juce::ChangeBroadcaster*)
    {
        rebuildEntries();
    }

    void BrowserPanel::rebuildEntries()
    {
        entries.clear();

        if (ctx.project != nullptr)
        {
            for (int i = 0; i < project().getNumTracks(); ++i)
            {
                auto& track = project().getTrack (i);
                entries.push_back ({ track.getId(), invalidClipId,
                                     track.getType() == TrackType::audio, true, track.name });

                for (const auto& clip : track.audioClips)
                    entries.push_back ({ track.getId(), clip.id, true, false,
                                         clip.name.isNotEmpty() ? clip.name
                                                                : clip.sourceFile.getFileNameWithoutExtension() });

                for (const auto& clip : track.midiClips)
                    entries.push_back ({ track.getId(), clip.id, false, false,
                                         clip.name.isNotEmpty() ? clip.name : TRANS ("MIDI") });
            }
        }

        projectList.updateContent();
        projectList.repaint();
    }

    int BrowserPanel::getNumRows() { return (int) entries.size(); }

    void BrowserPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool)
    {
        if (row < 0 || row >= (int) entries.size())
            return;

        const auto& entry = entries[(size_t) row];
        const auto& p = palette();

        const bool isCurrent = entry.isTrackRow ? entry.track == ui.selectedTrack
                                                : entry.clip == ui.selectedClip;
        if (isCurrent)
        {
            g.setColour (p.accentDim);
            g.fillRect (0, 0, width, height);
        }

        g.setColour (entry.isTrackRow ? p.text : p.textDim);
        g.setFont (juce::Font (juce::FontOptions (entry.isTrackRow ? 13.0f : 12.0f)));
        g.drawText ((entry.isTrackRow ? juce::String() : juce::String ("    ")) + entry.text,
                    8, 0, width - 12, height, juce::Justification::centredLeft, true);

        if (! entry.isTrackRow)
        {
            g.setColour (entry.isAudio ? p.waveformDim : p.note);
            g.fillRect (width - 10, height / 2 - 3, 6, 6);
        }
    }

    void BrowserPanel::listBoxItemClicked (int row, const juce::MouseEvent&)
    {
        if (row < 0 || row >= (int) entries.size())
            return;

        const auto& entry = entries[(size_t) row];
        ui.select (entry.track, entry.clip, entry.isAudio);
        projectList.repaint();
    }

    void BrowserPanel::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
    {
        if (row < 0 || row >= (int) entries.size())
            return;

        const auto& entry = entries[(size_t) row];
        if (entry.isTrackRow)
            return;

        ui.select (entry.track, entry.clip, entry.isAudio);

        if (entry.isAudio) WaveformEditorView::launch (ctx, ui);
        else               ui.goTo (MainComponent::View::pianoRoll);
    }

    void BrowserPanel::fileDoubleClicked (const juce::File& file)
    {
        if (! file.existsAsFile() || ctx.project == nullptr)
            return;

        if (file.hasFileExtension ("ssproj"))
        {
            // Opening a document is the command's job, not the browser's.
            ui.invoke (CommandIDs::fileOpen);
            return;
        }

        const double at = ctx.engine != nullptr ? juce::jmax (0.0, ctx.engine->getTransport().getPositionBeats())
                                                : 0.0;

        performProjectEdit (project(), TRANS ("Import file"), [this, file, at]
        {
            juce::String error;

            if (file.hasFileExtension ("mid;midi"))
            {
                io::importMidiFile (file, project(), error);
                return;
            }

            const auto target = project().addTrack (TrackType::audio,
                                                    file.getFileNameWithoutExtension()).getId();
            if (ctx.engine != nullptr)
                io::importAudioFile (file, project(), target, at, ctx.engine->getFormatManager(), error);
        });
    }

    void BrowserPanel::paint (juce::Graphics& g)
    {
        g.fillAll (palette().panelBg);
    }

    void BrowserPanel::resized()
    {
        tabs.setBounds (getLocalBounds());
    }

    //==========================================================================
    AiPanel::AiPanel (AppContext& c, UiState& s)
        : ProjectView (c, s)
    {
        titleLabel.setText (TRANS ("AI"), juce::dontSendNotification);
        titleLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        titleLabel.setColour (juce::Label::textColourId, palette().textBright);
        addAndMakeVisible (titleLabel);

        statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        statusLabel.setColour (juce::Label::textColourId, palette().textDim);
        statusLabel.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (statusLabel);

        transcribeButton.setButtonText (TRANS ("Transcribe this range"));
        transcribeButton.setColour (juce::TextButton::buttonColourId, palette().accentDim);
        transcribeButton.onClick = [this]
        {
            if (auto* clip = selectedAudioClip())
            {
                const auto& tempo = project().tempo;
                UiState::TranscribeRequest request;
                request.valid = true;
                request.file  = clip->sourceFile;
                request.offsetSeconds = clip->offsetSeconds;
                request.lengthSeconds = tempo.beatsToSeconds (clip->endBeats())
                                          - tempo.beatsToSeconds (clip->startBeats);
                request.placeAtBeat   = clip->startBeats;
                request.clipName      = clip->name.isNotEmpty()
                                          ? clip->name : clip->sourceFile.getFileNameWithoutExtension();
                ui.transcribeRequest = request;
                ui.sendChangeMessage();
            }

            ui.goTo (MainComponent::View::transcribe);
        };
        addAndMakeVisible (transcribeButton);

        generateButton.setButtonText (TRANS ("Generate from this clip"));
        generateButton.onClick = [this] { ui.goTo (MainComponent::View::generate); };
        addAndMakeVisible (generateButton);

        waveformButton.setButtonText (TRANS ("Edit waveform..."));
        waveformButton.onClick = [this] { WaveformEditorView::launch (ctx, ui); };
        addAndMakeVisible (waveformButton);

        confidenceTitle.setText (TRANS ("Confidence"), juce::dontSendNotification);
        confidenceTitle.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        confidenceTitle.setColour (juce::Label::textColourId, palette().text);
        addAndMakeVisible (confidenceTitle);

        confidenceLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        confidenceLabel.setColour (juce::Label::textColourId, palette().textDim);
        addAndMakeVisible (confidenceLabel);

        thresholdLabel.setText (TRANS ("Review below"), juce::dontSendNotification);
        thresholdLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        thresholdLabel.setColour (juce::Label::textColourId, palette().textDim);
        addAndMakeVisible (thresholdLabel);

        thresholdSlider.setRange (0.0, 1.0, 0.01);
        thresholdSlider.setValue (ui.confidenceThreshold, juce::dontSendNotification);
        thresholdSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
        thresholdSlider.onValueChange = [this]
        {
            ui.confidenceThreshold = (float) thresholdSlider.getValue();
            ui.sendChangeMessage();
            refresh();
        };
        addAndMakeVisible (thresholdSlider);

        reviewButton.setButtonText (TRANS ("Review in piano roll"));
        reviewButton.onClick = [this] { ui.goTo (MainComponent::View::pianoRoll); };
        addAndMakeVisible (reviewButton);

        refresh();
    }

    MidiClip* AiPanel::selectedMidiClip() const
    {
        if (ctx.project == nullptr || ui.selectedClipIsAudio)
            return nullptr;

        if (auto* t = project().findTrack (ui.selectedTrack))
            return t->findMidiClip (ui.selectedClip);

        return nullptr;
    }

    AudioClip* AiPanel::selectedAudioClip() const
    {
        if (ctx.project == nullptr || ! ui.selectedClipIsAudio)
            return nullptr;

        if (auto* t = project().findTrack (ui.selectedTrack))
            return t->findAudioClip (ui.selectedClip);

        return nullptr;
    }

    void AiPanel::changeListenerCallback (juce::ChangeBroadcaster*)
    {
        refresh();
    }

    void AiPanel::refresh()
    {
        for (auto& bucket : histogram)
            bucket = 0;
        lowConfidenceCount = 0;
        noteCount = 0;

        auto* midi = selectedMidiClip();
        auto* audio = selectedAudioClip();

        if (midi != nullptr)
        {
            noteCount = (int) midi->notes.size();
            for (const auto& note : midi->notes)
            {
                histogram[juce::jlimit (0, 9, (int) (note.confidence * 10.0f))]++;
                if (note.isLowConfidence (ui.confidenceThreshold))
                    ++lowConfidenceCount;
            }
        }

        transcribeButton.setEnabled (audio != nullptr);
        waveformButton.setEnabled (audio != nullptr);
        generateButton.setEnabled (midi != nullptr);
        reviewButton.setEnabled (lowConfidenceCount > 0);

        thresholdSlider.setValue (ui.confidenceThreshold, juce::dontSendNotification);

        statusLabel.setText (audio != nullptr
                               ? TRANS ("Audio clip selected") + "\n"
                                   + (audio->name.isNotEmpty() ? audio->name
                                                               : audio->sourceFile.getFileName())
                               : midi != nullptr
                                   ? TRANS ("MIDI clip selected") + "\n"
                                       + juce::String (noteCount) + " " + TRANS ("notes")
                                   : TRANS ("Select a clip in the timeline to work on it here"),
                             juce::dontSendNotification);

        confidenceLabel.setText (noteCount == 0
                                   ? TRANS ("No notes")
                                   : juce::String (lowConfidenceCount) + " / " + juce::String (noteCount)
                                       + " " + TRANS ("need review"),
                                 juce::dontSendNotification);
        confidenceLabel.setColour (juce::Label::textColourId,
                                   lowConfidenceCount > 0 ? palette().warning : palette().textDim);
        repaint();
    }

    void AiPanel::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        g.fillAll (p.panelBg);
        g.setColour (p.divider);
        g.drawVerticalLine (0, 0.0f, (float) getHeight());

        // Confidence histogram: the same ramp the piano roll shades notes with,
        // so the panel and the notes read as one display (spec 9.3 / 9.7).
        const auto area = histogramArea;
        if (area.isEmpty())
            return;

        g.setColour (p.laneBg);
        g.fillRect (area);

        int highest = 1;
        for (int count : histogram)
            highest = juce::jmax (highest, count);

        const float barWidth = area.getWidth() / 10.0f;
        for (int i = 0; i < 10; ++i)
        {
            const float h = area.getHeight() * (histogram[i] / (float) highest);
            juce::Rectangle<float> bar (area.getX() + i * barWidth, area.getBottom() - h,
                                        barWidth - 2.0f, h);
            g.setColour (confidenceColour ((i + 0.5f) / 10.0f));
            g.fillRect (bar);
            paintConfidenceHatch (g, bar, (i + 0.5f) / 10.0f, ui.confidenceThreshold);
        }

        // Where the review threshold sits on the histogram.
        const float x = area.getX() + area.getWidth() * ui.confidenceThreshold;
        g.setColour (p.warning);
        g.drawVerticalLine (juce::roundToInt (x), (float) area.getY(), (float) area.getBottom());

        g.setColour (p.outline);
        g.drawRect (area, 1);
    }

    void AiPanel::resized()
    {
        auto area = getLocalBounds().reduced (12, 10);

        titleLabel.setBounds (area.removeFromTop (20));
        area.removeFromTop (4);
        statusLabel.setBounds (area.removeFromTop (48));
        area.removeFromTop (6);

        transcribeButton.setBounds (area.removeFromTop (30));
        area.removeFromTop (5);
        generateButton.setBounds (area.removeFromTop (28));
        area.removeFromTop (5);
        waveformButton.setBounds (area.removeFromTop (28));

        auto bottom = area.removeFromBottom (juce::jmin (area.getHeight(), 216));
        confidenceTitle.setBounds (bottom.removeFromTop (20));
        confidenceLabel.setBounds (bottom.removeFromTop (18));

        auto thresholdRow = bottom.removeFromTop (24);
        thresholdLabel.setBounds (thresholdRow.removeFromLeft (96));
        thresholdSlider.setBounds (thresholdRow);

        reviewButton.setBounds (bottom.removeFromBottom (26));
        bottom.removeFromBottom (8);
        histogramArea = bottom;
    }
}
