#include "Plugins/PluginManager.h"
#include "Plugins/PluginHostProtocol.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <mutex>
#include <queue>
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

/*  WORKER side of gap 16 (spec 10.3/15.6): one child process per loaded plugin
    INSTANCE, so a crash while playing takes that process down, not the DAW.
    Same juce::ChildProcessWorker shape as PluginManager.cpp's ScannerWorker;
    see PluginHostProtocol.h for the wire format both this file and the
    in-process proxy (owned by a sibling agent) share verbatim.             */

namespace ss
{

namespace
{
    /** Writes into a freshly-sized MemoryBlock. MemoryOutputStream's
        block-writing constructor leaves spare capacity on the block until
        flush()/destruction trims it - done here once, so every call site
        below can't forget it and send extra bytes on the wire. */
    template <typename WriteFn>
    juce::MemoryBlock buildMessage (WriteFn&& writeFn)
    {
        juce::MemoryBlock block;
        juce::MemoryOutputStream out (block, false);
        writeFn (out);
        out.flush();
        return block;
    }

    //==========================================================================
    /** Watches the plugin's own editor for a resize it initiated itself (as
        opposed to one we made in response to a host resizeEditor request) and
        reports it to the parent unsolicited. See HostWorker::ignoreNextResize. */
    class EditorResizeWatcher final : public juce::ComponentListener
    {
    public:
        EditorResizeWatcher (juce::AudioProcessorEditor& editorToWatch, std::function<void (int, int)> onResize)
            : editor (editorToWatch), callback (std::move (onResize))
        {
            editor.addComponentListener (this);
        }

        ~EditorResizeWatcher() override { editor.removeComponentListener (this); }

    private:
        void componentMovedOrResized (juce::Component&, bool, bool wasResized) override
        {
            if (wasResized && callback)
                callback (editor.getWidth(), editor.getHeight());
        }

        juce::AudioProcessorEditor& editor;
        std::function<void (int, int)> callback;

        JUCE_DECLARE_NON_COPYABLE (EditorResizeWatcher)
    };

    //==========================================================================
    /** Runs processBlock() on its own thread so a busy/blocked message thread
        (e.g. painting a plugin editor) can never add latency to, or drop, one
        of this instance's audio blocks. JUCE/VST3/AU plugins are contractually
        required to support processBlock() being called from a dedicated
        thread that isn't the message thread - that's literally how every real
        host calls it, so this is the correct architecture, not a workaround. */
    class AudioThread final : public juce::Thread
    {
    public:
        AudioThread (juce::AudioPluginInstance& pluginToRun, std::function<bool (const juce::MemoryBlock&)> replyFn)
            : juce::Thread ("ScoreSmith plugin host: audio"), plugin (pluginToRun), sendReply (std::move (replyFn))
        {
            startThread (juce::Thread::Priority::high);
        }

        ~AudioThread() override
        {
            signalThreadShouldExit();
            wakeEvent.signal();
            stopThread (2000);
        }

        /** Called from the pipe's own thread - just queues and wakes run(),
            never touches the plugin itself. */
        void enqueueProcessRequest (const juce::MemoryBlock& request)
        {
            {
                const std::lock_guard<std::mutex> lock (queueMutex);
                pending.push (request);
            }

            wakeEvent.signal();
        }

    private:
        void run() override
        {
            while (! threadShouldExit())
            {
                wakeEvent.wait();

                for (;;)
                {
                    juce::MemoryBlock request;

                    {
                        const std::lock_guard<std::mutex> lock (queueMutex);

                        if (pending.empty())
                            break;

                        request = std::move (pending.front());
                        pending.pop();
                    }

                    processOneBlock (request);
                }
            }
        }

