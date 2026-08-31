#include "UI/MixerView.h"
#include "Engine/AudioEngine.h"
#include "Mixer/Mixer.h"
#include "Plugins/PluginManager.h"
#include "Plugins/BasicSynth.h"
#include <algorithm>
#include <cmath>

namespace ss
{
    namespace
    {
        constexpr int stripWidth   = 122;
        constexpr int slotHeight   = 19;
        constexpr int maxSlotRows  = 6;

        /** The built-in chain a (trackId, busId) pair names.  One lookup for the
            master chain, a bus chain and a track chain keeps every FX menu, every
            editor and every undo transaction on a single code path. */
        std::vector<BuiltinFxSlot>* chainIn (Project& p, TrackId trackId, int busId)
        {
            if (busId != 0)
            {
                auto* b = p.findBus (busId);
                return b != nullptr ? &b->builtinFx : nullptr;
            }

            if (trackId == invalidTrackId)
                return &p.masterChain;

            auto* t = p.findTrack (trackId);
            return t != nullptr ? &t->builtinFx : nullptr;
        }

        int busIndexOf (Project& p, int busId)
        {
            for (int i = 0; i < (int) p.buses.size(); ++i)
                if (p.buses[(size_t) i].id == busId)
                    return i;

            return -1;
        }
    }

    //==============================================================================
    BuiltinFxEditor::BuiltinFxEditor (AppContext& c, TrackId t, int slotIndex, int busIdToUse)
        : ctx (c), trackId (t), index (slotIndex), busId (busIdToUse)
    {
        auto* s = slot();
        if (s == nullptr)
            return;

        titleLabel.setText (getBuiltinEffectDisplayName (s->type), juce::dontSendNotification);
        titleLabel.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        addAndMakeVisible (titleLabel);

        bypassButton.setButtonText (TRANS ("Bypass"));
        bypassButton.setToggleState (s->bypassed, juce::dontSendNotification);
        bypassButton.onClick = [this]
        {
            if (auto* sl = slot()) sl->bypassed = bypassButton.getToggleState();
            pushToEffect();
            commit();
        };
        addAndMakeVisible (bypassButton);

        // The parameter list comes from the effect itself, so adding a new
        // built-in effect needs no UI work at all.
        if (auto effect = createBuiltinEffect (s->type))
            info = effect->getParameterInfo();

        for (const auto& param : info)
        {
            auto* label = labels.add (new juce::Label ({}, TRANS (param.label)));
            label->setFont (juce::Font (juce::FontOptions (12.0f)));
            label->setColour (juce::Label::textColourId, palette().textDim);
            addAndMakeVisible (label);

            auto* slider = sliders.add (new juce::Slider (juce::Slider::LinearHorizontal,
                                                          juce::Slider::TextBoxRight));
            slider->setRange (param.min, param.max, (param.max - param.min) / 1000.0);
            if (param.logarithmic && param.min > 0.0f)
                slider->setSkewFactorFromMidPoint (std::sqrt ((double) param.min * (double) param.max));
            slider->setTextValueSuffix (param.suffix.isNotEmpty() ? " " + param.suffix : juce::String());
            slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
            slider->setValue ((double) s->params.getWithDefault (juce::Identifier (param.id),
                                                                 param.defaultValue),
                              juce::dontSendNotification);
            slider->onValueChange = [this] { pushToEffect(); };
            slider->onDragEnd     = [this] { commit(); };
            addAndMakeVisible (slider);
        }

        setSize (360, 56 + (int) info.size() * 30);
    }

    BuiltinFxSlot* BuiltinFxEditor::slot() const
    {
        if (ctx.project == nullptr)
            return nullptr;

        auto* chain = chainIn (*ctx.project, trackId, busId);

        return (chain != nullptr && index >= 0 && index < (int) chain->size())
                 ? &(*chain)[(size_t) index] : nullptr;
    }

    void BuiltinFxEditor::pushToEffect()
    {
        auto* s = slot();
        if (s == nullptr)
            return;

        for (int i = 0; i < (int) info.size() && i < sliders.size(); ++i)
            s->params.set (juce::Identifier (info[(size_t) i].id), (float) sliders[i]->getValue());

        if (ctx.engine == nullptr || ctx.project == nullptr)
            return;

        auto& mixer = ctx.engine->getMixer();
        BuiltinEffect* effect = nullptr;

        if (busId != 0)
        {
            if (auto* strip = mixer.getBusStrip (busIndexOf (*ctx.project, busId)))
                effect = strip->getBuiltinEffect (index);
        }
        else if (trackId == invalidTrackId)
        {
            effect = mixer.getMasterEffect (index);
        }
        else if (auto* strip = mixer.getStripForTrack (trackId))
        {
            effect = strip->getBuiltinEffect (index);
        }

        // Live update so the change is audible before it is committed.
        if (effect != nullptr)
        {
            effect->setParameters (s->params);
            effect->bypassed = s->bypassed;
        }
    }

