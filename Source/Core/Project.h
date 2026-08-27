#pragma once
#include "Core/Types.h"
#include "Core/UtauTypes.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>
#include <map>
#include <memory>

namespace ss
{
    /*  Beat <-> second conversion driven by the tempo map (spec 8.4.7).

        THREADING: the audio thread walks these vectors directly.  There is no
        snapshot and no lock, so every edit must go through AudioEngine so the
        device callback is suspended first - editing a tempo map underneath a
        running callback is a crash, not a glitch.                              */
    class TempoMap
    {
    public:
        TempoMap();

        void   setEvents (std::vector<TempoEvent>);
        const std::vector<TempoEvent>& getEvents() const noexcept { return events; }

        double beatsToSeconds (double beats)   const noexcept;
        double secondsToBeats (double seconds) const noexcept;
        double bpmAt          (double beats)   const noexcept;

        void   setTimeSignatures (std::vector<TimeSignatureEvent>);
        const std::vector<TimeSignatureEvent>& getTimeSignatures() const noexcept { return timeSigs; }
        TimeSignatureEvent timeSignatureAt (double beats) const noexcept;

        /** Bar number (1-based) and beat-within-bar for a position. */
        void barAndBeat (double beats, int& barOut, double& beatInBarOut) const noexcept;

    private:
        std::vector<TempoEvent> events;
        std::vector<TimeSignatureEvent> timeSigs;
    };

    /** A region of an audio file on the timeline (spec 8.4.4, 9.2). */
    struct AudioClip
    {
        ClipId       id = invalidClipId;
        juce::String name;
        juce::File   sourceFile;

        double startBeats   = 0.0;   // position on the timeline
        double lengthBeats  = 4.0;
        double offsetSeconds = 0.0;  // read offset into sourceFile

        float  gainDb      = 0.0f;
        double fadeInSec   = 0.0;
        double fadeOutSec  = 0.0;
        bool   reversed    = false;
        double playbackRate = 1.0;   // time-stretch ratio (P1, spec 11)

        double endBeats() const noexcept { return startBeats + lengthBeats; }
    };

    /** A region of MIDI on the timeline (spec 9.2). */
    struct MidiClip
    {
        ClipId       id = invalidClipId;
        juce::String name;

        double startBeats  = 0.0;
        double lengthBeats = 4.0;
        std::vector<Note> notes;      // note times are RELATIVE to startBeats

        double endBeats() const noexcept { return startBeats + lengthBeats; }

        /** Lowest confidence found, or 1.0f when empty.  Spec 9.3. */
        float lowestConfidence() const noexcept;
        void  sortNotes();
    };

    /** One entry in a track's plugin chain (spec 8.4.2).  The live instance lives
        in the mixer; this is just the persistent description of it. */
    struct PluginSlot
    {
        juce::String identifier;   // juce::PluginDescription::createIdentifierString()
        juce::String displayName;
        bool         bypassed = false;
        /*  Which entry is the track's instrument.  Explicit rather than derived
            from PluginDescription::isInstrument, because "the first one that says
            it is an instrument" cannot express an effect placed before it, and a
            re-scan can change what the description claims.                       */
        bool         isInstrument = false;
        juce::MemoryBlock state;   // opaque plugin state
    };

    /** Built-in effect slot (spec 8.4.6).  `type` is one of the ids in
        Mixer/BuiltinFx.h; `params` is a flat name->value map. */
    struct BuiltinFxSlot
    {
        juce::String type;         // "eq" | "compressor" | "reverb" | "delay" | "limiter" | "gate"
        bool         bypassed = false;
        juce::NamedValueSet params;
    };

    /** A mix bus / group (spec 8.4.5).  Tracks route their output into one by id
        and can also feed it from a post-fader send; a bus always sums into the
        master.  Buses never feed other buses - that needs a routing graph, which
        8.4.5 does not ask for. */
    struct Bus
    {
        int          id = 0;              // 1-based; Track::outputBus == 0 means master
        juce::String name { "Bus" };
        float  gainDb = 0.0f;
        float  pan    = 0.0f;             // -1..1
        bool   muted  = false;
        std::vector<BuiltinFxSlot> builtinFx;
    };

    /** One row of the Session grid (spec: Session view, Phase 3). */
    struct Scene
    {
        SceneId      id = invalidSceneId;
        juce::String name;
    };

    class Track
    {
    public:
        Track (TrackId idToUse, TrackType typeToUse, juce::String nameToUse);

        TrackId      getId()   const noexcept { return id; }
        TrackType    getType() const noexcept { return type; }

        juce::String name;
        float  gainDb   = 0.0f;
        float  pan      = 0.0f;      // -1..1
        bool   muted    = false;
        bool   soloed   = false;
        bool   recordArmed = false;
        juce::Colour colour = juce::Colour (0xff4a90d9);

