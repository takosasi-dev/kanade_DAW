#include "Plugins/PluginHostProxy.h"

#include <juce_events/juce_events.h>
#include <chrono>
#include <condition_variable>

namespace ss
{

namespace
{
    // load can be slow (a plugin scanning its own preset bank, first-run
    // authorisation, ...) - generous so a merely-slow plugin doesn't get
    // mistaken for a hung one. Everything else on the control side is a quick
    // in-process call on the worker's end, so a few seconds is plenty and keeps
    // a genuinely wedged worker from stalling the message thread for long.
    constexpr double loadTimeoutSeconds = 10.0;
    constexpr double controlCallTimeoutSeconds = 3.0;
    constexpr double heartbeatTimeoutSeconds = 0.5;
    constexpr int heartbeatIntervalMs = 1500;
    // ponytail: two consecutive misses (~3s) before treating the worker as
    // hung and forcing the crash-recovery path. Upgrade path: make this
    // configurable if real-world plugins prove chatty/slow enough to false-
    // positive here.
    constexpr int maxHeartbeatMisses = 2;
}

//==============================================================================
/** Wraps one juce::ChildProcessCoordinator / worker pipe. A Coordinator is
    replaced wholesale (never reset in place) when the watchdog respawns after a
    crash - see PluginHostProxy::onWorkerCrashDetected(). */
class PluginHostProxy::Coordinator final : private juce::ChildProcessCoordinator
{
public:
    Coordinator() = default;

    bool launch()
    {
        return launchWorkerProcess (juce::File::getSpecialLocation (juce::File::currentExecutableFile),
                                    ss::hostproto::workerUID(), 0, 0);
    }

    /** Must be set once, right after construction/launch, before this
        Coordinator can be handed a request whose response might race a crash
        notification. `aliveFlag` lets a callback posted from the pipe's
        background thread tell - once it actually runs on the message thread -
        whether `owner` has started destructing, without ever dereferencing a
        dangling PluginHostProxy*. */
    void setOwner (PluginHostProxy* o, std::shared_ptr<std::atomic<bool>> flag)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        owner = o;
        ownerAlive = std::move (flag);
    }

    enum class WaitState { gotReply, timeout, connectionLost };

    /** Sends `request` and blocks the CALLING thread for up to `timeoutSeconds`.
        Callers serialise through PluginHostProxy::callMutex - the protocol
        guarantees only one request is ever in flight on this pipe at a time, so
        there's no need for request ids here (see PluginHostProtocol.h). */
    WaitState sendAndWait (const juce::MemoryBlock& request, double timeoutSeconds, juce::MemoryBlock& replyOut)
    {
        std::unique_lock<std::mutex> lock (mutex);
        awaitingReply = true;
        gotReply = false;
        lock.unlock();

        if (! sendMessageToWorker (request))
        {
            const std::lock_guard<std::mutex> relock (mutex);
            awaitingReply = false;
            return WaitState::connectionLost;
        }

        lock.lock();
        const bool arrived = condition.wait_for (lock, std::chrono::duration<double> (timeoutSeconds),
                                                 [this] { return gotReply || connectionLostFlag; });
        awaitingReply = false;

        if (connectionLostFlag)
            return WaitState::connectionLost;

        if (! arrived)
            return WaitState::timeout;

        replyOut = std::move (pendingReply);
        return WaitState::gotReply;
    }

private:
    void handleMessageFromWorker (const juce::MemoryBlock& message) override
    {
        std::unique_lock<std::mutex> lock (mutex);

        if (awaitingReply)
        {
            pendingReply = message;
            gotReply = true;
            lock.unlock();
            condition.notify_one();
            return;
        }

        // Not waiting on anything: the only message a worker ever sends
        // unprompted is the editorResized push (PluginHostProtocol.h) - unlike
        // every response, it carries its own Cmd byte precisely so it can be
        // told apart from a reply here.
        auto* o = owner;
        auto flag = ownerAlive;
        lock.unlock();

        if (message.getSize() == 0)
            return;

        juce::MemoryInputStream in (message, false);

        if (ss::hostproto::readCmd (in) != ss::hostproto::Cmd::editorResized)
            return;

        int width = 0, height = 0;
        ss::hostproto::readEditorResizedPush (in, width, height);

        if (o != nullptr)
            juce::MessageManager::callAsync ([o, flag, width, height]
            {
                if (flag != nullptr && flag->load())
                    o->handleRemoteEditorResize (width, height);
            });
    }

