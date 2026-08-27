#include "Plugins/PluginManager.h"
#include "Plugins/PluginHostProxy.h"
#include "Plugins/BasicSynth.h"

#include <juce_events/juce_events.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace ss
{

namespace
{
    /*  The token the parent puts on the child's command line.  Anything else and
        runScannerProcessIfRequested() lets the app start normally.              */
    const char* const scannerUID    = "scoresmith-plugin-scan";
    const char* const pluginListKey = "pluginList";

    /*  CustomScanner::shouldExit() only works inside a ThreadPoolJob and we scan
        on a plain Thread, so abort is signalled through this instead.           */
    std::atomic<bool> scanAborting { false };

    /** How long one plugin gets before we assume it hung the worker. */
    constexpr double scanTimeoutMs = 30000.0;

    /** Records the file being scanned so that a crash blacklists it next launch. */
    juce::File deadMansPedalFileFor (Settings& settings)
    {
        auto folder = settings.getCacheFolder();
        folder.createDirectory();
        return folder.getChildFile ("PluginScanCrash.txt");
    }

    //==============================================================================
    /*  WORKER SIDE.  This is the process that actually opens the plugin, so this
        is the process that dies when a plugin misbehaves.  Modelled on JUCE's
        AudioPluginHost PluginScannerSubprocess.                                 */
    class ScannerWorker final : public juce::ChildProcessWorker,
                                private juce::AsyncUpdater
    {
    public:
        ScannerWorker() { formats.addDefaultFormats(); }
        ~ScannerWorker() override { cancelPendingUpdate(); }

        using juce::ChildProcessWorker::initialiseFromCommandLine;

    private:
        // Arrives on the pipe's thread; plugin loading has to happen on the
        // message thread, so the request is queued and bounced across.
        void handleMessageFromCoordinator (const juce::MemoryBlock& request) override
        {
            if (request.isEmpty())
                return;

            {
                const std::lock_guard<std::mutex> lock (mutex);
                pending.push (request);
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
                    const std::lock_guard<std::mutex> lock (mutex);

                    if (pending.empty())
                        return;

                    request = pending.front();
                    pending.pop();
                }

                scan (request);
            }
        }

        void scan (const juce::MemoryBlock& request)
        {
            juce::MemoryInputStream stream (request, false);
            const auto formatName = stream.readString();
            const auto fileOrIdentifier = stream.readString();

            juce::OwnedArray<juce::PluginDescription> found;

            for (int i = 0; i < formats.getNumFormats(); ++i)
                if (auto* format = formats.getFormat (i))
                    if (format->getName() == formatName)
                        format->findAllTypesForFile (found, fileOrIdentifier);

            juce::XmlElement result ("SCANRESULT");

            for (auto* description : found)
                result.addChildElement (description->createXml().release());

            const auto text = result.toString();
            sendMessageToCoordinator ({ text.toRawUTF8(), text.getNumBytesAsUTF8() });
        }

        juce::AudioPluginFormatManager formats;
        std::mutex mutex;
        std::queue<juce::MemoryBlock> pending;
    };

    //==============================================================================
    /*  PARENT SIDE.  Owns the worker process and blocks the scan thread (never the
        message thread) until it answers or dies.                                 */
    class ScannerSuperprocess final : private juce::ChildProcessCoordinator
    {
    public:
        ScannerSuperprocess()
        {
            launchWorkerProcess (juce::File::getSpecialLocation (juce::File::currentExecutableFile),
                                 scannerUID, 0, 0);
        }

        enum class State { timeout, gotResult, connectionLost };

        struct Response
        {
            State state;
            std::unique_ptr<juce::XmlElement> xml;
        };

        /** Waits briefly for the worker.  `timeout` means "still working" - the
            caller loops so that an abort request can still get through. */
        Response getResponse()
        {
            std::unique_lock<std::mutex> lock (mutex);

            if (! condition.wait_for (lock, std::chrono::milliseconds (50),
                                      [this] { return gotResult || connectionLost; }))
                return { State::timeout, nullptr };

            const auto state = connectionLost ? State::connectionLost : State::gotResult;
            gotResult = false;
            connectionLost = false;
            return { state, std::move (pendingResult) };
        }

        using juce::ChildProcessCoordinator::sendMessageToWorker;

    private:
        void handleMessageFromWorker (const juce::MemoryBlock& reply) override
        {
            const std::lock_guard<std::mutex> lock (mutex);
            pendingResult = juce::parseXML (reply.toString());
            gotResult = true;
            condition.notify_one();
        }

        void handleConnectionLost() override
        {
            const std::lock_guard<std::mutex> lock (mutex);
            connectionLost = true;
            condition.notify_one();
        }

        std::mutex mutex;
        std::condition_variable condition;
        std::unique_ptr<juce::XmlElement> pendingResult;
        bool gotResult = false, connectionLost = false;
    };

    /*  Plugs the out-of-process scan into KnownPluginList.  Returning false makes
        PluginDirectoryScanner record the file as failed, which is how a plugin
        that crashed or hung the worker ends up blacklisted.                     */
    class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner
    {
    public:
        bool findPluginTypesFor (juce::AudioPluginFormat& format,
                                 juce::OwnedArray<juce::PluginDescription>& result,
                                 const juce::String& fileOrIdentifier) override
        {
            if (addDescriptions (format.getName(), fileOrIdentifier, result))
                return true;

            // The worker took the plugin with it - start a fresh one for the next file.
            superprocess = nullptr;
            return false;
        }

        void scanFinished() override { superprocess = nullptr; }

    private:
        bool addDescriptions (const juce::String& formatName, const juce::String& fileOrIdentifier,
                              juce::OwnedArray<juce::PluginDescription>& result)
        {
            if (superprocess == nullptr)
                superprocess = std::make_unique<ScannerSuperprocess>();

            juce::MemoryBlock request;

            {
                juce::MemoryOutputStream stream (request, true);
                stream.writeString (formatName);
                stream.writeString (fileOrIdentifier);
            }

            if (! superprocess->sendMessageToWorker (request))
                return false;

            const auto deadline = juce::Time::getMillisecondCounterHiRes() + scanTimeoutMs;

            for (;;)
            {
                if (shouldExit() || scanAborting.load())
                    return true;

                // Hung worker: report failure so this plugin is blacklisted and
                // the caller kills the process before moving to the next file.
                if (juce::Time::getMillisecondCounterHiRes() > deadline)
                    return false;

                auto response = superprocess->getResponse();

                if (response.state == ScannerSuperprocess::State::timeout)
                    continue;

                if (response.xml != nullptr)
                {
                    for (auto* item : response.xml->getChildIterator())
                    {
                        auto description = std::make_unique<juce::PluginDescription>();

                        if (description->loadFromXml (*item))
                            result.add (std::move (description));
                    }
                }

                return response.state == ScannerSuperprocess::State::gotResult;
            }
        }

        std::unique_ptr<ScannerSuperprocess> superprocess;
    };
}

