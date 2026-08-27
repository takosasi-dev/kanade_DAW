#include "Core/Project.h"

/*  .ssproj is JSON (spec 10.4).  Text, diffable, and readable when something goes
    wrong at 2am - which matters more for a project file than a few saved bytes.
    Notes are the one exception: they are stored as flat arrays because a busy
    project has tens of thousands of them and named fields would triple the file. */

namespace ss
{
    static constexpr int projectFormatVersion = 1;

    //==============================================================================
    namespace
    {
        juce::var toVar (const juce::NamedValueSet& set)
        {
            auto* obj = new juce::DynamicObject();

            for (const auto& p : set)
                obj->setProperty (p.name, p.value);

            return juce::var (obj);
        }

        juce::NamedValueSet namedValuesFrom (const juce::var& v)
        {
            juce::NamedValueSet set;

            if (auto* obj = v.getDynamicObject())
                set = obj->getProperties();

            return set;
        }

        juce::var notesToVar (const std::vector<Note>& notes)
        {
            juce::Array<juce::var> array;
            array.ensureStorageAllocated ((int) notes.size());

            for (const auto& n : notes)
                array.add (juce::var (juce::Array<juce::var> {
                    n.pitch, n.startBeats, n.lengthBeats, n.velocity, n.channel, (double) n.confidence }));

            return array;
        }

        std::vector<Note> notesFromVar (const juce::var& v)
        {
            std::vector<Note> notes;

            if (auto* array = v.getArray())
            {
                notes.reserve ((size_t) array->size());

                for (const auto& item : *array)
                {
                    if (auto* fields = item.getArray(); fields != nullptr && fields->size() >= 6)
                    {
                        Note n;
                        n.pitch       = (int)    (*fields)[0];
                        n.startBeats  = (double) (*fields)[1];
                        n.lengthBeats = (double) (*fields)[2];
                        n.velocity    = (int)    (*fields)[3];
                        n.channel     = (int)    (*fields)[4];
                        n.confidence  = (float) (double) (*fields)[5];
                        notes.push_back (n);
                    }
                }
            }

            return notes;
        }

        juce::var pitchBendToVar (const UtauPitchBend& pb)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("startMs", pb.startMs);
            obj->setProperty ("startSemitones", pb.startSemitones);

            juce::Array<juce::var> widths, heights, curves;
            for (auto v : pb.widthsMs) widths.add (v);
            for (auto v : pb.heightsSemitones) heights.add (v);
            for (auto& c : pb.curveTypes) curves.add (c);

            obj->setProperty ("widthsMs", widths);
            obj->setProperty ("heightsSemitones", heights);
            obj->setProperty ("curveTypes", curves);
            return juce::var (obj);
        }

        UtauPitchBend pitchBendFromVar (const juce::var& v)
        {
            UtauPitchBend pb;
            pb.startMs        = (double) v.getProperty ("startMs", 0.0);
            pb.startSemitones = (double) v.getProperty ("startSemitones", 0.0);

            if (auto* widths = v.getProperty ("widthsMs", {}).getArray())
                for (const auto& w : *widths) pb.widthsMs.push_back ((double) w);

            if (auto* heights = v.getProperty ("heightsSemitones", {}).getArray())
                for (const auto& h : *heights) pb.heightsSemitones.push_back ((double) h);

            if (auto* curves = v.getProperty ("curveTypes", {}).getArray())
                for (const auto& c : *curves) pb.curveTypes.push_back (c.toString());

            return pb;
        }