    void handleConnectionLost() override
    {
        PluginHostProxy* o;
        std::shared_ptr<std::atomic<bool>> flag;

        {
            const std::lock_guard<std::mutex> lock (mutex);
            connectionLostFlag = true;
            o = owner;
            flag = ownerAlive;
        }

        condition.notify_one();

        auto* self = this; // compared by address only in onWorkerCrashDetected, never dereferenced -
                           // safe even if this Coordinator is destroyed before the callback runs.

        if (o != nullptr)
            juce::MessageManager::callAsync ([o, flag, self]
            {
                if (flag != nullptr && flag->load())
                    o->onWorkerCrashDetected (self);
            });
    }

    PluginHostProxy* owner = nullptr;
    std::shared_ptr<std::atomic<bool>> ownerAlive;
    std::mutex mutex;
    std::condition_variable condition;
    juce::MemoryBlock pendingReply;
    bool awaitingReply = false, gotReply = false, connectionLostFlag = false;
};

//==============================================================================
/** One AudioProcessorParameter per remote parameter reported at load
    (LoadResult::params). setValue() is called from the audio thread by
    Mixer.cpp's applyAutomation - potentially once per automated parameter per
    block - so it must be realtime-safe: just an atomic store, never IPC.
    processBlock() drains every dirty parameter once per block and bundles the
    changes into that block's `process` request. */
class PluginHostProxy::RemoteParameter final : public juce::AudioPluginInstance::HostedParameter
{
public:
    RemoteParameter (int idx, const ss::hostproto::ParamShape& shape)
        : index (idx), paramName (shape.name), paramLabel (shape.label),
          discrete (shape.isDiscrete), steps (shape.numSteps), defaultVal (shape.defaultValue)
    {
        currentValue.store (shape.defaultValue, std::memory_order_relaxed);
    }

    juce::String getParameterID() const override { return juce::String (index); }

    float getValue() const override { return currentValue.load (std::memory_order_relaxed); }

    void setValue (float newValue) override
    {
        currentValue.store (newValue, std::memory_order_relaxed);
        dirty.store (true, std::memory_order_relaxed);
    }

    float getDefaultValue() const override { return defaultVal; }
    juce::String getName (int maximumStringLength) const override { return paramName.substring (0, maximumStringLength); }
    juce::String getLabel() const override { return paramLabel; }
    int getNumSteps() const override { return steps; }
    bool isDiscrete() const override { return discrete; }
    float getValueForText (const juce::String& text) const override { return text.getFloatValue(); }

    /** [audio thread, processBlock only] Take-and-clear: true (with `outValue`
        set) if this parameter changed since the last call. Lock-free. */
    bool consumeChangeIfAny (float& outValue) noexcept
    {
        if (! dirty.exchange (false, std::memory_order_relaxed))
            return false;

        outValue = currentValue.load (std::memory_order_relaxed);
        return true;
    }

    const int index;

private:
    juce::String paramName, paramLabel;
    bool discrete;
    int steps;
    float defaultVal;
    std::atomic<float> currentValue { 0.0f };
    std::atomic<bool> dirty { false };
};

//==============================================================================
/** Hosts the remote plugin's editor window, reparented in via a native handle
    (Windows: SetParent(), done worker-side - see PluginHostProtocol.h's
    openEditor). MixerView.cpp's PluginWindow makes this Component its
    DocumentWindow's sole content via setContentOwned(), so it never gets its
    own native peer; the worker embeds into the DocumentWindow's own HWND
    instead, which is why resizeEditor only ever carries a size, not a
    position - this component's own bounds are already (0, 0)-relative to that
    HWND's client area, since nothing else shares the window with it. */
class PluginHostProxyEditor final : public juce::AudioProcessorEditor
{
public:
    explicit PluginHostProxyEditor (PluginHostProxy& p) : juce::AudioProcessorEditor (p), owner (p)
    {
        addChildComponent (placeholder);
        placeholder.setJustificationType (juce::Justification::centred);
        // createEditorIfNeeded() asserts a non-zero size before the openEditor
        // round trip (which needs a peer, so it can't happen until later - see
        // parentHierarchyChanged()) has had any chance to answer.
        setSize (400, 300);
    }