    void BuiltinFxEditor::commit()
    {
        if (ctx.project == nullptr)
            return;

        auto* s = slot();
        if (s == nullptr)
            return;

        const auto stateCopy = *s;
        auto* p = ctx.project.get();
        const auto id = trackId;
        const auto bus = busId;
        const int slotIndex = index;

        performProjectEdit (*p, TRANS ("Change effect"), [p, id, bus, slotIndex, stateCopy]
        {
            auto* chain = chainIn (*p, id, bus);

            if (chain != nullptr && slotIndex >= 0 && slotIndex < (int) chain->size())
                (*chain)[(size_t) slotIndex] = stateCopy;
        });
    }

    void BuiltinFxEditor::paint (juce::Graphics& g)
    {
        g.fillAll (palette().panelBg);
    }

    void BuiltinFxEditor::resized()
    {
        auto area = getLocalBounds().reduced (10, 8);
        auto header = area.removeFromTop (24);
        bypassButton.setBounds (header.removeFromRight (90));
        titleLabel.setBounds (header);
        area.removeFromTop (8);

        for (int i = 0; i < sliders.size(); ++i)
        {
            auto row = area.removeFromTop (26);
            labels[i]->setBounds (row.removeFromLeft (110));
            sliders[i]->setBounds (row);
            area.removeFromTop (4);
        }
    }

    //==============================================================================
    MixerStrip::MixerStrip (AppContext& c, UiState& s, MixerView& o, TrackId t, int busIdToUse)
        : ctx (c), ui (s), owner (o), trackId (t), busId (busIdToUse)
    {
        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        nameLabel.setEditable (false, ! isMaster(), false);
        nameLabel.onTextChange = [this]
        {
            const auto newName = nameLabel.getText();

            if (isBus()) commitBus (TRANS ("Rename bus"),   [newName] (Bus& b)   { b.name = newName; });
            else         commit    (TRANS ("Rename track"), [newName] (Track& tr) { tr.name = newName; });
        };
        addAndMakeVisible (nameLabel);

        valueLabel.setJustificationType (juce::Justification::centred);
        valueLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
        valueLabel.setColour (juce::Label::textColourId, palette().textDim);
        addAndMakeVisible (valueLabel);

        fader.setRange (-60.0, 12.0, 0.1);
        fader.setDoubleClickReturnValue (true, 0.0);
        fader.onValueChange = [this]
        {
            if (updating) return;
            const float db = (float) fader.getValue();
            valueLabel.setText (formatDb (db), juce::dontSendNotification);

            if (isMaster())
            {
                if (ctx.engine != nullptr) ctx.engine->getMixer().setMasterGainDb (db);
            }
            else if (auto* live = liveStrip())
            {
                live->setGainDb (db);
            }
        };
        fader.onDragEnd = [this]
        {
            const float db = (float) fader.getValue();

            if (isBus())         commitBus (TRANS ("Change gain"), [db] (Bus& b)    { b.gainDb = db; });
            else if (! isMaster()) commit  (TRANS ("Change gain"), [db] (Track& tr) { tr.gainDb = db; });
        };
        addAndMakeVisible (fader);

        if (! isMaster())
        {
            panKnob.setRange (-1.0, 1.0, 0.01);
            panKnob.setDoubleClickReturnValue (true, 0.0);
            panKnob.setTooltip (TRANS ("Pan"));
            panKnob.onValueChange = [this]
            {
                if (updating) return;
                const float v = (float) panKnob.getValue();
                panValueLabel.setText (formatPan (v), juce::dontSendNotification);
                if (auto* live = liveStrip()) live->setPan (v);
            };
            panKnob.onDragEnd = [this]
            {
                const float v = (float) panKnob.getValue();

                if (isBus()) commitBus (TRANS ("Change pan"), [v] (Bus& b)    { b.pan = v; });
                else         commit    (TRANS ("Change pan"), [v] (Track& tr) { tr.pan = v; });
            };
            addAndMakeVisible (panKnob);

            panValueLabel.setJustificationType (juce::Justification::centred);
            panValueLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
            panValueLabel.setColour (juce::Label::textColourId, palette().textDim);
            addAndMakeVisible (panValueLabel);

            muteButton.setButtonText ("M");
            muteButton.setClickingTogglesState (true);
            muteButton.setColour (juce::TextButton::buttonOnColourId, palette().warning);
            muteButton.onClick = [this]
            {
                const bool v = muteButton.getToggleState();
                if (auto* live = liveStrip()) live->setMuted (v);

                if (isBus()) commitBus (TRANS ("Mute bus"),   [v] (Bus& b)    { b.muted = v; });
                else         commit    (TRANS ("Mute track"), [v] (Track& tr) { tr.muted = v; });
            };
            addAndMakeVisible (muteButton);
        }

        if (! isMaster() && ! isBus())
        {
            soloButton.setButtonText ("S");
            soloButton.setClickingTogglesState (true);
            soloButton.setColour (juce::TextButton::buttonOnColourId, palette().accent);
            soloButton.onClick = [this]
            {
                const bool v = soloButton.getToggleState();
                commit (TRANS ("Solo track"), [v] (Track& tr) { tr.soloed = v; });
            };
            addAndMakeVisible (soloButton);

            outButton.setTooltip (TRANS ("Output routing, sends and input monitoring"));
            outButton.onClick = [this] { showOutputMenu(); };
            addAndMakeVisible (outButton);
        }
        else
        {
            loudnessLabel.setJustificationType (juce::Justification::centred);
            loudnessLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
            loudnessLabel.setColour (juce::Label::textColourId, palette().textDim);
            addAndMakeVisible (loudnessLabel);
        }

        addFxButton.setButtonText ("+");
        addFxButton.setTooltip (TRANS ("Add an effect"));
        addFxButton.onClick = [this] { showAddFxMenu(); };
        addAndMakeVisible (addFxButton);

        refresh();
    }