        void processOneBlock (const juce::MemoryBlock& request)
        {
            juce::MemoryInputStream in (request, false);
            ss::hostproto::readCmd (in); // already known to be Cmd::process

            std::vector<ss::hostproto::ParamChange> changes;
            juce::AudioBuffer<float> audio;
            juce::MidiBuffer midi;
            ss::hostproto::readProcessRequest (in, changes, audio, midi);

            // Defensive only: a malformed/zero-sized block gets silence back
            // instead of risking an out-of-bounds call into the plugin.
            if (audio.getNumChannels() > 0 && audio.getNumSamples() > 0)
            {
                const auto& params = plugin.getParameters();

                for (const auto& c : changes)
                    if (c.index >= 0 && c.index < params.size())
                        params.getUnchecked (c.index)->setValue (c.value);

                // Deliberately no try/catch: a genuine crash in here is
                // expected to take this process down - that IS the isolation
                // this feature exists for, so swallowing it here would defeat
                // the point.
                plugin.processBlock (audio, midi);
            }
            else
            {
                audio.clear();
                midi.clear();
            }

            sendReply (buildMessage ([&] (juce::MemoryOutputStream& out)
            {
                ss::hostproto::writeProcessResponse (out, audio, midi);
            }));
        }

        juce::AudioPluginInstance& plugin;
        std::function<bool (const juce::MemoryBlock&)> sendReply;

        juce::WaitableEvent wakeEvent;
        std::mutex queueMutex;
        std::queue<juce::MemoryBlock> pending;

        JUCE_DECLARE_NON_COPYABLE (AudioThread)
    };