    ~PluginHostProxyEditor() override { owner.editorClosing(); }

    void parentHierarchyChanged() override
    {
        juce::AudioProcessorEditor::parentHierarchyChanged();

        if (openRequested)
            return;

        if (auto* topLevel = getTopLevelComponent())
            if (auto* peer = topLevel->getPeer())
            {
                openRequested = true;
                const auto handle = (juce::int64) reinterpret_cast<juce::pointer_sized_int> (peer->getNativeHandle());
                owner.editorOpening (*this, handle);
            }
    }

    void resized() override
    {
        placeholder.setBounds (getLocalBounds());

        if (opened && ! applyingRemoteResize)
            owner.editorHostResized (getWidth(), getHeight());
    }

    /** [message thread] openEditor answered ok - a real remote editor exists. */
    void applyOpened (int width, int height, bool resizableFlag)
    {
        opened = true;
        setResizable (resizableFlag, false);
        setSize (juce::jmax (1, width), juce::jmax (1, height));
    }

    /** [message thread] openEditor answered !ok - mirrors MixerView.cpp's own
        in-process "no editor" fallback for visual consistency. */
    void showNoEditorPlaceholder()
    {
        placeholder.setText (TRANS ("This plugin has no editor"), juce::dontSendNotification);
        placeholder.setVisible (true);
        setSize (320, 120);
    }

    /** [message thread, via MessageManager::callAsync] The remote editor
        resized itself; reflect it here without echoing a resizeEditor back. */
    void applyRemoteResize (int width, int height)
    {
        applyingRemoteResize = true;
        setSize (juce::jmax (1, width), juce::jmax (1, height));
        applyingRemoteResize = false;
    }

private:
    PluginHostProxy& owner;
    juce::Label placeholder { {}, {} };
    bool openRequested = false, opened = false, applyingRemoteResize = false;
};

//==============================================================================
PluginHostProxy::PluginHostProxy (const juce::PluginDescription& description, const ss::hostproto::LoadResult& loaded,
                                  double sampleRate, int blockSize, const juce::MemoryBlock& initialState,
                                  std::unique_ptr<Coordinator> coordinatorToOwn)
    : AudioPluginInstance (BusesProperties()
                            .withInput ("Input", juce::AudioChannelSet::canonicalChannelSet (loaded.numInputChannels), true)
                            .withOutput ("Output", juce::AudioChannelSet::canonicalChannelSet (loaded.numOutputChannels), true)),
      pluginName (loaded.name),
      acceptsMidiFlag (loaded.acceptsMidi),
      producesMidiFlag (loaded.producesMidi),
      tailLengthSecondsValue (loaded.tailLengthSeconds),
      cachedDescription (description),
      lastKnownState (initialState),
      currentSampleRate (sampleRate > 0.0 ? sampleRate : 44100.0),
      currentBlockSize (juce::jmax (1, blockSize)),
      coordinator (std::move (coordinatorToOwn))
{
    coordinator->setOwner (this, aliveFlag);

    for (size_t i = 0; i < loaded.params.size(); ++i)
        addHostedParameter (std::make_unique<RemoteParameter> ((int) i, loaded.params[i]));

    setLatencySamples (loaded.latencySamples);

    startTimer (heartbeatIntervalMs);
}

PluginHostProxy::~PluginHostProxy()
{
    // First line, before anything else: any async callback already posted from
    // the pipe's background thread (a push, a crash notice) checks this before
    // touching `this` once it actually runs on the message thread, so it can
    // never observe a half-destroyed object even if it raced the destructor.
    aliveFlag->store (false);

    stopTimer();
}