        juce::var utauClipsToVar (const std::vector<UtauClip>& clips, const juce::File& projectFile)
        {
            juce::Array<juce::var> array;

            for (const auto& c : clips)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("id", (juce::int64) c.id);
                obj->setProperty ("name", c.name);
                obj->setProperty ("start", c.startBeats);
                obj->setProperty ("length", c.lengthBeats);
                obj->setProperty ("voicebank", c.voicebankId);
                obj->setProperty ("renderedFile", c.renderedFile.getFullPathName().isNotEmpty() && projectFile.existsAsFile()
                                                      ? c.renderedFile.getRelativePathFrom (projectFile.getParentDirectory())
                                                      : c.renderedFile.getFullPathName());
                obj->setProperty ("notesHash", (juce::int64) c.notesHashAtRender);

                juce::Array<juce::var> notes;

                for (const auto& n : c.notes)
                {
                    auto* noteObj = new juce::DynamicObject();
                    noteObj->setProperty ("start", n.startBeats);
                    noteObj->setProperty ("length", n.lengthBeats);
                    noteObj->setProperty ("pitch", n.pitch);
                    noteObj->setProperty ("lyric", n.lyric);
                    noteObj->setProperty ("isRest", n.isRest);
                    noteObj->setProperty ("velocity", n.velocity);
                    noteObj->setProperty ("intensity", n.intensity);
                    noteObj->setProperty ("modulation", n.modulation);
                    noteObj->setProperty ("preUtterance", n.preUtteranceMs);
                    noteObj->setProperty ("overlap", n.voiceOverlapMs);
                    noteObj->setProperty ("flags", n.flags);
                    noteObj->setProperty ("envelope", n.envelope);
                    noteObj->setProperty ("pitchBend", pitchBendToVar (n.pitchBend));
                    noteObj->setProperty ("extra", toVar (n.extra));
                    notes.add (juce::var (noteObj));
                }

                obj->setProperty ("notes", notes);
                array.add (juce::var (obj));
            }