    //==========================================================================
    /** The worker process itself: owns exactly one plugin instance for its
        whole life. Modelled on ScannerWorker in PluginManager.cpp - control
        commands are queued and bounced to the message thread via
        AsyncUpdater; `process` instead goes to AudioThread above; `heartbeat`
        is answered synchronously right here, on the pipe's own callback
        thread, so a slow control call or a busy audio block can never make a
        healthy plugin look dead to the parent's watchdog. */
    class HostWorker final : public juce::ChildProcessWorker,
                             private juce::AsyncUpdater
    {
    public:
        HostWorker() { formats.addDefaultFormats(); }

        ~HostWorker() override
        {
            cancelPendingUpdate();

            // Stop the audio thread before the instance it calls into goes away.
            audioThread.reset();

            // The editor must be destroyed while the instance that owns it is
            // still alive - see closeEditorInternal().
            closeEditorInternal();

            if (pluginInstance != nullptr)
                pluginInstance->releaseResources();
        }

        using juce::ChildProcessWorker::initialiseFromCommandLine;

    private:
        //======================================================================
        // Arrives on the pipe's own thread (per ChildProcessWorker's docs).
        void handleMessageFromCoordinator (const juce::MemoryBlock& request) override
        {
            if (request.isEmpty())
                return;

            juce::MemoryInputStream peek (request, false);
            const auto cmd = ss::hostproto::readCmd (peek);

            if (cmd == ss::hostproto::Cmd::heartbeat)
            {
                sendReply (buildMessage ([] (juce::MemoryOutputStream& out) { ss::hostproto::writeAck (out, true); }));
                return;
            }

            if (cmd == ss::hostproto::Cmd::process)
            {
                if (audioThread != nullptr)
                    audioThread->enqueueProcessRequest (request);
                else
                    replyToProcessWithSilence (request); // no instance loaded (yet) - don't hang the caller

                return;
            }

            // load/prepare/release/getState/setState/openEditor/resizeEditor/
            // closeEditor all touch the real AudioPluginInstance/editor and,
            // per JUCE/VST3/AU convention, must run on the message thread.
            {
                const std::lock_guard<std::mutex> lock (controlMutex);
                pendingControl.push (request);
            }

            triggerAsyncUpdate();
        }

        void handleConnectionLost() override
        {
            juce::JUCEApplicationBase::quit();
        }

        void handleAsyncUpdate() override
        {
            for (;;)
            {
                juce::MemoryBlock request;

                {
                    const std::lock_guard<std::mutex> lock (controlMutex);

                    if (pendingControl.empty())
                        return;

                    request = pendingControl.front();
                    pendingControl.pop();
                }

                handleControlCommand (request);
            }
        }

        //======================================================================
        void handleControlCommand (const juce::MemoryBlock& request)
        {
            juce::MemoryInputStream in (request, false);

            switch (ss::hostproto::readCmd (in))
            {
                case ss::hostproto::Cmd::load:         handleLoad (in);         break;
                case ss::hostproto::Cmd::prepare:      handlePrepare (in);      break;
                case ss::hostproto::Cmd::release:      handleRelease();         break;
                case ss::hostproto::Cmd::getState:     handleGetState();        break;
                case ss::hostproto::Cmd::setState:     handleSetState (in);     break;
                case ss::hostproto::Cmd::openEditor:   handleOpenEditor (in);   break;
                case ss::hostproto::Cmd::resizeEditor: handleResizeEditor (in); break;
                case ss::hostproto::Cmd::closeEditor:  handleCloseEditor();     break;

                default:
                    jassertfalse; // process/heartbeat never reach here; editorResized is push-only
                    break;
            }
        }

        void handleLoad (juce::MemoryInputStream& in)
        {
            juce::String descriptionXml;
            double sampleRate = 44100.0;
            int blockSize = 512;
            juce::MemoryBlock initialState;
            ss::hostproto::readLoadRequest (in, descriptionXml, sampleRate, blockSize, initialState);

            ss::hostproto::LoadResult result;

            // Exception safety only covers load: a plugin that throws while
            // instantiating must produce a clean !ok reply instead of taking
            // the worker down before it can even report failure. Anything
            // that crashes for real (SEH, stack overflow) still kills the
            // process outright, which is the isolation working as intended.
            try
            {
                auto xml = juce::parseXML (descriptionXml);
                juce::PluginDescription desc;

                if (xml == nullptr || ! desc.loadFromXml (*xml))
                {
                    result.errorMessage = "Malformed plugin description";
                }
                else
                {
                    juce::String error;
                    auto instance = formats.createPluginInstance (desc, sampleRate, blockSize, error);

                    if (instance == nullptr)
                    {
                        result.errorMessage = error.isNotEmpty() ? error : ("Could not load " + desc.name);
                    }
                    else
                    {
                        if (! initialState.isEmpty())
                            instance->setStateInformation (initialState.getData(), (int) initialState.getSize());

                        instance->setRateAndBufferSizeDetails (sampleRate, blockSize);
                        instance->prepareToPlay (sampleRate, blockSize);

                        result.ok = true;
                        result.name = instance->getName();
                        result.acceptsMidi = instance->acceptsMidi();
                        result.producesMidi = instance->producesMidi();
                        result.isInstrument = instance->getPluginDescription().isInstrument;
                        result.numInputChannels = instance->getTotalNumInputChannels();
                        result.numOutputChannels = instance->getTotalNumOutputChannels();
                        result.latencySamples = instance->getLatencySamples();
                        result.tailLengthSeconds = instance->getTailLengthSeconds();

                        for (auto* param : instance->getParameters())
                        {
                            ss::hostproto::ParamShape shape;
                            shape.name = param->getName (128);
                            shape.label = param->getLabel();
                            shape.isDiscrete = param->isDiscrete();
                            shape.numSteps = param->getNumSteps();
                            shape.defaultValue = param->getDefaultValue();
                            result.params.push_back (shape);
                        }

                        // One worker hosts exactly one instance for the rest of its life.
                        pluginInstance = std::move (instance);
                        audioThread = std::make_unique<AudioThread> (*pluginInstance,
                            [this] (const juce::MemoryBlock& m) { return sendReply (m); });
                    }
                }
            }
            catch (const std::exception& e)
            {
                result = {};
                result.errorMessage = "Plugin threw while loading: " + juce::String (e.what());
            }
            catch (...)
            {
                result = {};
                result.errorMessage = "Plugin threw while loading";
            }

            sendReply (buildMessage ([&] (juce::MemoryOutputStream& out) { ss::hostproto::writeLoadResponse (out, result); }));
        }

        void handlePrepare (juce::MemoryInputStream& in)
        {
            double sampleRate = 44100.0;
            int blockSize = 512;
            ss::hostproto::readPrepareRequest (in, sampleRate, blockSize);

            bool ok = false;

            if (pluginInstance != nullptr)
            {
                pluginInstance->setRateAndBufferSizeDetails (sampleRate, blockSize);
                pluginInstance->prepareToPlay (sampleRate, blockSize);
                ok = true;
            }

            sendAck (ok);
        }

        void handleRelease()
        {
            bool ok = false;

            if (pluginInstance != nullptr)
            {
                pluginInstance->releaseResources();
                ok = true;
            }

            sendAck (ok);
        }

        void handleGetState()
        {
            juce::MemoryBlock state;

            if (pluginInstance != nullptr)
                pluginInstance->getStateInformation (state);

            sendReply (buildMessage ([&] (juce::MemoryOutputStream& out) { ss::hostproto::writeStateBlock (out, state); }));
        }

        void handleSetState (juce::MemoryInputStream& in)
        {
            juce::MemoryBlock state;
            ss::hostproto::readStateBlock (in, state);

            bool ok = false;

            if (pluginInstance != nullptr)
            {
                pluginInstance->setStateInformation (state.getData(), (int) state.getSize());
                ok = true;
            }

            sendAck (ok);
        }

        //======================================================================
        void handleOpenEditor (juce::MemoryInputStream& in)
        {
            const auto parentHandle = ss::hostproto::readOpenEditorRequest (in);

            if (editor == nullptr && pluginInstance != nullptr)
                openEditorInternal (parentHandle);

            ss::hostproto::OpenEditorResult result;

            if (editor != nullptr)
            {
                result.ok = true;
                result.width = editor->getWidth();
                result.height = editor->getHeight();
                result.resizable = editor->isResizable();
            }

            sendReply (buildMessage ([&] (juce::MemoryOutputStream& out) { ss::hostproto::writeOpenEditorResponse (out, result); }));
        }

        void openEditorInternal (juce::int64 parentNativeHandle)
        {
            editor = pluginInstance->createEditorIfNeeded();

            if (editor == nullptr)
                return; // no editor - not an error, see PluginHostProtocol.h

            editor->addToDesktop (0);

           #if JUCE_WINDOWS
            auto* editorHwnd = (HWND) editor->getPeer()->getNativeHandle();
            auto* parentHwnd = (HWND) parentNativeHandle;

            ::SetWindowLongPtr (editorHwnd, GWL_STYLE,
                                (::GetWindowLongPtr (editorHwnd, GWL_STYLE) & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME)) | WS_CHILD);
            ::SetParent (editorHwnd, parentHwnd);
            ::SetWindowPos (editorHwnd, nullptr, 0, 0, editor->getWidth(), editor->getHeight(),
                            SWP_NOZORDER | SWP_FRAMECHANGED);
            ::ShowWindow (editorHwnd, SW_SHOW);
           #else
            // ponytail: no Mac dev/test rig for this project - remoting an
            // NSView across processes needs private/XPC APIs that can't be
            // validated here, so the editor is left as its own independent
            // top-level window instead of being embedded in the host's
            // placeholder. Upgrade path: NSView/XPC remoting if a Mac rig
            // ever exists to test it against.
            juce::ignoreUnused (parentNativeHandle);
            editor->setVisible (true);
           #endif

            resizeWatcher = std::make_unique<EditorResizeWatcher> (*editor, [this] (int w, int h)
            {
                // Only spontaneous, plugin-driven resizes are pushed - one we
                // caused ourselves in handleResizeEditor() is suppressed here
                // to avoid a feedback loop with the parent's own resize logic.
                if (ignoreNextResize)
                    return;

                sendReply (buildMessage ([&] (juce::MemoryOutputStream& out) { ss::hostproto::writeEditorResizedPush (out, w, h); }));
            });
        }