//==============================================================================
std::unique_ptr<PluginHostProxy> PluginHostProxy::createAndLoad (const juce::PluginDescription& description,
                                                                  double sampleRate, int blockSize,
                                                                  const juce::MemoryBlock& initialState,
                                                                  juce::String& errorOut)
{
    errorOut.clear();

    auto coordinator = std::make_unique<Coordinator>();

    if (! coordinator->launch())
    {
        errorOut = "Could not launch the plugin host worker process";
        return nullptr;
    }

    juce::MemoryBlock request;
    {
        juce::MemoryOutputStream out (request, true);
        auto xml = description.createXml();
        ss::hostproto::writeLoadRequest (out, xml != nullptr ? xml->toString() : juce::String(),
                                         sampleRate, blockSize, initialState);
    }

    juce::MemoryBlock reply;
    const auto state = coordinator->sendAndWait (request, loadTimeoutSeconds, reply);

    if (state == Coordinator::WaitState::timeout)
    {
        errorOut = "Plugin host worker did not respond loading " + description.name;
        return nullptr;
    }

    if (state == Coordinator::WaitState::connectionLost)
    {
        errorOut = "Plugin host worker process crashed while loading " + description.name;
        return nullptr;
    }

    juce::MemoryInputStream in (reply, false);
    const auto result = ss::hostproto::readLoadResponse (in);

    if (! result.ok)
    {
        errorOut = result.errorMessage.isNotEmpty() ? result.errorMessage : ("Could not load " + description.name);
        return nullptr;
    }

    return std::unique_ptr<PluginHostProxy> (new PluginHostProxy (description, result, sampleRate, blockSize,
                                                                   initialState, std::move (coordinator)));
}

//==============================================================================
void PluginHostProxy::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    currentBlockSize = juce::jmax (1, samplesPerBlock);

    if (permanentlyFailed.load())
        return;

    juce::MemoryBlock request;
    { juce::MemoryOutputStream out (request, true);
      ss::hostproto::writePrepareRequest (out, currentSampleRate, currentBlockSize); }

    juce::MemoryBlock reply;
    const std::lock_guard<std::mutex> lock (callMutex);

    if (coordinator != nullptr)
        coordinator->sendAndWait (request, controlCallTimeoutSeconds, reply); // best-effort: a miss here just
                                                                              // means process() silences until
                                                                              // the watchdog notices and respawns
}

void PluginHostProxy::releaseResources()
{
    if (permanentlyFailed.load())
        return;

    juce::MemoryBlock request;
    { juce::MemoryOutputStream out (request, true); ss::hostproto::writeReleaseRequest (out); }

    juce::MemoryBlock reply;
    const std::lock_guard<std::mutex> lock (callMutex);

    if (coordinator != nullptr)
        coordinator->sendAndWait (request, controlCallTimeoutSeconds, reply); // best-effort
}

void PluginHostProxy::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (permanentlyFailed.load (std::memory_order_relaxed))
    {
        buffer.clear();
        return;
    }

    std::vector<ss::hostproto::ParamChange> changes;

    for (auto* p : getParameters())
    {
        auto* rp = static_cast<RemoteParameter*> (p);
        float v;

        if (rp->consumeChangeIfAny (v))
            changes.push_back ({ rp->index, v });
    }

    juce::MemoryBlock request;
    { juce::MemoryOutputStream out (request, true);
      ss::hostproto::writeProcessRequest (out, changes, buffer, midi); }

    // 3x the nominal block duration, clamped - never the load timeout. This
    // runs on the real audio thread, so a miss here is an audible dropout
    // across the whole mix, not just this plugin: it must always return
    // promptly no matter what state the worker is in.
    const auto blockSeconds = (double) currentBlockSize / juce::jmax (1.0, currentSampleRate);
    const auto timeoutSeconds = juce::jlimit (0.005, 0.05, blockSeconds * 3.0);

    juce::MemoryBlock reply;
    auto state = Coordinator::WaitState::connectionLost;

    {
        const std::lock_guard<std::mutex> lock (callMutex);

        if (coordinator != nullptr)
            state = coordinator->sendAndWait (request, timeoutSeconds, reply);
    }

    if (state != Coordinator::WaitState::gotReply)
    {
        buffer.clear();
        midi.clear();
        return;
    }

    juce::MemoryInputStream in (reply, false);
    ss::hostproto::readProcessResponse (in, buffer, midi);
}

void PluginHostProxy::getStateInformation (juce::MemoryBlock& destData)
{
    destData.reset();

    if (permanentlyFailed.load())
        return;

    juce::MemoryBlock request;
    { juce::MemoryOutputStream out (request, true); ss::hostproto::writeCmd (out, ss::hostproto::Cmd::getState); }

    juce::MemoryBlock reply;
    Coordinator::WaitState state = Coordinator::WaitState::connectionLost;

    {
        const std::lock_guard<std::mutex> lock (callMutex);

        if (coordinator != nullptr)
            state = coordinator->sendAndWait (request, controlCallTimeoutSeconds, reply);
    }

    if (state != Coordinator::WaitState::gotReply)
        return; // leave destData empty - AudioProcessor's documented "nothing to save" case

    juce::MemoryInputStream in (reply, false);
    ss::hostproto::readStateBlock (in, destData);
}

