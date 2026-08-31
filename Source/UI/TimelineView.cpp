#include "UI/TimelineView.h"
#include "Engine/AudioEngine.h"
#include "IO/FileIO.h"
#include "Mixer/BuiltinFx.h"
#include "Mixer/Mixer.h"
#include "Plugins/PluginManager.h"
#include "UI/AutomationEditor.h"
#include "UI/WaveformEditorView.h"
#include <algorithm>
#include <cmath>

namespace ss
{
    namespace
    {
        constexpr int headerWidth    = 214;
        constexpr int rulerHeight    = 26;
        constexpr int trackHeight    = 84;
        constexpr int scrollThickness = 12;
        constexpr float edgeGrab     = 6.0f;

        constexpr int   automationRowHeight = 44;
        constexpr int   automationRowInset  = 6;    // top/bottom margin inside a row
        constexpr float breakpointRadius    = 3.5f;
        constexpr float breakpointGrab      = 7.0f;

        const juce::Colour trackColours[] =
        {
            juce::Colour (0xff4a90d9), juce::Colour (0xff5fb87a), juce::Colour (0xffd98d4a),
            juce::Colour (0xffbf6bd9), juce::Colour (0xffd9534a), juce::Colour (0xff4ac4d9),
            juce::Colour (0xffd9c14a), juce::Colour (0xff8a93a5)
        };
    }

    //==============================================================================
    TrackHeader::TrackHeader (AppContext& c, UiState& s, TrackId id)
        : ctx (c), ui (s), trackId (id)
    {
        nameLabel.setEditable (false, true, false);
        nameLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
        nameLabel.onTextChange = [this]
        {
            const auto newName = nameLabel.getText();
            commit (TRANS ("Rename track"), [newName] (Track& t) { t.name = newName; });
        };
        addAndMakeVisible (nameLabel);

        auto setUpToggle = [this] (juce::TextButton& b, const juce::String& text,
                                   const juce::String& tip, juce::Colour on)
        {
            b.setButtonText (text);
            b.setTooltip (tip);
            b.setClickingTogglesState (true);
            b.setColour (juce::TextButton::buttonOnColourId, on);
            b.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
            addAndMakeVisible (b);
        };

        setUpToggle (armButton,  "R", TRANS ("Record arm"), palette().recordArm);
        setUpToggle (muteButton, "M", TRANS ("Mute"),       palette().warning);
        setUpToggle (soloButton, "S", TRANS ("Solo"),       palette().accent);

        armButton.onClick = [this]
        {
            const bool v = armButton.getToggleState();
            commit (TRANS ("Record arm"), [v] (Track& t) { t.recordArmed = v; });
        };
        muteButton.onClick = [this]
        {
            const bool v = muteButton.getToggleState();
            applyLiveMute (ctx, trackId, v);
            commit (TRANS ("Mute track"), [v] (Track& t) { t.muted = v; });
        };
        soloButton.onClick = [this]
        {
            const bool v = soloButton.getToggleState();
            commit (TRANS ("Solo track"), [v] (Track& t) { t.soloed = v; });
        };

        fxButton.setButtonText (TRANS ("FX"));
        fxButton.setTooltip (TRANS ("Plugin chain"));
        fxButton.onClick = [this] { showPluginChainMenu(); };
        addAndMakeVisible (fxButton);

        inputButton.setButtonText (TRANS ("In"));
        inputButton.setTooltip (TRANS ("Input"));
        inputButton.onClick = [this] { showInputMenu(); };
        addAndMakeVisible (inputButton);

        setUpToggle (autoButton, "A", TRANS ("Automation"), palette().accent);
        autoButton.onClick = [this]
        {
            ui.setAutomationVisible (trackId, autoButton.getToggleState());
        };

        gainSlider.setRange (-60.0, 12.0, 0.1);
        gainSlider.setDoubleClickReturnValue (true, 0.0);
        gainSlider.setTooltip (TRANS ("Gain"));
        gainSlider.onValueChange = [this]
        {
            if (! updating) applyLiveGain (ctx, trackId, (float) gainSlider.getValue());
        };
        gainSlider.onDragEnd = [this]
        {
            const float v = (float) gainSlider.getValue();
            commit (TRANS ("Change gain"), [v] (Track& t) { t.gainDb = v; });
        };
        addAndMakeVisible (gainSlider);

        panSlider.setRange (-1.0, 1.0, 0.01);
        panSlider.setDoubleClickReturnValue (true, 0.0);
        panSlider.setTooltip (TRANS ("Pan"));
        panSlider.onValueChange = [this]
        {
            if (! updating) applyLivePan (ctx, trackId, (float) panSlider.getValue());
        };
        panSlider.onDragEnd = [this]
        {
            const float v = (float) panSlider.getValue();
            commit (TRANS ("Change pan"), [v] (Track& t) { t.pan = v; });
        };
        addAndMakeVisible (panSlider);

        refresh();
    }

    Track* TrackHeader::track() const
    {
        return ctx.project != nullptr ? ctx.project->findTrack (trackId) : nullptr;
    }

    void TrackHeader::commit (const juce::String& name, std::function<void (Track&)> edit)
    {
        if (ctx.project == nullptr)
            return;

        auto id = trackId;
        auto* p = ctx.project.get();
        performProjectEdit (*p, name, [p, id, edit]
        {
            if (auto* t = p->findTrack (id))
                edit (*t);
        });
    }

    void TrackHeader::refresh()
    {
        auto* t = track();
        if (t == nullptr)
            return;

        const juce::ScopedValueSetter<bool> guard (updating, true);
        nameLabel.setText (t->name, juce::dontSendNotification);
        armButton.setToggleState (t->recordArmed, juce::dontSendNotification);
        muteButton.setToggleState (t->muted, juce::dontSendNotification);
        soloButton.setToggleState (t->soloed, juce::dontSendNotification);
        autoButton.setToggleState (ui.isAutomationVisible (trackId), juce::dontSendNotification);
        gainSlider.setValue (t->gainDb, juce::dontSendNotification);
        panSlider.setValue (t->pan, juce::dontSendNotification);

        fxButton.setButtonText (t->plugins.empty() && t->builtinFx.empty()
                                    ? TRANS ("FX")
                                    : "FX " + juce::String ((int) (t->plugins.size() + t->builtinFx.size())));
        armButton.setEnabled (true);
        repaint();
    }

    void TrackHeader::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        auto* t = track();

        const bool isCurrent = t != nullptr && t->getId() == ui.selectedTrack;
        g.setColour (isCurrent ? p.headerBg : p.panelBg);
        g.fillRect (getLocalBounds());

        if (t != nullptr)
        {
            g.setColour (t->colour);
            g.fillRect (0, 0, 5, getHeight());
        }

        g.setColour (p.divider);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

