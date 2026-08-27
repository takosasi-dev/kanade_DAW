#pragma once
#include "Core/Settings.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace ss
{
    /** VST3/AU discovery, blacklisting and instantiation (spec 8.4.2, 10.3).

        Scanning runs in a CHILD PROCESS so that a plugin which crashes on load
        takes down the scanner, not the DAW - the spec calls this the single
        highest-priority stability requirement.                                */
    class PluginManager : public juce::ChangeBroadcaster
    {
    public:
        PluginManager (Settings&);
        ~PluginManager() override;

        juce::AudioPluginFormatManager& getFormatManager() noexcept { return formats; }
        juce::KnownPluginList& getKnownPluginList() noexcept { return knownPlugins; }

        /** Scans `getScanPaths()` in the background.  Safe to call repeatedly. */
        void startScan (bool rescanEverything);
        void abortScan();
        bool isScanning() const noexcept;
        float getScanProgress() const noexcept;
        juce::String getCurrentlyScannedPlugin() const;

        juce::FileSearchPath getScanPaths (const juce::String& formatName) const;

        /** Blacklist control (spec 9.6 "manual blacklist"). */
        juce::StringArray getBlacklist() const;
        void setBlacklisted (const juce::String& identifier, bool shouldBeBlacklisted);

        /** Instantiates a plugin.  Returns nullptr and fills `errorOut` on
            failure; never throws into the caller. */
        std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::String& identifier,
                                                                   double sampleRate, int blockSize,
                                                                   juce::String& errorOut);

        const juce::PluginDescription* findDescription (const juce::String& identifier) const;

        /** Entry point used when the app is relaunched as a scan worker.
            Returns true when this process is a worker, in which case the caller
            must skip all UI setup and simply run the message loop - the worker
            loads plugins on the message thread and quits the app itself once the
            coordinator disconnects.                                            */
        static bool runScannerProcessIfRequested (const juce::String& commandLine);

        /** Entry point used when the app is relaunched as a plugin-host worker
            (gap 16, spec 10.3/15.6): one child process per loaded plugin
            instance, so a crash while playing takes that process, not the DAW.
            Same contract as runScannerProcessIfRequested() - return true means
            skip UI setup and just run the message loop. Implemented in
            PluginHostWorker.cpp, not PluginManager.cpp. */
        static bool runPluginHostProcessIfRequested (const juce::String& commandLine);

    private:
        Settings& settings;
        juce::AudioPluginFormatManager formats;
        juce::KnownPluginList knownPlugins;

        struct Scanner;
        std::unique_ptr<Scanner> scanner;

        /*  findDescription() must return a pointer that stays valid, but
            KnownPluginList::getTypes() hands back a copy - so resolved lookups
            live here.  Append-only, which keeps every pointer we ever returned
            alive for the manager's lifetime.
            ponytail: grows with the number of distinct plugins looked up (a few
            hundred bytes each); make it an LRU only if that ever matters.      */
        mutable juce::OwnedArray<juce::PluginDescription> descriptionCache;
        mutable juce::CriticalSection descriptionLock;

        void loadList();
        void saveList();

        JUCE_DECLARE_NON_COPYABLE (PluginManager)
    };
}