void PluginHostProxy::setStateInformation (const void* data, int sizeInBytes)
{
    // Kept fresh here (not just at construction) so a crash respawn restores
    // the plugin's actual last-known settings - see the field comment in the
    // header for why this deliberately isn't literally the construction-time
    // bytes.
    lastKnownState = juce::MemoryBlock (data, (size_t) juce::jmax (0, sizeInBytes));

    if (permanentlyFailed.load())
        return;

    juce::MemoryBlock request;
    {
        juce::MemoryOutputStream out (request, true);
        ss::hostproto::writeCmd (out, ss::hostproto::Cmd::setState);
        ss::hostproto::writeStateBlock (out, lastKnownState);
    }

    juce::MemoryBlock reply;
    const std::lock_guard<std::mutex> lock (callMutex);

    if (coordinator != nullptr)
        coordinator->sendAndWait (request, controlCallTimeoutSeconds, reply); // best-effort
}

//==============================================================================
juce::AudioProcessorEditor* PluginHostProxy::createEditor()
{
    return new PluginHostProxyEditor (*this);
}

void PluginHostProxy::editorOpening (PluginHostProxyEditor& editor, juce::int64 parentNativeHandle)
{
    activeEditor = &editor;

    if (permanentlyFailed.load())
    {
        editor.showNoEditorPlaceholder();
        return;
    }

    juce::MemoryBlock request;
    { juce::MemoryOutputStream out (request, true);
      ss::hostproto::writeOpenEditorRequest (out, parentNativeHandle); }

    juce::MemoryBlock reply;
    Coordinator::WaitState state;

    {
        const std::lock_guard<std::mutex> lock (callMutex);

        if (coordinator == nullptr)
        {
            editor.showNoEditorPlaceholder();
            return;
        }

        state = coordinator->sendAndWait (request, controlCallTimeoutSeconds, reply);
    }

    if (state != Coordinator::WaitState::gotReply)
    {
        editor.showNoEditorPlaceholder();
        return;
    }

    juce::MemoryInputStream in (reply, false);
    const auto result = ss::hostproto::readOpenEditorResponse (in);

    if (! result.ok)
    {
        editor.showNoEditorPlaceholder();
        return;
    }

    editorIsOpen = true;
    editor.applyOpened (result.width, result.height, result.resizable);
}

void PluginHostProxy::editorHostResized (int width, int height)
{
    if (! editorIsOpen || permanentlyFailed.load())
        return;

    juce::MemoryBlock request;
    { juce::MemoryOutputStream out (request, true);
      ss::hostproto::writeResizeEditorRequest (out, width, height); }

    juce::MemoryBlock reply;
    const std::lock_guard<std::mutex> lock (callMutex);

    if (coordinator != nullptr)
        coordinator->sendAndWait (request, controlCallTimeoutSeconds, reply); // best-effort ack
}

void PluginHostProxy::editorClosing()
{
    activeEditor = nullptr;
    const bool wasOpen = editorIsOpen;
    editorIsOpen = false;

    if (! wasOpen || permanentlyFailed.load())
        return;

    juce::MemoryBlock request;
    { juce::MemoryOutputStream out (request, true); ss::hostproto::writeCmd (out, ss::hostproto::Cmd::closeEditor); }

    // Waits briefly for the ack rather than firing-and-forgetting, so no stray
    // reply is left sitting on the pipe that a later handleMessageFromWorker()
    // could misread as an unsolicited push.
    juce::MemoryBlock reply;
    const std::lock_guard<std::mutex> lock (callMutex);

    if (coordinator != nullptr)
        coordinator->sendAndWait (request, controlCallTimeoutSeconds, reply);
}

void PluginHostProxy::handleRemoteEditorResize (int width, int height)
{
    if (activeEditor != nullptr)
        activeEditor->applyRemoteResize (width, height);
}