        int    inputChannel  = 0;     // hardware input index for recording
        int    outputBus     = 0;     // Bus::id, or 0 == straight to master
        /** Run the hardware input through this strip while armed (spec 8.4.3). */
        bool   inputMonitoring = false;
        juce::String midiInputDevice; // empty == all

        /** Post-fader send into a bus (spec 8.4.5).  `level` is a linear 0..1
            multiplier of the strip's own output, on top of the main path. */
        struct Send { int busId = 0; float level = 0.0f; };
        std::vector<Send> sends;

        std::vector<AudioClip>     audioClips;
        std::vector<MidiClip>      midiClips;
        std::vector<UtauClip>      utauClips;
        std::vector<PluginSlot>    plugins;      // instrument first, then FX
        std::vector<BuiltinFxSlot> builtinFx;

        /** Automation: parameter path -> breakpoints (beat, value 0..1).  Spec 8.4.5. */
        struct AutomationLane { juce::String parameterId; std::vector<std::pair<double, float>> points; };
        std::vector<AutomationLane> automation;

        std::map<SceneId, SessionClip> sessionSlots;     // sparse - most cells are empty

        AudioClip* findAudioClip (ClipId) noexcept;
        MidiClip*  findMidiClip  (ClipId) noexcept;
        UtauClip*  findUtauClip  (ClipId) noexcept;
        SessionClip* findSessionClip (SceneId) noexcept;
        void setSessionClip   (SceneId, SessionClip);
        void clearSessionClip (SceneId);
        double     endBeats() const noexcept;

    private:
        TrackId   id;
        TrackType type;
    };

    /** The document.  Broadcasts a change message whenever anything a view might
        be showing has been edited - UI components just listen. */
    class Project : public juce::ChangeBroadcaster
    {
    public:
        Project();
        ~Project() override;

        juce::String name { "Untitled" };
        double sampleRate = 48000.0;
        int    bitDepth   = 24;

        TempoMap tempo;
        std::vector<ChordEvent> chords;
        std::vector<Marker>     markers;

        /** Master-bus effect chain (spec 8.4.6).  It lives here rather than on the
            Mixer so that it is saved with the document and covered by undo. */
        std::vector<BuiltinFxSlot> masterChain;
        std::vector<Bus> buses;

        double loopStartBeats = 0.0;
        double loopEndBeats   = 16.0;
        bool   loopEnabled    = false;

        // --- tracks ---------------------------------------------------------
        Track& addTrack (TrackType, const juce::String& name);
        void   removeTrack (TrackId);
        void   moveTrack (int fromIndex, int toIndex);
        Track* findTrack (TrackId) noexcept;
        int    getNumTracks() const noexcept { return (int) tracks.size(); }
        Track& getTrack (int index)          { return *tracks[(size_t) index]; }
        const std::vector<std::unique_ptr<Track>>& getTracks() const noexcept { return tracks; }

        // --- buses (spec 8.4.5) ---------------------------------------------
        Bus& addBus (const juce::String& name);
        /** Also re-points anything that fed the bus, so no route dangles. */
        void removeBus (int busId);
        Bus* findBus (int busId) noexcept;

        // --- scenes (Session view, Phase 3) ----------------------------------
        std::vector<Scene> scenes;                       // row order == vector order

        Scene& addScene (const juce::String& name);
        /** Also erases every track's slot for this id - a scene reference left
            dangling on a track would show as a phantom cell. */
        void   removeScene (SceneId);
        Scene* findScene (SceneId) noexcept;

        bool   anyTrackSoloed() const noexcept;
        double endBeats() const noexcept;

        ClipId nextClipId() noexcept { return ++lastClipId; }
        SceneId nextSceneId() noexcept { return ++lastSceneId; }

        // --- persistence (.ssproj, spec 10.4) -------------------------------
        juce::var  toVar() const;
        bool       loadFromVar (const juce::var&);
        /** Adopts `file` as the document's location, rotates a .bak, clears the
            dirty flag and broadcasts.  Do NOT use it to write autosaves or
            bounces - it would repoint the user's next Ctrl+S at that copy. */
        juce::Result saveTo   (const juce::File&);
        juce::Result loadFrom (const juce::File&);

        juce::File getFile() const { return file; }
        bool  hasUnsavedChanges() const noexcept { return dirty; }
        void  markDirty();
        void  clearDirty() noexcept { dirty = false; }

        juce::UndoManager& getUndoManager() noexcept { return undoManager; }

        /** Where recordings, bounces and separated stems are written for this
            project.  <projectFolder>/Media, or a temp folder when unsaved. */
        juce::File getMediaFolder() const;

    private:
        std::vector<std::unique_ptr<Track>> tracks;
        TrackId lastTrackId = 0;
        ClipId  lastClipId  = 0;
        SceneId lastSceneId = 0;
        int     lastBusId   = 0;
        juce::File file;
        bool dirty = false;
        juce::UndoManager undoManager;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Project)
    };
}
