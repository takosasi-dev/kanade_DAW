#pragma once
#include "Plugins/PluginHostProtocol.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <mutex>

namespace ss
{
    class PluginHostProxyEditor;

    /** Out-of-process VST3/AU host (gap 16, spec 10.3/15.6): a transparent
        juce::AudioPluginInstance that forwards every AudioProcessor call across a
        juce::ChildProcessCoordinator pipe to a worker process (PluginHostWorker.cpp)
        that actually holds the plugin. If that process crashes, this proxy
        silences its output, logs, and attempts exactly one respawn - it never
        takes ScoreSmith down with it.

        Every existing caller (Mixer.cpp's ChannelStrip, MixerView.cpp's
        PluginWindow) already talks to plugins purely through the abstract
        juce::AudioPluginInstance / juce::AudioProcessor interface, so this class
        is a drop-in replacement for PluginManager::createInstance's old
        in-process juce::AudioPluginFormatManager::createPluginInstance() call -
        nothing else in the codebase needs to change. */
    class PluginHostProxy final : public juce::AudioPluginInstance,
                                   private juce::Timer
    {
    public:
        ~PluginHostProxy() override;

        /** Spawns a worker process, loads `description` into it, and BLOCKS the
            calling thread (message thread only - see PluginManager::createInstance,
            called from ChannelStrip::rebuildFrom() while the audio graph is
            already suspended) for up to ~10s for the result. Returns nullptr and
            fills `errorOut` on any failure; never throws - exactly the contract
            PluginManager::createInstance already documents for the path this
            replaces. */
        static std::unique_ptr<PluginHostProxy> createAndLoad (const juce::PluginDescription& description,
                                                                double sampleRate, int blockSize,
                                                                const juce::MemoryBlock& initialState,
                                                                juce::String& errorOut);

        //==========================================================================
        // juce::AudioProcessor / juce::AudioPluginInstance
        const juce::String getName() const override                       { return pluginName; }
        void prepareToPlay (double sampleRate, int samplesPerBlock) override;
        void releaseResources() override;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
        bool acceptsMidi() const override                                  { return acceptsMidiFlag; }
        bool producesMidi() const override                                 { return producesMidiFlag; }
        double getTailLengthSeconds() const override                       { return tailLengthSecondsValue; }

        // We can't know until the openEditor round trip whether the remote plugin
        // actually has an editor - createEditor() always hands back a component
        // that either hosts the reparented remote window or shows a "no editor"
        // placeholder once it hears back, mirroring MixerView.cpp's own in-process
        // fallback. See PluginHostProxyEditor in the .cpp.
        bool hasEditor() const override                                    { return true; }
        juce::AudioProcessorEditor* createEditor() override;

        void getStateInformation (juce::MemoryBlock& destData) override;
        void setStateInformation (const void* data, int sizeInBytes) override;

        // Plugin "programs" aren't used anywhere in this codebase (grepped) - one
        // program, empty name, satisfies the pure virtuals without inventing a
        // feature nobody calls.
        int getNumPrograms() override                                      { return 1; }
        int getCurrentProgram() override                                   { return 0; }
        void setCurrentProgram (int) override                              {}
        const juce::String getProgramName (int) override                   { return {}; }
        void changeProgramName (int, const juce::String&) override         {}

        // Never called by anything in this codebase (grepped); trivial and correct.
        void fillInPluginDescription (juce::PluginDescription& d) const override { d = cachedDescription; }

    private:
        friend class PluginHostProxyEditor;

        class Coordinator;
        class RemoteParameter;

        PluginHostProxy (const juce::PluginDescription& description, const ss::hostproto::LoadResult& loaded,
                        double sampleRate, int blockSize, const juce::MemoryBlock& initialState,
                        std::unique_ptr<Coordinator> coordinatorToOwn);

        // -- called from PluginHostProxyEditor, message thread only ------------
        void editorOpening (PluginHostProxyEditor&, juce::int64 parentNativeHandle);
        void editorHostResized (int width, int height);
        void editorClosing();

        // -- watchdog / crash recovery -------------------------------------------
        void timerCallback() override;
        /** Called (via MessageManager::callAsync, so always on the message
            thread) when `reportingCoordinator` has lost its worker, either from
            handleConnectionLost() firing immediately or from a heartbeat miss.
            `reportingCoordinator` is compared by address only, never
            dereferenced, so it's safe even if that Coordinator has since been
            replaced and destroyed - see the .cpp for why. */
        void onWorkerCrashDetected (Coordinator* reportingCoordinator);
        /** Unsolicited PUSH from the worker: its own editor window resized. */
        void handleRemoteEditorResize (int width, int height);

        juce::String pluginName;
        bool acceptsMidiFlag = false, producesMidiFlag = false;
        double tailLengthSecondsValue = 0.0;

        juce::PluginDescription cachedDescription;
        /** Seed for a respawn's `load` request. Starts as the state passed to
            createAndLoad() and is kept fresh by every setStateInformation() call,
            so a crash respawn restores the plugin's actual last-known settings
            rather than resetting it to whatever it loaded with originally (which
            in practice is always empty - see PluginManager.cpp's integration
            note: Mixer.cpp always calls setStateInformation as a separate step
            right after createInstance()). */
        juce::MemoryBlock lastKnownState;

        double currentSampleRate = 44100.0;
        int currentBlockSize = 512;

        // "One request in flight at a time" for the whole pipe (PluginHostProtocol.h) -
        // covers both control calls (message thread) and process() (audio thread),
        // and also serialises the respawn's own coordinator swap against both.
        std::mutex callMutex;
        std::unique_ptr<Coordinator> coordinator;
        bool hasRespawnedOnce = false;
        std::atomic<bool> permanentlyFailed { false };
        int consecutiveHeartbeatMisses = 0;

        // Lets an in-flight async callback (posted from the pipe's background
        // thread) tell whether `this` is still alive by the time it actually
        // runs on the message thread, without ever dereferencing a dangling
        // PluginHostProxy* - see the .cpp for the destruction race this closes.
        std::shared_ptr<std::atomic<bool>> aliveFlag = std::make_shared<std::atomic<bool>> (true);

        // Message thread only: set in editorOpening(), cleared in editorClosing()
        // (called from ~PluginHostProxyEditor before the editor object goes away),
        // so this is never dangling when read.
        PluginHostProxyEditor* activeEditor = nullptr;
        bool editorIsOpen = false;

        JUCE_DECLARE_NON_COPYABLE (PluginHostProxy)
    };
}
