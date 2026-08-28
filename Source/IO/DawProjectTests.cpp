#include "IO/DawProject.h"

namespace ss
{

class DawProjectUnitTests final : public juce::UnitTest
{
public:
    DawProjectUnitTests() : juce::UnitTest ("DawProject", "ScoreSmith") {}

    // Several XSD types declare ordered (xs:sequence) content - this makes the child order
    // readable in one expectEquals instead of a pile of index arithmetic.
    static juce::String childTagOrder (const juce::XmlElement& parent)
    {
        juce::StringArray tags;
        for (auto* c : parent.getChildIterator())
            tags.add (c->getTagName());
        return tags.joinIntoString (",");
    }

    void runTest() override
    {
        beginTest ("ArchiveWriter/ArchiveReader round-trip an XML root and a byte entry");
        {
            juce::TemporaryFile temp (".dawproject");

            juce::XmlElement root ("Project");
            root.setAttribute ("version", "1.0");
            auto* app = root.createNewChildElement ("Application");
            app->setAttribute ("name", "KANADE DAW");
            app->setAttribute ("version", "0.2.0");

            const juce::MemoryBlock payload ("hello-audio-bytes", 17);

            io::dawproject::ArchiveWriter writer;
            writer.addXml ("project.xml", root);
            writer.addBytes ("audio/test.txt", payload);

            juce::String writeError;
            expect (writer.writeTo (temp.getFile(), writeError), writeError);

            io::dawproject::ArchiveReader reader (temp.getFile());

            juce::String readError;
            auto readRoot = reader.readProjectXml (readError);
            expect (readRoot != nullptr, readError);
            expectEquals (readRoot->getStringAttribute ("version"), juce::String ("1.0"));
            auto* readApp = readRoot->getChildByName ("Application");
            expect (readApp != nullptr);
            expectEquals (readApp->getStringAttribute ("name"), juce::String ("KANADE DAW"));

            juce::MemoryBlock readPayload;
            expect (reader.readEntry ("audio/test.txt", readPayload));
            expect (readPayload == payload);
        }

        beginTest ("ArchiveReader.readProjectXml fails cleanly on a non-zip file");
        {
            juce::TemporaryFile temp (".dawproject");
            temp.getFile().replaceWithText ("not a zip file at all");

            io::dawproject::ArchiveReader reader (temp.getFile());
            juce::String error;
            auto result = reader.readProjectXml (error);
            expect (result == nullptr);
            expect (error.isNotEmpty());
        }

        beginTest ("buildProjectSkeleton maps tracks, buses, master, tempo and markers");
        {
            Project project;
            project.tempo.setEvents ({ { 0.0, 140.0 } });
            project.tempo.setTimeSignatures ({ { 0.0, 3, 4 } });
            project.markers.push_back ({ 8.0, "Chorus" });

            auto& bus = project.addBus ("Drum Bus");

            auto& guitar = project.addTrack (TrackType::audio, "Guitar");
            guitar.gainDb = -3.0f;
            guitar.pan = -0.5f;
            guitar.muted = true;
            guitar.outputBus = bus.id;
            guitar.sends.push_back ({ bus.id, 0.25f });

            project.addTrack (TrackType::midi, "Synth");
            project.addTrack (TrackType::utau, "Vocal");

            juce::StringArray warnings;
            auto root = io::dawproject::buildProjectSkeleton (project, warnings);

            expect (root != nullptr);
            expectEquals (root->getStringAttribute ("version"), juce::String ("1.0"));

            auto* transport = root->getChildByName ("Transport");
            expect (transport != nullptr);
            expectWithinAbsoluteError (transport->getChildByName ("Tempo")->getDoubleAttribute ("value"), 140.0, 1.0e-6);
            auto* ts = transport->getChildByName ("TimeSignature");
            expectEquals (ts->getIntAttribute ("numerator"), 3);
            expectEquals (ts->getIntAttribute ("denominator"), 4);

            auto* structure = root->getChildByName ("Structure");
            expect (structure != nullptr);

            int numTracks = 0;
            for (auto* t : structure->getChildIterator())
                if (t->hasTagName ("Track")) ++numTracks;
            expectEquals (numTracks, 4);   // Guitar + Synth + Bus-track + Master (Vocal/utau skipped)

            auto* guitarTrack = structure->getChildByName ("Track");   // first <Track> == first non-utau KANADE track
            expectEquals (guitarTrack->getStringAttribute ("name"), juce::String ("Guitar"));
            expectEquals (guitarTrack->getStringAttribute ("contentType"), juce::String ("audio"));

            auto* guitarChannel = guitarTrack->getChildByName ("Channel");
            expect (guitarChannel != nullptr);
            expectWithinAbsoluteError (guitarChannel->getChildByName ("Volume")->getDoubleAttribute ("value"), -3.0, 1.0e-6);
            expectEquals (guitarChannel->getChildByName ("Mute")->getStringAttribute ("value"), juce::String ("true"));
            expectEquals (guitarChannel->getStringAttribute ("destination"), io::dawproject::busChannelXmlId (bus.id));

            auto* sends = guitarChannel->getChildByName ("Sends");
            expect (sends != nullptr);
            auto* send = sends->getChildByName ("Send");
            expect (send != nullptr);
            expectEquals (send->getStringAttribute ("type"), juce::String ("post"));

            expectEquals (warnings.size(), 1);
            expect (warnings[0].contains ("Vocal"));

            auto* markers = root->getChildByName ("Arrangement")->getChildByName ("Markers");
            expect (markers != nullptr);
            expectWithinAbsoluteError (markers->getChildByName ("Marker")->getDoubleAttribute ("time"), 8.0, 1.0e-6);
        }

        beginTest ("addClips writes audio and MIDI clips, with gain and playbackRate wrapping");
        {
            Project project;
            project.tempo.setEvents ({ { 0.0, 120.0 } });   // 1 beat == 0.5s at 120bpm

            juce::TemporaryFile wav (".wav");
            {
                juce::WavAudioFormat wavFormat;
                std::unique_ptr<juce::FileOutputStream> out (wav.getFile().createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (
                    wavFormat.createWriterFor (out.get(), 48000.0, 2, 16, {}, 0));
                expect (writer != nullptr);
                out.release();   // writer now owns the stream
                juce::AudioBuffer<float> silence (2, 4800);
                silence.clear();
                writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());
            }

            auto& track = project.addTrack (TrackType::audio, "Guitar");
            auto& clip = track.audioClips.emplace_back();
            clip.id = project.nextClipId();
            clip.name = "Take 1";
            clip.sourceFile = wav.getFile();
            clip.startBeats = 4.0;
            clip.lengthBeats = 2.0;
            clip.gainDb = -6.0f;
            clip.playbackRate = 2.0;

            auto& midiTrack = project.addTrack (TrackType::midi, "Synth");
            auto& midiClip = midiTrack.midiClips.emplace_back();
            midiClip.id = project.nextClipId();
            midiClip.startBeats = 0.0;
            midiClip.lengthBeats = 4.0;
            midiClip.notes.push_back ({ 60, 0.0, 1.0, 127, 1, 1.0f });

            juce::StringArray warnings;
            auto root = io::dawproject::buildProjectSkeleton (project, warnings);
            auto* lanes = root->getChildByName ("Arrangement")->getChildByName ("Lanes");

            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            io::dawproject::ArchiveWriter writer;
            io::dawproject::addClips (*lanes, project, formats, writer, warnings);

            juce::XmlElement* audioClipsEl = nullptr;
            juce::XmlElement* midiClipsEl  = nullptr;
            for (auto* el : lanes->getChildIterator())
            {
                if (! el->hasTagName ("Clips")) continue;
                if (el->getStringAttribute ("track") == io::dawproject::trackXmlId (track.getId()))
                    audioClipsEl = el;
                if (el->getStringAttribute ("track") == io::dawproject::trackXmlId (midiTrack.getId()))
                    midiClipsEl = el;
            }
            expect (audioClipsEl != nullptr);
            expect (midiClipsEl  != nullptr);

            auto* clipEl = audioClipsEl->getChildByName ("Clip");
            expect (clipEl != nullptr);
            expectWithinAbsoluteError (clipEl->getDoubleAttribute ("time"), 4.0, 1.0e-6);
            expectWithinAbsoluteError (clipEl->getDoubleAttribute ("duration"), 2.0, 1.0e-6);

            // gainDb != 0 -> wrapped in <Lanes><Points Target expression="gain">...
            auto* gainLanes = clipEl->getChildByName ("Lanes");
            expect (gainLanes != nullptr);
            auto* points = gainLanes->getChildByName ("Points");
            expect (points != nullptr);
            expectEquals (points->getChildByName ("Target")->getStringAttribute ("expression"), juce::String ("gain"));
            expectWithinAbsoluteError (points->getChildByName ("RealPoint")->getDoubleAttribute ("value"), -6.0, 1.0e-6);

            // playbackRate != 1 -> <Warps> nested inside that <Lanes>, wrapping <Audio>
            auto* warps = gainLanes->getChildByName ("Warps");
            expect (warps != nullptr);
            expect (warps->getChildByName ("Audio") != nullptr);
            expectEquals (warps->getChildByName ("Audio")->getIntAttribute ("channels"), 2);

            auto* midiClipEl = midiClipsEl->getChildByName ("Clip");
            expect (midiClipEl != nullptr);
            auto* notesEl = midiClipEl->getChildByName ("Notes");
            expect (notesEl != nullptr);
            auto* noteEl = notesEl->getChildByName ("Note");
            expect (noteEl != nullptr);
            expectEquals (noteEl->getIntAttribute ("key"), 60);
            expectEquals (noteEl->getIntAttribute ("channel"), 0);   // KANADE 1-based -> DAWproject 0-based

            expect (warnings.isEmpty());
        }

        beginTest ("addDevicesAndAutomation maps plugin state, builtin FX, and automation lanes");
        {
            Project project;
            auto& track = project.addTrack (TrackType::audio, "Guitar");

            ss::PluginSlot plugin;
            plugin.identifier = "unknown-plugin-id";   // not installed - exercises the "loaded=false" path
            plugin.displayName = "Some Amp Sim";
            plugin.isInstrument = false;
            plugin.state.append ("fake-state-bytes", 16);
            track.plugins.push_back (plugin);

            ss::BuiltinFxSlot comp;
            comp.type = "compressor";
            comp.params.set ("threshold", -20.0);
            comp.params.set ("ratio", 4.0);
            comp.params.set ("attack", 10.0);
            comp.params.set ("release", 120.0);
            comp.params.set ("makeup", 2.0);
            comp.params.set ("knee", 6.0);
            comp.params.set ("mix", 1.0);
            track.builtinFx.push_back (comp);

            ss::BuiltinFxSlot verb;
            verb.type = "reverb";
            track.builtinFx.push_back (verb);

            track.automation.push_back ({ "gain", { { 0.0, -6.0f }, { 4.0, 0.0f } } });
            track.automation.push_back ({ "plugin:0:3", { { 0.0, 0.2f }, { 2.0, 0.8f } } });
            track.automation.push_back ({ "fx:0:threshold", { { 0.0, -30.0f } } });
            track.automation.push_back ({ "fx:0:knee", { { 0.0, 0.5f } } });   // no DAWproject id - must be skipped silently

            juce::StringArray warnings;
            Settings settings;
            PluginManager plugins (settings);
            io::dawproject::ArchiveWriter writer;
            auto root = io::dawproject::buildProjectSkeleton (project, warnings);
            io::dawproject::addDevicesAndAutomation (*root, project, plugins, writer, warnings);

            auto* channel = root->getChildByName ("Structure")->getChildByName ("Track")->getChildByName ("Channel");
            auto* devices = channel->getChildByName ("Devices");
            expect (devices != nullptr);

            auto* vst = devices->getChildByName ("Vst3Plugin");
            expect (vst != nullptr);
            expectEquals (vst->getStringAttribute ("loaded"), juce::String ("false"));
            expect (vst->getChildByName ("State") != nullptr);
            auto* pluginParams = vst->getChildByName ("Parameters");
            expect (pluginParams != nullptr);
            expectEquals (pluginParams->getChildByName ("RealParameter")->getIntAttribute ("parameterID"), 3);
            // XSD `device` sequence: Parameters, Enabled, State.
            expectEquals (childTagOrder (*vst), juce::String ("Parameters,Enabled,State"));

            // Built-in FX are direct children of <Devices>, never wrapped in a <BuiltinDevice>.
            auto* compressorEl = devices->getChildByName ("Compressor");
            expect (compressorEl != nullptr);
            expect (devices->getChildByName ("BuiltinDevice") == nullptr);
            expectEquals (compressorEl->getStringAttribute ("deviceName"), juce::String ("Compressor"));   // required by the XSD
            expectWithinAbsoluteError (compressorEl->getChildByName ("Threshold")->getDoubleAttribute ("value"), -20.0, 1.0e-6);
            expectWithinAbsoluteError (compressorEl->getChildByName ("OutputGain")->getDoubleAttribute ("value"), 2.0, 1.0e-6);
            expect (compressorEl->getChildByName ("Threshold")->getStringAttribute ("id").isNotEmpty());   // automated
            // XSD `compressor` sequence, after the inherited Parameters/Enabled/State.
            expectEquals (childTagOrder (*compressorEl),
                          juce::String ("Enabled,Attack,OutputGain,Ratio,Release,Threshold"));

            int numFxDevices = 0;
            for (auto* d : devices->getChildIterator())
                if (! d->hasTagName ("Vst3Plugin")) ++numFxDevices;
            expectEquals (numFxDevices, 1);   // reverb skipped
            expect (warnings.size() == 1 && warnings[0].contains ("reverb"));

            // XSD `channel` sequence: Devices, Mute, Pan, Sends, Volume (no Sends here).
            expectEquals (childTagOrder (*channel), juce::String ("Devices,Mute,Pan,Volume"));

            auto* lanes = root->getChildByName ("Arrangement")->getChildByName ("Lanes");
            int numPointsLanes = 0;
            bool foundKneeTarget = false;
            for (auto* p : lanes->getChildIterator())
            {
                if (! p->hasTagName ("Points")) continue;
                ++numPointsLanes;
                auto* target = p->getChildByName ("Target");
                if (target != nullptr && target->getStringAttribute ("parameter").contains ("knee"))
                    foundKneeTarget = true;
            }
            expectEquals (numPointsLanes, 3);   // gain, one plugin param, one fx param - "knee" has no id, skipped silently
            expect (! foundKneeTarget);         // never a dangling IDREF for an unmapped fx sub-parameter
        }

        beginTest ("exportDawProject writes a readable archive with structure, clips and a scene");
        {
            Project project;
            project.name = "Demo Song";
            auto& scene = project.addScene ("Verse");

            auto& track = project.addTrack (TrackType::midi, "Synth");
            SessionClip sc;
            sc.kind = SessionClip::Kind::midi;
            sc.name = "Riff";
            sc.lengthBeats = 4.0;
            sc.notes.push_back ({ 67, 0.0, 1.0, 100, 1, 1.0f });
            track.setSessionClip (scene.id, sc);

            juce::TemporaryFile temp (".dawproject");
            juce::String error;
            juce::StringArray warnings;
            Settings settings;
            PluginManager plugins (settings);

            const bool ok = io::exportDawProject (temp.getFile(), project, plugins, error, warnings);
            expect (ok, error);
            expect (temp.getFile().existsAsFile());

            io::dawproject::ArchiveReader reader (temp.getFile());
            juce::String readError;
            auto root = reader.readProjectXml (readError);
            expect (root != nullptr, readError);
            expectEquals (root->getChildByName ("Application")->getStringAttribute ("name"), juce::String ("KANADE DAW"));

            juce::MemoryBlock metaBytes;
            expect (reader.readEntry ("metadata.xml", metaBytes));
            auto meta = juce::parseXML (metaBytes.toString());
            expect (meta != nullptr);
            expectEquals (meta->getChildByName ("Title")->getAllSubText(), juce::String ("Demo Song"));

            auto* scenesEl = root->getChildByName ("Scenes");
            expect (scenesEl != nullptr);
            auto* sceneEl = scenesEl->getChildByName ("Scene");
            expect (sceneEl != nullptr);
            auto* slot = sceneEl->getChildByName ("Lanes")->getChildByName ("ClipSlot");
            expect (slot != nullptr);
            expectEquals (slot->getStringAttribute ("track"), io::dawproject::trackXmlId (track.getId()));
            expect (slot->getChildByName ("Clip")->getChildByName ("Notes") != nullptr);
        }

        beginTest ("exportDawProject replaces an existing file instead of appending to it");
        {
            Project project;
            project.name = "Overwrite Me";
            auto& track = project.addTrack (TrackType::midi, "Synth");
            auto& clip = track.midiClips.emplace_back();
            clip.id = project.nextClipId();
            clip.startBeats = 0.0;
            clip.lengthBeats = 4.0;
            clip.notes.push_back ({ 60, 0.0, 1.0, 100, 1, 1.0f });

            // juce::TemporaryFile hands out a path but never creates the file, so it can't
            // exercise the bug at all - the target has to genuinely exist before export #2.
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("kanade-dawproject-overwrite-" + juce::Uuid().toString());
            dir.createDirectory();
            const auto target = dir.getChildFile ("twice.dawproject");

            juce::String error;
            juce::StringArray warnings;
            Settings settings;
            PluginManager plugins (settings);

            expect (io::exportDawProject (target, project, plugins, error, warnings), error);
            const auto firstSize = target.getSize();
            expect (firstSize > 0);
            expect (io::exportDawProject (target, project, plugins, error, warnings), error);
            expectEquals (target.getSize(), firstSize, "re-export must replace the file, not append to it");

            Project imported;
            juce::StringArray importWarnings;
            expect (io::importDawProject (target, imported, plugins, error, importWarnings), error);
            expectEquals (imported.getNumTracks(), 1);
            expect (! imported.getTrack (0).midiClips.empty());

            dir.deleteRecursively();
        }

        beginTest ("buildProjectSkeleton warns when a tempo or time-signature map is truncated");
        {
            Project project;
            project.tempo.setEvents ({ { 0.0, 120.0 }, { 8.0, 150.0 } });

            juce::StringArray warnings;
            auto root = io::dawproject::buildProjectSkeleton (project, warnings);
            expect (root != nullptr);
            expectWithinAbsoluteError (root->getChildByName ("Transport")->getChildByName ("Tempo")
                                           ->getDoubleAttribute ("value"), 120.0, 1.0e-6);
            expectEquals (warnings.size(), 1);
            expect (warnings[0].contains ("tempo"));
        }

        beginTest ("exportDawProject skips unreadable session audio clip and does not create an empty ClipSlot");
        {
            Project project;
            project.name = "Skip Test";
            auto& scene = project.addScene ("Section");

            auto& audioTrack = project.addTrack (TrackType::audio, "Bad Track");
            SessionClip badClip;
            badClip.kind = SessionClip::Kind::audio;
            badClip.name = "Nonexistent";
            badClip.sourceFile = juce::File ("/nonexistent/file.wav");
            badClip.lengthBeats = 2.0;
            audioTrack.setSessionClip (scene.id, badClip);

            juce::TemporaryFile temp (".dawproject");
            juce::String error;
            juce::StringArray warnings;
            Settings settings;
            PluginManager plugins (settings);

            const bool ok = io::exportDawProject (temp.getFile(), project, plugins, error, warnings);
            expect (ok, error);   // export succeeds (warning, not failure)
            expectGreaterThan (warnings.size(), 0);
            expect (warnings[0].contains ("Skipped") && warnings[0].contains ("Nonexistent"));

            io::dawproject::ArchiveReader reader (temp.getFile());
            juce::String readError;
            auto root = reader.readProjectXml (readError);
            expect (root != nullptr, readError);

            auto* scenesEl = root->getChildByName ("Scenes");
            expect (scenesEl != nullptr);
            auto* sceneEl = scenesEl->getChildByName ("Scene");
            expect (sceneEl != nullptr);
            auto* lanes = sceneEl->getChildByName ("Lanes");
            expect (lanes != nullptr);
            int clipSlotCount = 0;
            for (auto* child : lanes->getChildIterator())
                if (child->hasTagName ("ClipSlot"))
                    ++clipSlotCount;
            expectEquals (clipSlotCount, 0, "No ClipSlot should be created for unreadable audio file");
        }

        beginTest ("parseStructure rebuilds tracks, a bus, routing, sends and tempo from XML");
        {
            const auto xml = juce::String (R"XML(
<Project version="1.0">
  <Application name="Bitwig Studio" version="5.0.9"/>
  <Transport>
    <Tempo unit="bpm" value="128"/>
    <TimeSignature numerator="7" denominator="8"/>
  </Transport>
  <Structure>
    <Track id="idGuitar" name="Guitar" contentType="audio" color="#ff0000">
      <Channel id="chGuitar" role="regular" destination="chBus" solo="false">
        <Volume unit="decibel" value="-4.5"/>
        <Pan unit="linear" value="0.25"/>
        <Mute value="true"/>
        <Sends>
          <Send destination="chBus" type="post"><Volume unit="linear" value="0.6"/></Send>
        </Sends>
      </Channel>
    </Track>
    <Track id="idBus" name="Drum Bus" contentType="audio">
      <Channel id="chBus" role="submix" destination="chMaster"/>
    </Track>
    <Track id="idMaster" name="Master" contentType="audio">
      <Channel id="chMaster" role="master"/>
    </Track>
  </Structure>
</Project>
)XML");
            auto root = juce::parseXML (xml);
            expect (root != nullptr);

            Project project;
            juce::StringArray warnings;
            auto ids = io::dawproject::parseStructure (*root, project, warnings);

            expectWithinAbsoluteError (project.tempo.bpmAt (0.0), 128.0, 1.0e-6);
            const auto ts = project.tempo.timeSignatureAt (0.0);
            expectEquals (ts.numerator, 7);
            expectEquals (ts.denominator, 8);

            expectEquals (project.getNumTracks(), 1);   // bus and master are not regular Tracks
            expectEquals ((int) project.buses.size(), 1);

            auto& guitar = project.getTrack (0);
            expectEquals (guitar.name, juce::String ("Guitar"));
            expectWithinAbsoluteError (guitar.gainDb, -4.5f, 1.0e-4f);
            expectWithinAbsoluteError (guitar.pan, 0.25f, 1.0e-4f);
            expect (guitar.muted);
            expectEquals (guitar.colour.getAlpha(), (juce::uint8) 0xff);
            expectEquals (guitar.outputBus, project.buses[0].id);
            expect (! guitar.sends.empty());
            expectEquals (guitar.sends[0].busId, project.buses[0].id);
            expectWithinAbsoluteError (guitar.sends[0].level, 0.6f, 1.0e-4f);

            expect (ids.trackByXmlId.count ("idGuitar") == 1);
            expectEquals (ids.trackByXmlId.at ("idGuitar"), guitar.getId());
            expect (ids.busIdByChannelXmlId.count ("chBus") == 1);
            expectEquals (ids.masterChannelXmlId, juce::String ("chMaster"));
        }

        beginTest ("parseClips unwraps gain/warp wrappers and reads audio + MIDI clips");
        {
            juce::TemporaryFile wav (".wav");
            {
                juce::WavAudioFormat wavFormat;
                std::unique_ptr<juce::FileOutputStream> out (wav.getFile().createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (
                    wavFormat.createWriterFor (out.get(), 48000.0, 2, 16, {}, 0));
                out.release();
                juce::AudioBuffer<float> silence (2, 4800);
                silence.clear();
                writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());
            }

            io::dawproject::ArchiveWriter archiveWriter;
            archiveWriter.addFile ("audio/take.wav", wav.getFile());
            archiveWriter.addFile ("audio/alt/take.wav", wav.getFile());   // same basename, different entry
            juce::XmlElement dummyProjectXml ("Project");
            archiveWriter.addXml ("project.xml", dummyProjectXml);
            juce::TemporaryFile archiveFile (".dawproject");
            juce::String writeError;
            expect (archiveWriter.writeTo (archiveFile.getFile(), writeError), writeError);

            const auto xml = juce::String (R"XML(
<Project version="1.0">
  <Structure>
    <Track id="idGuitar" name="Guitar" contentType="audio"><Channel id="chGuitar" role="regular"/></Track>
    <Track id="idSynth"  name="Synth"  contentType="notes"><Channel id="chSynth"  role="regular"/></Track>
  </Structure>
  <Arrangement>
    <Lanes timeUnit="beats">
      <Clips track="idGuitar">
        <Clip time="4" duration="2">
          <Lanes>
            <Warps contentTimeUnit="seconds">
              <Audio channels="2" sampleRate="48000" duration="0.1"><File path="audio/take.wav"/></Audio>
              <Warp time="0" contentTime="0"/>
              <Warp time="2" contentTime="2"/>
            </Warps>
            <Points unit="decibel"><Target expression="gain"/><RealPoint time="0" value="-6"/></Points>
          </Lanes>
        </Clip>
        <Clip time="8" duration="2">
          <Audio channels="2" sampleRate="48000" duration="0.1"><File path="audio/alt/take.wav"/></Audio>
        </Clip>
      </Clips>
      <Clips track="idSynth">
        <Clip time="0" duration="4">
          <Notes>
            <Note time="0" duration="1" key="60" channel="0" vel="1.0"/>
          </Notes>
        </Clip>
      </Clips>
    </Lanes>
  </Arrangement>
</Project>
)XML");
            auto root = juce::parseXML (xml);
            expect (root != nullptr);

            Project project;
            juce::StringArray warnings;
            auto ids = io::dawproject::parseStructure (*root, project, warnings);

            io::dawproject::ArchiveReader reader (archiveFile.getFile());
            io::dawproject::parseClips (*root, project, ids, reader, warnings);

            auto& guitar = project.getTrack (0);
            expect (! guitar.audioClips.empty());
            auto& audioClip = guitar.audioClips[0];
            expectWithinAbsoluteError (audioClip.startBeats, 4.0, 1.0e-6);
            expectWithinAbsoluteError (audioClip.gainDb, -6.0f, 1.0e-4f);
            // dContent (2-0) / dTimeSeconds (2 beats at the default 120bpm == 1.0s) == 2.0
            expectWithinAbsoluteError (audioClip.playbackRate, 2.0, 1.0e-6);
            expect (audioClip.sourceFile.existsAsFile());

            // Two clips whose stored paths share a basename must extract to distinct files -
            // the extracted name is derived from the whole path, not just its last segment.
            expectEquals ((int) guitar.audioClips.size(), 2);
            expect (guitar.audioClips[1].sourceFile.existsAsFile());
            expect (guitar.audioClips[0].sourceFile != guitar.audioClips[1].sourceFile);

            auto& synth = project.getTrack (1);
            expect (! synth.midiClips.empty());
            expectEquals (synth.midiClips[0].notes[0].pitch, 60);
            expectEquals (synth.midiClips[0].notes[0].channel, 1);   // 0-based -> 1-based
        }

        beginTest ("parseDevicesAndAutomation rebuilds plugin state, builtin FX and automation");
        {
            const auto xml = juce::String (R"XML(
<Project version="1.0">
  <Structure>
    <Track id="idGuitar" name="Guitar" contentType="audio">
      <Channel id="chGuitar" role="regular">
        <Volume unit="decibel" value="0" id="volId"/>
        <Devices>
          <Vst3Plugin id="devPlugin" deviceRole="audioFX" deviceName="Nope" deviceVendor="Nobody" loaded="false">
            <State path="plugins/state.bin"/>
            <Parameters><RealParameter id="p0" parameterID="2" unit="normalized"/></Parameters>
          </Vst3Plugin>
          <Vst3Plugin id="devPlugin2" deviceRole="audioFX" deviceName="Broken" deviceVendor="Nobody" loaded="false">
            <State path="plugins/nonexistent.vst3-preset"/>
          </Vst3Plugin>
          <Compressor id="devFx" deviceRole="audioFX" deviceName="Compressor">
            <OutputGain unit="normalized" value="0.5"/>
            <Ratio unit="normalized" value="0.3"/>
            <Threshold unit="normalized" value="0.7" id="threshId"/>
          </Compressor>
          <ClapPlugin id="devClap" deviceRole="audioFX" deviceName="Unsupported" deviceVendor="NoBody" loaded="false"/>
          <BuiltinDevice id="devGeneric" deviceRole="audioFX" deviceName="Something Else"/>
        </Devices>
      </Channel>
    </Track>
  </Structure>
  <Arrangement>
    <Lanes timeUnit="beats">
      <Points track="idGuitar" unit="normalized"><Target parameter="volId"/><RealPoint time="0" value="0.6"/></Points>
      <Points track="idGuitar" unit="normalized"><Target parameter="p0"/><RealPoint time="0" value="0.9"/></Points>
      <Points track="idGuitar" unit="normalized"><Target parameter="threshId"/><RealPoint time="0" value="0.4"/></Points>
      <Points track="idGuitar" unit="normalized"><Target parameter="unknownParamId"/><RealPoint time="0" value="0.5"/></Points>
    </Lanes>
  </Arrangement>
</Project>
)XML");
            auto root = juce::parseXML (xml);
            expect (root != nullptr);

            io::dawproject::ArchiveWriter archiveWriter;
            juce::MemoryBlock stateBytes ("plugin-state", 12);
            archiveWriter.addBytes ("plugins/state.bin", stateBytes);
            juce::XmlElement dummy ("Project");
            archiveWriter.addXml ("project.xml", dummy);
            juce::TemporaryFile archiveFile (".dawproject");
            juce::String writeError;
            expect (archiveWriter.writeTo (archiveFile.getFile(), writeError), writeError);

            Project project;
            juce::StringArray warnings;
            auto ids = io::dawproject::parseStructure (*root, project, warnings);

            Settings settings;
            PluginManager plugins (settings);
            io::dawproject::ArchiveReader reader (archiveFile.getFile());
            io::dawproject::parseDevicesAndAutomation (*root, project, plugins, ids, reader, warnings);

            auto& guitar = project.getTrack (0);
            expectEquals ((int) guitar.plugins.size(), 2);          // <ClapPlugin> is unsupported - not added
            expect (guitar.plugins[0].state == stateBytes);

            // Unreadable <State> archive entry: slot is still kept (unlike an unreadable
            // audio clip, which is dropped), just with empty/default state and a warning.
            expect (guitar.plugins[1].state.getSize() == 0);

            expectEquals ((int) guitar.builtinFx.size(), 1);
            expectEquals (guitar.builtinFx[0].type, juce::String ("compressor"));
            expectWithinAbsoluteError ((double) guitar.builtinFx[0].params["threshold"], 0.7, 1.0e-6);
            expectWithinAbsoluteError ((double) guitar.builtinFx[0].params["makeup"], 0.5, 1.0e-6);

            bool foundGain = false, foundPlugin = false, foundFx = false;
            for (const auto& lane : guitar.automation)
            {
                if (lane.parameterId == "gain")            { foundGain = true;   expectWithinAbsoluteError (lane.points[0].second, 0.6f, 1.0e-4f); }
                if (lane.parameterId == "plugin:0:2")       { foundPlugin = true; expectWithinAbsoluteError (lane.points[0].second, 0.9f, 1.0e-4f); }
                if (lane.parameterId == "fx:0:threshold")   { foundFx = true;     expectWithinAbsoluteError (lane.points[0].second, 0.4f, 1.0e-4f); }
            }
            expect (foundGain && foundPlugin && foundFx);
            expectEquals ((int) guitar.automation.size(), 3);   // unknownParamId target resolved to nothing - no 4th lane

            // Nothing may be silently dropped without a warning: the unsupported device,
            // the one unresolved automation target, and the unreadable plugin state must
            // each surface one.
            bool foundUnsupportedDeviceWarning = false;
            bool foundUnresolvedAutomationWarning = false;
            bool foundUnreadableStateWarning = false;
            bool foundGenericBuiltinWarning = false;
            for (const auto& w : warnings)
            {
                if (w.contains ("BuiltinDevice") && w.contains ("Guitar")) foundGenericBuiltinWarning = true;
                if (w.contains ("ClapPlugin") && w.contains ("Guitar")) foundUnsupportedDeviceWarning = true;
                if (w.contains ("1 automation lane") && w.contains ("Guitar")) foundUnresolvedAutomationWarning = true;
                if (w.contains ("Broken") && w.contains ("Guitar")) foundUnreadableStateWarning = true;
            }
            expect (foundUnsupportedDeviceWarning);
            expect (foundUnresolvedAutomationWarning);
            expect (foundUnreadableStateWarning);
            // A bare <BuiltinDevice> carries nothing mappable (empty sequence in the XSD) -
            // it must warn like any other unsupported device rather than vanish.
            expect (foundGenericBuiltinWarning);
        }

        beginTest ("importDawProject reads structure/clips/scenes and rejects a corrupt file cleanly");
        {
            // Round-trip through our own exporter first, the cheapest way to get a
            // realistic, fully-formed archive to import back.
            Project original;
            original.name = "Round Trip";
            auto& scene = original.addScene ("Verse");
            auto& track = original.addTrack (TrackType::midi, "Synth");
            SessionClip sc;
            sc.kind = SessionClip::Kind::midi;
            sc.lengthBeats = 4.0;
            sc.notes.push_back ({ 64, 0.0, 1.0, 90, 1, 1.0f });
            track.setSessionClip (scene.id, sc);

            auto& audioTrack = original.addTrack (TrackType::audio, "Guitar");
            audioTrack.gainDb = -2.0f;

            juce::TemporaryFile temp (".dawproject");
            juce::String exportError;
            juce::StringArray exportWarnings;
            Settings settings;
            PluginManager plugins (settings);
            expect (io::exportDawProject (temp.getFile(), original, plugins, exportError, exportWarnings), exportError);

            Project imported;
            juce::String importError;
            juce::StringArray importWarnings;
            const bool ok = io::importDawProject (temp.getFile(), imported, plugins, importError, importWarnings);
            expect (ok, importError);

            expectEquals (imported.getNumTracks(), 2);
            bool foundSynth = false, foundGuitar = false;
            for (int i = 0; i < imported.getNumTracks(); ++i)
            {
                auto& t = imported.getTrack (i);
                if (t.name == "Synth")  { foundSynth = true;  expectEquals ((int) t.sessionSlots.size(), 1); }
                if (t.name == "Guitar") { foundGuitar = true; expectWithinAbsoluteError (t.gainDb, -2.0f, 1.0e-4f); }
            }
            expect (foundSynth && foundGuitar);

            // A genuinely malformed file must fail cleanly and leave `project` untouched.
            Project untouched;
            untouched.name = "Do Not Touch";
            juce::TemporaryFile badFile (".dawproject");
            badFile.getFile().replaceWithText ("this is not a zip file");

            juce::String badError;
            juce::StringArray badWarnings;
            const bool badOk = io::importDawProject (badFile.getFile(), untouched, plugins, badError, badWarnings);
            expect (! badOk);
            expect (badError.isNotEmpty());
            expectEquals (untouched.getNumTracks(), 0);
            expectEquals (untouched.name, juce::String ("Do Not Touch"));
        }

        beginTest ("full round trip: structure, bus, clips, builtin FX, automation, scenes, markers");
        {
            Project original;
            original.name = "Full Fidelity";
            original.tempo.setEvents ({ { 0.0, 95.0 } });
            original.tempo.setTimeSignatures ({ { 0.0, 5, 4 } });
            original.markers.push_back ({ 16.0, "Bridge" });

            auto& bus = original.addBus ("Drum Bus");
            bus.gainDb = -1.5f;

            auto& drums = original.addTrack (TrackType::audio, "Drums");
            drums.outputBus = bus.id;
            drums.gainDb = 1.0f;

            ss::BuiltinFxSlot comp;
            comp.type = "compressor";
            comp.params.set ("threshold", 0.3);
            comp.params.set ("ratio", 0.5);
            comp.params.set ("attack", 0.2);
            comp.params.set ("release", 0.4);
            comp.params.set ("makeup", 0.6);
            drums.builtinFx.push_back (comp);
            drums.automation.push_back ({ "fx:0:threshold", { { 0.0, 0.3f }, { 8.0, 0.1f } } });
            drums.automation.push_back ({ "gain", { { 0.0, 0.5f }, { 8.0, 0.9f } } });

            juce::TemporaryFile wav (".wav");
            {
                juce::WavAudioFormat wavFormat;
                std::unique_ptr<juce::FileOutputStream> out (wav.getFile().createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer (
                    wavFormat.createWriterFor (out.get(), 48000.0, 2, 16, {}, 0));
                out.release();
                juce::AudioBuffer<float> silence (2, 4800);
                silence.clear();
                writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());
            }
            auto& kickClip = drums.audioClips.emplace_back();
            kickClip.id = original.nextClipId();
            kickClip.sourceFile = wav.getFile();
            kickClip.startBeats = 0.0;
            kickClip.lengthBeats = 4.0;

            auto& synth = original.addTrack (TrackType::midi, "Synth");
            auto& synthClip = synth.midiClips.emplace_back();
            synthClip.id = original.nextClipId();
            synthClip.startBeats = 0.0;
            synthClip.lengthBeats = 4.0;
            synthClip.notes.push_back ({ 72, 0.0, 2.0, 110, 1, 1.0f });

            auto& scene = original.addScene ("Chorus");
            SessionClip sessionClip;
            sessionClip.kind = SessionClip::Kind::midi;
            sessionClip.lengthBeats = 2.0;
            sessionClip.notes.push_back ({ 75, 0.0, 1.0, 100, 1, 1.0f });
            synth.setSessionClip (scene.id, sessionClip);

            juce::TemporaryFile archive (".dawproject");
            juce::String error;
            juce::StringArray exportWarnings;
            Settings settings;
            PluginManager plugins (settings);
            expect (io::exportDawProject (archive.getFile(), original, plugins, error, exportWarnings), error);

            Project imported;
            juce::StringArray importWarnings;
            expect (io::importDawProject (archive.getFile(), imported, plugins, error, importWarnings), error);

            expectEquals (imported.getNumTracks(), 2);
            expectEquals ((int) imported.buses.size(), 1);
            expectWithinAbsoluteError (imported.tempo.bpmAt (0.0), 95.0, 1.0e-6);
            const auto ts = imported.tempo.timeSignatureAt (0.0);
            expectEquals (ts.numerator, 5);
            expectEquals (ts.denominator, 4);
            expect (! imported.markers.empty());
            expectWithinAbsoluteError (imported.markers[0].beat, 16.0, 1.0e-6);

            Track* importedDrums = nullptr;
            Track* importedSynth = nullptr;
            for (int i = 0; i < imported.getNumTracks(); ++i)
            {
                auto& t = imported.getTrack (i);
                if (t.name == "Drums") importedDrums = &t;
                if (t.name == "Synth") importedSynth = &t;
            }
            expect (importedDrums != nullptr && importedSynth != nullptr);

            expectEquals (importedDrums->outputBus, imported.buses[0].id);
            expectWithinAbsoluteError (imported.buses[0].gainDb, -1.5f, 1.0e-3f);
            expect (! importedDrums->audioClips.empty());
            expect (importedDrums->audioClips[0].sourceFile.existsAsFile());
            expect (! importedDrums->builtinFx.empty());
            expectEquals (importedDrums->builtinFx[0].type, juce::String ("compressor"));
            expectWithinAbsoluteError ((double) importedDrums->builtinFx[0].params["threshold"], 0.3, 1.0e-6);

            bool foundFxAutomation = false, foundGainAutomation = false;
            for (const auto& lane : importedDrums->automation)
            {
                if (lane.parameterId == "fx:0:threshold") foundFxAutomation = true;
                if (lane.parameterId == "gain")            foundGainAutomation = true;
            }
            expect (foundFxAutomation && foundGainAutomation);

            expect (! importedSynth->midiClips.empty());
            expectEquals (importedSynth->midiClips[0].notes[0].pitch, 72);
            expectEquals ((int) importedSynth->sessionSlots.size(), 1);
        }

    }
};

static DawProjectUnitTests dawProjectUnitTests;

}