//==============================================================================
/*  Background scan: one thread walks the directories, one timer keeps the UI
    informed and saves the list when the walk is done.                          */
struct PluginManager::Scanner final : private juce::Thread,
                                      private juce::Timer
{
    Scanner (PluginManager& o, bool rescanAll)
        : juce::Thread ("ScoreSmith plugin scan"), owner (o), rescanEverything (rescanAll)
    {
        scanAborting.store (false);
        startTimer (200);
        startThread();
    }

    ~Scanner() override
    {
        stopTimer();
        scanAborting.store (true);
        signalThreadShouldExit();
        stopThread (10000);
    }

    bool isRunning() const noexcept  { return isThreadRunning(); }
    float getProgress() const noexcept { return progress.load(); }

    juce::String getCurrentName() const
    {
        const juce::ScopedLock sl (nameLock);
        return currentName;
    }

private:
    void run() override
    {
        const auto deadMansPedal = deadMansPedalFileFor (owner.settings);
        const auto numFormats = juce::jmax (1, owner.formats.getNumFormats());

        for (int i = 0; i < owner.formats.getNumFormats() && ! threadShouldExit(); ++i)
        {
            auto* format = owner.formats.getFormat (i);

            if (format == nullptr || ! format->canScanForPlugins())
                continue;

            juce::PluginDirectoryScanner directoryScanner (owner.knownPlugins, *format,
                                                           owner.getScanPaths (format->getName()),
                                                           true, deadMansPedal, false);
            juce::String name;

            while (! threadShouldExit())
            {
                const bool more = directoryScanner.scanNextFile (! rescanEverything, name);

                {
                    const juce::ScopedLock sl (nameLock);
                    currentName = name;
                }

                progress.store ((float) (((double) i + directoryScanner.getProgress())
                                            / (double) numFormats));

                if (! more)
                    break;
            }

            // Anything that crashed, hung or refused to load gets isolated (spec 10.3).
            for (const auto& failed : directoryScanner.getFailedFiles())
                owner.knownPlugins.addToBlacklist (failed);
        }

        progress.store (1.0f);
    }

    void timerCallback() override
    {
        owner.sendChangeMessage();

        if (! isThreadRunning())
        {
            stopTimer();
            owner.saveList();
            owner.sendChangeMessage();
        }
    }

    PluginManager& owner;
    const bool rescanEverything;
    std::atomic<float> progress { 0.0f };
    juce::CriticalSection nameLock;
    juce::String currentName;
};