    Track* MixerStrip::track() const
    {
        return (! isMaster() && ! isBus() && ctx.project != nullptr) ? ctx.project->findTrack (trackId)
                                                                    : nullptr;
    }

    Bus* MixerStrip::bus() const
    {
        return (isBus() && ctx.project != nullptr) ? ctx.project->findBus (busId) : nullptr;
    }

    int MixerStrip::busIndex() const
    {
        return (isBus() && ctx.project != nullptr) ? busIndexOf (*ctx.project, busId) : -1;
    }

    ChannelStrip* MixerStrip::liveStrip() const
    {
        if (ctx.engine == nullptr || isMaster())
            return nullptr;

        auto& mixer = ctx.engine->getMixer();
        return isBus() ? mixer.getBusStrip (busIndex()) : mixer.getStripForTrack (trackId);
    }

    void MixerStrip::commitBus (const juce::String& name, std::function<void (Bus&)> fn)
    {
        if (ctx.project == nullptr || ! isBus())
            return;

        auto* p = ctx.project.get();
        const auto id = busId;
        performProjectEdit (*p, name, [p, id, fn]
        {
            if (auto* b = p->findBus (id)) fn (*b);
        });
    }

    void MixerStrip::commitChain (const juce::String& name,
                                  std::function<void (std::vector<BuiltinFxSlot>&)> fn)
    {
        if (ctx.project == nullptr)
            return;

        auto* p = ctx.project.get();
        const auto id = trackId;
        const auto bus = busId;

        performProjectEdit (*p, name, [p, id, bus, fn]
        {
            if (auto* chain = chainIn (*p, id, bus)) fn (*chain);
        });
    }

    void MixerStrip::commit (const juce::String& name, std::function<void (Track&)> fn)
    {
        if (ctx.project == nullptr || isMaster() || isBus())
            return;

        auto* p = ctx.project.get();
        const auto id = trackId;
        performProjectEdit (*p, name, [p, id, fn]
        {
            if (auto* t = p->findTrack (id)) fn (*t);
        });
    }

    void MixerStrip::refresh()
    {
        const juce::ScopedValueSetter<bool> guard (updating, true);

        if (isMaster())
        {
            nameLabel.setText (TRANS ("Master"), juce::dontSendNotification);
            if (ctx.engine != nullptr)
                fader.setValue (ctx.engine->getMixer().getMasterGainDb(), juce::dontSendNotification);
        }
        else if (auto* b = bus())
        {
            nameLabel.setText (b->name, juce::dontSendNotification);
            fader.setValue (b->gainDb, juce::dontSendNotification);
            panKnob.setValue (b->pan, juce::dontSendNotification);
            panValueLabel.setText (formatPan (b->pan), juce::dontSendNotification);
            muteButton.setToggleState (b->muted, juce::dontSendNotification);
        }
        else if (auto* t = track())
        {
            nameLabel.setText (t->name, juce::dontSendNotification);
            fader.setValue (t->gainDb, juce::dontSendNotification);
            panKnob.setValue (t->pan, juce::dontSendNotification);
            panValueLabel.setText (formatPan (t->pan), juce::dontSendNotification);
            muteButton.setToggleState (t->muted, juce::dontSendNotification);
            soloButton.setToggleState (t->soloed, juce::dontSendNotification);

            const auto* destination = ctx.project != nullptr ? ctx.project->findBus (t->outputBus)
                                                             : nullptr;
            outButton.setButtonText (destination != nullptr ? destination->name
                                                            : TRANS ("Master"));
        }

        valueLabel.setText (formatDb ((float) fader.getValue()), juce::dontSendNotification);
        rebuildSlotButtons();
        resized();
        repaint();
    }

