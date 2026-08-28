#include "IO/DawProject.h"
#include <algorithm>
#include <set>

namespace ss::io::dawproject
{
    juce::String trackXmlId      (TrackId id)  { return "track-"   + juce::String (id); }
    juce::String channelXmlId    (TrackId id)  { return "channel-" + juce::String (id); }
    juce::String busTrackXmlId   (int busId)   { return "bus-track-"   + juce::String (busId); }
    juce::String busChannelXmlId (int busId)   { return "bus-channel-" + juce::String (busId); }
    const juce::String masterTrackXmlId   = "track-master";
    const juce::String masterChannelXmlId = "channel-master";

    namespace
    {
        void setRealParam (juce::XmlElement& parent, const juce::String& tag,
                            const juce::String& unit, double value)
        {
            auto* p = parent.createNewChildElement (tag);
            p->setAttribute ("unit", unit);
            p->setAttribute ("value", value);
        }

        void setBoolParam (juce::XmlElement& parent, const juce::String& tag, bool value)
        {
            parent.createNewChildElement (tag)->setAttribute ("value", value ? "true" : "false");
        }

        void addChannel (juce::XmlElement& trackEl, const juce::String& channelId,
                          const juce::String& mixerRole, const juce::String& destinationId,
                          float gainDb, float pan, bool muted, bool soloed)
        {
            auto* channel = trackEl.createNewChildElement ("Channel");
            channel->setAttribute ("id", channelId);
            channel->setAttribute ("audioChannels", 2);
            channel->setAttribute ("role", mixerRole);
            channel->setAttribute ("solo", soloed ? "true" : "false");
            if (destinationId.isNotEmpty())
                channel->setAttribute ("destination", destinationId);

            setRealParam (*channel, "Volume", "decibel", (double) gainDb);
            setRealParam (*channel, "Pan",    "linear",  (double) pan);
            setBoolParam (*channel, "Mute", muted);
        }

        juce::XmlElement* addTrackElement (juce::XmlElement& structure, const juce::String& id,
                                            const juce::String& name, const juce::String& contentType)
        {
            auto* trackEl = structure.createNewChildElement ("Track");
            trackEl->setAttribute ("id", id);
            trackEl->setAttribute ("name", name);
            trackEl->setAttribute ("contentType", contentType);
            trackEl->setAttribute ("loaded", "true");
            return trackEl;
        }
    }

    std::unique_ptr<juce::XmlElement> buildProjectSkeleton (const Project& project,
                                                             juce::StringArray& warningsOut)
    {
        auto root = std::make_unique<juce::XmlElement> ("Project");
        root->setAttribute ("version", "1.0");

        auto* application = root->createNewChildElement ("Application");
        application->setAttribute ("name", "KANADE DAW");
        application->setAttribute ("version", JUCE_APPLICATION_VERSION_STRING);

        auto* transport = root->createNewChildElement ("Transport");
        setRealParam (*transport, "Tempo", "bpm", project.tempo.bpmAt (0.0));
        {
            const auto ts = project.tempo.timeSignatureAt (0.0);
            auto* tsEl = transport->createNewChildElement ("TimeSignature");
            tsEl->setAttribute ("numerator", ts.numerator);
            tsEl->setAttribute ("denominator", ts.denominator);
        }
        if (project.tempo.getEvents().size() > 1 || project.tempo.getTimeSignatures().size() > 1)
            warningsOut.add ("Only the tempo and time signature at the start of the project were exported"
                              " - later tempo/time-signature changes were dropped.");

        auto* structure = root->createNewChildElement ("Structure");

        for (const auto& trackPtr : project.getTracks())
        {
            const auto& track = *trackPtr;
            if (track.getType() == TrackType::utau)
            {
                warningsOut.add ("Skipped UTAU track \"" + track.name
                                  + "\" - no equivalent in DAWproject.");
                continue;
            }

            auto* trackEl = addTrackElement (*structure, trackXmlId (track.getId()), track.name,
                                             track.getType() == TrackType::audio ? "audio" : "notes");
            trackEl->setAttribute ("color", "#" + track.colour.toDisplayString (false));

            const auto destination = track.outputBus == 0 ? masterChannelXmlId
                                                            : busChannelXmlId (track.outputBus);
            addChannel (*trackEl, channelXmlId (track.getId()), "regular", destination,
                       track.gainDb, track.pan, track.muted, track.soloed);

            if (! track.sends.empty())
            {
                auto* sends = trackEl->getChildByName ("Channel")->createNewChildElement ("Sends");
                for (const auto& send : track.sends)
                {
                    auto* sendEl = sends->createNewChildElement ("Send");
                    sendEl->setAttribute ("destination", busChannelXmlId (send.busId));
                    sendEl->setAttribute ("type", "post");
                    setRealParam (*sendEl, "Volume", "linear", (double) send.level);
                }
            }
        }

        for (const auto& bus : project.buses)
        {
            auto* busTrackEl = addTrackElement (*structure, busTrackXmlId (bus.id), bus.name, "audio");
            addChannel (*busTrackEl, busChannelXmlId (bus.id), "submix", masterChannelXmlId,
                       bus.gainDb, bus.pan, bus.muted, false);
        }

        auto* masterTrackEl = addTrackElement (*structure, masterTrackXmlId, "Master", "audio");
        addChannel (*masterTrackEl, masterChannelXmlId, "master", juce::String(), 0.0f, 0.0f, false, false);

        auto* arrangement = root->createNewChildElement ("Arrangement");
        auto* lanes = arrangement->createNewChildElement ("Lanes");
        lanes->setAttribute ("timeUnit", "beats");

        if (! project.markers.empty())
        {
            auto* markers = arrangement->createNewChildElement ("Markers");
            for (const auto& marker : project.markers)
            {
                auto* markerEl = markers->createNewChildElement ("Marker");
                markerEl->setAttribute ("time", marker.beat);
                markerEl->setAttribute ("name", marker.name);
            }
        }

        root->createNewChildElement ("Scenes");

        return root;
    }

    namespace
    {
        std::unique_ptr<juce::XmlElement> buildAudioElement (juce::AudioFormatReader& reader,
                                                               const juce::String& storedPath)
        {
            auto audio = std::make_unique<juce::XmlElement> ("Audio");
            audio->setAttribute ("channels", (int) reader.numChannels);
            audio->setAttribute ("sampleRate", (int) reader.sampleRate);
            audio->setAttribute ("duration", (double) reader.lengthInSamples / reader.sampleRate);
            audio->createNewChildElement ("File")->setAttribute ("path", storedPath);
            return audio;
        }

        std::unique_ptr<juce::XmlElement> wrapInGainLanesIfNeeded (std::unique_ptr<juce::XmlElement> content,
                                                                     float gainDb)
        {
            if (gainDb == 0.0f)
                return content;

            auto lanesWrap = std::make_unique<juce::XmlElement> ("Lanes");
            lanesWrap->addChildElement (content.release());
            auto* points = lanesWrap->createNewChildElement ("Points");
            points->setAttribute ("unit", "decibel");
            points->createNewChildElement ("Target")->setAttribute ("expression", "gain");
            auto* point = points->createNewChildElement ("RealPoint");
            point->setAttribute ("time", 0.0);
            point->setAttribute ("value", (double) gainDb);
            return lanesWrap;
        }

        std::unique_ptr<juce::XmlElement> wrapInWarpsIfNeeded (std::unique_ptr<juce::XmlElement> content,
                                                                 const AudioClip& clip, const TempoMap& tempo)
        {
            if (clip.playbackRate == 1.0)
                return content;

            const auto startSec = tempo.beatsToSeconds (clip.startBeats);
            const auto endSec   = tempo.beatsToSeconds (clip.endBeats());
            const auto contentSecondsConsumed = (endSec - startSec) * clip.playbackRate;

            auto warps = std::make_unique<juce::XmlElement> ("Warps");
            warps->setAttribute ("contentTimeUnit", "seconds");
            warps->addChildElement (content.release());
            auto* w0 = warps->createNewChildElement ("Warp");
            w0->setAttribute ("time", 0.0);
            w0->setAttribute ("contentTime", 0.0);
            auto* w1 = warps->createNewChildElement ("Warp");
            w1->setAttribute ("time", clip.lengthBeats);
            w1->setAttribute ("contentTime", contentSecondsConsumed);
            return warps;
        }