            return array;
        }

        std::vector<UtauClip> utauClipsFromVar (const juce::var& v, const juce::File& projectFile)
        {
            std::vector<UtauClip> clips;

            if (auto* array = v.getArray())
            {
                for (const auto& item : *array)
                {
                    UtauClip clip;
                    clip.id          = (ClipId) (juce::int64) item.getProperty ("id", 0);
                    clip.name        = item.getProperty ("name", {}).toString();
                    clip.startBeats  = (double) item.getProperty ("start", 0.0);
                    clip.lengthBeats = (double) item.getProperty ("length", 4.0);
                    clip.voicebankId = item.getProperty ("voicebank", {}).toString();
                    clip.notesHashAtRender = (juce::int64) item.getProperty ("notesHash", 0);

                    const auto renderedPath = item.getProperty ("renderedFile", {}).toString();
                    if (renderedPath.isNotEmpty())
                        clip.renderedFile = juce::File::isAbsolutePath (renderedPath)
                                                ? juce::File (renderedPath)
                                                : projectFile.getParentDirectory().getChildFile (renderedPath);

                    if (auto* notes = item.getProperty ("notes", {}).getArray())
                    {
                        for (const auto& n : *notes)
                        {
                            UtauNote note;
                            note.startBeats     = (double) n.getProperty ("start", 0.0);
                            note.lengthBeats    = (double) n.getProperty ("length", 1.0);
                            note.pitch          = (int) n.getProperty ("pitch", 60);
                            note.lyric           = n.getProperty ("lyric", {}).toString();
                            note.isRest          = (bool) n.getProperty ("isRest", false);
                            note.velocity        = (int) n.getProperty ("velocity", 100);
                            note.intensity       = (int) n.getProperty ("intensity", 100);
                            note.modulation      = (int) n.getProperty ("modulation", 0);
                            note.preUtteranceMs  = (double) n.getProperty ("preUtterance", -1.0);
                            note.voiceOverlapMs  = (double) n.getProperty ("overlap", -1.0);
                            note.flags           = n.getProperty ("flags", {}).toString();
                            note.envelope        = n.getProperty ("envelope", {}).toString();
                            note.pitchBend       = pitchBendFromVar (n.getProperty ("pitchBend", {}));
                            note.extra           = namedValuesFrom (n.getProperty ("extra", {}));
                            clip.notes.push_back (std::move (note));
                        }
                    }

                    clips.push_back (std::move (clip));
                }
            }

            return clips;
        }

        juce::var sessionSlotsToVar (const std::map<SceneId, SessionClip>& slots, const juce::File& projectFile)
        {
            juce::Array<juce::var> array;

            for (const auto& [sceneId, clip] : slots)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("sceneId", (juce::int64) sceneId);
                obj->setProperty ("kind", clip.kind == SessionClip::Kind::audio ? "audio" : "midi");
                obj->setProperty ("name", clip.name);
                obj->setProperty ("length", clip.lengthBeats);
                obj->setProperty ("notes", notesToVar (clip.notes));
                obj->setProperty ("file", clip.sourceFile.getFullPathName().isNotEmpty() && projectFile.existsAsFile()
                                              ? clip.sourceFile.getRelativePathFrom (projectFile.getParentDirectory())
                                              : clip.sourceFile.getFullPathName());
                obj->setProperty ("offset", clip.offsetSeconds);
                obj->setProperty ("gainDb", clip.gainDb);
                obj->setProperty ("fadeIn", clip.fadeInSec);
                obj->setProperty ("fadeOut", clip.fadeOutSec);
                obj->setProperty ("reversed", clip.reversed);
                obj->setProperty ("rate", clip.playbackRate);
                array.add (juce::var (obj));
            }

            return array;
        }

        std::map<SceneId, SessionClip> sessionSlotsFromVar (const juce::var& v, const juce::File& projectFile)
        {
            std::map<SceneId, SessionClip> slots;

            if (auto* array = v.getArray())
            {
                for (const auto& item : *array)
                {
                    const auto sceneId = (SceneId) (juce::int64) item.getProperty ("sceneId", 0);

                    SessionClip clip;
                    clip.kind        = item.getProperty ("kind", "midi").toString() == "audio"
                                           ? SessionClip::Kind::audio : SessionClip::Kind::midi;
                    clip.name        = item.getProperty ("name", {}).toString();
                    clip.lengthBeats = (double) item.getProperty ("length", 4.0);
                    clip.notes       = notesFromVar (item.getProperty ("notes", {}));

                    const auto path = item.getProperty ("file", {}).toString();
                    if (path.isNotEmpty())
                        clip.sourceFile = juce::File::isAbsolutePath (path)
                                              ? juce::File (path)
                                              : projectFile.getParentDirectory().getChildFile (path);

                    clip.offsetSeconds = (double) item.getProperty ("offset", 0.0);
                    clip.gainDb        = (float) (double) item.getProperty ("gainDb", 0.0);
                    clip.fadeInSec     = (double) item.getProperty ("fadeIn", 0.0);
                    clip.fadeOutSec    = (double) item.getProperty ("fadeOut", 0.0);
                    clip.reversed      = (bool) item.getProperty ("reversed", false);
                    clip.playbackRate  = (double) item.getProperty ("rate", 1.0);

                    slots[sceneId] = std::move (clip);
                }
            }

            return slots;
        }

        juce::var pluginsToVar (const std::vector<PluginSlot>& slots)
        {
            juce::Array<juce::var> array;

            for (const auto& s : slots)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("identifier", s.identifier);
                obj->setProperty ("name",       s.displayName);
                obj->setProperty ("bypassed",   s.bypassed);
                obj->setProperty ("instrument", s.isInstrument);
                obj->setProperty ("state",      s.state.toBase64Encoding());
                array.add (juce::var (obj));
            }

            return array;
        }

        std::vector<PluginSlot> pluginsFromVar (const juce::var& v)
        {
            std::vector<PluginSlot> slots;

            if (auto* array = v.getArray())
            {
                for (const auto& item : *array)
                {
                    PluginSlot s;
                    s.identifier  = item.getProperty ("identifier", {}).toString();
                    s.displayName = item.getProperty ("name", {}).toString();
                    s.bypassed    = (bool) item.getProperty ("bypassed", false);
                    s.isInstrument = (bool) item.getProperty ("instrument", false);
                    s.state.fromBase64Encoding (item.getProperty ("state", {}).toString());
                    slots.push_back (std::move (s));
                }
            }

            return slots;
        }

        juce::var builtinFxToVar (const std::vector<BuiltinFxSlot>& slots)
        {
            juce::Array<juce::var> array;

            for (const auto& s : slots)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("type",     s.type);
                obj->setProperty ("bypassed", s.bypassed);
                obj->setProperty ("params",   toVar (s.params));
                array.add (juce::var (obj));
            }

            return array;
        }

        std::vector<BuiltinFxSlot> builtinFxFromVar (const juce::var& v)
        {
            std::vector<BuiltinFxSlot> slots;

            if (auto* array = v.getArray())
            {
                for (const auto& item : *array)
                {
                    BuiltinFxSlot s;
                    s.type     = item.getProperty ("type", {}).toString();
                    s.bypassed = (bool) item.getProperty ("bypassed", false);
                    s.params   = namedValuesFrom (item.getProperty ("params", {}));
                    slots.push_back (std::move (s));
                }
            }

            return slots;
        }

        juce::var sendsToVar (const std::vector<Track::Send>& sends)
        {
            juce::Array<juce::var> array;

            for (const auto& s : sends)
                array.add (juce::var (juce::Array<juce::var> { s.busId, (double) s.level }));

            return array;
        }

        std::vector<Track::Send> sendsFromVar (const juce::var& v)
        {
            std::vector<Track::Send> sends;

            if (auto* array = v.getArray())
                for (const auto& item : *array)
                    if (auto* fields = item.getArray(); fields != nullptr && fields->size() >= 2)
                        sends.push_back ({ (int) (*fields)[0],
                                           juce::jlimit (0.0f, 1.0f, (float) (double) (*fields)[1]) });

            return sends;
        }

        juce::var busesToVar (const std::vector<Bus>& buses)
        {
            juce::Array<juce::var> array;

            for (const auto& b : buses)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("id",        b.id);
                obj->setProperty ("name",      b.name);
                obj->setProperty ("gainDb",    b.gainDb);
                obj->setProperty ("pan",       b.pan);
                obj->setProperty ("muted",     b.muted);
                obj->setProperty ("builtinFx", builtinFxToVar (b.builtinFx));
                array.add (juce::var (obj));
            }

            return array;
        }
    }

    //==============================================================================
    juce::var Project::toVar() const
    {
        auto* root = new juce::DynamicObject();
        root->setProperty ("format",     projectFormatVersion);
        root->setProperty ("app",        "KANADE DAW");
        root->setProperty ("name",       name);
        root->setProperty ("sampleRate", sampleRate);
        root->setProperty ("bitDepth",   bitDepth);
        root->setProperty ("loopStart",  loopStartBeats);
        root->setProperty ("loopEnd",    loopEndBeats);
        root->setProperty ("loopEnabled", loopEnabled);

        /*  The id counters are part of the document, not derived state: deleting
            the highest-numbered clip would otherwise lower the watermark and let
            the next clip reuse a live id.  Snapshot-based undo restores through
            here, so a collision would survive into the UI's clip lookups.      */
        root->setProperty ("lastTrackId", (juce::int64) lastTrackId);
        root->setProperty ("lastClipId",  (juce::int64) lastClipId);
        root->setProperty ("lastBusId",   lastBusId);

        root->setProperty ("masterFx", builtinFxToVar (masterChain));
        root->setProperty ("buses",    busesToVar (buses));

        {
            juce::Array<juce::var> array;

            for (const auto& t : tempo.getEvents())
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("beat", t.beat);
                obj->setProperty ("bpm",  t.bpm);
                array.add (juce::var (obj));
            }

            root->setProperty ("tempo", array);
        }

        {
            juce::Array<juce::var> array;

            for (const auto& t : tempo.getTimeSignatures())
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("beat", t.beat);
                obj->setProperty ("num",  t.numerator);
                obj->setProperty ("den",  t.denominator);
                array.add (juce::var (obj));
            }

            root->setProperty ("timeSignatures", array);
        }

        {
            juce::Array<juce::var> array;

            for (const auto& c : chords)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("beat",   c.beat);
                obj->setProperty ("length", c.lengthBeats);
                obj->setProperty ("symbol", c.symbol);
                obj->setProperty ("root",   c.root);

                juce::Array<juce::var> intervals;
                for (auto i : c.intervals)
                    intervals.add (i);

                obj->setProperty ("intervals", intervals);
                array.add (juce::var (obj));
            }

            root->setProperty ("chords", array);
        }

        {
            juce::Array<juce::var> array;

            for (const auto& m : markers)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("beat", m.beat);
                obj->setProperty ("name", m.name);
                array.add (juce::var (obj));
            }

            root->setProperty ("markers", array);
        }

        {
            juce::Array<juce::var> array;

            for (const auto& s : scenes)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("id",   (juce::int64) s.id);
                obj->setProperty ("name", s.name);
                array.add (juce::var (obj));
            }

            root->setProperty ("scenes", array);
        }

        root->setProperty ("lastSceneId", (juce::int64) lastSceneId);

        {
            juce::Array<juce::var> trackArray;

            for (const auto& t : tracks)
            {
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("id",       (juce::int64) t->getId());
                obj->setProperty ("type",     ss::toString (t->getType()));
                obj->setProperty ("name",     t->name);
                obj->setProperty ("gainDb",   t->gainDb);
                obj->setProperty ("pan",      t->pan);
                obj->setProperty ("muted",    t->muted);
                obj->setProperty ("soloed",   t->soloed);
                obj->setProperty ("armed",    t->recordArmed);
                obj->setProperty ("colour",   t->colour.toString());
                obj->setProperty ("input",    t->inputChannel);
                obj->setProperty ("outputBus", t->outputBus);
                obj->setProperty ("monitor",  t->inputMonitoring);
                obj->setProperty ("sends",    sendsToVar (t->sends));
                obj->setProperty ("midiIn",   t->midiInputDevice);
                obj->setProperty ("plugins",  pluginsToVar (t->plugins));
                obj->setProperty ("builtinFx", builtinFxToVar (t->builtinFx));

                juce::Array<juce::var> audio;

                for (const auto& c : t->audioClips)
                {
                    auto* clip = new juce::DynamicObject();
                    clip->setProperty ("id",     (juce::int64) c.id);
                    clip->setProperty ("name",   c.name);
                    // Relative when the media sits under the project folder, so a
                    // project folder stays movable between machines.
                    clip->setProperty ("file",   file.existsAsFile()
                                                     ? c.sourceFile.getRelativePathFrom (file.getParentDirectory())
                                                     : c.sourceFile.getFullPathName());
                    clip->setProperty ("start",  c.startBeats);
                    clip->setProperty ("length", c.lengthBeats);
                    clip->setProperty ("offset", c.offsetSeconds);
                    clip->setProperty ("gainDb", c.gainDb);
                    clip->setProperty ("fadeIn", c.fadeInSec);
                    clip->setProperty ("fadeOut", c.fadeOutSec);
                    clip->setProperty ("reversed", c.reversed);
                    clip->setProperty ("rate",   c.playbackRate);
                    audio.add (juce::var (clip));
                }

                obj->setProperty ("audioClips", audio);

                juce::Array<juce::var> midi;

                for (const auto& c : t->midiClips)
                {
                    auto* clip = new juce::DynamicObject();
                    clip->setProperty ("id",     (juce::int64) c.id);
                    clip->setProperty ("name",   c.name);
                    clip->setProperty ("start",  c.startBeats);
                    clip->setProperty ("length", c.lengthBeats);
                    clip->setProperty ("notes",  notesToVar (c.notes));
                    midi.add (juce::var (clip));
                }

                obj->setProperty ("midiClips", midi);

                obj->setProperty ("utauClips", utauClipsToVar (t->utauClips, file));

                obj->setProperty ("sessionSlots", sessionSlotsToVar (t->sessionSlots, file));

                juce::Array<juce::var> lanes;

                for (const auto& lane : t->automation)
                {
                    auto* laneObj = new juce::DynamicObject();
                    laneObj->setProperty ("parameter", lane.parameterId);

                    juce::Array<juce::var> points;

                    for (const auto& p : lane.points)
                        points.add (juce::var (juce::Array<juce::var> { p.first, (double) p.second }));

                    laneObj->setProperty ("points", points);
                    lanes.add (juce::var (laneObj));
                }

                obj->setProperty ("automation", lanes);
                trackArray.add (juce::var (obj));
            }

            root->setProperty ("tracks", trackArray);
        }

        return juce::var (root);
    }

    //==============================================================================
    bool Project::loadFromVar (const juce::var& root)
    {
        if (! root.isObject())
            return false;

        if ((int) root.getProperty ("format", 0) > projectFormatVersion)
            return false;      // written by a newer build - refuse rather than mangle

        name       = root.getProperty ("name", "Untitled").toString();
        sampleRate = (double) root.getProperty ("sampleRate", 48000.0);
        bitDepth   = (int)    root.getProperty ("bitDepth", 24);
        loopStartBeats = (double) root.getProperty ("loopStart", 0.0);
        loopEndBeats   = (double) root.getProperty ("loopEnd", 16.0);
        loopEnabled    = (bool)   root.getProperty ("loopEnabled", false);

        {
            std::vector<TempoEvent> events;

            if (auto* array = root.getProperty ("tempo", {}).getArray())
                for (const auto& item : *array)
                    events.push_back ({ (double) item.getProperty ("beat", 0.0),
                                        (double) item.getProperty ("bpm", 120.0) });

            tempo.setEvents (std::move (events));
        }

        {
            std::vector<TimeSignatureEvent> sigs;

            if (auto* array = root.getProperty ("timeSignatures", {}).getArray())
                for (const auto& item : *array)
                    sigs.push_back ({ (double) item.getProperty ("beat", 0.0),
                                      (int)    item.getProperty ("num", 4),
                                      (int)    item.getProperty ("den", 4) });

            tempo.setTimeSignatures (std::move (sigs));
        }

        chords.clear();

        if (auto* array = root.getProperty ("chords", {}).getArray())
        {
            for (const auto& item : *array)
            {
                ChordEvent c;
                c.beat        = (double) item.getProperty ("beat", 0.0);
                c.lengthBeats = (double) item.getProperty ("length", 4.0);
                c.symbol      = item.getProperty ("symbol", {}).toString();
                c.root        = (int) item.getProperty ("root", 0);
                c.intervals.clear();

                if (auto* intervals = item.getProperty ("intervals", {}).getArray())
                    for (const auto& i : *intervals)
                        c.intervals.push_back ((int) i);

                if (c.intervals.empty())
                    c.intervals = { 0, 4, 7 };

                chords.push_back (std::move (c));
            }
        }

        markers.clear();

        if (auto* array = root.getProperty ("markers", {}).getArray())
            for (const auto& item : *array)
                markers.push_back ({ (double) item.getProperty ("beat", 0.0),
                                     item.getProperty ("name", {}).toString() });

        tracks.clear();

        // Older files predate the stored watermarks; the max-id scan below then
        // recovers a safe (if lower) value.
        lastTrackId = (TrackId) (juce::int64) root.getProperty ("lastTrackId", 0);
        lastClipId  = (ClipId)  (juce::int64) root.getProperty ("lastClipId", 0);
        lastBusId   = (int) root.getProperty ("lastBusId", 0);
        lastSceneId = (SceneId) (juce::int64) root.getProperty ("lastSceneId", 0);

        scenes.clear();

        if (auto* sceneArray = root.getProperty ("scenes", {}).getArray())
        {
            for (const auto& item : *sceneArray)
            {
                Scene scene;
                scene.id   = (SceneId) (juce::int64) item.getProperty ("id", 0);
                scene.name = item.getProperty ("name", {}).toString();

                lastSceneId = juce::jmax (lastSceneId, scene.id);
                scenes.push_back (std::move (scene));
            }
        }

        masterChain = builtinFxFromVar (root.getProperty ("masterFx", {}));
        buses.clear();

        if (auto* array = root.getProperty ("buses", {}).getArray())
        {
            for (const auto& item : *array)
            {
                Bus bus;
                bus.id        = (int) item.getProperty ("id", 0);
                bus.name      = item.getProperty ("name", "Bus").toString();
                bus.gainDb    = (float) (double) item.getProperty ("gainDb", 0.0);
                bus.pan       = (float) (double) item.getProperty ("pan", 0.0);
                bus.muted     = (bool) item.getProperty ("muted", false);
                bus.builtinFx = builtinFxFromVar (item.getProperty ("builtinFx", {}));

                lastBusId = juce::jmax (lastBusId, bus.id);
                buses.push_back (std::move (bus));
            }
        }

        if (auto* trackArray = root.getProperty ("tracks", {}).getArray())
        {
            for (const auto& item : *trackArray)
            {
                const auto id   = (TrackId) (juce::int64) item.getProperty ("id", 0);
                const auto type = trackTypeFromString (item.getProperty ("type", "midi").toString());

                auto track = std::make_unique<Track> (id, type, item.getProperty ("name", "Track").toString());
                lastTrackId = juce::jmax (lastTrackId, id);

                track->gainDb      = (float) (double) item.getProperty ("gainDb", 0.0);
                track->pan         = (float) (double) item.getProperty ("pan", 0.0);
                track->muted       = (bool) item.getProperty ("muted", false);
                track->soloed      = (bool) item.getProperty ("soloed", false);
                track->recordArmed = (bool) item.getProperty ("armed", false);
                track->colour      = juce::Colour::fromString (item.getProperty ("colour", "ff4a90d9").toString());
                track->inputChannel = (int) item.getProperty ("input", 0);
                track->outputBus    = (int) item.getProperty ("outputBus", 0);
                track->inputMonitoring = (bool) item.getProperty ("monitor", false);
                track->sends        = sendsFromVar (item.getProperty ("sends", {}));
                track->midiInputDevice = item.getProperty ("midiIn", {}).toString();
                track->plugins   = pluginsFromVar (item.getProperty ("plugins", {}));
                track->builtinFx = builtinFxFromVar (item.getProperty ("builtinFx", {}));

                if (auto* clips = item.getProperty ("audioClips", {}).getArray())
                {
                    for (const auto& c : *clips)
                    {
                        AudioClip clip;
                        clip.id   = (ClipId) (juce::int64) c.getProperty ("id", 0);
                        clip.name = c.getProperty ("name", {}).toString();

                        const auto path = c.getProperty ("file", {}).toString();
                        clip.sourceFile = juce::File::isAbsolutePath (path)
                                              ? juce::File (path)
                                              : file.getParentDirectory().getChildFile (path);

                        clip.startBeats    = (double) c.getProperty ("start", 0.0);
                        clip.lengthBeats   = (double) c.getProperty ("length", 4.0);
                        clip.offsetSeconds = (double) c.getProperty ("offset", 0.0);
                        clip.gainDb        = (float) (double) c.getProperty ("gainDb", 0.0);
                        clip.fadeInSec     = (double) c.getProperty ("fadeIn", 0.0);
                        clip.fadeOutSec    = (double) c.getProperty ("fadeOut", 0.0);
                        clip.reversed      = (bool) c.getProperty ("reversed", false);
                        clip.playbackRate  = (double) c.getProperty ("rate", 1.0);

                        lastClipId = juce::jmax (lastClipId, clip.id);
                        track->audioClips.push_back (std::move (clip));
                    }
                }

                if (auto* clips = item.getProperty ("midiClips", {}).getArray())
                {
                    for (const auto& c : *clips)
                    {
                        MidiClip clip;
                        clip.id          = (ClipId) (juce::int64) c.getProperty ("id", 0);
                        clip.name        = c.getProperty ("name", {}).toString();
                        clip.startBeats  = (double) c.getProperty ("start", 0.0);
                        clip.lengthBeats = (double) c.getProperty ("length", 4.0);
                        clip.notes       = notesFromVar (c.getProperty ("notes", {}));

                        lastClipId = juce::jmax (lastClipId, clip.id);
                        track->midiClips.push_back (std::move (clip));
                    }
                }

                track->utauClips = utauClipsFromVar (item.getProperty ("utauClips", {}), file);

                for (const auto& c : track->utauClips)
                    lastClipId = juce::jmax (lastClipId, c.id);

                track->sessionSlots = sessionSlotsFromVar (item.getProperty ("sessionSlots", {}), file);

                if (auto* lanes = item.getProperty ("automation", {}).getArray())
                {
                    for (const auto& l : *lanes)
                    {
                        Track::AutomationLane lane;
                        lane.parameterId = l.getProperty ("parameter", {}).toString();

                        if (auto* points = l.getProperty ("points", {}).getArray())
                            for (const auto& p : *points)
                                if (auto* pair = p.getArray(); pair != nullptr && pair->size() >= 2)
                                    lane.points.emplace_back ((double) (*pair)[0], (float) (double) (*pair)[1]);

                        track->automation.push_back (std::move (lane));
                    }
                }

                tracks.push_back (std::move (track));
            }
        }

        undoManager.clearUndoHistory();
        dirty = false;
        sendChangeMessage();
        return true;
    }

    //==============================================================================
    juce::Result Project::saveTo (const juce::File& destination)
    {
        file = destination;      // set first so clip paths are written relative to it

        const auto json = juce::JSON::toString (toVar(), false);

        // Write to a sibling temp file and swap, so a crash mid-write cannot
        // destroy the previous save.
        auto temp = destination.getSiblingFile (destination.getFileName() + ".tmp");
        temp.deleteFile();

        if (! temp.replaceWithText (json))
            return juce::Result::fail ("Could not write to " + temp.getFullPathName());

        if (destination.existsAsFile())
        {
            auto backup = destination.getSiblingFile (destination.getFileNameWithoutExtension() + ".ssproj.bak");
            backup.deleteFile();
            destination.copyFileTo (backup);
        }

        if (! temp.moveFileTo (destination))
        {
            temp.deleteFile();
            return juce::Result::fail ("Could not replace " + destination.getFullPathName());
        }

        destination.getParentDirectory().getChildFile ("Media").createDirectory();
        dirty = false;
        sendChangeMessage();
        return juce::Result::ok();
    }

    juce::Result Project::loadFrom (const juce::File& source)
    {
        if (! source.existsAsFile())
            return juce::Result::fail (source.getFullPathName() + " does not exist");

        juce::var parsed;
        const auto result = juce::JSON::parse (source.loadFileAsString(), parsed);

        if (result.failed())
            return juce::Result::fail ("Not a valid KANADE DAW project: " + result.getErrorMessage());

        file = source;

        if (! loadFromVar (parsed))
            return juce::Result::fail ("This project was saved by a newer version of KANADE DAW");

        return juce::Result::ok();
    }
}
