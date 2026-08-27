#pragma once
#include "Core/Project.h"
#include "Core/Settings.h"
#include "Engine/Transport.h"
#include "Engine/Recorder.h"
#include "Engine/SessionClock.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace ss
{
    class Mixer;
    class PluginManager;

    /** The realtime heart of the app (spec 7-2, 7-3).
        Owns the device manager, pulls audio/MIDI for every track, feeds the
        mixer, and drives the recorder.                                        */
    class AudioEngine : private juce::AudioIODeviceCallback,
                        private juce::MidiInputCallback,
                        private juce::ChangeListener
    {
    public:
        AudioEngine (Settings&, PluginManager&);
        ~AudioEngine() override;

        void setProject (Project*);
        Project* getProject() const noexcept { return project; }

        juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
        Transport& getTransport() noexcept { return transport; }
        Mixer&     getMixer()     noexcept { return *mixer; }
        Recorder&  getRecorder()  noexcept { return recorder; }
        juce::AudioFormatManager& getFormatManager() noexcept { return formatManager; }

        /** Opens the device stored in Settings, falling back to the default. */
        juce::String initialiseAudioDevice();
        void saveAudioDeviceState();

        /** Round-trip latency of the current device, in ms. */
        double getLatencyMs() const;

        // --- transport shortcuts that also handle recording ------------------
        void play();
        void stop();
        void startRecording();
        void stopRecording();

        /** Renders the whole project offline to a file (spec 8.1 output, 10.4).
            `stemPerTrack` writes one file per track instead of a stereo mix.   */
        juce::Result renderToFile (const juce::File& destination, double startBeat, double endBeat,
                                   int bitDepth, double sampleRate, bool stemPerTrack,
                                   std::function<void (float)> progress = {});

        /** Auditions a single MIDI clip through its track's instrument, for the
            candidate gallery's instant preview (spec 9.4). */
        void previewMidiClip (TrackId, const MidiClip&);
        void stopPreview();

        // --- Session view (spec: Session view, Phase 3) -----------------------
        void launchSessionClip (TrackId, const SessionClip&);
        void stopSessionClip (TrackId);
        bool isSessionClipActive (TrackId) const noexcept;
        bool isSessionClipQueued (TrackId) const noexcept;
        SessionClock& getSessionClock() noexcept { return sessionClock; }

        /** Peak level of the master bus, for the transport meter. */
        float getMasterPeak (int channel) const noexcept;

        /** Re-reads clip/plugin state after the project was edited. */
        void projectChanged();

        std::function<void (const juce::String&)> onError;

    private:
        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                               float* const* outputChannelData, int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override;
        void audioDeviceAboutToStart (juce::AudioIODevice*) override;
        void audioDeviceStopped() override;
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override;

        /** Calls onError on the message thread, whichever thread we are on. */
        void reportError (const juce::String&);

        /** Blocks until the audio callback is out of the way, then keeps it silent
            and idle.  Message / background threads only - never the audio thread. */
        void suspendAudio();
        void resumeAudio();

        struct ScopedSuspend
        {
            explicit ScopedSuspend (AudioEngine& e) : engine (e) { engine.suspendAudio(); }
            ~ScopedSuspend() { engine.resumeAudio(); }
            AudioEngine& engine;
        };

        /** Pushes gain/pan/mute/solo onto the live strips without a graph rebuild. */
        void pushMixerState();
        void renderMetronome (juce::AudioBuffer<float>&, juce::int64 blockStart, int numSamples) noexcept;

        Settings& settings;
        PluginManager& pluginManager;
        Project* project = nullptr;
        juce::AudioDeviceManager deviceManager;
        juce::AudioFormatManager formatManager;
        Transport transport;
        SessionClock sessionClock;
        Recorder recorder;
        std::unique_ptr<Mixer> mixer;

        juce::MidiMessageCollector midiCollector;   // MIDI thread -> audio thread hand-off
        juce::MidiBuffer liveMidi;                  // audio thread scratch
        juce::AudioBuffer<float> renderBuffer, inputBuffer;
        MidiClip previewClip;                       // the mixer only holds a pointer

        juce::CriticalSection suspendLock;
        int  suspendCount = 0;                      // guarded by suspendLock
        std::atomic<bool> suspended { false }, insideCallback { false };
        std::atomic<float> masterPeak[2] { { 0.0f }, { 0.0f } };
        std::atomic<juce::int64> recordFromSample { 0 };   // count-in ends here

        juce::String topology;                      // avoids re-instantiating plugins on every edit
        bool metronomeBeforeCountIn = false;
        double clickPhase = 0.0, clickPhaseDelta = 0.0;    // audio thread only
        float  clickLevel = 0.0f, clickDecay = 0.999f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
    };
}