        void addAudioClip (juce::XmlElement& clipsEl, const AudioClip& clip, const TempoMap& tempo,
                            juce::AudioFormatManager& formats, ArchiveWriter& writer,
                            juce::StringArray& warningsOut)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (clip.sourceFile));
            if (reader == nullptr)
            {
                warningsOut.add ("Skipped audio clip \"" + clip.name + "\" - could not read \""
                                  + clip.sourceFile.getFullPathName() + "\".");
                return;
            }

            const auto storedPath = "audio/" + juce::String (clip.id) + "-" + clip.sourceFile.getFileName();
            writer.addFile (storedPath, clip.sourceFile);

            auto* clipEl = clipsEl.createNewChildElement ("Clip");
            clipEl->setAttribute ("time", clip.startBeats);
            clipEl->setAttribute ("duration", clip.lengthBeats);
            clipEl->setAttribute ("enable", "true");
            if (clip.name.isNotEmpty())
                clipEl->setAttribute ("name", clip.name);
            if (clip.fadeInSec > 0.0)
            {
                clipEl->setAttribute ("fadeInTime", clip.fadeInSec);
                clipEl->setAttribute ("fadeTimeUnit", "seconds");
            }
            if (clip.fadeOutSec > 0.0)
            {
                clipEl->setAttribute ("fadeOutTime", clip.fadeOutSec);
                clipEl->setAttribute ("fadeTimeUnit", "seconds");
            }