//==============================================================================
void PluginHostProxy::timerCallback()
{
    if (permanentlyFailed.load())
    {
        stopTimer();
        return;
    }

    juce::MemoryBlock request;
    { juce::MemoryOutputStream out (request, true); ss::hostproto::writeCmd (out, ss::hostproto::Cmd::heartbeat); }

    juce::MemoryBlock reply;
    auto state = Coordinator::WaitState::connectionLost;
    Coordinator* liveCoordinator = nullptr;

    {
        const std::lock_guard<std::mutex> lock (callMutex);
        liveCoordinator = coordinator.get();

        if (liveCoordinator != nullptr)
            state = liveCoordinator->sendAndWait (request, heartbeatTimeoutSeconds, reply);
    }

    if (state == Coordinator::WaitState::gotReply)
    {
        consecutiveHeartbeatMisses = 0;
        return;
    }

    if (++consecutiveHeartbeatMisses < maxHeartbeatMisses)
        return; // could just be one slow block sharing the pipe - see PluginHostProtocol.h's ponytail note

    consecutiveHeartbeatMisses = 0;
    onWorkerCrashDetected (liveCoordinator);
}

void PluginHostProxy::onWorkerCrashDetected (Coordinator* reportingCoordinator)
{
    const std::lock_guard<std::mutex> lock (callMutex);

    if (permanentlyFailed.load())
        return;

    if (reportingCoordinator != coordinator.get())
        return; // stale report from a worker we've already replaced

    if (hasRespawnedOnce)
    {
        // ponytail: exactly one respawn attempt, then give up for good - a
        // plugin that keeps crashing would otherwise burn CPU and flood the
        // log in an infinite restart loop. Upgrade path: exponential backoff
        // if real-world use shows transient crashes worth retrying harder for.
        permanentlyFailed.store (true);
        coordinator.reset();
        juce::Logger::writeToLog ("ScoreSmith: plugin host worker for \"" + pluginName
                                  + "\" crashed again after its one respawn attempt - giving up (gap 16).");
        return;
    }

    hasRespawnedOnce = true;
    editorIsOpen = false; // don't try to resurrect the editor mid-recovery - user can reopen it
    juce::Logger::writeToLog ("ScoreSmith: plugin host worker for \"" + pluginName + "\" crashed - respawning once.");

    // ponytail: this blocks the message thread for up to loadTimeoutSeconds,
    // same as the original load - it mirrors that already-accepted precedent
    // (PluginHostProxy::createAndLoad) rather than adding a background thread
    // for what should be a rare recovery path. Upgrade path: move this send off
    // the message thread if a crash-during-playback UI freeze proves to matter.
    auto fresh = std::make_unique<Coordinator>();

    if (! fresh->launch())
    {
        permanentlyFailed.store (true);
        juce::Logger::writeToLog ("ScoreSmith: respawn failed to launch a new worker for \"" + pluginName + "\".");
        return;
    }

    juce::MemoryBlock loadRequest;
    {
        juce::MemoryOutputStream out (loadRequest, true);
        auto xml = cachedDescription.createXml();
        ss::hostproto::writeLoadRequest (out, xml != nullptr ? xml->toString() : juce::String(),
                                         currentSampleRate, currentBlockSize, lastKnownState);
    }

    juce::MemoryBlock loadReply;
    const auto loadState = fresh->sendAndWait (loadRequest, loadTimeoutSeconds, loadReply);

    if (loadState != Coordinator::WaitState::gotReply)
    {
        permanentlyFailed.store (true);
        juce::Logger::writeToLog ("ScoreSmith: respawned worker for \"" + pluginName + "\" did not answer load.");
        return;
    }

    juce::MemoryInputStream loadIn (loadReply, false);
    const auto result = ss::hostproto::readLoadResponse (loadIn);

    if (! result.ok)
    {
        permanentlyFailed.store (true);
        juce::Logger::writeToLog ("ScoreSmith: respawned worker for \"" + pluginName + "\" failed to reload - "
                                  + result.errorMessage);
        return;
    }

    juce::MemoryBlock prepRequest;
    { juce::MemoryOutputStream out (prepRequest, true);
      ss::hostproto::writePrepareRequest (out, currentSampleRate, currentBlockSize); }
    juce::MemoryBlock prepReply;
    fresh->sendAndWait (prepRequest, controlCallTimeoutSeconds, prepReply); // best-effort

    fresh->setOwner (this, aliveFlag);
    coordinator = std::move (fresh);
    juce::Logger::writeToLog ("ScoreSmith: respawned worker for \"" + pluginName + "\" is back online.");
}

}