//==============================================================================
PluginManager::PluginManager (Settings& s) : settings (s)
{
   #if JUCE_PLUGINHOST_VST3
    formats.addFormat (new juce::VST3PluginFormat());
   #endif

   #if JUCE_PLUGINHOST_AU && JUCE_MAC
    formats.addFormat (new juce::AudioUnitPluginFormat());
   #endif

    if (settings.getSandboxPlugins())
        knownPlugins.setCustomScanner (std::make_unique<OutOfProcessScanner>());

    loadList();
}

PluginManager::~PluginManager()
{
    scanner.reset();
    saveList();
}

void PluginManager::startScan (bool rescanEverything)
{
    scanner.reset();

    if (rescanEverything)
    {
        knownPlugins.clear();
        knownPlugins.clearBlacklistedFiles();
    }

    scanner = std::make_unique<Scanner> (*this, rescanEverything);
    sendChangeMessage();
}

void PluginManager::abortScan()
{
    scanner.reset();
    saveList();
    sendChangeMessage();
}

bool PluginManager::isScanning() const noexcept
{
    return scanner != nullptr && scanner->isRunning();
}

float PluginManager::getScanProgress() const noexcept
{
    return scanner != nullptr ? scanner->getProgress() : 1.0f;
}

juce::String PluginManager::getCurrentlyScannedPlugin() const
{
    return scanner != nullptr ? scanner->getCurrentName() : juce::String();
}

juce::FileSearchPath PluginManager::getScanPaths (const juce::String& formatName) const
{
    juce::FileSearchPath path;

    for (const auto& entry : settings.getPluginScanPaths())
    {
        const juce::File folder (entry);

        if (folder.isDirectory())
            path.addIfNotAlreadyThere (folder);
    }

    if (path.getNumPaths() > 0)
        return path;

    // Nothing configured: fall back to the platform's standard folders, which is
    // exactly what AudioPluginFormat::getDefaultLocationsToSearch() knows.
    for (int i = 0; i < formats.getNumFormats(); ++i)
        if (auto* format = formats.getFormat (i))
            if (format->getName() == formatName)
                return format->getDefaultLocationsToSearch();

    return path;
}

juce::StringArray PluginManager::getBlacklist() const
{
    return knownPlugins.getBlacklistedFiles();
}