    void MixerStrip::rebuildSlotButtons()
    {
        slotButtons.clear();
        slotRefs.clear();

        auto addSlotButton = [this] (const juce::String& text, bool builtin, int index, bool bypassed)
        {
            auto* b = slotButtons.add (new juce::TextButton (text));
            b->setColour (juce::TextButton::buttonColourId,
                          bypassed ? palette().panelBg : palette().panelAltBg);
            b->setColour (juce::TextButton::textColourOffId,
                          bypassed ? palette().textDim : palette().text);
            const int buttonIndex = slotButtons.size() - 1;
            b->onClick = [this, buttonIndex] { showSlotMenu (buttonIndex); };
            addAndMakeVisible (b);
            slotRefs.push_back ({ builtin, index });
        };

        if (ctx.project != nullptr)
            if (auto* chain = chainIn (*ctx.project, trackId, busId))
                for (int i = 0; i < (int) chain->size(); ++i)
                    addSlotButton (getBuiltinEffectDisplayName ((*chain)[(size_t) i].type), true, i,
                                   (*chain)[(size_t) i].bypassed);

        if (auto* t = track())
            for (int i = 0; i < (int) t->plugins.size(); ++i)
                addSlotButton (t->plugins[(size_t) i].displayName, false, i,
                               t->plugins[(size_t) i].bypassed);
    }