            std::unique_ptr<juce::XmlElement> content = buildAudioElement (*reader, storedPath);
            content = wrapInWarpsIfNeeded (std::move (content), clip, tempo);
            content = wrapInGainLanesIfNeeded (std::move (content), clip.gainDb);
            clipEl->addChildElement (content.release());
        }

        void addMidiClip (juce::XmlElement& clipsEl, const MidiClip& clip)
        {
            auto* clipEl = clipsEl.createNewChildElement ("Clip");
            clipEl->setAttribute ("time", clip.startBeats);
            clipEl->setAttribute ("duration", clip.lengthBeats);
            clipEl->setAttribute ("enable", "true");
            if (clip.name.isNotEmpty())
                clipEl->setAttribute ("name", clip.name);

            auto* notesEl = clipEl->createNewChildElement ("Notes");
            for (const auto& note : clip.notes)
            {
                auto* noteEl = notesEl->createNewChildElement ("Note");
                noteEl->setAttribute ("time", note.startBeats);
                noteEl->setAttribute ("duration", note.lengthBeats);
                noteEl->setAttribute ("key", note.pitch);
                noteEl->setAttribute ("channel", note.channel - 1);          // 1-based -> 0-based
                noteEl->setAttribute ("vel", (double) note.velocity / 127.0); // 1..127 -> 0..1
            }
        }
    }

    void addClips (juce::XmlElement& lanes, const Project& project, juce::AudioFormatManager& formats,
                    ArchiveWriter& writer, juce::StringArray& warningsOut)
    {
        for (const auto& trackPtr : project.getTracks())
        {
            const auto& track = *trackPtr;
            if (track.getType() == TrackType::utau)
                continue;   // already warned about in buildProjectSkeleton

            if (! track.audioClips.empty())
            {
                auto* clipsEl = lanes.createNewChildElement ("Clips");
                clipsEl->setAttribute ("track", trackXmlId (track.getId()));
                for (const auto& clip : track.audioClips)
                    addAudioClip (*clipsEl, clip, project.tempo, formats, writer, warningsOut);
            }

            if (! track.midiClips.empty())
            {
                auto* clipsEl = lanes.createNewChildElement ("Clips");
                clipsEl->setAttribute ("track", trackXmlId (track.getId()));
                for (const auto& clip : track.midiClips)
                    addMidiClip (*clipsEl, clip);
            }
        }
    }

    namespace
    {
        juce::XmlElement* findTrackElement (juce::XmlElement& structure, const juce::String& trackId)
        {
            for (auto* t : structure.getChildIterator())
                if (t->hasTagName ("Track") && t->getStringAttribute ("id") == trackId)
                    return t;
            return nullptr;
        }

        juce::String pluginDeviceXmlId (TrackId trackId, int slot)
        {
            return "track-" + juce::String (trackId) + "-plugin" + juce::String (slot);
        }
        juce::String pluginParamXmlId (TrackId trackId, int slot, int paramIndex)
        {
            return pluginDeviceXmlId (trackId, slot) + "-param" + juce::String (paramIndex);
        }
        juce::String builtinFxDeviceXmlId (TrackId trackId, int slot)
        {
            return "track-" + juce::String (trackId) + "-fx" + juce::String (slot);
        }
        juce::String builtinFxParamXmlId (TrackId trackId, int slot, const juce::String& paramId)
        {
            return builtinFxDeviceXmlId (trackId, slot) + "-" + paramId;
        }
        juce::String trackParamXmlId (TrackId trackId, const juce::String& which)
        {
            return channelXmlId (trackId) + "-" + which;
        }

        // Sets `tag`'s value attribute from fx.params[paramId], and (only if `automatedIds`
        // contains paramId) an `id` attribute so automation can target it.
        void addFxRealParam (juce::XmlElement& parent, const juce::String& tag, const juce::String& unit,
                              const ss::BuiltinFxSlot& fx, const juce::String& paramId,
                              TrackId trackId, int slot, const std::vector<juce::String>& automatedIds)
        {
            auto* p = parent.createNewChildElement (tag);
            p->setAttribute ("unit", unit);
            p->setAttribute ("value", (double) fx.params[juce::Identifier (paramId)]);
            if (std::find (automatedIds.begin(), automatedIds.end(), paramId) != automatedIds.end())
                p->setAttribute ("id", builtinFxParamXmlId (trackId, slot, paramId));
        }

        // True only for paramIds that addEqualizerParams/addCompressorParams/addGateParams/
        // addLimiterParams actually call addFxRealParam for (i.e. paramIds that get a real
        // XML element with an id when automated). hpOn/lpOn are deliberately excluded - they
        // get a static <Enabled> value but never an id, so there is nothing to point at.
        bool isKnownFxParam (const juce::String& fxType, const juce::String& paramId)
        {
            static const std::map<juce::String, std::set<juce::String>> known {
                { "eq",         { "hpFreq", "lowFreq", "lowGain", "mid1Freq", "mid1Gain", "mid1Q",
                                  "mid2Freq", "mid2Gain", "mid2Q", "highFreq", "highGain", "lpFreq" } },
                { "compressor", { "threshold", "ratio", "attack", "release", "makeup" } },
                { "gate",       { "threshold", "range", "attack", "release" } },
                { "limiter",    { "input", "release" } },
            };
            const auto it = known.find (fxType);
            return it != known.end() && it->second.count (paramId) > 0;
        }

        void addEqualizerParams (juce::XmlElement& eq, const ss::BuiltinFxSlot& fx,
                                  TrackId trackId, int slot, const std::vector<juce::String>& automated)
        {
            struct Band { const char* type; const char* freqId; const char* gainId; const char* qId; const char* onId; int order; };
            static const Band bands[] = {
                { "highPass", "hpFreq",   nullptr,      nullptr,     "hpOn", 0 },
                { "lowShelf", "lowFreq",  "lowGain",    nullptr,     nullptr, 1 },
                { "bell",     "mid1Freq", "mid1Gain",   "mid1Q",     nullptr, 2 },
                { "bell",     "mid2Freq", "mid2Gain",   "mid2Q",     nullptr, 3 },
                { "highShelf","highFreq", "highGain",   nullptr,     nullptr, 4 },
                { "lowPass",  "lpFreq",   nullptr,      nullptr,     "lpOn", 5 },
            };
            for (const auto& band : bands)
            {
                auto* bandEl = eq.createNewChildElement ("Band");
                bandEl->setAttribute ("type", band.type);
                bandEl->setAttribute ("order", band.order);
                addFxRealParam (*bandEl, "Freq", "hertz", fx, band.freqId, trackId, slot, automated);
                if (band.gainId != nullptr)
                    addFxRealParam (*bandEl, "Gain", "decibel", fx, band.gainId, trackId, slot, automated);
                if (band.qId != nullptr)
                    addFxRealParam (*bandEl, "Q", "linear", fx, band.qId, trackId, slot, automated);
                if (band.onId != nullptr)
                {
                    const bool on = (double) fx.params[juce::Identifier (band.onId)] >= 0.5;
                    bandEl->createNewChildElement ("Enabled")->setAttribute ("value", on ? "true" : "false");
                }
            }
        }

        void addCompressorParams (juce::XmlElement& c, const ss::BuiltinFxSlot& fx,
                                   TrackId trackId, int slot, const std::vector<juce::String>& automated)
        {
            // Emission order is the XSD's declared xs:sequence for `compressor`:
            // Attack, AutoMakeup, InputGain, OutputGain, Ratio, Release, Threshold.
            addFxRealParam (c, "Attack",      "seconds", fx, "attack",    trackId, slot, automated);
            addFxRealParam (c, "OutputGain",  "decibel", fx, "makeup",    trackId, slot, automated);
            addFxRealParam (c, "Ratio",       "linear",  fx, "ratio",     trackId, slot, automated);
            addFxRealParam (c, "Release",     "seconds", fx, "release",   trackId, slot, automated);
            addFxRealParam (c, "Threshold",   "decibel", fx, "threshold", trackId, slot, automated);
            // knee/mix have no DAWproject compressor equivalent - omitted, see Task 4 note.
        }

        void addGateParams (juce::XmlElement& g, const ss::BuiltinFxSlot& fx,
                             TrackId trackId, int slot, const std::vector<juce::String>& automated)
        {
            // XSD `noiseGate` sequence: Attack, Range, Ratio, Release, Threshold.
            addFxRealParam (g, "Attack",    "seconds", fx, "attack",    trackId, slot, automated);
            addFxRealParam (g, "Range",     "decibel", fx, "range",     trackId, slot, automated);
            addFxRealParam (g, "Release",   "seconds", fx, "release",   trackId, slot, automated);
            addFxRealParam (g, "Threshold", "decibel", fx, "threshold", trackId, slot, automated);
            // hold has no DAWproject equivalent - omitted.
        }

        void addLimiterParams (juce::XmlElement& l, const ss::BuiltinFxSlot& fx,
                                TrackId trackId, int slot, const std::vector<juce::String>& automated)
        {
            // XSD `limiter` sequence: Attack, InputGain, OutputGain, Release, Threshold.
            addFxRealParam (l, "InputGain", "decibel", fx, "input",   trackId, slot, automated);
            addFxRealParam (l, "Release",   "seconds", fx, "release", trackId, slot, automated);
            // ceiling/lookahead have no DAWproject equivalent - omitted rather than mismapped.
        }

        // The XSD element for each KANADE built-in FX type. These are *direct* children of
        // <Devices> (the schema lists them as alternatives alongside Vst3Plugin) - they are
        // NOT nested inside a <BuiltinDevice>, which is a generic device with an empty
        // sequence. The tag doubles as the required `deviceName`.
        const std::map<juce::String, juce::String>& builtinFxTagByType()
        {
            static const std::map<juce::String, juce::String> m {
                { "eq", "Equalizer" }, { "compressor", "Compressor" },
                { "gate", "NoiseGate" }, { "limiter", "Limiter" }
            };
            return m;
        }

        // Several XSD types declare xs:sequence (ordered) content, but this exporter builds
        // some children out of order across functions (a Channel's <Devices> is only known
        // after buildProjectSkeleton has already written Volume/Pan/Mute). Sorts `parent`'s
        // children into `tagOrder`; tags not listed keep their relative order and go last.
        void reorderChildrenByTagOrder (juce::XmlElement& parent, const juce::StringArray& tagOrder)
        {
            const auto rank = [&] (const juce::XmlElement* e)
            {
                const auto i = tagOrder.indexOf (e->getTagName());
                return i < 0 ? tagOrder.size() : i;
            };

            std::vector<juce::XmlElement*> children;
            for (auto* c : parent.getChildIterator())
                children.push_back (c);
            std::stable_sort (children.begin(), children.end(),
                              [&] (auto* a, auto* b) { return rank (a) < rank (b); });

            for (auto* c : children) parent.removeChildElement (c, false);   // detach, keep ownership
            for (auto* c : children) parent.addChildElement (c);
        }

        struct AutomationIndex
        {
            bool gain = false, pan = false, mute = false;
            std::map<int, std::vector<int>>          plugin;   // slot -> param indices
            std::map<int, std::vector<juce::String>> fx;       // slot -> param ids
        };

        AutomationIndex indexAutomation (const Track& track)
        {
            AutomationIndex idx;
            for (const auto& lane : track.automation)
            {
                if (lane.points.empty()) continue;
                if (lane.parameterId == "gain") idx.gain = true;
                else if (lane.parameterId == "pan") idx.pan = true;
                else if (lane.parameterId == "mute") idx.mute = true;
                else if (lane.parameterId.startsWith ("plugin:"))
                {
                    const auto body = lane.parameterId.substring (7);
                    idx.plugin[body.upToFirstOccurrenceOf (":", false, false).getIntValue()]
                        .push_back (body.fromFirstOccurrenceOf (":", false, false).getIntValue());
                }
                else if (lane.parameterId.startsWith ("fx:"))
                {
                    const auto body = lane.parameterId.substring (3);
                    idx.fx[body.upToFirstOccurrenceOf (":", false, false).getIntValue()]
                        .push_back (body.fromFirstOccurrenceOf (":", false, false));
                }
            }
            return idx;
        }

        void addPointsForLane (juce::XmlElement& lanes, const Track& track, const juce::String& laneParamId,
                                const juce::String& targetId, const juce::String& unit, bool isBool)
        {
            auto* points = lanes.createNewChildElement ("Points");
            points->setAttribute ("track", trackXmlId (track.getId()));
            points->setAttribute ("unit", unit);
            points->createNewChildElement ("Target")->setAttribute ("parameter", targetId);

            for (const auto& lane : track.automation)
            {
                if (lane.parameterId != laneParamId) continue;
                for (const auto& pt : lane.points)
                {
                    auto* pointEl = points->createNewChildElement (isBool ? "BoolPoint" : "RealPoint");
                    pointEl->setAttribute ("time", pt.first);
                    if (isBool) pointEl->setAttribute ("value", pt.second >= 0.5f ? "true" : "false");
                    else        pointEl->setAttribute ("value", (double) pt.second);
                }
            }
        }
    }

    void addDevicesAndAutomation (juce::XmlElement& root, const Project& project, PluginManager& plugins,
                                   ArchiveWriter& writer, juce::StringArray& warningsOut)
    {
        auto* structure   = root.getChildByName ("Structure");
        auto* arrangement = root.getChildByName ("Arrangement");
        jassert (structure != nullptr && arrangement != nullptr);   // buildProjectSkeleton always creates both
        auto* lanes = arrangement->getChildByName ("Lanes");
        jassert (lanes != nullptr);

        // Bus/master effect chains are not exported in this v1 pass - the mapping is
        // identical in principle (same addEqualizerParams-family helpers), but scoped out
        // to bound this plan's size (spec Scope). Never silent about it, though.
        for (const auto& bus : project.buses)
            if (! bus.builtinFx.empty())
                warningsOut.add ("Skipped the effect chain on bus \"" + bus.name
                                  + "\" - bus/master effect export isn't implemented yet.");
        if (! project.masterChain.empty())
            warningsOut.add ("Skipped the master bus effect chain - bus/master effect export isn't implemented yet.");

        for (const auto& trackPtr : project.getTracks())
        {
            const auto& track = *trackPtr;
            if (track.getType() == TrackType::utau)
                continue;

            auto* trackEl = findTrackElement (*structure, trackXmlId (track.getId()));
            jassert (trackEl != nullptr);                             // buildProjectSkeleton wrote every non-utau track
            auto* channel = trackEl->getChildByName ("Channel");
            jassert (channel != nullptr);                             // ...and addChannel gave each one a <Channel>
            const auto idx = indexAutomation (track);

            if (! track.plugins.empty())
            {
                auto* devices = channel->createNewChildElement ("Devices");
                for (int slot = 0; slot < (int) track.plugins.size(); ++slot)
                {
                    const auto& pluginSlot = track.plugins[(size_t) slot];
                    const auto* desc = plugins.findDescription (pluginSlot.identifier);

                    auto* device = devices->createNewChildElement ("Vst3Plugin");
                    device->setAttribute ("id", pluginDeviceXmlId (track.getId(), slot));
                    device->setAttribute ("deviceRole", pluginSlot.isInstrument ? "instrument" : "audioFX");
                    device->setAttribute ("deviceName", desc != nullptr ? desc->name : pluginSlot.displayName);
                    device->setAttribute ("deviceVendor", desc != nullptr ? desc->manufacturerName : juce::String());
                    device->setAttribute ("loaded", desc != nullptr ? "true" : "false");

                    // XSD `device` sequence: Parameters, Enabled, State - emit in that order.
                    auto it = idx.plugin.find (slot);
                    if (it != idx.plugin.end())
                    {
                        auto* params = device->createNewChildElement ("Parameters");
                        for (int paramIndex : it->second)
                        {
                            auto* p = params->createNewChildElement ("RealParameter");
                            p->setAttribute ("id", pluginParamXmlId (track.getId(), slot, paramIndex));
                            p->setAttribute ("parameterID", paramIndex);
                            p->setAttribute ("unit", "normalized");
                        }
                    }

                    device->createNewChildElement ("Enabled")->setAttribute ("value", pluginSlot.bypassed ? "false" : "true");

                    if (pluginSlot.state.getSize() > 0)
                    {
                        const auto stateName = "plugins/" + juce::Uuid().toString() + ".vst3-preset";
                        writer.addBytes (stateName, pluginSlot.state);
                        device->createNewChildElement ("State")->setAttribute ("path", stateName);
                    }
                }
            }

            for (int slot = 0; slot < (int) track.builtinFx.size(); ++slot)
            {
                const auto& fx = track.builtinFx[(size_t) slot];
                const auto tagIt = builtinFxTagByType().find (fx.type);
                if (tagIt == builtinFxTagByType().end())
                {
                    warningsOut.add ("Skipped \"" + fx.type + "\" effect on track \"" + track.name
                                      + "\" - DAWproject has no built-in device for it.");
                    continue;
                }

                auto* devices = channel->getChildByName ("Devices");
                if (devices == nullptr)
                    devices = channel->createNewChildElement ("Devices");

                auto* fxDevice = devices->createNewChildElement (tagIt->second);
                fxDevice->setAttribute ("id", builtinFxDeviceXmlId (track.getId(), slot));
                fxDevice->setAttribute ("deviceRole", "audioFX");
                fxDevice->setAttribute ("deviceName", tagIt->second);   // required by the XSD's `device` base
                // <Parameters> is never emitted here (automated built-in params carry their id
                // inline), so Enabled-then-type-params already matches the inherited sequence.
                fxDevice->createNewChildElement ("Enabled")->setAttribute ("value", fx.bypassed ? "false" : "true");

                const auto automatedIds = idx.fx.count (slot) ? idx.fx.at (slot) : std::vector<juce::String>{};

                if (fx.type == "eq")
                    addEqualizerParams (*fxDevice, fx, track.getId(), slot, automatedIds);
                else if (fx.type == "compressor")
                    addCompressorParams (*fxDevice, fx, track.getId(), slot, automatedIds);
                else if (fx.type == "gate")
                    addGateParams (*fxDevice, fx, track.getId(), slot, automatedIds);
                else
                    addLimiterParams (*fxDevice, fx, track.getId(), slot, automatedIds);
            }

            // Every KANADE DAW automation value - gain, pan, mute, builtin FX, plugin - is
            // already stored as a 0..1 normalized breakpoint (confirmed against
            // Source/Mixer/Mixer.cpp:961-973: gain becomes -60+66*v, pan becomes v*2-1,
            // mute is v>=0.5, builtinFx/plugin call setParameterNormalised/setValue(v)
            // directly). Writing unit="normalized" everywhere means no denormalization
            // step anywhere in this function, not just for plugin parameters.
            if (idx.gain)
            {
                const auto id = trackParamXmlId (track.getId(), "gain");
                channel->getChildByName ("Volume")->setAttribute ("id", id);
                addPointsForLane (*lanes, track, "gain", id, "normalized", false);
            }
            if (idx.pan)
            {
                const auto id = trackParamXmlId (track.getId(), "pan");
                channel->getChildByName ("Pan")->setAttribute ("id", id);
                addPointsForLane (*lanes, track, "pan", id, "normalized", false);
            }
            if (idx.mute)
            {
                const auto id = trackParamXmlId (track.getId(), "mute");
                channel->getChildByName ("Mute")->setAttribute ("id", id);
                addPointsForLane (*lanes, track, "mute", id, "normalized", true);
            }
            for (const auto& [slot, indices] : idx.plugin)
                for (int paramIndex : indices)
                    addPointsForLane (*lanes, track, "plugin:" + juce::String (slot) + ":" + juce::String (paramIndex),
                                      pluginParamXmlId (track.getId(), slot, paramIndex), "normalized", false);
            for (const auto& [slot, ids] : idx.fx)
            {
                if (slot < 0 || slot >= (int) track.builtinFx.size())
                    continue;
                const auto& fxType = track.builtinFx[(size_t) slot].type;
                for (const auto& paramId : ids)
                {
                    if (! isKnownFxParam (fxType, paramId))
                        continue;   // no XML element/id exists for this param - nothing to point at
                    addPointsForLane (*lanes, track, "fx:" + juce::String (slot) + ":" + paramId,
                                      builtinFxParamXmlId (track.getId(), slot, paramId), "normalized", false);
                }
            }
        }

        // <Channel> content is an ordered xs:sequence in the XSD, but Volume/Pan/Mute come
        // from addChannel, Sends from buildProjectSkeleton and Devices only from here - so
        // straighten every channel (tracks, buses and master alike) once, at the very end.
        for (auto* trackEl : structure->getChildIterator())
            if (auto* channelEl = trackEl->getChildByName ("Channel"))
                reorderChildrenByTagOrder (*channelEl, { "Devices", "Mute", "Pan", "Sends", "Volume" });
    }

    namespace
    {
        juce::String sceneXmlId (SceneId id) { return "scene-" + juce::String (id); }

        void addSessionClip (juce::XmlElement& sceneLanes, const juce::String& trackXmlIdStr,
                              const SessionClip& sc, TrackId trackId, SceneId sceneId,
                              const TempoMap& tempo, juce::AudioFormatManager& formats,
                              ArchiveWriter& writer, juce::StringArray& warningsOut)
        {
            std::unique_ptr<juce::XmlElement> content;

            if (sc.kind == SessionClip::Kind::audio)
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (sc.sourceFile));
                if (reader == nullptr)
                {
                    warningsOut.add ("Skipped session clip \"" + sc.name + "\" - could not read \""
                                      + sc.sourceFile.getFullPathName() + "\".");
                    return;   // nothing created - a true skip
                }
                const auto storedPath = "audio/scene-" + juce::String (sceneId) + "-track-"
                                         + juce::String (trackId) + "-" + sc.sourceFile.getFileName();
                writer.addFile (storedPath, sc.sourceFile);
                content = buildAudioElement (*reader, storedPath);

                if (sc.playbackRate != 1.0)
                {
                    auto warps = std::make_unique<juce::XmlElement> ("Warps");
                    warps->setAttribute ("contentTimeUnit", "seconds");
                    warps->addChildElement (content.release());
                    auto* w0 = warps->createNewChildElement ("Warp");
                    w0->setAttribute ("time", 0.0);
                    w0->setAttribute ("contentTime", 0.0);
                    auto* w1 = warps->createNewChildElement ("Warp");
                    w1->setAttribute ("time", sc.lengthBeats);
                    w1->setAttribute ("contentTime", tempo.beatsToSeconds (sc.lengthBeats) * sc.playbackRate);
                    content = std::move (warps);
                }
            }
            else
            {
                auto notesRoot = std::make_unique<juce::XmlElement> ("Notes");
                for (const auto& note : sc.notes)
                {
                    auto* n = notesRoot->createNewChildElement ("Note");
                    n->setAttribute ("time", note.startBeats);
                    n->setAttribute ("duration", note.lengthBeats);
                    n->setAttribute ("key", note.pitch);
                    n->setAttribute ("channel", note.channel - 1);
                    n->setAttribute ("vel", (double) note.velocity / 127.0);
                }
                content = std::move (notesRoot);
            }

            content = wrapInGainLanesIfNeeded (std::move (content), sc.gainDb);

            auto* slotEl = sceneLanes.createNewChildElement ("ClipSlot");
            slotEl->setAttribute ("track", trackXmlIdStr);
            auto* clipEl = slotEl->createNewChildElement ("Clip");
            clipEl->setAttribute ("time", 0.0);
            clipEl->setAttribute ("duration", sc.lengthBeats);
            if (sc.name.isNotEmpty())
                clipEl->setAttribute ("name", sc.name);
            clipEl->addChildElement (content.release());
        }
    }

    void addScenes (juce::XmlElement& root, const Project& project, juce::AudioFormatManager& formats,
                     ArchiveWriter& writer, juce::StringArray& warningsOut)
    {
        if (project.scenes.empty())
            return;

        auto* scenesEl = root.getChildByName ("Scenes");
        for (const auto& scene : project.scenes)
        {
            auto* sceneEl = scenesEl->createNewChildElement ("Scene");
            sceneEl->setAttribute ("id", sceneXmlId (scene.id));
            sceneEl->setAttribute ("name", scene.name);
            auto* sceneLanes = sceneEl->createNewChildElement ("Lanes");

            for (const auto& trackPtr : project.getTracks())
            {
                const auto& track = *trackPtr;
                if (track.getType() == TrackType::utau)
                    continue;

                const auto it = track.sessionSlots.find (scene.id);
                if (it == track.sessionSlots.end())
                    continue;

                addSessionClip (*sceneLanes, trackXmlId (track.getId()), it->second, track.getId(), scene.id,
                                project.tempo, formats, writer, warningsOut);
            }
        }
    }

    namespace
    {
        TrackType contentTypeToTrackType (const juce::String& contentType)
        {
            return contentType.trim().startsWith ("notes") ? TrackType::midi : TrackType::audio;
        }

        double xmlRealParam (const juce::XmlElement& parent, const juce::String& tag, double defaultValue)
        {
            auto* el = parent.getChildByName (tag);
            return el != nullptr ? el->getDoubleAttribute ("value", defaultValue) : defaultValue;
        }

        bool xmlBoolParam (const juce::XmlElement& parent, const juce::String& tag, bool defaultValue)
        {
            auto* el = parent.getChildByName (tag);
            return el != nullptr ? el->getBoolAttribute ("value", defaultValue) : defaultValue;
        }
    }

    ImportIds parseStructure (const juce::XmlElement& root, Project& project, juce::StringArray& warningsOut)
    {
        ImportIds ids;

        if (auto* transport = root.getChildByName ("Transport"))
        {
            project.tempo.setEvents ({ { 0.0, xmlRealParam (*transport, "Tempo", 120.0) } });
            if (auto* ts = transport->getChildByName ("TimeSignature"))
                project.tempo.setTimeSignatures ({ { 0.0, ts->getIntAttribute ("numerator", 4),
                                                           ts->getIntAttribute ("denominator", 4) } });
        }

        if (auto* arrangement = root.getChildByName ("Arrangement"))
            if (auto* markers = arrangement->getChildByName ("Markers"))
                for (auto* markerEl : markers->getChildIterator())
                    if (markerEl->hasTagName ("Marker"))
                        project.markers.push_back ({ markerEl->getDoubleAttribute ("time"),
                                                      markerEl->getStringAttribute ("name") });

        auto* structure = root.getChildByName ("Structure");
        if (structure == nullptr)
            return ids;

        std::map<juce::String, juce::String>            destinationByTrackXmlId;
        std::vector<std::tuple<Track*, size_t, juce::String>> pendingSends;   // track, sends-index, destination channel xml id

        for (auto* trackEl : structure->getChildIterator())
        {
            if (! trackEl->hasTagName ("Track"))
                continue;

            auto* channel = trackEl->getChildByName ("Channel");
            if (channel == nullptr)
            {
                warningsOut.add ("Skipped track \"" + trackEl->getStringAttribute ("name")
                                  + "\" - it has no <Channel>.");
                continue;
            }

            const auto role  = channel->getStringAttribute ("role", "regular");
            const auto xmlId = trackEl->getStringAttribute ("id");

            if (role == "master")
            {
                ids.masterChannelXmlId = channel->getStringAttribute ("id");
                continue;
            }

            if (role == "submix")
            {
                auto& bus = project.addBus (trackEl->getStringAttribute ("name", "Bus"));
                bus.gainDb = (float) xmlRealParam (*channel, "Volume", 0.0);
                bus.pan    = (float) xmlRealParam (*channel, "Pan", 0.0);
                bus.muted  = xmlBoolParam (*channel, "Mute", false);
                ids.busIdByChannelXmlId[channel->getStringAttribute ("id")] = bus.id;
                continue;   // Bus has no destination field - nothing to resolve in pass 2
            }

            auto& track = project.addTrack (contentTypeToTrackType (trackEl->getStringAttribute ("contentType", "audio")),
                                            trackEl->getStringAttribute ("name", "Track"));
            track.gainDb = (float) xmlRealParam (*channel, "Volume", 0.0);
            track.pan    = (float) xmlRealParam (*channel, "Pan", 0.0);
            track.muted  = xmlBoolParam (*channel, "Mute", false);
            track.soloed = channel->getBoolAttribute ("solo", false);
            if (trackEl->hasAttribute ("color"))
                track.colour = juce::Colour::fromString ("ff" + trackEl->getStringAttribute ("color").removeCharacters ("#"));

            ids.trackByXmlId[xmlId] = track.getId();
            destinationByTrackXmlId[xmlId] = channel->getStringAttribute ("destination");

            if (auto* sends = channel->getChildByName ("Sends"))
                for (auto* sendEl : sends->getChildIterator())
                {
                    if (! sendEl->hasTagName ("Send"))
                        continue;
                    Track::Send send;
                    send.level = (float) xmlRealParam (*sendEl, "Volume", 0.0);
                    track.sends.push_back (send);
                    pendingSends.emplace_back (&track, track.sends.size() - 1, sendEl->getStringAttribute ("destination"));
                }
        }

        // Pass 2: every Track/Bus now has a KANADE id - resolve destination/Send IDREFs.
        for (auto& [xmlId, kanadeId] : ids.trackByXmlId)
        {
            auto* track = project.findTrack (kanadeId);
            if (track == nullptr) continue;
            const auto dest = destinationByTrackXmlId[xmlId];
            if (dest.isNotEmpty() && ids.busIdByChannelXmlId.count (dest))
                track->outputBus = ids.busIdByChannelXmlId.at (dest);
        }
        for (auto& [track, sendIndex, destXmlId] : pendingSends)
            if (ids.busIdByChannelXmlId.count (destXmlId))
                track->sends[sendIndex].busId = ids.busIdByChannelXmlId.at (destXmlId);

        return ids;
    }

    namespace
    {
        struct UnwrappedClipContent
        {
            const juce::XmlElement* base = nullptr;   // <Audio> or <Notes>, never null if content was found
            float  gainDb = 0.0f;
            double playbackRate = 1.0;
        };

        UnwrappedClipContent unwrapClipContent (const juce::XmlElement& clipEl, const TempoMap& tempo)
        {
            UnwrappedClipContent result;
            const juce::XmlElement* current = clipEl.getFirstChildElement();
            const auto clipStartBeats = clipEl.getDoubleAttribute ("time");

            while (current != nullptr)
            {
                if (current->hasTagName ("Lanes"))
                {
                    const juce::XmlElement* inner = nullptr;
                    for (auto* c : current->getChildIterator())
                    {
                        if (c->hasTagName ("Points"))
                        {
                            auto* target = c->getChildByName ("Target");
                            if (target != nullptr && target->getStringAttribute ("expression") == "gain")
                                if (auto* pt = c->getChildByName ("RealPoint"))
                                    result.gainDb = (float) pt->getDoubleAttribute ("value");
                        }
                        else
                        {
                            inner = c;
                        }
                    }
                    current = inner;
                }
                else if (current->hasTagName ("Warps"))
                {
                    const juce::XmlElement* firstWarp = nullptr;
                    const juce::XmlElement* lastWarp = nullptr;
                    const juce::XmlElement* inner = nullptr;
                    for (auto* c : current->getChildIterator())
                    {
                        if (c->hasTagName ("Warp")) { if (firstWarp == nullptr) firstWarp = c; lastWarp = c; }
                        else inner = c;
                    }
                    if (firstWarp != nullptr && lastWarp != nullptr && firstWarp != lastWarp)
                    {
                        const auto dTimeSeconds = tempo.beatsToSeconds (clipStartBeats + lastWarp->getDoubleAttribute ("time"))
                                                 - tempo.beatsToSeconds (clipStartBeats + firstWarp->getDoubleAttribute ("time"));
                        const auto dContent = lastWarp->getDoubleAttribute ("contentTime") - firstWarp->getDoubleAttribute ("contentTime");
                        if (dTimeSeconds > 1.0e-9)
                            result.playbackRate = dContent / dTimeSeconds;
                    }
                    current = inner;
                }
                else
                {
                    result.base = current;
                    break;
                }
            }
            return result;
        }

        void addAudioClipFromXml (Track& track, const juce::XmlElement& clipEl, const UnwrappedClipContent& unwrapped,
                                   Project& project, ArchiveReader& archive, juce::StringArray& warningsOut)
        {
            auto* fileEl = unwrapped.base->getChildByName ("File");
            if (fileEl == nullptr)
            {
                warningsOut.add ("Skipped an audio clip with no <File> reference.");
                return;
            }

            const auto storedPath = fileEl->getStringAttribute ("path");
            // Flatten the *whole* stored path, not just its basename - unsaved projects share
            // one media folder, so "audio/1-kick.wav" and "audio/2-kick.wav" must not collide.
            const auto targetFile = project.getMediaFolder().getChildFile (
                juce::File::createLegalFileName (storedPath.replaceCharacter ('/', '_')));

            if (! archive.extractEntryToFile (storedPath, targetFile))
            {
                warningsOut.add ("Skipped audio clip - could not extract \"" + storedPath + "\" from the archive.");
                return;
            }

            AudioClip clip;
            clip.id = project.nextClipId();
            clip.name = clipEl.getStringAttribute ("name");
            clip.sourceFile = targetFile;
            clip.startBeats = clipEl.getDoubleAttribute ("time");
            clip.lengthBeats = clipEl.getDoubleAttribute ("duration", 4.0);
            clip.fadeInSec = clipEl.getDoubleAttribute ("fadeInTime", 0.0);
            clip.fadeOutSec = clipEl.getDoubleAttribute ("fadeOutTime", 0.0);
            clip.gainDb = unwrapped.gainDb;
            clip.playbackRate = unwrapped.playbackRate;
            track.audioClips.push_back (clip);
        }

        void addMidiClipFromXml (Track& track, const juce::XmlElement& clipEl,
                                  const UnwrappedClipContent& unwrapped, Project& project)
        {
            MidiClip clip;
            clip.id = project.nextClipId();
            clip.name = clipEl.getStringAttribute ("name");
            clip.startBeats = clipEl.getDoubleAttribute ("time");
            clip.lengthBeats = clipEl.getDoubleAttribute ("duration", 4.0);

            for (auto* noteEl : unwrapped.base->getChildIterator())
            {
                if (! noteEl->hasTagName ("Note"))
                    continue;
                Note note;
                note.pitch       = noteEl->getIntAttribute ("key", 60);
                note.startBeats  = noteEl->getDoubleAttribute ("time");
                note.lengthBeats = noteEl->getDoubleAttribute ("duration", 1.0);
                note.channel     = noteEl->getIntAttribute ("channel", 0) + 1;   // 0-based -> 1-based
                note.velocity    = juce::jlimit (1, 127, (int) std::lround (noteEl->getDoubleAttribute ("vel", 100.0 / 127.0) * 127.0));
                clip.notes.push_back (note);
            }
            track.midiClips.push_back (clip);
        }
    }

    void parseClips (const juce::XmlElement& root, Project& project, const ImportIds& ids,
                      ArchiveReader& archive, juce::StringArray& warningsOut)
    {
        auto* arrangement = root.getChildByName ("Arrangement");
        auto* lanes = arrangement != nullptr ? arrangement->getChildByName ("Lanes") : nullptr;
        if (lanes == nullptr)
            return;

        for (auto* clipsEl : lanes->getChildIterator())
        {
            if (! clipsEl->hasTagName ("Clips"))
                continue;

            const auto trackXml = clipsEl->getStringAttribute ("track");
            if (! ids.trackByXmlId.count (trackXml))
            {
                warningsOut.add ("Skipped clips for an unrecognised track (\"" + trackXml + "\").");
                continue;
            }
            auto* track = project.findTrack (ids.trackByXmlId.at (trackXml));
            if (track == nullptr)
                continue;

            for (auto* clipEl : clipsEl->getChildIterator())
            {
                if (! clipEl->hasTagName ("Clip"))
                    continue;

                const auto unwrapped = unwrapClipContent (*clipEl, project.tempo);
                if (unwrapped.base == nullptr)
                {
                    warningsOut.add ("Skipped a clip with no recognised content.");
                    continue;
                }

                if (unwrapped.base->hasTagName ("Audio"))
                    addAudioClipFromXml (*track, *clipEl, unwrapped, project, archive, warningsOut);
                else if (unwrapped.base->hasTagName ("Notes"))
                    addMidiClipFromXml (*track, *clipEl, unwrapped, project);
                else
                    warningsOut.add ("Skipped a clip of unsupported type <" + unwrapped.base->getTagName() + ">.");
            }
        }
    }

    namespace
    {
        using ParamIdMap = std::map<juce::String, juce::String>;   // xml id -> KANADE AutomationLane::parameterId

        void registerIfIdPresent (const juce::XmlElement* el, const juce::String& kanadeParamId, ParamIdMap& out)
        {
            if (el != nullptr && el->hasAttribute ("id"))
                out[el->getStringAttribute ("id")] = kanadeParamId;
        }

        void parseEqualizerParams (const juce::XmlElement& eq, ss::BuiltinFxSlot& fx, int slot, ParamIdMap& targetIds)
        {
            struct Band { int order; const char* freqId; const char* gainId; const char* qId; const char* onId; };
            static const Band bands[] = {
                { 0, "hpFreq",   nullptr,    nullptr,   "hpOn" },
                { 1, "lowFreq",  "lowGain",  nullptr,   nullptr },
                { 2, "mid1Freq", "mid1Gain", "mid1Q",   nullptr },
                { 3, "mid2Freq", "mid2Gain", "mid2Q",   nullptr },
                { 4, "highFreq", "highGain", nullptr,   nullptr },
                { 5, "lpFreq",   nullptr,    nullptr,   "lpOn" },
            };
            for (auto* bandEl : eq.getChildIterator())
            {
                if (! bandEl->hasTagName ("Band")) continue;
                const auto order = bandEl->getIntAttribute ("order", -1);
                for (const auto& band : bands)
                {
                    if (band.order != order) continue;
                    if (auto* freq = bandEl->getChildByName ("Freq"))
                    {
                        fx.params.set (band.freqId, freq->getDoubleAttribute ("value"));
                        registerIfIdPresent (freq, "fx:" + juce::String (slot) + ":" + band.freqId, targetIds);
                    }
                    if (band.gainId != nullptr)
                        if (auto* gain = bandEl->getChildByName ("Gain"))
                        {
                            fx.params.set (band.gainId, gain->getDoubleAttribute ("value"));
                            registerIfIdPresent (gain, "fx:" + juce::String (slot) + ":" + juce::String (band.gainId), targetIds);
                        }
                    if (band.qId != nullptr)
                        if (auto* q = bandEl->getChildByName ("Q"))
                        {
                            fx.params.set (band.qId, q->getDoubleAttribute ("value"));
                            registerIfIdPresent (q, "fx:" + juce::String (slot) + ":" + juce::String (band.qId), targetIds);
                        }
                    if (band.onId != nullptr)
                        if (auto* en = bandEl->getChildByName ("Enabled"))
                            fx.params.set (band.onId, en->getBoolAttribute ("value") ? 1.0 : 0.0);
                }
            }
        }

        // Reads `tag`'s value into fx.params[paramId], registering its id (if present) for automation.
        void parseFxParam (const juce::XmlElement& parent, const juce::String& tag, const juce::String& paramId,
                            ss::BuiltinFxSlot& fx, int slot, ParamIdMap& targetIds)
        {
            auto* el = parent.getChildByName (tag);
            if (el == nullptr) return;
            fx.params.set (paramId, el->getDoubleAttribute ("value"));
            registerIfIdPresent (el, "fx:" + juce::String (slot) + ":" + paramId, targetIds);
        }

        // `device` IS the <Equalizer>/<Compressor>/<NoiseGate>/<Limiter> element - per the XSD
        // those types extend builtinDevice directly and sit straight under <Devices>, they are
        // not wrapped in a <BuiltinDevice>. Mirrors the export side's addXxxParams helpers.
        ss::BuiltinFxSlot parseBuiltinFxDevice (const juce::XmlElement& device, const juce::String& fxType,
                                                 int slot, ParamIdMap& targetIds)
        {
            ss::BuiltinFxSlot fx;
            fx.type = fxType;
            fx.bypassed = ! xmlBoolParam (device, "Enabled", true);

            if (fxType == "eq")
            {
                parseEqualizerParams (device, fx, slot, targetIds);
            }
            else if (fxType == "compressor")
            {
                parseFxParam (device, "Threshold",  "threshold", fx, slot, targetIds);
                parseFxParam (device, "Ratio",      "ratio",     fx, slot, targetIds);
                parseFxParam (device, "Attack",     "attack",    fx, slot, targetIds);
                parseFxParam (device, "Release",    "release",   fx, slot, targetIds);
                parseFxParam (device, "OutputGain", "makeup",    fx, slot, targetIds);
            }
            else if (fxType == "gate")
            {
                parseFxParam (device, "Threshold", "threshold", fx, slot, targetIds);
                parseFxParam (device, "Range",     "range",     fx, slot, targetIds);
                parseFxParam (device, "Attack",    "attack",    fx, slot, targetIds);
                parseFxParam (device, "Release",   "release",   fx, slot, targetIds);
            }
            else   // limiter
            {
                parseFxParam (device, "InputGain", "input",   fx, slot, targetIds);
                parseFxParam (device, "Release",   "release", fx, slot, targetIds);
            }
            return fx;
        }

        void parsePointsForTrack (const juce::XmlElement& root, Track& track, const juce::String& trackXmlIdStr,
                                   const ParamIdMap& targetIds, juce::StringArray& warningsOut)
        {
            auto* arrangement = root.getChildByName ("Arrangement");
            auto* lanes = arrangement != nullptr ? arrangement->getChildByName ("Lanes") : nullptr;
            if (lanes == nullptr) return;

            int unresolved = 0;
            for (auto* pointsEl : lanes->getChildIterator())
            {
                if (! pointsEl->hasTagName ("Points")) continue;
                if (pointsEl->getStringAttribute ("track") != trackXmlIdStr) continue;

                auto* target = pointsEl->getChildByName ("Target");
                if (target == nullptr) continue;
                const auto targetId = target->getStringAttribute ("parameter");
                if (! targetIds.count (targetId)) { ++unresolved; continue; }

                Track::AutomationLane lane;
                lane.parameterId = targetIds.at (targetId);
                for (auto* pt : pointsEl->getChildIterator())
                {
                    if (pt->hasTagName ("RealPoint"))
                        lane.points.emplace_back (pt->getDoubleAttribute ("time"), (float) pt->getDoubleAttribute ("value"));
                    else if (pt->hasTagName ("BoolPoint"))
                        lane.points.emplace_back (pt->getDoubleAttribute ("time"), pt->getBoolAttribute ("value") ? 1.0f : 0.0f);
                }
                if (! lane.points.empty())
                    track.automation.push_back (lane);
            }

            if (unresolved > 0)
                warningsOut.add (juce::String (unresolved) + " automation lane(s) on track \"" + track.name
                                  + "\" target a parameter KANADE DAW doesn't track and were skipped.");
        }
    }

    void parseDevicesAndAutomation (const juce::XmlElement& root, Project& project, PluginManager& plugins,
                                     const ImportIds& ids, ArchiveReader& archive, juce::StringArray& warningsOut)
    {
        auto* structure = root.getChildByName ("Structure");
        if (structure == nullptr) return;

        for (const auto& [xmlId, kanadeId] : ids.trackByXmlId)
        {
            auto* track = project.findTrack (kanadeId);
            auto* trackEl = findTrackElement (*structure, xmlId);
            if (track == nullptr || trackEl == nullptr) continue;
            auto* channel = trackEl->getChildByName ("Channel");
            if (channel == nullptr) continue;

            ParamIdMap targetIds;
            registerIfIdPresent (channel->getChildByName ("Volume"), "gain", targetIds);
            registerIfIdPresent (channel->getChildByName ("Pan"),    "pan",  targetIds);
            registerIfIdPresent (channel->getChildByName ("Mute"),   "mute", targetIds);

            if (auto* devices = channel->getChildByName ("Devices"))
            {
                int pluginSlot = 0, fxSlot = 0;
                for (auto* device : devices->getChildIterator())
                {
                    if (device->hasTagName ("Vst3Plugin"))
                    {
                        ss::PluginSlot slot;
                        slot.displayName  = device->getStringAttribute ("deviceName");
                        slot.isInstrument = device->getStringAttribute ("deviceRole") == "instrument";
                        slot.bypassed     = ! xmlBoolParam (*device, "Enabled", true);

                        const auto vendor = device->getStringAttribute ("deviceVendor");
                        const auto name   = device->getStringAttribute ("deviceName");
                        bool found = false;
                        for (const auto& desc : plugins.getKnownPluginList().getTypes())
                        {
                            if (desc.pluginFormatName == "VST3" && desc.manufacturerName == vendor && desc.name == name)
                            {
                                slot.identifier = desc.createIdentifierString();
                                found = true;
                                break;
                            }
                        }
                        if (! found)
                        {
                            slot.identifier = "missing:" + vendor + ":" + name;
                            warningsOut.add ("Plugin \"" + name + "\" by \"" + vendor + "\" on track \""
                                              + track->name + "\" is not installed - added as missing.");
                        }

                        if (auto* stateEl = device->getChildByName ("State"))
                        {
                            juce::MemoryBlock bytes;
                            if (archive.readEntry (stateEl->getStringAttribute ("path"), bytes))
                                slot.state = bytes;
                            else
                                warningsOut.add ("Could not read saved state for plugin \"" + name
                                                  + "\" on track \"" + track->name + "\" - state was reset.");
                        }

                        track->plugins.push_back (slot);
                        const int thisSlot = pluginSlot++;

                        if (auto* params = device->getChildByName ("Parameters"))
                            for (auto* p : params->getChildIterator())
                                if (p->hasTagName ("RealParameter") && p->hasAttribute ("id"))
                                    registerIfIdPresent (p, "plugin:" + juce::String (thisSlot) + ":"
                                                             + juce::String (p->getIntAttribute ("parameterID")), targetIds);
                    }
                    else if (auto fxType = std::find_if (builtinFxTagByType().begin(), builtinFxTagByType().end(),
                                                          [device] (const auto& e) { return device->hasTagName (e.second); });
                             fxType != builtinFxTagByType().end())
                    {
                        track->builtinFx.push_back (parseBuiltinFxDevice (*device, fxType->first, fxSlot++, targetIds));
                    }
                    else
                    {
                        // <BuiltinDevice> lands here too: the XSD gives it an empty sequence, so a
                        // generic one carries nothing KANADE DAW could map onto a built-in effect.
                        warningsOut.add ("Skipped an unsupported device (<" + device->getTagName()
                                          + ">) on track \"" + track->name + "\" - KANADE DAW only hosts VST3 plugins"
                                          + " and its own EQ/compressor/gate/limiter.");
                    }
                }
            }

            parsePointsForTrack (root, *track, xmlId, targetIds, warningsOut);
        }
    }

    namespace
    {
        void addSessionClipFromXml (Track& track, SceneId sceneId, const juce::XmlElement& clipEl,
                                     const UnwrappedClipContent& unwrapped, Project& project,
                                     ArchiveReader& archive, juce::StringArray& warningsOut)
        {
            SessionClip sc;
            sc.name = clipEl.getStringAttribute ("name");
            sc.lengthBeats = clipEl.getDoubleAttribute ("duration", 4.0);
            sc.gainDb = unwrapped.gainDb;
            sc.playbackRate = unwrapped.playbackRate;

            if (unwrapped.base->hasTagName ("Audio"))
            {
                sc.kind = SessionClip::Kind::audio;
                auto* fileEl = unwrapped.base->getChildByName ("File");
                if (fileEl == nullptr)
                {
                    warningsOut.add ("Skipped a session clip with no <File> reference.");
                    return;
                }
                const auto storedPath = fileEl->getStringAttribute ("path");
                // Same flattening as addAudioClipFromXml - keep the stored path's uniqueness.
                const auto targetFile = project.getMediaFolder().getChildFile (
                    juce::File::createLegalFileName (storedPath.replaceCharacter ('/', '_')));
                if (! archive.extractEntryToFile (storedPath, targetFile))
                {
                    warningsOut.add ("Skipped session clip - could not extract \"" + storedPath + "\" from the archive.");
                    return;
                }
                sc.sourceFile = targetFile;
            }
            else if (unwrapped.base->hasTagName ("Notes"))
            {
                sc.kind = SessionClip::Kind::midi;
                for (auto* noteEl : unwrapped.base->getChildIterator())
                {
                    if (! noteEl->hasTagName ("Note"))
                        continue;
                    Note note;
                    note.pitch       = noteEl->getIntAttribute ("key", 60);
                    note.startBeats  = noteEl->getDoubleAttribute ("time");
                    note.lengthBeats = noteEl->getDoubleAttribute ("duration", 1.0);
                    note.channel     = noteEl->getIntAttribute ("channel", 0) + 1;
                    note.velocity    = juce::jlimit (1, 127, (int) std::lround (noteEl->getDoubleAttribute ("vel", 100.0 / 127.0) * 127.0));
                    sc.notes.push_back (note);
                }
            }
            else
            {
                warningsOut.add ("Skipped a session clip of unsupported type <" + unwrapped.base->getTagName() + ">.");
                return;
            }

            track.setSessionClip (sceneId, sc);
        }
    }

    void parseScenes (const juce::XmlElement& root, Project& project, const ImportIds& ids,
                       ArchiveReader& archive, juce::StringArray& warningsOut)
    {
        auto* scenesEl = root.getChildByName ("Scenes");
        if (scenesEl == nullptr)
            return;

        for (auto* sceneEl : scenesEl->getChildIterator())
        {
            if (! sceneEl->hasTagName ("Scene"))
                continue;

            auto& scene = project.addScene (sceneEl->getStringAttribute ("name", "Scene"));
            auto* sceneLanes = sceneEl->getChildByName ("Lanes");
            if (sceneLanes == nullptr)
                continue;

            for (auto* slotEl : sceneLanes->getChildIterator())
            {
                if (! slotEl->hasTagName ("ClipSlot"))
                    continue;

                const auto trackXml = slotEl->getStringAttribute ("track");
                if (! ids.trackByXmlId.count (trackXml))
                {
                    warningsOut.add ("Skipped a scene clip for an unrecognised track (\"" + trackXml + "\").");
                    continue;
                }
                auto* track = project.findTrack (ids.trackByXmlId.at (trackXml));
                if (track == nullptr)
                    continue;

                auto* clipEl = slotEl->getChildByName ("Clip");
                if (clipEl == nullptr)
                {
                    warningsOut.add ("Skipped an empty clip slot in scene \"" + scene.name + "\" - it has no <Clip>.");
                    continue;
                }

                const auto unwrapped = unwrapClipContent (*clipEl, project.tempo);
                if (unwrapped.base == nullptr)
                {
                    warningsOut.add ("Skipped a scene clip with no recognised content.");
                    continue;
                }

                addSessionClipFromXml (*track, scene.id, *clipEl, unwrapped, project, archive, warningsOut);
            }
        }
    }
}