void PluginManager::setBlacklisted (const juce::String& identifier, bool shouldBeBlacklisted)
{
    // The list keys on fileOrIdentifier, but callers naturally reach for the
    // PluginDescription identifier string - accept either.
    auto key = identifier;

    if (auto description = knownPlugins.getTypeForIdentifierString (identifier))
        key = description->fileOrIdentifier;

    if (shouldBeBlacklisted)
        knownPlugins.addToBlacklist (key);
    else
        knownPlugins.removeFromBlacklist (key);

    saveList();
    sendChangeMessage();
}

std::unique_ptr<juce::AudioPluginInstance> PluginManager::createInstance (const juce::String& identifier,
                                                                          double sampleRate, int blockSize,
                                                                          juce::String& errorOut)
{
    errorOut.clear();

    // ScoreSmith's own always-available instrument (MixerView.cpp's "Add
    // instrument" menu) - never scanned, sandboxed or looked up in
    // knownPlugins, since it isn't a real installed plugin.
    if (identifier == BasicSynth::identifier)
        return std::make_unique<BasicSynth>();

    const auto* description = findDescription (identifier);

    if (description == nullptr)
    {
        errorOut = "Plugin not installed: " + identifier;
        return nullptr;
    }

    if (knownPlugins.getBlacklistedFiles().contains (description->fileOrIdentifier))
    {
        errorOut = description->name + " is blacklisted";
        return nullptr;
    }

    // Gap 16 (spec 10.3/15.6): runtime hosting is out-of-process too, gated by
    // the same "Scan and run plugins in a separate process" setting the
    // Preferences checkbox already promises - a worker process per plugin
    // instance, so a crash while playing takes that process, not the DAW.
    if (settings.getSandboxPlugins())
    {
        // Loads with no state: Mixer.cpp's ChannelStrip::Impl::rebuildFrom()
        // always calls setStateInformation() as a separate step right after
        // createInstance() returns, exactly as it does for the in-process path -
        // PluginHostProxy::setStateInformation() applies it over the wire.
        auto proxy = PluginHostProxy::createAndLoad (*description, sampleRate, blockSize,
                                                     juce::MemoryBlock(), errorOut);

        if (proxy == nullptr && errorOut.isEmpty())
            errorOut = "Could not load " + description->name;

        return proxy;
    }

    try
    {
        auto instance = formats.createPluginInstance (*description, sampleRate, blockSize, errorOut);

        if (instance == nullptr && errorOut.isEmpty())
            errorOut = "Could not load " + description->name;

        return instance;
    }
    catch (const std::exception& e)
    {
        errorOut = "Plugin threw while loading: " + juce::String (e.what());
    }
    catch (...)
    {
        errorOut = "Plugin threw while loading: " + description->name;
    }

    return nullptr;
}

const juce::PluginDescription* PluginManager::findDescription (const juce::String& identifier) const
{
    const juce::ScopedLock sl (descriptionLock);

    for (auto* cached : descriptionCache)
        if (cached->createIdentifierString() == identifier)
            return cached;

    if (auto found = knownPlugins.getTypeForIdentifierString (identifier))
        return descriptionCache.add (found.release());

    return nullptr;
}

bool PluginManager::runScannerProcessIfRequested (const juce::String& commandLine)
{
    if (! commandLine.contains (scannerUID))
        return false;

    /*  Kept alive for the life of the process: after this returns true the caller
        must skip all UI setup and just run the message loop.  The worker quits
        the app itself when the coordinator goes away.                           */
    static std::unique_ptr<ScannerWorker> worker;
    worker = std::make_unique<ScannerWorker>();

    if (worker->initialiseFromCommandLine (commandLine, scannerUID))
        return true;

    worker.reset();
    return false;
}

void PluginManager::loadList()
{
    // KnownPluginList's XML carries the blacklist too, so this is the whole store.
    if (auto xml = settings.raw().getXmlValue (pluginListKey))
        knownPlugins.recreateFromXml (*xml);
}

void PluginManager::saveList()
{
    if (auto xml = knownPlugins.createXml())
        settings.raw().setValue (pluginListKey, xml.get());

    settings.flush();
}

}