        void handleResizeEditor (juce::MemoryInputStream& in)
        {
            int width = 0, height = 0;
            ss::hostproto::readResizeEditorRequest (in, width, height);

            if (editor != nullptr)
            {
                ignoreNextResize = true;
                editor->setSize (width, height);

               #if JUCE_WINDOWS
                if (auto* peer = editor->getPeer())
                    ::SetWindowPos ((HWND) peer->getNativeHandle(), nullptr, 0, 0, width, height,
                                    SWP_NOZORDER | SWP_NOMOVE);
               #endif

                ignoreNextResize = false;
            }

            sendAck (true); // no editor open is a no-op, not an error
        }

        void handleCloseEditor()
        {
            closeEditorInternal();
            sendAck (true);
        }

        void closeEditorInternal()
        {
            if (editor == nullptr)
                return;

            resizeWatcher.reset();

            // AudioProcessorEditor's destructor asserts that the wrapper
            // (that's us) called this first - see juce_AudioProcessorEditor.cpp.
            if (pluginInstance != nullptr)
                pluginInstance->editorBeingDeleted (editor);

            delete editor;
            editor = nullptr;
        }

        //======================================================================
        void replyToProcessWithSilence (const juce::MemoryBlock& request)
        {
            juce::MemoryInputStream in (request, false);
            ss::hostproto::readCmd (in);

            std::vector<ss::hostproto::ParamChange> changes;
            juce::AudioBuffer<float> audio;
            juce::MidiBuffer midi;
            ss::hostproto::readProcessRequest (in, changes, audio, midi);

            audio.clear();
            midi.clear();

            sendReply (buildMessage ([&] (juce::MemoryOutputStream& out) { ss::hostproto::writeProcessResponse (out, audio, midi); }));
        }