namespace ss::io
{
    bool exportDawProject (const juce::File& target, const Project& project, PluginManager& plugins,
                            juce::String& errorOut, juce::StringArray& warningsOut)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        auto root = dawproject::buildProjectSkeleton (project, warningsOut);
        auto* arrangement = root->getChildByName ("Arrangement");
        jassert (arrangement != nullptr);                    // buildProjectSkeleton always writes <Arrangement><Lanes>
        auto* lanes = arrangement->getChildByName ("Lanes");
        jassert (lanes != nullptr);

        dawproject::ArchiveWriter writer;
        dawproject::addClips (*lanes, project, formats, writer, warningsOut);
        dawproject::addDevicesAndAutomation (*root, project, plugins, writer, warningsOut);
        dawproject::addScenes (*root, project, formats, writer, warningsOut);

        juce::XmlElement meta ("MetaData");
        if (project.name.isNotEmpty())
            meta.createNewChildElement ("Title")->addTextElement (project.name);

        writer.addXml ("project.xml", *root);
        writer.addXml ("metadata.xml", meta);

        return writer.writeTo (target, errorOut);
    }

    bool importDawProject (const juce::File& source, Project& project, PluginManager& plugins,
                            juce::String& errorOut, juce::StringArray& warningsOut)
    {
        dawproject::ArchiveReader archive (source);
        auto root = archive.readProjectXml (errorOut);
        if (root == nullptr)
            return false;   // nothing below has touched `project` yet

        // parseClips/parsePointsForTrack only scan direct children of <Arrangement><Lanes>.
        // Other DAWs nest per-track content in a child <Lanes track="...">, which would come
        // through as zero clips and zero automation - warn once here rather than in both.
        if (auto* arrangement = root->getChildByName ("Arrangement"))
            if (auto* lanes = arrangement->getChildByName ("Lanes"))
                if (lanes->getChildByName ("Lanes") != nullptr)
                    warningsOut.add ("This file uses a nested lane structure that KANADE DAW's importer"
                                      " doesn't yet support - some clips or automation from a project made"
                                      " in another DAW may be missing.");

        auto ids = dawproject::parseStructure (*root, project, warningsOut);
        dawproject::parseClips (*root, project, ids, archive, warningsOut);
        dawproject::parseDevicesAndAutomation (*root, project, plugins, ids, archive, warningsOut);
        dawproject::parseScenes (*root, project, ids, archive, warningsOut);

        return true;
    }
}