        if (t != nullptr)
        {
            g.setColour (p.textDim);
            g.setFont (juce::Font (juce::FontOptions (9.5f)));
            g.drawText (toString (t->getType()), getWidth() - 46, 2, 42, 12,
                        juce::Justification::topRight, false);
        }
    }

    void TrackHeader::resized()
    {
        auto area = getLocalBounds().reduced (8, 4);
        area.removeFromLeft (2);

        nameLabel.setBounds (area.removeFromTop (18));
        area.removeFromTop (2);

        auto row = area.removeFromTop (20);
        armButton.setBounds (row.removeFromLeft (26));
        muteButton.setBounds (row.removeFromLeft (26));
        soloButton.setBounds (row.removeFromLeft (26));
        row.removeFromLeft (6);
        fxButton.setBounds (row.removeFromLeft (40));
        row.removeFromLeft (3);
        inputButton.setBounds (row.removeFromLeft (38));
        row.removeFromLeft (3);
        autoButton.setBounds (row.removeFromLeft (24));

        area.removeFromTop (3);
        gainSlider.setBounds (area.removeFromTop (16));
        panSlider.setBounds (area.removeFromTop (16));
    }

    void TrackHeader::mouseDown (const juce::MouseEvent& e)
    {
        auto* t = track();
        if (t == nullptr)
            return;

        ui.select (trackId, invalidClipId, t->getType() == TrackType::audio);

        if (! e.mods.isPopupMenu())
            return;

        juce::PopupMenu menu;
        menu.addSectionHeader (t->name);
        menu.addItem (1, TRANS ("Rename"));

        juce::PopupMenu colours;
        for (int i = 0; i < (int) juce::numElementsInArray (trackColours); ++i)
            colours.addColouredItem (100 + i, TRANS ("Colour") + " " + juce::String (i + 1), trackColours[i]);
        menu.addSubMenu (TRANS ("Track colour"), colours);

        menu.addSeparator();
        menu.addItem (2, TRANS ("Delete track"));

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [this] (int result)
        {
            if (result == 1)
            {
                nameLabel.showEditor();
            }
            else if (result == 2)
            {
                if (ctx.project == nullptr) return;
                auto* p = ctx.project.get();
                auto id = trackId;
                performProjectEdit (*p, TRANS ("Delete track"), [p, id] { p->removeTrack (id); });
            }
            else if (result >= 100)
            {
                const auto c = trackColours[(size_t) juce::jlimit (0, (int) juce::numElementsInArray (trackColours) - 1,
                                                                   result - 100)];
                commit (TRANS ("Track colour"), [c] (Track& t) { t.colour = c; });
            }
        });
    }

    void TrackHeader::showInputMenu()
    {
        auto* t = track();
        if (t == nullptr || ctx.engine == nullptr)
            return;

        juce::PopupMenu menu;

        if (t->getType() == TrackType::audio)
        {
            menu.addSectionHeader (TRANS ("Audio input"));
            if (auto* device = ctx.engine->getDeviceManager().getCurrentAudioDevice())
            {
                const auto names = device->getInputChannelNames();
                for (int i = 0; i < names.size(); ++i)
                    menu.addItem (i + 1, names[i], true, t->inputChannel == i);
            }
            else
            {
                menu.addItem (999, TRANS ("No audio device open"), false);
            }
        }
        else
        {
            menu.addSectionHeader (TRANS ("MIDI input"));
            menu.addItem (1, TRANS ("All MIDI inputs"), true, t->midiInputDevice.isEmpty());
            const auto devices = juce::MidiInput::getAvailableDevices();
            for (int i = 0; i < devices.size(); ++i)
                menu.addItem (i + 2, devices[i].name, true, t->midiInputDevice == devices[i].identifier);
        }

        const bool isAudio = t->getType() == TrackType::audio;
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&inputButton),
                            [this, isAudio] (int result)
        {
            if (result <= 0 || result == 999) return;

            if (isAudio)
            {
                const int channel = result - 1;
                commit (TRANS ("Change input"), [channel] (Track& t) { t.inputChannel = channel; });
            }
            else
            {
                juce::String identifier;
                if (result >= 2)
                {
                    const auto devices = juce::MidiInput::getAvailableDevices();
                    if (result - 2 < devices.size())
                        identifier = devices[result - 2].identifier;
                }
                commit (TRANS ("Change input"), [identifier] (Track& t) { t.midiInputDevice = identifier; });
            }
        });
    }

    void TrackHeader::showPluginChainMenu()
    {
        auto* t = track();
        if (t == nullptr)
            return;

        juce::PopupMenu menu;
        menu.addSectionHeader (TRANS ("Plugin chain"));

        for (int i = 0; i < (int) t->plugins.size(); ++i)
        {
            juce::PopupMenu slot;
            slot.addItem (2000 + i, TRANS ("Bypass"), true, t->plugins[(size_t) i].bypassed);
            slot.addItem (3000 + i, TRANS ("Remove"));
            menu.addSubMenu (t->plugins[(size_t) i].displayName, slot);
        }

        if (t->plugins.empty())
            menu.addItem (900, TRANS ("(empty)"), false);

        juce::PopupMenu available;
        juce::Array<juce::PluginDescription> descriptions;
        if (ctx.plugins != nullptr)
        {
            descriptions = ctx.plugins->getKnownPluginList().getTypes();
            for (int i = 0; i < descriptions.size(); ++i)
                available.addItem (4000 + i, descriptions[i].name + "  (" + descriptions[i].pluginFormatName + ")");
        }
        if (descriptions.isEmpty())
            available.addItem (901, TRANS ("No plugins found - scan in Preferences"), false);

        menu.addSubMenu (TRANS ("Add plugin"), available);
        menu.addSeparator();
        menu.addItem (1, TRANS ("Open mixer"));

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&fxButton),
                            [this, descriptions] (int result)
        {
            if (result <= 0) return;

            if (result == 1)
            {
                ui.goTo (MainComponent::View::mixer);
            }
            else if (result >= 4000)
            {
                const int index = result - 4000;
                if (index < descriptions.size())
                {
                    PluginSlot slot;
                    slot.identifier  = descriptions[index].createIdentifierString();
                    slot.displayName = descriptions[index].name;
                    commit (TRANS ("Add plugin"), [slot] (Track& t) { t.plugins.push_back (slot); });
                }
            }
            else if (result >= 3000)
            {
                const int index = result - 3000;
                commit (TRANS ("Remove plugin"), [index] (Track& t)
                {
                    if (index < (int) t.plugins.size())
                        t.plugins.erase (t.plugins.begin() + index);
                });
            }
            else if (result >= 2000)
            {
                const int index = result - 2000;
                commit (TRANS ("Bypass plugin"), [index] (Track& t)
                {
                    if (index < (int) t.plugins.size())
                        t.plugins[(size_t) index].bypassed = ! t.plugins[(size_t) index].bypassed;
                });
            }
        });
    }

    //==============================================================================
    TimelineView::TimelineView (AppContext& c, UiState& s)
        : ProjectView (c, s)
    {
        addAudioTrackButton.setButtonText (TRANS ("+ Audio"));
        addAudioTrackButton.setTooltip (TRANS ("Add an audio track"));
        addAudioTrackButton.onClick = [this]
        {
            auto* p = ctx.project.get();
            if (p == nullptr) return;
            performProjectEdit (*p, TRANS ("Add audio track"),
                                [p] { p->addTrack (TrackType::audio, TRANS ("Audio")); });
        };
        addAndMakeVisible (addAudioTrackButton);

        addMidiTrackButton.setButtonText (TRANS ("+ MIDI"));
        addMidiTrackButton.setTooltip (TRANS ("Add a MIDI track"));
        addMidiTrackButton.onClick = [this]
        {
            auto* p = ctx.project.get();
            if (p == nullptr) return;
            performProjectEdit (*p, TRANS ("Add MIDI track"),
                                [p] { p->addTrack (TrackType::midi, TRANS ("MIDI")); });
        };
        addAndMakeVisible (addMidiTrackButton);

        hScroll.addListener (this);
        vScroll.addListener (this);
        addAndMakeVisible (hScroll);
        addAndMakeVisible (vScroll);

        rebuildHeaders();
        startTimerHz (ctx.settings != nullptr ? ctx.settings->getTimelineRefreshHz() : 24);
    }

    TimelineView::~TimelineView()
    {
        stopTimer();
        hScroll.removeListener (this);
        vScroll.removeListener (this);

        for (auto& t : thumbnails)
            t.second->removeChangeListener (this);
    }

    //==============================================================================
    void TimelineView::changeListenerCallback (juce::ChangeBroadcaster* source)
    {
        if (dynamic_cast<juce::AudioThumbnail*> (source) != nullptr)
        {
            repaint (laneArea());
            return;
        }

        // Breakpoints are addressed by index, so an edit somewhere else in the
        // document can leave the selection pointing past the end of a lane.
        selectedPoints.erase (std::remove_if (selectedPoints.begin(), selectedPoints.end(),
                                              [this] (const PointRef& r)
                                              {
                                                  auto* lane = laneFor (r.track, r.parameterId);
                                                  return lane == nullptr
                                                          || ! juce::isPositiveAndBelow (r.index, (int) lane->points.size());
                                              }),
                              selectedPoints.end());

        rebuildHeaders();
        updateScrollRanges();
        repaint();
    }

    void TimelineView::rebuildHeaders()
    {
        if (ctx.project == nullptr)
            return;

        // Reuse the header for a track that is still there; a project reload
        // replaces the Track objects but keeps the ids.
        std::vector<std::unique_ptr<TrackHeader>> updated;

        for (int i = 0; i < project().getNumTracks(); ++i)
        {
            const auto id = project().getTrack (i).getId();

            auto existing = std::find_if (headers.begin(), headers.end(),
                                          [id] (const std::unique_ptr<TrackHeader>& h)
                                          { return h != nullptr && h->getTrackId() == id; });

            if (existing != headers.end())
            {
                (*existing)->refresh();
                updated.push_back (std::move (*existing));
            }
            else
            {
                auto h = std::make_unique<TrackHeader> (ctx, ui, id);
                addAndMakeVisible (*h);
                updated.push_back (std::move (h));
            }
        }

        headers = std::move (updated);
        layOutHeaders();
    }

    void TimelineView::layOutHeaders()
    {
        updateLaneLayout();

        const auto lanes = laneArea();
        for (int i = 0; i < (int) headers.size(); ++i)
            if (headers[(size_t) i] != nullptr)
                headers[(size_t) i]->setBounds (0, lanes.getY() + blockTopFor (i) - scrollY,
                                                headerWidth, trackHeight);
    }

    //==============================================================================
    /*  Tracks are no longer all the same height: an expanded one carries its
        automation rows underneath it (spec 8.4.5).  blockTops holds the top of
        every track in content space, plus the total height at the end, so the
        hit tests and the painter agree without either of them re-adding it.   */
    void TimelineView::updateLaneLayout()
    {
        blockTops.clear();

        const int numTracks = ctx.project != nullptr ? project().getNumTracks() : 0;
        int top = 0;

        for (int i = 0; i < numTracks; ++i)
        {
            blockTops.push_back (top);
            top += trackHeight + automationRowsFor (i) * automationRowHeight;
        }

        blockTops.push_back (top);
    }

    int TimelineView::automationRowsFor (int trackIndex) const
    {
        if (ctx.project == nullptr || ! juce::isPositiveAndBelow (trackIndex, project().getNumTracks()))
            return 0;

        auto& track = project().getTrack (trackIndex);
        return ui.isAutomationVisible (track.getId()) ? (int) track.automation.size() + 1 : 0;
    }

    int TimelineView::blockTopFor (int trackIndex) const
    {
        return juce::isPositiveAndBelow (trackIndex, (int) blockTops.size())
                 ? blockTops[(size_t) trackIndex] : 0;
    }

    //==============================================================================
    juce::Rectangle<int> TimelineView::rulerArea() const
    {
        auto r = getLocalBounds();
        r.removeFromLeft (headerWidth);
        r.removeFromRight (scrollThickness);
        return r.removeFromTop (rulerHeight);
    }

    juce::Rectangle<int> TimelineView::laneArea() const
    {
        auto r = getLocalBounds();
        r.removeFromTop (rulerHeight);
        r.removeFromBottom (scrollThickness);
        r.removeFromLeft (headerWidth);
        r.removeFromRight (scrollThickness);
        return r;
    }

    juce::Rectangle<int> TimelineView::headerArea() const
    {
        auto r = getLocalBounds();
        r.removeFromTop (rulerHeight);
        r.removeFromBottom (scrollThickness);
        return r.removeFromLeft (headerWidth);
    }

    juce::Rectangle<int> TimelineView::laneForTrackIndex (int index) const
    {
        const auto lanes = laneArea();
        return { lanes.getX(), lanes.getY() + blockTopFor (index) - scrollY, lanes.getWidth(), trackHeight };
    }

    juce::Rectangle<int> TimelineView::automationRowRect (int trackIndex, int row) const
    {
        const auto lanes = laneArea();
        return { lanes.getX(),
                 lanes.getY() + blockTopFor (trackIndex) + trackHeight
                   + row * automationRowHeight - scrollY,
                 lanes.getWidth(), automationRowHeight };
    }

    /*  Deliberately only the arrangement strip: a y inside an automation row
        belongs to the track but is not somewhere a clip can be, and every
        caller here (clip hit test, file drop, candidate drop) means "clips".  */
    int TimelineView::trackIndexAt (int y) const
    {
        if (ctx.project == nullptr) return -1;

        const int content = y - laneArea().getY() + scrollY;

        for (int i = 0; i < project().getNumTracks(); ++i)
        {
            const int top = blockTopFor (i);
            if (content >= top && content < top + trackHeight)
                return i;
        }

        return -1;
    }

    bool TimelineView::automationRowAt (juce::Point<int> p, int& trackIndexOut, int& rowOut) const
    {
        const auto lanes = laneArea();
        if (ctx.project == nullptr || p.y < lanes.getY() || p.y >= lanes.getBottom())
            return false;

        const int content = p.y - lanes.getY() + scrollY;

        for (int i = 0; i < project().getNumTracks(); ++i)
        {
            const int rows = automationRowsFor (i);
            if (rows == 0)
                continue;

            const int top = blockTopFor (i) + trackHeight;
            if (content < top || content >= top + rows * automationRowHeight)
                continue;

            trackIndexOut = i;
            rowOut = (content - top) / automationRowHeight;
            return true;
        }

        return false;
    }

    double TimelineView::beatForX (int x) const
    {
        return scrollBeats + (x - laneArea().getX()) / juce::jmax (1.0, pixelsPerBeat);
    }

    float TimelineView::xForBeat (double beat) const
    {
        return (float) (laneArea().getX() + (beat - scrollBeats) * pixelsPerBeat);
    }

    void TimelineView::updateScrollRanges()
    {
        const auto lanes = laneArea();
        if (lanes.isEmpty() || ctx.project == nullptr)
            return;

        const double visibleBeats = lanes.getWidth() / juce::jmax (1.0, pixelsPerBeat);
        const double contentEnd = juce::jmax (64.0, project().endBeats() + 16.0);

        hScroll.setRangeLimits (0.0, juce::jmax (contentEnd, scrollBeats + visibleBeats), juce::dontSendNotification);
        hScroll.setCurrentRange (scrollBeats, visibleBeats, juce::dontSendNotification);

        const int contentHeight = juce::jmax (lanes.getHeight(),
                                              (blockTops.empty() ? 0 : blockTops.back()) + 20);
        vScroll.setRangeLimits (0.0, contentHeight, juce::dontSendNotification);
        vScroll.setCurrentRange (scrollY, lanes.getHeight(), juce::dontSendNotification);
    }

    void TimelineView::scrollBarMoved (juce::ScrollBar* bar, double newRangeStart)
    {
        if (bar == &hScroll) scrollBeats = juce::jmax (0.0, newRangeStart);
        else                 scrollY = juce::jmax (0, (int) newRangeStart);

        layOutHeaders();
        repaint();
    }

    void TimelineView::timerCallback()
    {
        if (ctx.engine == nullptr || ! ctx.engine->getTransport().isPlaying())
            return;

        repaint (laneArea().withTop (rulerArea().getY()));

        // The lane header column holds the value-at-the-playhead readout, and
        // the repaint above stops at headerWidth.  Repaint the readouts only -
        // widening the rectangle would drag every TrackHeader component along.
        if (ctx.project == nullptr || ui.automationTracks.empty())
            return;

        for (int i = 0; i < project().getNumTracks(); ++i)
            for (int row = automationRowsFor (i); --row >= 0;)
                repaint (automationRowRect (i, row).withX (0).withWidth (headerWidth));
    }

    void TimelineView::zoom (double factor)
    {
        pixelsPerBeat = juce::jlimit (1.5, 400.0, pixelsPerBeat * factor);
        updateScrollRanges();
        repaint();
    }

    void TimelineView::resized()
    {
        auto corner = getLocalBounds().removeFromTop (rulerHeight).removeFromLeft (headerWidth).reduced (4, 3);
        addAudioTrackButton.setBounds (corner.removeFromLeft (corner.getWidth() / 2 - 2));
        corner.removeFromLeft (4);
        addMidiTrackButton.setBounds (corner);

        hScroll.setBounds (headerWidth, getHeight() - scrollThickness,
                           juce::jmax (0, getWidth() - headerWidth - scrollThickness), scrollThickness);
        const auto lanes = laneArea();
        vScroll.setBounds (lanes.getRight(), lanes.getY(), scrollThickness, lanes.getHeight());

        layOutHeaders();
        updateScrollRanges();
    }

    //==============================================================================
    juce::AudioThumbnail* TimelineView::thumbnailFor (const juce::File& file)
    {
        const auto key = file.getFullPathName();
        auto it = thumbnails.find (key);
        if (it != thumbnails.end())
            return it->second.get();

        if (ctx.engine == nullptr || ! file.existsAsFile())
            return nullptr;

        auto thumb = std::make_unique<juce::AudioThumbnail> (512, ctx.engine->getFormatManager(), thumbnailCache);
        thumb->addChangeListener (this);
        thumb->setSource (new juce::FileInputSource (file));

        auto* raw = thumb.get();
        thumbnails[key] = std::move (thumb);
        return raw;
    }

    // ponytail: linear scan per clip per repaint (O(tracks * clips) overall).
    // Fine for the track counts this app is for; cache a ClipId -> rect map if a
    // project ever gets big enough to feel it.
    juce::Rectangle<float> TimelineView::rectForClip (const ClipRef& ref) const
    {
        if (ctx.project == nullptr)
            return {};

        for (int i = 0; i < project().getNumTracks(); ++i)
        {
            auto& track = project().getTrack (i);
            if (track.getId() != ref.track)
                continue;

            double start = 0.0, length = 0.0;
            if (ref.isAudio)
            {
                for (const auto& c : track.audioClips)
                    if (c.id == ref.clip) { start = c.startBeats; length = c.lengthBeats; }
            }
            else
            {
                for (const auto& c : track.midiClips)
                    if (c.id == ref.clip) { start = c.startBeats; length = c.lengthBeats; }
            }

            if (length <= 0.0)
                return {};

            const auto lane = laneForTrackIndex (i);
            return { xForBeat (start), (float) lane.getY() + 3.0f,
                     (float) (length * pixelsPerBeat), (float) lane.getHeight() - 7.0f };
        }

        return {};
    }

    TimelineView::ClipRef TimelineView::clipAt (juce::Point<int> p) const
    {
        const int index = trackIndexAt (p.y);
        if (index < 0 || ctx.project == nullptr)
            return {};

        auto& track = project().getTrack (index);
        const double beat = beatForX (p.x);

        for (const auto& c : track.audioClips)
            if (beat >= c.startBeats && beat < c.endBeats())
                return { track.getId(), c.id, true };

        for (const auto& c : track.midiClips)
            if (beat >= c.startBeats && beat < c.endBeats())
                return { track.getId(), c.id, false };

        return {};
    }

    bool TimelineView::isSelected (const ClipRef& ref) const
    {
        return std::find (selected.begin(), selected.end(), ref) != selected.end();
    }

    //==============================================================================
    void TimelineView::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        g.fillAll (p.windowBg);

        if (ctx.project == nullptr)
            return;

        const auto lanes = laneArea();

        // --- lane backgrounds -------------------------------------------------
        g.setColour (p.laneBg);
        g.fillRect (lanes);

        for (int i = 0; i < project().getNumTracks(); ++i)
        {
            const auto lane = laneForTrackIndex (i);
            if (lane.getBottom() < lanes.getY() || lane.getY() > lanes.getBottom())
                continue;

            g.setColour (i % 2 == 0 ? p.laneBg : p.laneAltBg);
            g.fillRect (lane.getIntersection (lanes));
            g.setColour (p.divider);
            g.drawHorizontalLine (lane.getBottom() - 1, (float) lanes.getX(), (float) lanes.getRight());
        }

        // Automation row backgrounds go in before the grid so the grid runs
        // through them; the curves themselves are drawn with the clips.
        for (int i = 0; i < project().getNumTracks(); ++i)
        {
            for (int row = automationRowsFor (i); --row >= 0;)
            {
                const auto full = automationRowRect (i, row);
                if (full.getBottom() < lanes.getY() || full.getY() > lanes.getBottom())
                    continue;

                g.setColour (p.panelBg.withAlpha (0.85f));
                g.fillRect (full.getIntersection (lanes));
                g.setColour (p.divider);
                g.drawHorizontalLine (full.getBottom() - 1, (float) lanes.getX(), (float) lanes.getRight());
            }
        }

        paintBeatGrid (g, lanes, project().tempo, scrollBeats, pixelsPerBeat, ui.grid);

        if (project().loopEnabled)
        {
            const float x1 = xForBeat (project().loopStartBeats);
            const float x2 = xForBeat (project().loopEndBeats);
            g.setColour (p.loopRegion);
            g.fillRect (juce::Rectangle<float> (x1, (float) lanes.getY(),
                                                juce::jmax (0.0f, x2 - x1), (float) lanes.getHeight()));
        }

        // --- clips ------------------------------------------------------------
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (lanes);

            for (int i = 0; i < project().getNumTracks(); ++i)
            {
                auto& track = project().getTrack (i);
                const auto lane = laneForTrackIndex (i);
                if (lane.getBottom() < lanes.getY() || lane.getY() > lanes.getBottom())
                    continue;

                for (const auto& c : track.audioClips)
                {
                    const ClipRef ref { track.getId(), c.id, true };
                    auto r = rectForClip (ref);
                    if (r.getRight() >= lanes.getX() && r.getX() <= lanes.getRight())
                        paintAudioClip (g, track, c, r, isSelected (ref));
                }

                for (const auto& c : track.midiClips)
                {
                    const ClipRef ref { track.getId(), c.id, false };
                    auto r = rectForClip (ref);
                    if (r.getRight() >= lanes.getX() && r.getX() <= lanes.getRight())
                        paintMidiClip (g, track, c, r, isSelected (ref));
                }
            }

            paintAutomationCurves (g);
        }

        if (! marquee.isEmpty())
        {
            g.setColour (p.accent.withAlpha (0.18f));
            g.fillRect (marquee);
            g.setColour (p.accent);
            g.drawRect (marquee, 1);
        }

        // --- markers ----------------------------------------------------------
        for (const auto& marker : project().markers)
        {
            const float x = xForBeat (marker.beat);
            if (x < lanes.getX() || x > lanes.getRight()) continue;
            g.setColour (p.warning);
            g.drawVerticalLine (juce::roundToInt (x), (float) lanes.getY(), (float) lanes.getBottom());
        }

        // --- header column background (behind the header components) ----------
        g.setColour (p.panelBg);
        g.fillRect (headerArea());
        paintAutomationHeaders (g);
        g.setColour (p.headerBg);
        g.fillRect (getLocalBounds().removeFromTop (rulerHeight).removeFromLeft (headerWidth));

        paintBeatRuler (g, rulerArea(), project().tempo, scrollBeats, pixelsPerBeat);

        // --- playhead ---------------------------------------------------------
        if (ctx.engine != nullptr)
        {
            const float x = xForBeat (ctx.engine->getTransport().getPositionBeats());
            if (x >= lanes.getX() && x <= lanes.getRight())
            {
                g.setColour (p.playhead);
                g.drawVerticalLine (juce::roundToInt (x), (float) rulerArea().getY(), (float) lanes.getBottom());

                juce::Path head;
                head.addTriangle (x - 5.0f, (float) rulerArea().getY(),
                                  x + 5.0f, (float) rulerArea().getY(),
                                  x, (float) rulerArea().getY() + 7.0f);
                g.fillPath (head);
            }
        }

        if (project().getNumTracks() == 0)
        {
            g.setColour (p.textDim);
            g.setFont (juce::Font (juce::FontOptions (15.0f)));
            g.drawText (TRANS ("Drop an audio or MIDI file here, or add a track to start"),
                        lanes, juce::Justification::centred, false);
        }
    }

    void TimelineView::paintAudioClip (juce::Graphics& g, const Track& track, const AudioClip& clip,
                                       juce::Rectangle<float> r, bool selected_)
    {
        const auto& p = palette();
        const auto lanes = laneArea();

        g.setColour (selected_ ? track.colour.withAlpha (0.55f) : track.colour.withAlpha (0.32f));
        g.fillRoundedRectangle (r, 3.0f);

        auto visible = r.getIntersection (lanes.toFloat());
        if (! visible.isEmpty())
        {
            if (auto* thumb = thumbnailFor (clip.sourceFile))
            {
                if (thumb->getTotalLength() > 0.0)
                {
                    // Map the visible slice of the clip back to source time so a
                    // clip that runs off the edge still draws in constant time.
                    const auto& tempo = project().tempo;
                    const double clipStartSec = tempo.beatsToSeconds (clip.startBeats);
                    const double t0 = clip.offsetSeconds
                                        + (tempo.beatsToSeconds (beatForX ((int) visible.getX())) - clipStartSec);
                    const double t1 = clip.offsetSeconds
                                        + (tempo.beatsToSeconds (beatForX ((int) visible.getRight())) - clipStartSec);

                    g.setColour (p.waveform);
                    thumb->drawChannels (g, visible.toNearestInt().reduced (2, 12),
                                         juce::jmax (0.0, t0), juce::jmax (0.01, t1), 0.95f);
                }
                else
                {
                    g.setColour (p.textDim);
                    g.setFont (juce::Font (juce::FontOptions (10.0f)));
                    g.drawText (TRANS ("Loading..."), visible.toNearestInt(), juce::Justification::centred, false);
                }
            }
        }

        // Fades, drawn as the wedges they actually are (spec 8.4.4).
        const auto& tempo = project().tempo;
        const double secondsPerBeat = juce::jmax (1.0e-6, tempo.beatsToSeconds (clip.startBeats + 1.0)
                                                            - tempo.beatsToSeconds (clip.startBeats));
        g.setColour (p.windowBg.withAlpha (0.6f));
        if (clip.fadeInSec > 0.0)
        {
            const float w = (float) (clip.fadeInSec / secondsPerBeat * pixelsPerBeat);
            juce::Path fade;
            fade.addTriangle (r.getX(), r.getY(), r.getX() + w, r.getY(), r.getX(), r.getBottom());
            g.fillPath (fade);
        }
        if (clip.fadeOutSec > 0.0)
        {
            const float w = (float) (clip.fadeOutSec / secondsPerBeat * pixelsPerBeat);
            juce::Path fade;
            fade.addTriangle (r.getRight(), r.getY(), r.getRight() - w, r.getY(), r.getRight(), r.getBottom());
            g.fillPath (fade);
        }

        g.setColour (selected_ ? p.textBright : p.outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, selected_ ? 2.0f : 1.0f);

        g.setColour (p.textBright);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        auto label = clip.name.isNotEmpty() ? clip.name : clip.sourceFile.getFileNameWithoutExtension();
        if (clip.reversed)       label += "  " + TRANS ("(reversed)");
        if (clip.gainDb != 0.0f) label += "  " + formatDb (clip.gainDb);
        g.drawText (label, r.toNearestInt().reduced (5, 2).withHeight (12),
                    juce::Justification::topLeft, false);
    }

    void TimelineView::paintMidiClip (juce::Graphics& g, const Track& track, const MidiClip& clip,
                                      juce::Rectangle<float> r, bool selected_)
    {
        const auto& p = palette();

        g.setColour (selected_ ? track.colour.withAlpha (0.5f) : track.colour.withAlpha (0.28f));
        g.fillRoundedRectangle (r, 3.0f);

        // Mini piano roll: the notes are squeezed into whatever range they use.
        if (! clip.notes.empty())
        {
            int lowest = 127, highest = 0;
            for (const auto& n : clip.notes)
            {
                lowest  = juce::jmin (lowest, n.pitch);
                highest = juce::jmax (highest, n.pitch);
            }
            const int span = juce::jmax (6, highest - lowest + 1);

            auto inner = r.reduced (2.0f, 0.0f).withTrimmedTop (13.0f).withTrimmedBottom (3.0f);
            const float noteHeight = juce::jmax (1.5f, inner.getHeight() / (float) span);

            for (const auto& n : clip.notes)
            {
                const float x1 = xForBeat (clip.startBeats + n.startBeats);
                const float x2 = xForBeat (clip.startBeats + n.endBeats());
                const float y  = inner.getBottom() - (float) (n.pitch - lowest + 1) * noteHeight;

                g.setColour (n.confidence >= 0.999f ? p.note : confidenceColour (n.confidence));
                g.fillRect (juce::Rectangle<float> (x1, y, juce::jmax (1.5f, x2 - x1), noteHeight));
            }
        }

        g.setColour (selected_ ? p.textBright : p.outline);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, selected_ ? 2.0f : 1.0f);

        g.setColour (p.textBright);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        auto label = clip.name.isNotEmpty() ? clip.name : TRANS ("MIDI");
        const float lowest = clip.lowestConfidence();
        if (lowest < ui.confidenceThreshold)
            label += "  " + TRANS ("needs review");
        g.drawText (label, r.toNearestInt().reduced (5, 2).withHeight (12),
                    juce::Justification::topLeft, false);
    }

    //==============================================================================
    // Automation lanes (spec 8.4.5).  Parameter ids follow the convention the
    // mixer reads on the audio thread - see UI/AutomationEditor.h.
    //==============================================================================
    Track::AutomationLane* TimelineView::laneFor (TrackId trackId, const juce::String& parameterId)
    {
        if (ctx.project == nullptr)
            return nullptr;

        auto* track = project().findTrack (trackId);
        if (track == nullptr)
            return nullptr;

        for (auto& lane : track->automation)
            if (lane.parameterId == parameterId)
                return &lane;

        return nullptr;
    }

    float TimelineView::yForValue (juce::Rectangle<int> plot, float value) noexcept
    {
        return (float) plot.getBottom() - juce::jlimit (0.0f, 1.0f, value) * (float) plot.getHeight();
    }

    float TimelineView::valueForY (juce::Rectangle<int> plot, int y) noexcept
    {
        if (plot.getHeight() <= 0)
            return 0.0f;

        return juce::jlimit (0.0f, 1.0f, (float) (plot.getBottom() - y) / (float) plot.getHeight());
    }

    int TimelineView::pointIndexAt (const Track::AutomationLane& lane, juce::Rectangle<int> plot,
                                    juce::Point<int> pos) const
    {
        int   best = -1;
        float bestDistance = breakpointGrab;

        for (int i = 0; i < (int) lane.points.size(); ++i)
        {
            const float dx = xForBeat (lane.points[(size_t) i].first) - (float) pos.x;
            const float dy = yForValue (plot, lane.points[(size_t) i].second) - (float) pos.y;
            const float distance = std::sqrt (dx * dx + dy * dy);

            if (distance <= bestDistance)
            {
                bestDistance = distance;
                best = i;
            }
        }

        return best;
    }

    bool TimelineView::isPointSelected (const PointRef& ref) const
    {
        return std::find (selectedPoints.begin(), selectedPoints.end(), ref) != selectedPoints.end();
    }

    juce::AudioPluginInstance* TimelineView::liveInstanceForSlot (const Track& track, int slot) const
    {
        if (ctx.engine == nullptr || ctx.plugins == nullptr
             || ! juce::isPositiveAndBelow (slot, (int) track.plugins.size()))
            return nullptr;

        auto* strip = ctx.engine->getMixer().getStripForTrack (track.getId());
        if (strip == nullptr)
            return nullptr;

        // Mirrors how ChannelStrip splits the chain: the first instrument in the
        // list becomes the instrument, everything else is an FX slot.
        // ponytail: assumes every slot loaded. A plugin that failed to load
        // shifts the live FX indices, so the picker would name that slot's
        // parameters from its neighbour - names only, the automation id is the
        // project slot index and stays right either way.
        int fxIndex = 0;
        bool instrumentTaken = false;

        for (int i = 0; i <= slot; ++i)
        {
            const auto* description = ctx.plugins->findDescription (track.plugins[(size_t) i].identifier);
            const bool isInstrument = description != nullptr && description->isInstrument && ! instrumentTaken;

            if (isInstrument)
                instrumentTaken = true;

            if (i == slot)
                return isInstrument ? strip->getInstrument() : strip->getPluginInstance (fxIndex);

            if (! isInstrument)
                ++fxIndex;
        }

        return nullptr;
    }

    juce::String TimelineView::laneLabel (const Track& track, const juce::String& parameterId) const
    {
        if (parameterId.startsWith ("plugin:"))
        {
            const int slot = automation::slotIndexOf (parameterId);
            const int index = parameterId.fromLastOccurrenceOf (":", false, false).getIntValue();

            if (auto* instance = liveInstanceForSlot (track, slot))
                if (juce::isPositiveAndBelow (index, instance->getParameters().size()))
                    return track.plugins[(size_t) slot].displayName + ": "
                             + instance->getParameters()[index]->getName (18);
        }

        return automation::displayName (track, parameterId);
    }

    //==============================================================================
    void TimelineView::paintAutomationCurves (juce::Graphics& g)
    {
        const auto& p = palette();
        const auto lanes = laneArea();
        const float left = (float) lanes.getX(), right = (float) lanes.getRight();

        for (int i = 0; i < project().getNumTracks(); ++i)
        {
            const int rows = automationRowsFor (i);
            if (rows == 0)
                continue;

            auto& track = project().getTrack (i);

            for (int row = 0; row < (int) track.automation.size(); ++row)
            {
                const auto r = automationRowRect (i, row);
                if (r.getBottom() < lanes.getY() || r.getY() > lanes.getBottom())
                    continue;

                const auto& lane = track.automation[(size_t) row];
                const auto plot = r.reduced (0, automationRowInset);

                if (lane.points.empty())
                {
                    g.setColour (p.textDim);
                    g.setFont (juce::Font (juce::FontOptions (10.0f)));
                    g.drawText (TRANS ("Click to add a point"), r.reduced (10, 0),
                                juce::Justification::centredLeft, false);
                    continue;
                }

                // One polyline: the interpolated value at each edge, and every
                // breakpoint in between.  Points outside the view are skipped -
                // the edge samples already carry the slope they contribute.
                juce::Path path;
                path.startNewSubPath (left, yForValue (plot, automation::valueAt (lane.points,
                                                                                 beatForX (lanes.getX()))));

                for (const auto& point : lane.points)
                {
                    const float x = xForBeat (point.first);
                    if (x >= left && x <= right)
                        path.lineTo (x, yForValue (plot, point.second));
                }

                path.lineTo (right, yForValue (plot, automation::valueAt (lane.points,
                                                                         beatForX (lanes.getRight()))));

                g.setColour (p.accent);
                g.strokePath (path, juce::PathStrokeType (1.5f));

                for (int k = 0; k < (int) lane.points.size(); ++k)
                {
                    const float x = xForBeat (lane.points[(size_t) k].first);
                    if (x < left - breakpointGrab || x > right + breakpointGrab)
                        continue;

                    const float y = yForValue (plot, lane.points[(size_t) k].second);
                    const bool isSel = isPointSelected ({ track.getId(), lane.parameterId, k });

                    g.setColour (isSel ? p.textBright : p.accent);
                    g.fillEllipse (x - breakpointRadius, y - breakpointRadius,
                                   breakpointRadius * 2.0f, breakpointRadius * 2.0f);

                    if (! isSel)
                        continue;

                    g.setFont (juce::Font (juce::FontOptions (10.0f)));
                    g.drawText (automation::formatValue (lane.parameterId, lane.points[(size_t) k].second),
                                juce::Rectangle<int> ((int) x + 6, (int) y - 14, 70, 12),
                                juce::Justification::centredLeft, false);
                }
            }
        }

        if (pendingAdd && laneFor (pendingAddTrack, pendingAddParam) != nullptr)
        {
            const float x = xForBeat (pendingAddBeat);
            g.setColour (p.textBright);
            g.drawEllipse (x - breakpointRadius, (float) dragStartValueY - breakpointRadius,
                           breakpointRadius * 2.0f, breakpointRadius * 2.0f, 1.5f);
        }

        if (! pointMarquee.isEmpty())
        {
            g.setColour (p.accent.withAlpha (0.18f));
            g.fillRect (pointMarquee);
            g.setColour (p.accent);
            g.drawRect (pointMarquee, 1);
        }
    }

    void TimelineView::paintAutomationHeaders (juce::Graphics& g)
    {
        if (ctx.project == nullptr || ui.automationTracks.empty())
            return;

        const auto column = headerArea();
        if (! g.getClipBounds().intersects (column))
            return;

        const auto& p = palette();
        const double playhead = ctx.engine != nullptr ? ctx.engine->getTransport().getPositionBeats() : 0.0;

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (column);

        for (int i = 0; i < project().getNumTracks(); ++i)
        {
            const int rows = automationRowsFor (i);
            if (rows == 0)
                continue;

            auto& track = project().getTrack (i);

            for (int row = 0; row < rows; ++row)
            {
                const auto r = automationRowRect (i, row).withX (0).withWidth (headerWidth);
                if (r.getBottom() < column.getY() || r.getY() > column.getBottom())
                    continue;

                g.setColour (p.panelBg);
                g.fillRect (r);
                g.setColour (track.colour);
                g.fillRect (r.getX(), r.getY(), 5, r.getHeight());
                g.setColour (p.divider);
                g.drawHorizontalLine (r.getBottom() - 1, 0.0f, (float) headerWidth);
                g.drawVerticalLine (headerWidth - 1, (float) r.getY(), (float) r.getBottom());

                auto text = r.reduced (14, 5);

                if (row >= (int) track.automation.size())
                {
                    g.setColour (p.accent);
                    g.setFont (juce::Font (juce::FontOptions (11.0f)));
                    g.drawText ("+ " + TRANS ("Automation lane"), text,
                                juce::Justification::centredLeft, false);
                    continue;
                }

                const auto& lane = track.automation[(size_t) row];

                g.setColour (p.text);
                g.setFont (juce::Font (juce::FontOptions (11.0f)));
                g.drawText (laneLabel (track, lane.parameterId) + "  "
                              + juce::String::charToString ((juce::juce_wchar) 0x25be),
                            text.removeFromTop (15), juce::Justification::centredLeft, false);

                g.setColour (p.textDim);
                g.setFont (juce::Font (juce::FontOptions (10.0f)));
                g.drawText (lane.points.empty()
                              ? juce::String ("-")
                              : automation::formatValue (lane.parameterId,
                                                         automation::valueAt (lane.points, playhead)),
                            text, juce::Justification::centredLeft, false);
            }
        }
    }

    //==============================================================================
    void TimelineView::automationMouseDown (const juce::MouseEvent& e, int trackIndex, int row)
    {
        auto& track = project().getTrack (trackIndex);
        const auto pos = e.getPosition();
        const bool isAddRow = row >= (int) track.automation.size();

        ui.select (track.getId(), invalidClipId, track.getType() == TrackType::audio);

        // The lane's header strip is the parameter picker.
        if (pos.x < headerWidth)
        {
            showParameterMenu (trackIndex, isAddRow ? -1 : row, pos);
            return;
        }

        if (isAddRow)
        {
            if (! e.mods.isPopupMenu())
                showParameterMenu (trackIndex, -1, pos);
            return;
        }

        const auto& lane = track.automation[(size_t) row];
        const auto plot = automationRowRect (trackIndex, row).reduced (0, automationRowInset);
        const int hit = pointIndexAt (lane, plot, pos);

        if (e.mods.isPopupMenu())
        {
            if (hit < 0)
            {
                showParameterMenu (trackIndex, row, pos);
                return;
            }

            selectedPoints = { { track.getId(), lane.parameterId, hit } };
            deleteSelectedAutomationPoints();
            return;
        }

        if (hit >= 0)
        {
            const PointRef ref { track.getId(), lane.parameterId, hit };

            if (e.mods.isShiftDown())
            {
                if (! isPointSelected (ref)) selectedPoints.push_back (ref);
            }
            else if (! isPointSelected (ref))
            {
                selectedPoints = { ref };
            }

            dragMode        = DragMode::automationPoint;
            dragStartPoint  = pos;
            dragStartBeat   = beatForX (pos.x);
            dragStartValueY = pos.y;

            pointOriginals.clear();
            for (const auto& sel : selectedPoints)
                if (auto* selLane = laneFor (sel.track, sel.parameterId))
                    if (juce::isPositiveAndBelow (sel.index, (int) selLane->points.size()))
                        pointOriginals.push_back ({ sel,
                                                    selLane->points[(size_t) sel.index].first,
                                                    selLane->points[(size_t) sel.index].second });
            repaint();
            return;
        }

        // Empty space: a click adds a breakpoint, a drag rubber-bands instead.
        if (! e.mods.isShiftDown())
            selectedPoints.clear();

        dragMode        = DragMode::automationMarquee;
        dragStartPoint  = pos;
        dragStartValueY = pos.y;
        pointMarquee    = {};

        pendingAdd      = true;
        pendingAddTrack = track.getId();
        pendingAddParam = lane.parameterId;
        pendingAddBeat  = juce::jmax (0.0, ui.snap (beatForX (pos.x)));
        pendingAddValue = valueForY (plot, pos.y);
        repaint();
    }

    void TimelineView::dragAutomationPoints (const juce::MouseEvent& e)
    {
        if (pointOriginals.empty())
            return;

        const auto& anchor = pointOriginals.front();

        double delta = beatForX (e.x) - dragStartBeat;
        if (ui.snapEnabled)
            delta = ui.snap (anchor.beat + delta) - anchor.beat;

        // Every dragged point moves by the same delta, so the tightest lane wins.
        std::vector<PointRef> lanesSeen;
        for (const auto& original : pointOriginals)
        {
            if (std::any_of (lanesSeen.begin(), lanesSeen.end(), [&original] (const PointRef& seen)
                             { return seen.track == original.ref.track
                                        && seen.parameterId == original.ref.parameterId; }))
                continue;

            lanesSeen.push_back (original.ref);

            auto* lane = laneFor (original.ref.track, original.ref.parameterId);
            if (lane == nullptr)
                continue;

            std::vector<int> indices;
            for (const auto& other : pointOriginals)
                if (other.ref.track == original.ref.track
                     && other.ref.parameterId == original.ref.parameterId)
                    indices.push_back (other.ref.index);

            const double allowed = automation::clampBeatDelta (lane->points, indices, delta);
            delta = delta >= 0.0 ? juce::jmin (delta, allowed) : juce::jmax (delta, allowed);
        }

        const float valueDelta = (float) (dragStartPoint.y - e.getPosition().y)
                                   / (float) juce::jmax (1, automationRowHeight - 2 * automationRowInset);

        // Values only - the vectors never resize during a drag, so the audio
        // thread reading the same lanes only ever sees numbers moving.
        for (const auto& original : pointOriginals)
            if (auto* lane = laneFor (original.ref.track, original.ref.parameterId))
                if (juce::isPositiveAndBelow (original.ref.index, (int) lane->points.size()))
                {
                    lane->points[(size_t) original.ref.index].first  = juce::jmax (0.0, original.beat + delta);
                    lane->points[(size_t) original.ref.index].second = juce::jlimit (0.0f, 1.0f,
                                                                                     original.value + valueDelta);
                }
    }

    void TimelineView::selectPointsIn (juce::Rectangle<int> area)
    {
        selectedPoints.clear();

        for (int i = 0; i < project().getNumTracks(); ++i)
        {
            const int rows = automationRowsFor (i);
            if (rows == 0)
                continue;

            auto& track = project().getTrack (i);

            for (int row = 0; row < (int) track.automation.size(); ++row)
            {
                const auto plot = automationRowRect (i, row).reduced (0, automationRowInset);
                const auto& lane = track.automation[(size_t) row];

                for (int k = 0; k < (int) lane.points.size(); ++k)
                {
                    const juce::Point<int> at { juce::roundToInt (xForBeat (lane.points[(size_t) k].first)),
                                                juce::roundToInt (yForValue (plot, lane.points[(size_t) k].second)) };
                    if (area.contains (at))
                        selectedPoints.push_back ({ track.getId(), lane.parameterId, k });
                }
            }
        }
    }

    void TimelineView::commitAutomationDrag()
    {
        if (ctx.project == nullptr || pointOriginals.empty())
            return;

        // Same shape as commitClipDrag: keep the dragged result, rewind the
        // document, then re-apply it inside one undoable transaction.
        struct Final { PointRef ref; double beat; float value; };
        std::vector<Final> finals;

        for (const auto& original : pointOriginals)
            if (auto* lane = laneFor (original.ref.track, original.ref.parameterId))
                if (juce::isPositiveAndBelow (original.ref.index, (int) lane->points.size()))
                {
                    auto& point = lane->points[(size_t) original.ref.index];
                    finals.push_back ({ original.ref, point.first, point.second });
                    point = { original.beat, original.value };
                }

        if (finals.empty())
            return;

        performProjectEdit (project(), TRANS ("Move automation"), [this, finals]
        {
            for (const auto& f : finals)
                if (auto* lane = laneFor (f.ref.track, f.ref.parameterId))
                    if (juce::isPositiveAndBelow (f.ref.index, (int) lane->points.size()))
                        lane->points[(size_t) f.ref.index] = { f.beat, f.value };
        });
    }

    void TimelineView::commitAutomationAdd()
    {
        if (ctx.project == nullptr || laneFor (pendingAddTrack, pendingAddParam) == nullptr)
            return;

        const auto trackId = pendingAddTrack;
        const auto parameterId = pendingAddParam;
        const double beat = pendingAddBeat;
        const float value = pendingAddValue;

        performProjectEdit (project(), TRANS ("Add automation point"),
                            [this, trackId, parameterId, beat, value]
        {
            if (auto* lane = laneFor (trackId, parameterId))
                automation::insertPoint (lane->points, beat, value);
        });

        // Leave the new point selected so it can be dragged straight away.
        selectedPoints.clear();
        if (auto* lane = laneFor (trackId, parameterId))
            for (int k = 0; k < (int) lane->points.size(); ++k)
                if (std::abs (lane->points[(size_t) k].first - beat) < 1.0e-6)
                    selectedPoints.push_back ({ trackId, parameterId, k });
    }

    bool TimelineView::deleteSelectedAutomationPoints()
    {
        if (ctx.project == nullptr || selectedPoints.empty())
            return false;

        const auto refs = selectedPoints;

        performProjectEdit (project(), TRANS ("Delete automation points"), [this, refs]
        {
            std::vector<PointRef> lanesDone;

            for (const auto& ref : refs)
            {
                if (std::any_of (lanesDone.begin(), lanesDone.end(), [&ref] (const PointRef& done)
                                 { return done.track == ref.track && done.parameterId == ref.parameterId; }))
                    continue;

                lanesDone.push_back (ref);

                std::vector<int> indices;
                for (const auto& other : refs)
                    if (other.track == ref.track && other.parameterId == ref.parameterId)
                        indices.push_back (other.index);

                // An emptied lane stays: no points means no automation, which is
                // exactly what the mixer already does with an empty lane.
                if (auto* lane = laneFor (ref.track, ref.parameterId))
                    automation::erasePoints (lane->points, indices);
            }
        });

        selectedPoints.clear();
        return true;
    }

    void TimelineView::toggleAutomationForSelectedTrack()
    {
        if (ui.selectedTrack == invalidTrackId)
            return;

        ui.setAutomationVisible (ui.selectedTrack, ! ui.isAutomationVisible (ui.selectedTrack));
    }

    //==============================================================================
    void TimelineView::showParameterMenu (int trackIndex, int laneIndex, juce::Point<int> where)
    {
        if (ctx.project == nullptr || ! juce::isPositiveAndBelow (trackIndex, project().getNumTracks()))
            return;

        auto& track = project().getTrack (trackIndex);
        const auto trackId = track.getId();
        const juce::String current = juce::isPositiveAndBelow (laneIndex, (int) track.automation.size())
                                       ? track.automation[(size_t) laneIndex].parameterId : juce::String();

        juce::PopupMenu menu;
        menu.addSectionHeader (laneIndex < 0 ? TRANS ("Automation lane")
                                             : laneLabel (track, current));

        std::vector<juce::String> ids;
        auto addParameter = [&] (juce::PopupMenu& target, const juce::String& id, const juce::String& label)
        {
            // A parameter already on another lane is off the menu; the same one
            // this lane is showing is ticked.
            auto* existing = laneFor (trackId, id);
            const bool taken = existing != nullptr && id != current;
            ids.push_back (id);
            target.addItem ((int) ids.size(), label, ! taken, id == current);
        };

        addParameter (menu, "gain", TRANS ("Gain"));
        addParameter (menu, "pan",  TRANS ("Pan"));
        addParameter (menu, "mute", TRANS ("Mute"));

        for (int slot = 0; slot < (int) track.builtinFx.size(); ++slot)
        {
            juce::PopupMenu sub;

            if (auto effect = createBuiltinEffect (track.builtinFx[(size_t) slot].type))
                for (const auto& info : effect->getParameterInfo())
                    addParameter (sub, "fx:" + juce::String (slot) + ":" + info.id, info.label);

            menu.addSubMenu (getBuiltinEffectDisplayName (track.builtinFx[(size_t) slot].type), sub);
        }

        for (int slot = 0; slot < (int) track.plugins.size(); ++slot)
        {
            juce::PopupMenu sub;

            // ponytail: the whole parameter list, flat. A plugin with hundreds
            // of them gets a very long menu - add a filter box if that bites.
            if (auto* instance = liveInstanceForSlot (track, slot))
            {
                const auto& params = instance->getParameters();
                for (int k = 0; k < params.size(); ++k)
                    addParameter (sub, "plugin:" + juce::String (slot) + ":" + juce::String (k),
                                  params[k]->getName (24));
            }

            if (sub.getNumItems() == 0)
                sub.addItem (30000, TRANS ("Not loaded"), false);

            menu.addSubMenu (track.plugins[(size_t) slot].displayName, sub);
        }

        if (laneIndex >= 0)
        {
            menu.addSeparator();
            menu.addItem (20001, TRANS ("Clear lane"));
            menu.addItem (20002, TRANS ("Remove lane"));
        }

        const auto screenPos = localPointToGlobal (where);
        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetComponent (this)
                                .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                            [this, trackId, laneIndex, ids] (int result)
        {
            if (result <= 0 || ctx.project == nullptr)
                return;

            if (result == 20001 || result == 20002)
            {
                const bool remove = result == 20002;
                performProjectEdit (project(), remove ? TRANS ("Remove lane") : TRANS ("Clear lane"),
                                    [this, trackId, laneIndex, remove]
                {
                    auto* track = project().findTrack (trackId);
                    if (track == nullptr || ! juce::isPositiveAndBelow (laneIndex, (int) track->automation.size()))
                        return;

                    if (remove) track->automation.erase (track->automation.begin() + laneIndex);
                    else        track->automation[(size_t) laneIndex].points.clear();
                });
                selectedPoints.clear();
                return;
            }

            if (result > (int) ids.size())
                return;

            const auto chosen = ids[(size_t) result - 1];

            performProjectEdit (project(), TRANS ("Automation lane"), [this, trackId, laneIndex, chosen]
            {
                auto* track = project().findTrack (trackId);
                if (track == nullptr)
                    return;

                // Re-pointing keeps the breakpoints: they are normalised 0..1,
                // so they mean the same thing on any parameter.
                if (juce::isPositiveAndBelow (laneIndex, (int) track->automation.size()))
                    track->automation[(size_t) laneIndex].parameterId = chosen;
                else
                    track->automation.push_back ({ chosen, {} });
            });

            selectedPoints.clear();
        });
    }

    //==============================================================================
    void TimelineView::mouseMove (const juce::MouseEvent& e)
    {
        auto cursor = juce::MouseCursor::NormalCursor;
        const auto ref = clipAt (e.getPosition());

        if (ref.clip != invalidClipId)
        {
            const auto r = rectForClip (ref);
            if (e.position.x > r.getRight() - edgeGrab || e.position.x < r.getX() + edgeGrab)
                cursor = juce::MouseCursor::LeftRightResizeCursor;
        }

        setMouseCursor (cursor);
    }

    void TimelineView::mouseDown (const juce::MouseEvent& e)
    {
        if (ctx.project == nullptr)
            return;

        const auto pos = e.getPosition();
        dragMoved = false;
        pendingAdd = false;

        if (rulerArea().contains (pos))
        {
            if (e.mods.isAltDown())
            {
                // Live during the drag, one undo entry on release.
                dragMode = DragMode::loop;
                dragStartBeat = ui.snap (beatForX (pos.x));
                loopStartBefore   = project().loopStartBeats;
                loopEndBefore     = project().loopEndBeats;
                loopEnabledBefore = project().loopEnabled;
                project().loopStartBeats = dragStartBeat;
                project().loopEndBeats   = dragStartBeat;
                project().loopEnabled    = true;
                repaint();
            }
            else
            {
                dragMode = DragMode::playhead;
                if (ctx.engine != nullptr)
                    ctx.engine->getTransport().setPositionBeats (juce::jmax (0.0, ui.snap (beatForX (pos.x))));
                repaint();
            }
            return;
        }

        // Automation rows sit under their track and stretch across the header
        // column too, so they get first refusal before the clip handling below.
        int automationTrack = -1, automationRow = -1;
        if (automationRowAt (pos, automationTrack, automationRow))
        {
            if (pos.x < headerWidth || laneArea().contains (pos))
                automationMouseDown (e, automationTrack, automationRow);
            return;
        }

        if (! selectedPoints.empty())
        {
            selectedPoints.clear();
            repaint();
        }

        if (! laneArea().contains (pos))
            return;

        const auto ref = clipAt (pos);

        if (e.mods.isPopupMenu())
        {
            if (ref.clip != invalidClipId)
            {
                if (! isSelected (ref)) selected = { ref };
                ui.select (ref.track, ref.clip, ref.isAudio);
                showClipMenu (ref, pos);
            }
            else
            {
                showEmptyLaneMenu (trackIndexAt (pos.y), ui.snap (beatForX (pos.x)), pos);
            }
            return;
        }

        if (ref.clip == invalidClipId)
        {
            if (! e.mods.isShiftDown()) selected.clear();
            dragMode = DragMode::marquee;
            dragStartPoint = pos;
            marquee = {};
            repaint();
            return;
        }

        if (e.mods.isShiftDown())
        {
            if (! isSelected (ref)) selected.push_back (ref);
        }
        else if (! isSelected (ref))
        {
            selected = { ref };
        }

        ui.select (ref.track, ref.clip, ref.isAudio);

        const auto r = rectForClip (ref);
        exportingClipDrag = e.mods.isCommandDown() && ! ref.isAudio;
        exportClipRef     = ref;
        dragMode = e.position.x < r.getX() + edgeGrab     ? DragMode::resizeStart
                 : e.position.x > r.getRight() - edgeGrab ? DragMode::resizeEnd
                                                          : DragMode::move;

        dragStartPoint      = pos;
        dragStartBeat       = beatForX (pos.x);
        dragStartTrackIndex = trackIndexAt (pos.y);

        dragOriginals.clear();
        for (const auto& sel : selected)
        {
            for (int i = 0; i < project().getNumTracks(); ++i)
            {
                auto& track = project().getTrack (i);
                if (track.getId() != sel.track) continue;

                if (sel.isAudio)
                {
                    if (auto* c = track.findAudioClip (sel.clip))
                        dragOriginals.push_back ({ sel, c->startBeats, c->lengthBeats, c->offsetSeconds, i });
                }
                else
                {
                    if (auto* c = track.findMidiClip (sel.clip))
                        dragOriginals.push_back ({ sel, c->startBeats, c->lengthBeats, 0.0, i });
                }
            }
        }

        repaint();
    }

    void TimelineView::mouseDrag (const juce::MouseEvent& e)
    {
        if (ctx.project == nullptr || dragMode == DragMode::none)
            return;

        if (exportingClipDrag && dragMode == DragMode::move && e.getDistanceFromDragStart() >= 8)
        {
            // Every mouseDrag call below the 8px threshold already ran the
            // ordinary move-preview code further down this function, which
            // writes straight into the live clips (not a shadow copy) - undo
            // that preview here so a Cmd-drag export leaves the ORIGINAL
            // clip completely untouched.  Restored directly, with no
            // performProjectEdit/undo entry: nothing should look like it
            // moved, because nothing should have, from the user's point of
            // view.  Mirrors the restore half of commitClipDrag() below, but
            // skips its re-apply-as-one-undoable-transaction step.
            for (const auto& original : dragOriginals)
            {
                auto* track = project().findTrack (original.ref.track);
                if (track == nullptr) continue;

                if (original.ref.isAudio)
                {
                    if (auto* c = track->findAudioClip (original.ref.clip))
                    {
                        c->startBeats    = original.startBeats;
                        c->lengthBeats   = original.lengthBeats;
                        c->offsetSeconds = original.offsetSeconds;
                    }
                }
                else
                {
                    if (auto* c = track->findMidiClip (original.ref.clip))
                    {
                        c->startBeats  = original.startBeats;
                        c->lengthBeats = original.lengthBeats;
                    }
                }
            }

            if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            {
                // exportClipRef, not selected.front() - a pre-existing
                // multi-selection's first entry can be a different clip (or
                // an audio clip) than the one actually under the cursor and
                // being dragged.
                if (auto* track = project().findTrack (exportClipRef.track))
                    if (auto* clip = track->findMidiClip (exportClipRef.clip))
                    {
                        ui.dragPayload = std::make_shared<MidiClip> (*clip);
                        container->startDragging ("ss.timelineClip", this);
                    }
            }

            exportingClipDrag = false;
            dragMode = DragMode::none;
            repaint();
            return;
        }

        if (dragMode == DragMode::playhead)
        {
            if (ctx.engine != nullptr)
                ctx.engine->getTransport().setPositionBeats (juce::jmax (0.0, ui.snap (beatForX (e.x))));
            repaint();
            return;
        }

        if (dragMode == DragMode::loop)
        {
            const double b = ui.snap (beatForX (e.x));
            project().loopStartBeats = juce::jmin (dragStartBeat, b);
            project().loopEndBeats   = juce::jmax (dragStartBeat, b);
            repaint();
            return;
        }

        if (dragMode == DragMode::automationPoint)
        {
            dragMoved = true;
            dragAutomationPoints (e);
            repaint();
            return;
        }

        if (dragMode == DragMode::automationMarquee)
        {
            // The press was going to add a point; moving turns it into a
            // rubber band over the breakpoints instead.
            dragMoved = true;
            pendingAdd = false;
            pointMarquee = juce::Rectangle<int> (dragStartPoint, e.getPosition());
            selectPointsIn (pointMarquee);
            repaint();
            return;
        }

        if (dragMode == DragMode::marquee)
        {
            marquee = juce::Rectangle<int> (dragStartPoint, e.getPosition());
            selected.clear();

            for (int i = 0; i < project().getNumTracks(); ++i)
            {
                auto& track = project().getTrack (i);
                for (const auto& c : track.audioClips)
                {
                    const ClipRef ref { track.getId(), c.id, true };
                    if (rectForClip (ref).toNearestInt().intersects (marquee)) selected.push_back (ref);
                }
                for (const auto& c : track.midiClips)
                {
                    const ClipRef ref { track.getId(), c.id, false };
                    if (rectForClip (ref).toNearestInt().intersects (marquee)) selected.push_back (ref);
                }
            }
            repaint();
            return;
        }

        if (dragOriginals.empty())
            return;

        dragMoved = true;

        const double rawDelta = beatForX (e.x) - dragStartBeat;
        const auto& first = dragOriginals.front();

        double deltaBeats = rawDelta;
        if (ui.snapEnabled)
        {
            const double reference = dragMode == DragMode::resizeEnd
                                       ? first.startBeats + first.lengthBeats : first.startBeats;
            deltaBeats = ui.snap (reference + rawDelta) - reference;
        }

        const double minLength = juce::jmax (0.125, quantiseStepInBeats (ui.grid));

        for (const auto& original : dragOriginals)
        {
            auto* track = project().findTrack (original.ref.track);
            if (track == nullptr) continue;

            auto applyTo = [&] (double& start, double& length, double* offsetSeconds)
            {
                switch (dragMode)
                {
                    case DragMode::move:
                        start = juce::jmax (0.0, original.startBeats + deltaBeats);
                        break;

                    case DragMode::resizeEnd:
                        length = juce::jmax (minLength, original.lengthBeats + deltaBeats);
                        break;

                    case DragMode::resizeStart:
                    {
                        const double newStart = juce::jlimit (0.0,
                                                              original.startBeats + original.lengthBeats - minLength,
                                                              original.startBeats + deltaBeats);
                        // Trimming the front of an audio clip has to walk the read
                        // offset along with it or the audio slides under the edit.
                        if (offsetSeconds != nullptr)
                            *offsetSeconds = juce::jmax (0.0, original.offsetSeconds
                                                                + project().tempo.beatsToSeconds (newStart)
                                                                - project().tempo.beatsToSeconds (original.startBeats));
                        length = original.startBeats + original.lengthBeats - newStart;
                        start  = newStart;
                        break;
                    }

                    default: break;
                }
            };

            if (original.ref.isAudio)
            {
                if (auto* c = track->findAudioClip (original.ref.clip))
                    applyTo (c->startBeats, c->lengthBeats, &c->offsetSeconds);
            }
            else
            {
                if (auto* c = track->findMidiClip (original.ref.clip))
                    applyTo (c->startBeats, c->lengthBeats, nullptr);
            }
        }

        repaint();
    }

    void TimelineView::mouseUp (const juce::MouseEvent& e)
    {
        // Cleared here unconditionally (rather than only in the branches that
        // reset dragMode) so a cmd-click that never crossed the drag threshold -
        // exportingClipDrag stays true, dragMode stays DragMode::move - can't
        // leak into the next, unrelated gesture.
        exportingClipDrag = false;

        if (dragMode == DragMode::automationPoint)
        {
            if (dragMoved)
                commitAutomationDrag();

            dragMode = DragMode::none;
            pointOriginals.clear();
            return;
        }

        if (dragMode == DragMode::automationMarquee)
        {
            pointMarquee = {};

            if (pendingAdd && ! dragMoved)
                commitAutomationAdd();

            pendingAdd = false;
            dragMode = DragMode::none;
            repaint();
            return;
        }

        if (dragMode == DragMode::marquee)
        {
            marquee = {};
            if (! selected.empty())
                ui.select (selected.front().track, selected.front().clip, selected.front().isAudio);
            dragMode = DragMode::none;
            repaint();
            return;
        }

        if (dragMode == DragMode::loop)
        {
            const double start = project().loopStartBeats, end = project().loopEndBeats;

            // Rewind, so the snapshot the undo action takes is the pre-drag state.
            project().loopStartBeats = loopStartBefore;
            project().loopEndBeats   = loopEndBefore;
            project().loopEnabled    = loopEnabledBefore;

            performProjectEdit (project(), TRANS ("Set loop"), [this, start, end]
            {
                project().loopStartBeats = start;
                project().loopEndBeats   = juce::jmax (end, start + 1.0);
                project().loopEnabled    = true;
            });
            dragMode = DragMode::none;
            return;
        }

        if (dragMode == DragMode::move && dragMoved)
        {
            const int targetIndex = trackIndexAt (e.y);
            const int delta = dragStartTrackIndex >= 0 && targetIndex >= 0
                                ? targetIndex - dragStartTrackIndex : 0;
            commitClipDrag (delta);

            if (delta != 0)
                selected.clear();
        }
        else if ((dragMode == DragMode::resizeEnd || dragMode == DragMode::resizeStart) && dragMoved)
        {
            commitClipDrag (0);
        }

        dragMode = DragMode::none;
        dragOriginals.clear();
    }

    void TimelineView::commitClipDrag (int trackDelta)
    {
        if (ctx.project == nullptr || dragOriginals.empty())
            return;

        // Capture the dragged result, rewind the document, then re-apply it as
        // one undoable transaction - position and lane change together, so one
        // press of undo puts the gesture back the way it was.
        struct Final { ClipRef ref; double start, length, offset; int sourceTrackIndex; };
        std::vector<Final> finals;

        for (const auto& original : dragOriginals)
        {
            auto* track = project().findTrack (original.ref.track);
            if (track == nullptr) continue;

            if (original.ref.isAudio)
            {
                if (auto* c = track->findAudioClip (original.ref.clip))
                {
                    finals.push_back ({ original.ref, c->startBeats, c->lengthBeats,
                                        c->offsetSeconds, original.trackIndex });
                    c->startBeats = original.startBeats;
                    c->lengthBeats = original.lengthBeats;
                    c->offsetSeconds = original.offsetSeconds;
                }
            }
            else
            {
                if (auto* c = track->findMidiClip (original.ref.clip))
                {
                    finals.push_back ({ original.ref, c->startBeats, c->lengthBeats,
                                        0.0, original.trackIndex });
                    c->startBeats = original.startBeats;
                    c->lengthBeats = original.lengthBeats;
                }
            }
        }

        if (finals.empty())
            return;

        performProjectEdit (project(), TRANS ("Edit clips"), [this, finals, trackDelta]
        {
            for (const auto& f : finals)
            {
                auto* track = project().findTrack (f.ref.track);
                if (track == nullptr) continue;

                if (f.ref.isAudio)
                {
                    if (auto* c = track->findAudioClip (f.ref.clip))
                    {
                        c->startBeats = f.start; c->lengthBeats = f.length; c->offsetSeconds = f.offset;
                    }
                }
                else
                {
                    if (auto* c = track->findMidiClip (f.ref.clip))
                    {
                        c->startBeats = f.start; c->lengthBeats = f.length;
                    }
                }

                if (trackDelta == 0)
                    continue;

                // Cross-lane drop: move the clip struct into the target track,
                // but only where the track types match.
                const int destIndex = f.sourceTrackIndex + trackDelta;
                if (destIndex < 0 || destIndex >= project().getNumTracks())
                    continue;

                auto& dest = project().getTrack (destIndex);
                if (track->getId() == dest.getId() || track->getType() != dest.getType())
                    continue;

                if (f.ref.isAudio)
                {
                    auto it = std::find_if (track->audioClips.begin(), track->audioClips.end(),
                                            [&f] (const AudioClip& c) { return c.id == f.ref.clip; });
                    if (it == track->audioClips.end()) continue;
                    const auto copy = *it;
                    track->audioClips.erase (it);
                    dest.audioClips.push_back (copy);
                }
                else
                {
                    auto it = std::find_if (track->midiClips.begin(), track->midiClips.end(),
                                            [&f] (const MidiClip& c) { return c.id == f.ref.clip; });
                    if (it == track->midiClips.end()) continue;
                    const auto copy = *it;
                    track->midiClips.erase (it);
                    dest.midiClips.push_back (copy);
                }
            }
        });
    }

    void TimelineView::mouseDoubleClick (const juce::MouseEvent& e)
    {
        int automationTrack = -1, automationRow = -1;
        if (automationRowAt (e.getPosition(), automationTrack, automationRow))
        {
            auto& track = project().getTrack (automationTrack);

            if (e.getPosition().x >= headerWidth
                 && automationRow < (int) track.automation.size())
            {
                const auto& lane = track.automation[(size_t) automationRow];
                const auto plot = automationRowRect (automationTrack, automationRow)
                                    .reduced (0, automationRowInset);
                const int hit = pointIndexAt (lane, plot, e.getPosition());

                if (hit >= 0)
                {
                    selectedPoints = { { track.getId(), lane.parameterId, hit } };
                    deleteSelectedAutomationPoints();
                }
            }

            return;
        }

        const auto ref = clipAt (e.getPosition());
        if (ref.clip == invalidClipId)
            return;

        ui.select (ref.track, ref.clip, ref.isAudio);
        ui.goTo (ref.isAudio ? MainComponent::View::timeline : MainComponent::View::pianoRoll);
    }

    void TimelineView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        if (e.mods.isCommandDown())
        {
            const double beatUnderMouse = beatForX (e.x);
            pixelsPerBeat = juce::jlimit (1.5, 400.0, pixelsPerBeat * (1.0 + wheel.deltaY * 0.6));
            scrollBeats = juce::jmax (0.0, beatUnderMouse - (e.x - laneArea().getX()) / pixelsPerBeat);
        }
        else if (e.mods.isShiftDown())
        {
            scrollBeats = juce::jmax (0.0, scrollBeats - wheel.deltaY * 8.0);
        }
        else
        {
            scrollY = juce::jmax (0, scrollY - (int) std::round (wheel.deltaY * 60.0f));
        }

        layOutHeaders();
        updateScrollRanges();
        repaint();
    }

    //==============================================================================
    void TimelineView::splitSelectionAtPlayhead()
    {
        if (ctx.project == nullptr || ctx.engine == nullptr || selected.empty())
            return;

        const double at = ctx.engine->getTransport().getPositionBeats();
        auto refs = selected;

        performProjectEdit (project(), TRANS ("Split clip"), [this, refs, at]
        {
            for (const auto& ref : refs)
            {
                auto* track = project().findTrack (ref.track);
                if (track == nullptr) continue;

                if (ref.isAudio)
                {
                    auto* c = track->findAudioClip (ref.clip);
                    if (c == nullptr || at <= c->startBeats + 1.0e-6 || at >= c->endBeats() - 1.0e-6) continue;

                    AudioClip right = *c;
                    right.id = project().nextClipId();
                    right.startBeats = at;
                    right.lengthBeats = c->endBeats() - at;
                    right.offsetSeconds = c->offsetSeconds
                                            + project().tempo.beatsToSeconds (at)
                                            - project().tempo.beatsToSeconds (c->startBeats);
                    right.fadeInSec = 0.0;

                    c->lengthBeats = at - c->startBeats;
                    c->fadeOutSec  = 0.0;
                    track->audioClips.push_back (right);
                }
                else
                {
                    auto* c = track->findMidiClip (ref.clip);
                    if (c == nullptr || at <= c->startBeats + 1.0e-6 || at >= c->endBeats() - 1.0e-6) continue;

                    const double cut = at - c->startBeats;

                    MidiClip right;
                    right.id = project().nextClipId();
                    right.name = c->name;
                    right.startBeats = at;
                    right.lengthBeats = c->endBeats() - at;

                    std::vector<Note> left;
                    for (const auto& n : c->notes)
                    {
                        if (n.startBeats >= cut)
                        {
                            auto moved = n;
                            moved.startBeats -= cut;
                            right.notes.push_back (moved);
                        }
                        else
                        {
                            auto kept = n;
                            kept.lengthBeats = juce::jmin (kept.lengthBeats, cut - kept.startBeats);
                            if (kept.lengthBeats > 1.0e-4) left.push_back (kept);
                        }
                    }

                    c->notes = std::move (left);
                    c->lengthBeats = cut;
                    track->midiClips.push_back (right);
                }
            }
        });
    }

    //==============================================================================
    void TimelineView::showClipMenu (const ClipRef& ref, juce::Point<int> where)
    {
        juce::PopupMenu menu;

        if (ref.isAudio)
        {
            // The spec calls this the link that makes the AI loop feel native (9.2).
            menu.addItem (1, TRANS ("Transcribe this range"));
            menu.addItem (2, TRANS ("Edit waveform..."));
            menu.addSeparator();
        }
        else
        {
            menu.addItem (3, TRANS ("Edit in piano roll"));
            menu.addItem (4, TRANS ("Generate from this clip"));
            menu.addSeparator();
        }

        menu.addItem (5, TRANS ("Split at playhead"));
        menu.addItem (6, TRANS ("Duplicate"));
        menu.addItem (7, TRANS ("Delete"));

        const auto screenPos = localPointToGlobal (where);
        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetComponent (this)
                                .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                            [this, ref] (int result)
        {
            switch (result)
            {
                case 1: requestTranscribe (ref, true); break;

                // The waveform editor is a window rather than a workspace view -
                // MainComponent::View has no entry for it and the header is frozen.
                case 2: ui.select (ref.track, ref.clip, true);
                        WaveformEditorView::launch (ctx, ui);
                        break;

                case 3: ui.goTo (MainComponent::View::pianoRoll); break;
                case 4: ui.goTo (MainComponent::View::generate); break;
                case 5: splitSelectionAtPlayhead(); break;

                case 6:
                    performProjectEdit (project(), TRANS ("Duplicate clip"), [this, ref]
                    {
                        auto* track = project().findTrack (ref.track);
                        if (track == nullptr) return;

                        if (ref.isAudio)
                        {
                            if (auto* c = track->findAudioClip (ref.clip))
                            {
                                auto copy = *c;
                                copy.id = project().nextClipId();
                                copy.startBeats = c->endBeats();
                                track->audioClips.push_back (copy);
                            }
                        }
                        else if (auto* c = track->findMidiClip (ref.clip))
                        {
                            auto copy = *c;
                            copy.id = project().nextClipId();
                            copy.startBeats = c->endBeats();
                            track->midiClips.push_back (copy);
                        }
                    });
                    break;

                case 7:
                    performProjectEdit (project(), TRANS ("Delete clip"), [this, ref]
                    {
                        auto* track = project().findTrack (ref.track);
                        if (track == nullptr) return;

                        if (ref.isAudio)
                            track->audioClips.erase (std::remove_if (track->audioClips.begin(),
                                                                     track->audioClips.end(),
                                                                     [&] (const AudioClip& c) { return c.id == ref.clip; }),
                                                     track->audioClips.end());
                        else
                            track->midiClips.erase (std::remove_if (track->midiClips.begin(),
                                                                    track->midiClips.end(),
                                                                    [&] (const MidiClip& c) { return c.id == ref.clip; }),
                                                    track->midiClips.end());
                    });
                    selected.clear();
                    break;

                default: break;
            }
        });
    }

    void TimelineView::showEmptyLaneMenu (int trackIndex, double beat, juce::Point<int> where)
    {
        juce::PopupMenu menu;
        menu.addItem (1, TRANS ("Import file here..."), trackIndex >= 0);
        menu.addItem (2, TRANS ("New MIDI clip here"), trackIndex >= 0
                                                        && ctx.project != nullptr
                                                        && project().getTrack (trackIndex).getType() == TrackType::midi);
        menu.addSeparator();
        menu.addItem (3, TRANS ("Add marker here"));

        const auto screenPos = localPointToGlobal (where);
        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetComponent (this)
                                .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                            [this, trackIndex, beat] (int result)
        {
            if (result == 1)
            {
                auto extensions = io::getSupportedAudioExtensions();
                extensions.addArray (juce::StringArray { "*.mid", "*.midi" });

                chooser = std::make_unique<juce::FileChooser> (TRANS ("Import file"), juce::File(),
                                                               extensions.joinIntoString (";"));
                chooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectFiles
                                        | juce::FileBrowserComponent::canSelectMultipleItems,
                                      [this, trackIndex, beat] (const juce::FileChooser& fc)
                {
                    juce::StringArray paths;
                    for (const auto& f : fc.getResults())
                        paths.add (f.getFullPathName());
                    if (! paths.isEmpty())
                        importFiles (paths, trackIndex, beat);
                });
            }
            else if (result == 2 && trackIndex >= 0 && trackIndex < project().getNumTracks())
            {
                performProjectEdit (project(), TRANS ("New MIDI clip"), [this, trackIndex, beat]
                {
                    auto& track = project().getTrack (trackIndex);
                    MidiClip clip;
                    clip.id = project().nextClipId();
                    clip.name = TRANS ("MIDI");
                    clip.startBeats = beat;
                    clip.lengthBeats = 8.0;
                    track.midiClips.push_back (clip);
                });
            }
            else if (result == 3)
            {
                performProjectEdit (project(), TRANS ("Add marker"), [this, beat]
                {
                    project().markers.push_back ({ beat, TRANS ("Marker") });
                });
            }
        });
    }

    void TimelineView::requestTranscribe (const ClipRef& ref, bool)
    {
        if (ctx.project == nullptr)
            return;

        auto* track = project().findTrack (ref.track);
        if (track == nullptr) return;

        auto* clip = track->findAudioClip (ref.clip);
        if (clip == nullptr) return;

        const auto& tempo = project().tempo;

        UiState::TranscribeRequest request;
        request.valid         = true;
        request.file          = clip->sourceFile;
        request.offsetSeconds = clip->offsetSeconds;
        request.lengthSeconds = tempo.beatsToSeconds (clip->endBeats()) - tempo.beatsToSeconds (clip->startBeats);
        request.placeAtBeat   = clip->startBeats;
        request.clipName      = clip->name.isNotEmpty() ? clip->name
                                                        : clip->sourceFile.getFileNameWithoutExtension();

        ui.transcribeRequest = request;
        ui.select (ref.track, ref.clip, true);
        ui.sendChangeMessage();
        ui.goTo (MainComponent::View::transcribe);
    }

    //==============================================================================
    bool TimelineView::isInterestedInFileDrag (const juce::StringArray& files)
    {
        auto extensions = io::getSupportedAudioExtensions();
        extensions.addArray (juce::StringArray { "mid", "midi" });

        // FileIO may hand these back as ".wav" or "*.wav" - normalise both.
        for (const auto& f : files)
            for (auto e : extensions)
            {
                e = e.trim();
                if (e.startsWith ("*")) e = e.substring (1);
                if (! e.startsWith (".")) e = "." + e;
                if (f.endsWithIgnoreCase (e))
                    return true;
            }

        return false;
    }

    void TimelineView::filesDropped (const juce::StringArray& files, int x, int y)
    {
        importFiles (files, trackIndexAt (y), juce::jmax (0.0, ui.snap (beatForX (x))));
    }

    void TimelineView::importFiles (const juce::StringArray& files, int trackIndex, double atBeat)
    {
        if (ctx.project == nullptr || files.isEmpty())
            return;

        performProjectEdit (project(), TRANS ("Import files"), [this, files, trackIndex, atBeat]
        {
            for (const auto& path : files)
            {
                const juce::File file (path);
                juce::String error;

                if (file.hasFileExtension ("mid;midi"))
                {
                    io::importMidiFile (file, project(), error);
                    continue;
                }

                // Land on the lane it was dropped on when the type matches,
                // otherwise make a new track rather than silently retargeting.
                TrackId target = invalidTrackId;
                if (trackIndex >= 0 && trackIndex < project().getNumTracks()
                     && project().getTrack (trackIndex).getType() == TrackType::audio)
                    target = project().getTrack (trackIndex).getId();

                if (target == invalidTrackId)
                    target = project().addTrack (TrackType::audio, file.getFileNameWithoutExtension()).getId();

                if (ctx.engine != nullptr)
                    io::importAudioFile (file, project(), target, atBeat,
                                         ctx.engine->getFormatManager(), error);
            }
        });
    }

    bool TimelineView::isInterestedInDragSource (const SourceDetails& details)
    {
        return details.description.toString() == "ss.candidate" && ui.dragPayload != nullptr;
    }

    void TimelineView::itemDropped (const SourceDetails& details)
    {
        if (ctx.project == nullptr || ui.dragPayload == nullptr)
            return;

        const int trackIndex = trackIndexAt (details.localPosition.y);
        const double beat = juce::jmax (0.0, ui.snap (beatForX (details.localPosition.x)));
        auto payload = *ui.dragPayload;

        performProjectEdit (project(), TRANS ("Adopt candidate"), [this, payload, trackIndex, beat]
        {
            TrackId target = invalidTrackId;
            if (trackIndex >= 0 && trackIndex < project().getNumTracks()
                 && project().getTrack (trackIndex).getType() == TrackType::midi)
                target = project().getTrack (trackIndex).getId();

            if (target == invalidTrackId)
                target = project().addTrack (TrackType::midi,
                                             payload.name.isNotEmpty() ? payload.name
                                                                       : TRANS ("Generated")).getId();

            if (auto* track = project().findTrack (target))
            {
                auto clip = payload;
                clip.id = project().nextClipId();
                clip.startBeats = beat;
                track->midiClips.push_back (clip);
            }
        });
    }
}