    /*  Output bus, sends and input monitoring for one track (spec 8.4.3, 8.4.5).
        A menu rather than three more controls: the strip is 122 px wide, and
        routing is set once and then left alone.                                  */
    void MixerStrip::showOutputMenu()
    {
        auto* t = track();

        if (t == nullptr || ctx.project == nullptr)
            return;

        const auto& buses = ctx.project->buses;

        juce::PopupMenu menu;
        menu.addSectionHeader (TRANS ("Output"));
        menu.addItem (1, TRANS ("Master"), true, t->outputBus == 0);

        for (int i = 0; i < (int) buses.size(); ++i)
            menu.addItem (100 + i, buses[(size_t) i].name, true, t->outputBus == buses[(size_t) i].id);

        if (! buses.empty())
        {
            menu.addSectionHeader (TRANS ("Sends"));

            for (int i = 0; i < (int) buses.size(); ++i)
            {
                float level = 0.0f;

                for (const auto& send : t->sends)
                    if (send.busId == buses[(size_t) i].id)
                        level = send.level;

                juce::PopupMenu levels;
                const int percentages[] = { 0, 25, 50, 75, 100 };

                for (int k = 0; k < 5; ++k)
                    levels.addItem (200 + i * 10 + k, juce::String (percentages[k]) + "%", true,
                                    juce::roundToInt (level * 100.0f) == percentages[k]);

                menu.addSubMenu (buses[(size_t) i].name, levels);
            }
        }

        menu.addSeparator();
        menu.addItem (2, TRANS ("Input monitoring"), t->getType() == TrackType::audio,
                      t->inputMonitoring);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&outButton),
                            [this, buses] (int result)
        {
            if (result == 1)
            {
                commit (TRANS ("Change output"), [] (Track& tr) { tr.outputBus = 0; });
            }
            else if (result == 2)
            {
                commit (TRANS ("Input monitoring"),
                        [] (Track& tr) { tr.inputMonitoring = ! tr.inputMonitoring; });
            }
            else if (result >= 200)
            {
                const int i = (result - 200) / 10;
                const int k = (result - 200) % 10;

                if (i >= (int) buses.size())
                    return;

                const int id = buses[(size_t) i].id;
                const float level = 0.25f * (float) k;

                commit (TRANS ("Change send"), [id, level] (Track& tr)
                {
                    for (auto& send : tr.sends)
                        if (send.busId == id)
                        {
                            send.level = level;
                            return;
                        }

                    if (level > 0.0f)
                        tr.sends.push_back ({ id, level });
                });
            }
            else if (result >= 100)
            {
                const int i = result - 100;

                if (i < (int) buses.size())
                {
                    const int id = buses[(size_t) i].id;
                    commit (TRANS ("Change output"), [id] (Track& tr) { tr.outputBus = id; });
                }
            }
        });
    }

    void MixerStrip::showAddFxMenu()
    {
        juce::PopupMenu menu;

        juce::PopupMenu builtins;
        const auto types = getBuiltinEffectTypes();
        for (int i = 0; i < types.size(); ++i)
            builtins.addItem (1000 + i, getBuiltinEffectDisplayName (types[i]));
        menu.addSubMenu (TRANS ("Built-in effects"), builtins);

        juce::Array<juce::PluginDescription> descriptions;
        if (ctx.plugins != nullptr && ! isMaster() && ! isBus())
        {
            juce::PopupMenu instruments;
            instruments.addItem (3000, "KANADE DAW Basic Synth");
            menu.addSubMenu (TRANS ("Built-in instruments"), instruments);

            juce::PopupMenu pluginMenu;
            const auto scanned = ctx.plugins->getKnownPluginList().getTypes();
            const auto pinned = ctx.settings != nullptr ? ctx.settings->getPinnedPlugins() : juce::StringArray();

            // Pinned plugins first (most-recently-pinned first), then everything
            // else in scan order - descriptions is rebuilt in this same order so
            // the popup result index (2000 + i) always lines up with it. A pinned
            // identifier with no matching scanned plugin (e.g. since uninstalled)
            // is simply skipped, so pinnedCount below is the number that actually
            // matched rather than pinned.size().
            for (const auto& id : pinned)
                for (const auto& d : scanned)
                    if (d.createIdentifierString() == id)
                        descriptions.add (d);

            const int pinnedCount = descriptions.size();

            for (const auto& d : scanned)
                if (! pinned.contains (d.createIdentifierString()))
                    descriptions.add (d);

            if (pinnedCount > 0)
                pluginMenu.addSectionHeader (TRANS ("Favourites"));

            for (int i = 0; i < descriptions.size(); ++i)
            {
                if (i == pinnedCount && pinnedCount > 0)
                    pluginMenu.addSectionHeader (TRANS ("All plugins"));

                pluginMenu.addItem (2000 + i, descriptions[i].name + "  (" + descriptions[i].pluginFormatName + ")");
            }

            if (descriptions.isEmpty())
                pluginMenu.addItem (99, TRANS ("No plugins found - scan in Preferences"), false);
            menu.addSubMenu (TRANS ("Plugins"), pluginMenu);
        }

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addFxButton),
                            [this, types, descriptions] (int result)
        {
            if (result == 3000)
            {
                PluginSlot slot;
                slot.identifier   = BasicSynth::identifier;
                slot.displayName  = "KANADE DAW Basic Synth";
                slot.isInstrument = true;
                commit (TRANS ("Add plugin"), [slot] (Track& t) { t.plugins.push_back (slot); });
            }
            else if (result >= 2000)
            {
                const int i = result - 2000;
                if (i < descriptions.size())
                {
                    PluginSlot slot;
                    slot.identifier  = descriptions[i].createIdentifierString();
                    slot.displayName = descriptions[i].name;
                    // Recorded now, from the description we actually picked, rather
                    // than guessed again at load time.
                    slot.isInstrument = descriptions[i].isInstrument;
                    commit (TRANS ("Add plugin"), [slot] (Track& t) { t.plugins.push_back (slot); });
                }
            }
            else if (result >= 1000)
            {
                const int i = result - 1000;
                if (i >= types.size()) return;

                BuiltinFxSlot slot;
                slot.type = types[i];
                if (auto effect = createBuiltinEffect (slot.type))
                    for (const auto& param : effect->getParameterInfo())
                        slot.params.set (juce::Identifier (param.id), param.defaultValue);

                commitChain (TRANS ("Add effect"),
                             [slot] (std::vector<BuiltinFxSlot>& chain) { chain.push_back (slot); });
            }
        });
    }

    void MixerStrip::showSlotMenu (int buttonIndex)
    {
        if (buttonIndex < 0 || buttonIndex >= (int) slotRefs.size())
            return;

        const auto ref = slotRefs[(size_t) buttonIndex];

        int slotCount = 0;
        if (ref.builtin)
        {
            if (ctx.project != nullptr)
                if (auto* chain = chainIn (*ctx.project, trackId, busId))
                    slotCount = (int) chain->size();
        }
        else if (auto* t = track())
            slotCount = (int) t->plugins.size();

        juce::String identifier;
        bool isPinned = false;
        if (! ref.builtin)
            if (auto* t = track(); t != nullptr && ref.index < (int) t->plugins.size())
            {
                identifier = t->plugins[(size_t) ref.index].identifier;
                isPinned = ctx.settings != nullptr && ctx.settings->getPinnedPlugins().contains (identifier);
            }

        juce::PopupMenu menu;
        menu.addItem (1, ref.builtin ? TRANS ("Edit") : TRANS ("Open plugin editor"));
        menu.addItem (2, TRANS ("Bypass"));
        menu.addSeparator();
        menu.addItem (4, TRANS ("Move up"), ref.index > 0);
        menu.addItem (5, TRANS ("Move down"), ref.index < slotCount - 1);

        if (! ref.builtin && identifier.isNotEmpty())
        {
            menu.addSeparator();
            menu.addItem (6, isPinned ? TRANS ("Unpin from favourites") : TRANS ("Pin to favourites"));
        }

        menu.addSeparator();
        menu.addItem (3, TRANS ("Remove"));

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (slotButtons[buttonIndex]),
                            [this, ref, identifier, isPinned] (int result)
        {
            if (result == 1)
            {
                if (ref.builtin) owner.showBuiltinFxEditor (trackId, ref.index, busId);
                else             owner.showPluginEditor (trackId, ref.index);
            }
            else if (result == 6)
            {
                if (ctx.settings != nullptr)
                {
                    auto pinned = ctx.settings->getPinnedPlugins();
                    pinned.removeString (identifier);
                    if (! isPinned)
                        pinned.insert (0, identifier);
                    ctx.settings->setPinnedPlugins (pinned);
                }
            }
            else if (result == 4 || result == 5)
            {
                const int other = ref.index + (result == 4 ? -1 : 1);

                if (ref.builtin)
                {
                    commitChain (TRANS ("Move effect"), [ref, other] (std::vector<BuiltinFxSlot>& chain)
                    {
                        if (ref.index >= 0 && ref.index < (int) chain.size()
                            && other >= 0 && other < (int) chain.size())
                            std::swap (chain[(size_t) ref.index], chain[(size_t) other]);
                    });
                }
                else
                {
                    commit (TRANS ("Move plugin"), [ref, other] (Track& t)
                    {
                        if (ref.index >= 0 && ref.index < (int) t.plugins.size()
                            && other >= 0 && other < (int) t.plugins.size())
                            std::swap (t.plugins[(size_t) ref.index], t.plugins[(size_t) other]);
                    });
                }
            }
            else if (result == 2)
            {
                if (ref.builtin)
                {
                    commitChain (TRANS ("Bypass"), [ref] (std::vector<BuiltinFxSlot>& chain)
                    {
                        if (ref.index < (int) chain.size())
                            chain[(size_t) ref.index].bypassed = ! chain[(size_t) ref.index].bypassed;
                    });
                }
                else
                {
                    // Straight at the live instance first: bypass no longer belongs
                    // to the chain signature, so nothing is reloaded.
                    if (auto* t = track(); t != nullptr && ref.index < (int) t->plugins.size())
                        if (ctx.engine != nullptr)
                            ctx.engine->getMixer().setPluginBypassed (
                                trackId, ref.index, ! t->plugins[(size_t) ref.index].bypassed);

                    commit (TRANS ("Bypass"), [ref] (Track& t)
                    {
                        if (ref.index < (int) t.plugins.size())
                            t.plugins[(size_t) ref.index].bypassed = ! t.plugins[(size_t) ref.index].bypassed;
                    });
                }
            }
            else if (result == 3)
            {
                if (ref.builtin)
                {
                    commitChain (TRANS ("Remove effect"), [ref] (std::vector<BuiltinFxSlot>& chain)
                    {
                        if (ref.index < (int) chain.size())
                            chain.erase (chain.begin() + ref.index);
                    });
                }
                else
                {
                    commit (TRANS ("Remove effect"), [ref] (Track& t)
                    {
                        if (ref.index < (int) t.plugins.size())
                            t.plugins.erase (t.plugins.begin() + ref.index);
                    });
                }
            }
        });
    }

    void MixerStrip::updateMeters()
    {
        if (ctx.engine == nullptr)
            return;

        for (int ch = 0; ch < 2; ++ch)
        {
            float p = 0.0f, r = 0.0f;

            if (isMaster())
            {
                p = ctx.engine->getMixer().getMasterPeak (ch);
                r = ctx.engine->getMixer().getMasterRms (ch);
            }
            else if (auto* strip = liveStrip())
            {
                p = strip->getPeak (ch);
                r = strip->getRms (ch);
            }

            peak[ch] = p > peak[ch] ? p : peak[ch] * 0.8f;
            rms[ch]  = r;
        }

        if (isMaster())
        {
            /*  Sample peak and true peak are labelled apart on purpose: they differ
                by a dB or more on a limited master, and it is the true peak that a
                delivery spec means (spec 8.4.6).                                   */
            auto& mixer = ctx.engine->getMixer();
            const auto samplePeakDb = juce::Decibels::gainToDecibels (juce::jmax (peak[0], peak[1]),
                                                                      -60.0f);
            loudnessLabel.setText (juce::String (mixer.getMasterLufs(), 1) + " LUFS\n"
                                     + TRANS ("Smp pk") + " " + formatDb (samplePeakDb) + "\n"
                                     + TRANS ("True pk") + " "
                                     + juce::String (mixer.getMasterTruePeakDb(), 1) + " dBTP",
                                   juce::dontSendNotification);
        }

        repaint (meterArea.getSmallestIntegerContainer().expanded (1));
    }

    void MixerStrip::mouseDown (const juce::MouseEvent&)
    {
        if (! isMaster() && ! isBus())
            ui.select (trackId, invalidClipId, false);
    }

    void MixerStrip::paint (juce::Graphics& g)
    {
        const auto& p = palette();
        const bool isCurrent = ! isMaster() && ! isBus() && trackId == ui.selectedTrack;

        g.fillAll ((isMaster() || isBus()) ? p.headerBg : (isCurrent ? p.panelAltBg : p.panelBg));
        g.setColour (p.divider);
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

        if (auto* t = track())
        {
            g.setColour (t->colour);
            g.fillRect (0, 0, getWidth() - 1, 3);
        }

        if (! meterArea.isEmpty())
        {
            auto left  = meterArea.withWidth (meterArea.getWidth() * 0.5f - 1.0f);
            auto right = left.withX (meterArea.getCentreX() + 1.0f);
            paintMeter (g, left,  peak[0], rms[0], true);
            paintMeter (g, right, peak[1], rms[1], true);
        }
    }

    void MixerStrip::resized()
    {
        auto area = getLocalBounds().reduced (4, 4);
        area.removeFromTop (2);

        nameLabel.setBounds (area.removeFromTop (18));
        area.removeFromTop (2);

        // Effect slots (spec 8.4.5 says the chain length is CPU-bound, so the
        // list simply scrolls out of view past what fits).
        auto slots = area.removeFromTop (juce::jmin (maxSlotRows, juce::jmax (1, slotButtons.size() + 1)) * slotHeight);
        for (auto* b : slotButtons)
        {
            if (slots.getHeight() < slotHeight) break;
            b->setBounds (slots.removeFromTop (slotHeight).reduced (0, 1));
        }
        addFxButton.setBounds (slots.removeFromTop (juce::jmin (slotHeight, juce::jmax (0, slots.getHeight())))
                                    .reduced (0, 1));

        area.removeFromTop (4);

        if (! isMaster())
        {
            const bool showPanValue = ctx.settings != nullptr && ctx.settings->getShowPanValueLabel();
            panValueLabel.setVisible (showPanValue);

            panKnob.setBounds (area.removeFromTop (42).withSizeKeepingCentre (40, 40));
            if (showPanValue)
                panValueLabel.setBounds (area.removeFromTop (14));
            area.removeFromTop (2);

            auto buttons = area.removeFromBottom (22);

            if (isBus())
            {
                muteButton.setBounds (buttons.reduced (1));
            }
            else
            {
                muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (1));
                soloButton.setBounds (buttons.reduced (1));
                outButton.setBounds (area.removeFromBottom (20).reduced (1));
            }
        }
        else
        {
            loudnessLabel.setBounds (area.removeFromBottom (46));
        }

        valueLabel.setBounds (area.removeFromBottom (16));

        // Fader on the left, stereo meter on the right.
        auto faderArea = area.removeFromLeft (area.getWidth() - 26);
        fader.setBounds (faderArea.reduced (4, 2));
        meterArea = area.reduced (2, 2).toFloat();
    }

    //==============================================================================
    /** Hosts a plugin's own editor.  Closing only hides it, so re-opening is
        instant and the lifetime stays owned by MixerView. */
    class MixerView::PluginWindow final : public juce::DocumentWindow
    {
    public:
        PluginWindow (juce::AudioPluginInstance& p, TrackId t, int i)
            : DocumentWindow (p.getName(), palette().windowBg, juce::DocumentWindow::closeButton),
              trackId (t), pluginIndex (i)
        {
            setUsingNativeTitleBar (true);

            if (auto* editor = p.createEditorIfNeeded())
            {
                setContentOwned (editor, true);
                setResizable (editor->isResizable(), false);
            }
            else
            {
                auto* placeholder = new juce::Label ({}, TRANS ("This plugin has no editor"));
                placeholder->setJustificationType (juce::Justification::centred);
                placeholder->setSize (320, 120);
                setContentOwned (placeholder, true);
            }

            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { setVisible (false); }

        TrackId trackId;
        int     pluginIndex;
    };

    //==============================================================================
    MixerView::MixerView (AppContext& c, UiState& s)
        : ProjectView (c, s)
    {
        viewport.setViewedComponent (&stripHolder, false);
        viewport.setScrollBarsShown (false, true);
        addAndMakeVisible (viewport);

        masterStrip = std::make_unique<MixerStrip> (ctx, ui, *this, invalidTrackId);
        addAndMakeVisible (*masterStrip);

        addBusButton.setButtonText (TRANS ("+ Bus"));
        addBusButton.setTooltip (TRANS ("Add a group bus"));
        addBusButton.onClick = [this]
        {
            if (ctx.project == nullptr)
                return;

            auto* p = ctx.project.get();
            performProjectEdit (*p, TRANS ("Add bus"), [p] { p->addBus ({}); });
        };
        addAndMakeVisible (addBusButton);

        rebuildStrips();
        startTimerHz (ctx.settings != nullptr ? ctx.settings->getMixerMeterRefreshHz() : 30);
    }

    MixerView::~MixerView()
    {
        stopTimer();
        pluginWindows.clear();
    }

    void MixerView::changeListenerCallback (juce::ChangeBroadcaster*)
    {
        rebuildStrips();
    }

    void MixerView::rebuildStrips()
    {
        if (ctx.project == nullptr)
            return;

        // Only rebuild when the set of tracks actually changed; otherwise refresh
        // in place so an in-flight interaction is not yanked out from under the
        // mouse.
        const auto numTracks = project().getNumTracks();
        const auto numBuses  = (int) project().buses.size();

        bool sameStrips = strips.size() == numTracks + numBuses;

        for (int i = 0; sameStrips && i < numTracks; ++i)
            sameStrips = strips[i]->getTrackId() == project().getTrack (i).getId();

        for (int i = 0; sameStrips && i < numBuses; ++i)
            sameStrips = strips[numTracks + i]->getBusId() == project().buses[(size_t) i].id;

        if (! sameStrips)
        {
            strips.clear();

            for (int i = 0; i < numTracks; ++i)
            {
                auto* strip = strips.add (new MixerStrip (ctx, ui, *this, project().getTrack (i).getId()));
                stripHolder.addAndMakeVisible (strip);
            }

            // Buses sit to the right of the tracks that feed them, before the master.
            for (int i = 0; i < numBuses; ++i)
            {
                auto* strip = strips.add (new MixerStrip (ctx, ui, *this, invalidTrackId,
                                                          project().buses[(size_t) i].id));
                stripHolder.addAndMakeVisible (strip);
            }
        }
        else
        {
            for (auto* strip : strips)
                strip->refresh();
        }

        masterStrip->refresh();
        resized();
        repaint();
    }

    void MixerView::timerCallback()
    {
        for (auto* strip : strips)
            strip->updateMeters();

        if (masterStrip != nullptr)
            masterStrip->updateMeters();
    }

    void MixerView::showBuiltinFxEditor (TrackId trackId, int slotIndex, int busId)
    {
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (new BuiltinFxEditor (ctx, trackId, slotIndex, busId));
        options.dialogTitle            = TRANS ("Effect");
        options.dialogBackgroundColour = palette().panelBg;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar      = true;
        options.resizable              = false;
        options.launchAsync();
    }

    void MixerView::showPluginEditor (TrackId trackId, int pluginIndex)
    {
        for (auto* w : pluginWindows)
            if (w->trackId == trackId && w->pluginIndex == pluginIndex)
            {
                w->setVisible (true);
                w->toFront (true);
                return;
            }

        if (ctx.engine == nullptr)
            return;

        auto* strip = ctx.engine->getMixer().getStripForTrack (trackId);
        if (strip == nullptr)
            return;

        auto* instance = strip->getPluginInstance (pluginIndex);
        if (instance == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    TRANS ("Plugin not loaded"),
                                                    TRANS ("The plugin for this slot could not be instantiated. "
                                                           "Check Preferences > Plugins."),
                                                    TRANS ("OK"), this);
            return;
        }

        pluginWindows.add (new PluginWindow (*instance, trackId, pluginIndex));
    }

    void MixerView::paint (juce::Graphics& g)
    {
        g.fillAll (palette().windowBg);
    }

    void MixerView::resized()
    {
        auto area = getLocalBounds();
        auto master = area.removeFromRight (stripWidth + 20);
        addBusButton.setBounds (master.removeFromTop (22).reduced (2, 2));

        if (masterStrip != nullptr)
            masterStrip->setBounds (master);

        viewport.setBounds (area);
        stripHolder.setBounds (0, 0, juce::jmax (area.getWidth(), strips.size() * stripWidth),
                               juce::jmax (0, area.getHeight() - 12));

        for (int i = 0; i < strips.size(); ++i)
            strips[i]->setBounds (i * stripWidth, 0, stripWidth, stripHolder.getHeight());
    }
}