        void sendAck (bool ok)
        {
            sendReply (buildMessage ([ok] (juce::MemoryOutputStream& out) { ss::hostproto::writeAck (out, ok); }));
        }

        bool sendReply (const juce::MemoryBlock& message)
        {
            /*  ChildProcessWorker::sendMessageToCoordinator() ends up at
                InterprocessConnection::writeData(), which only takes a *read*
                lock around the pipe handle - so two threads sending at once
                (a heartbeat answered right here on the pipe thread, a control
                reply from the message thread, a process reply from
                AudioThread) can interleave bytes on the wire. Every outgoing
                send funnels through here to serialise them.                */
            const std::lock_guard<std::mutex> lock (sendMutex);
            return sendMessageToCoordinator (message);
        }

        //======================================================================
        juce::AudioPluginFormatManager formats;
        std::unique_ptr<juce::AudioPluginInstance> pluginInstance;
        std::unique_ptr<AudioThread> audioThread;

        juce::AudioProcessorEditor* editor = nullptr; // not owned by pluginInstance - see closeEditorInternal()
        std::unique_ptr<EditorResizeWatcher> resizeWatcher;
        bool ignoreNextResize = false;

        std::mutex controlMutex;
        std::queue<juce::MemoryBlock> pendingControl;

        std::mutex sendMutex;

        JUCE_DECLARE_NON_COPYABLE (HostWorker)
    };
}

//==============================================================================
bool PluginManager::runPluginHostProcessIfRequested (const juce::String& commandLine)
{
    if (! commandLine.contains (ss::hostproto::workerUID()))
        return false;

    /*  Kept alive for the life of the process: after this returns true the
        caller must skip all UI setup and just run the message loop. The
        worker quits the app itself when the coordinator goes away.        */
    static std::unique_ptr<HostWorker> worker;
    worker = std::make_unique<HostWorker>();

    if (worker->initialiseFromCommandLine (commandLine, ss::hostproto::workerUID()))
        return true;

    worker.reset();
    return false;
}

}
