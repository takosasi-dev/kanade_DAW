#pragma once
#include "Core/Project.h"
#include "Mixer/BuiltinFx.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace ss
{
    class PluginManager;

    /** Equal-power (-3 dB centre) pan law used by every channel strip.  Exposed
        so the mixer UI can draw the same curve the audio thread applies. */
    void equalPowerPanGains (float pan, float& leftGain, float& rightGain) noexcept;

    /** Value of an automation lane at `beat` (spec 8.4.5): linear between
        breakpoints, holding the nearest value before the first and after the
        last one.  `points` must be sorted by beat; an empty lane returns
        `fallback` so a lane with no breakpoints changes nothing. */
    float automationValueAt (const std::vector<std::pair<double, float>>& points,
                             double beat, float fallback) noexcept;

    /** Per-track signal path: instrument -> built-in FX -> plugin FX -> pan/gain
        -> bus.  Owned by Mixer, touched only from the audio thread once built. */
    class ChannelStrip
    {
    public:
        ChannelStrip (TrackId, PluginManager&);
        ~ChannelStrip();

        TrackId getTrackId() const noexcept { return trackId; }

        void prepare (double sampleRate, int blockSize, int numChannels);
        void releaseResources();

        /** [audio thread] Renders `midi` through the instrument (if any) into
            `buffer`, then runs the FX chain in place. */
        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

        /** Rebuilds the chain from the track's persisted state.  Message thread
            only; the audio thread is suspended by the caller. */
        void rebuildFrom (const Track&);
        /** A bus is the same strip minus the instrument and the plugin chain. */
        void rebuildFrom (const Bus&);

        void setGainDb (float) noexcept;
        void setPan (float) noexcept;
        void setMuted (bool) noexcept;
        /** Flips a hosted plugin's bypass live - `slotIndex` indexes the track's
            own `plugins` vector, and the instance is not reloaded. */
        void setPluginBypassed (int slotIndex, bool) noexcept;
        float getPeak (int channel) const noexcept;
        float getRms  (int channel) const noexcept;

        BuiltinEffect* getBuiltinEffect (int index) noexcept;
        juce::AudioPluginInstance* getPluginInstance (int index) noexcept;
        /** The instance for a slot in the track's own `plugins` vector, whether
            that slot ended up as the instrument or as an insert. */
        juce::AudioPluginInstance* getPluginForSlot (int slotIndex) noexcept;
        juce::AudioPluginInstance* getInstrument() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
        TrackId trackId;
    };

    /** Sums every channel strip into buses and the master (spec 8.4.5). */
    class Mixer
    {
    public:
        explicit Mixer (PluginManager&);
        ~Mixer();

        void setProject (Project*);
        void prepare (double sampleRate, int blockSize, int numOutputChannels);
        void releaseResources();

        /** [audio thread] Fills `output` for this block.
            `positionSamples` is the timeline position of sample 0.
            `liveMidi` is incoming MIDI for record-armed / monitored tracks.
            `deviceInput` is this block of hardware input, for input monitoring
            (spec 8.4.3); pass a channel-less buffer when there is none.
            `isPlaying` is the transport state.  It has to be told, not inferred
            from the playhead moving: resuming from exactly where playback
            stopped would otherwise lose the first block.
            `sessionPositionSamples` is this block's start on SessionClock's OWN
            free-running timeline (SessionClock::currentSample() BEFORE this
            block's advance()) - deliberately NOT `positionSamples`, which is
            Transport's position and freezes/jumps/loops independently of it.
            Every Session view launch/stop boundary is compared against this
            value, never against `positionSamples`.

            Session view is a live-performance layer; it has no meaning for an
            offline render, which renders the arrangement only.  Pass a negative
            value (the default, -1) to mean exactly that: "no real SessionClock
            position for this call".  Session hand-off/adoption is not touched
            and no session clip is rendered for a call with a negative value -
            a session clip stays silent and its internal position (sessionPos-
            Samples/sessionMidiCursor/activeSessionSlot) is left completely
            alone, so an offline export can never bake a live session clip into
            the file, nor scramble that clip's live playback position by having
            advanced it during the render.  Only a genuine live sample count
            (>= 0, e.g. SessionClock::currentSample() from the real-time audio
            callback) should ever be passed here.                               */
        void process (juce::AudioBuffer<float>& output, juce::int64 positionSamples,
                      juce::MidiBuffer& liveMidi, const juce::AudioBuffer<float>& deviceInput,
                      bool isPlaying, juce::int64 sessionPositionSamples = -1);

        /** Rebuild all strips (after load, track add/remove, plugin change). */
        void rebuild();

        /** Re-reads each existing track's clips (add/remove/move/replace, or a
            UtauClip's renderedFile changing) without touching strips, plugins,
            routing, or automation - the cheap counterpart to rebuild() for the
            case topologyFingerprint() deliberately ignores. Does nothing for a
            track that doesn't have a TrackState yet (a genuinely new track is
            a topology change and goes through rebuild() instead). */
        void syncClips();

        /** Live state pushes that must not go through rebuild(), which reloads
            plugins and re-opens clip readers. */
        void setSolo (TrackId, bool) noexcept;
        void setInputMonitoring (TrackId, bool recordArmed, bool monitoring) noexcept;
        void setPluginBypassed (TrackId, int slotIndex, bool) noexcept;

        ChannelStrip* getStripForTrack (TrackId) noexcept;
        /** Bus strips, in Project::buses order. */
        ChannelStrip* getBusStrip (int index) noexcept;
        BuiltinEffect* getMasterEffect (int index) noexcept;

        /** Sample peak of the master - what a per-sample meter shows. */
        float getMasterPeak (int channel) const noexcept;
        /** True peak in dBTP (4x oversampled, spec 8.4.6).  Always at or above
            the sample peak, and the number a delivery spec actually asks for. */
        float getMasterTruePeakDb() const noexcept;
        float getMasterRms  (int channel) const noexcept;

        /** Integrated / short-term loudness of the master bus (spec 8.4.6). */
        float getMasterLufs() const noexcept;
        void  resetLoudness();

        void setMasterGainDb (float) noexcept;
        float getMasterGainDb() const noexcept;

        /** Auditions one clip without disturbing the timeline (spec 9.4). */
        void setPreviewClip (TrackId, const MidiClip*);

        /** Launches a Session view clip on `track`, looping until stopped or
            replaced.  `launchAtSample` is an absolute sample on the SAME
            timeline SessionClock::nextBarBoundarySample() returns - Mixer
            does not know about bars or tempo, only "at this sample, swap." */
        void launchSessionClip (TrackId track, const SessionClip& clip, juce::int64 launchAtSample);
        /** Schedules the track's session clip to stop at `stopAtSample`. */
        void stopSessionClip (TrackId track, juce::int64 stopAtSample);
        /** True while a session clip is audibly looping on `track` right now. */
        bool isSessionClipActive (TrackId track) const noexcept;
        /** True while a launch/stop is scheduled but its boundary sample has
            not been reached yet. */
        bool isSessionClipQueued (TrackId track) const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE (Mixer)
    };
}
