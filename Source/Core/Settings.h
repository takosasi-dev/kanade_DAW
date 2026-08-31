#pragma once
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_devices/juce_audio_devices.h>

namespace ss
{
    /** Persistent preferences (spec 9.6).  Thin typed wrapper over
        juce::PropertiesFile - no schema, no migration, no config classes. */
    class Settings
    {
    public:
        Settings();
        ~Settings();

        juce::PropertiesFile& raw() noexcept { return *props.getUserSettings(); }
        void flush();

        // general / appearance
        juce::String getLanguage() const;              void setLanguage (const juce::String&);
        juce::String getTheme() const;                 void setTheme (const juce::String&);   // dark|light|system
        float        getUiScale() const;               void setUiScale (float);
        int          getAutoSaveIntervalSeconds() const;  void setAutoSaveIntervalSeconds (int);
        int          getBackupGenerations() const;     void setBackupGenerations (int);
        juce::String getStartupBehaviour() const;      void setStartupBehaviour (const juce::String&); // new|last|picker

        /** Named dock layout to apply on launch, or empty to restore whatever
            the workspace looked like when the app last closed (the existing
            default). Sits alongside the existing named-layout store
            (getDockLayout/setDockLayout) - this just says which saved name,
            if any, wins at startup. */
        juce::String getStartupDockLayoutName() const; void setStartupDockLayoutName (const juce::String&);

        /** Last app version the user has seen the "What's new" dialog for.
            Empty means never shown (including a fresh install - the dialog
            is skipped on first run so it isn't mistaken for a tutorial). */
        juce::String getLastSeenVersion() const;       void setLastSeenVersion (const juce::String&);

        /** Show each channel strip's pan knob value as text (e.g. "L20") in
            addition to the knob itself. Off by default - the knob-only look
            is the existing default and shouldn't change under existing users. */
        bool getShowPanValueLabel() const;             void setShowPanValueLabel (bool);

        /** How often the timeline (playhead, waveforms) and the mixer's level
            meters repaint themselves, in Hz. Defaults match the previously
            hardcoded values (24/30) so existing behaviour is unchanged until
            a user actually lowers them for a machine that needs it. */
        int getTimelineRefreshHz() const;              void setTimelineRefreshHz (int);
        int getMixerMeterRefreshHz() const;            void setMixerMeterRefreshHz (int);

        /** Undo history cap, in edit steps (not juce::UndoManager's internal
            "size units" - see the call site that converts between the two). */
        int getUndoHistoryLimit() const;               void setUndoHistoryLimit (int);

        // project defaults
        double getDefaultBpm() const;                  void setDefaultBpm (double);
        double getDefaultSampleRate() const;           void setDefaultSampleRate (double);
        int    getDefaultBitDepth() const;             void setDefaultBitDepth (int);

        // plugins (spec 9.6, 8.4.2)
        juce::StringArray getPluginScanPaths() const;  void setPluginScanPaths (const juce::StringArray&);

        /** Folders scanned for format extensions (Source/Extensions/) - same
            multi-folder shape as getPluginScanPaths(). */
        juce::StringArray getExtensionScanPaths() const;  void setExtensionScanPaths (const juce::StringArray&);
        bool  getSandboxPlugins() const;               void setSandboxPlugins (bool);
        bool  getScanPluginsOnStartup() const;         void setScanPluginsOnStartup (bool);

        /** Plugin identifiers (PluginDescription::createIdentifierString()) pinned
            to the top of the "Add plugin" menu, most-recently-pinned first. */
        juce::StringArray getPinnedPlugins() const;    void setPinnedPlugins (const juce::StringArray&);

        // paths
        juce::File getProjectsFolder() const;          void setProjectsFolder (const juce::File&);
        juce::File getCacheFolder() const;             void setCacheFolder (const juce::File&);
        juce::File getBackupFolder() const;            void setBackupFolder (const juce::File&);
        juce::StringArray getSampleLibraryFolders() const;
        void setSampleLibraryFolders (const juce::StringArray&);

        // AI (spec 8.2 - local inference only, v0.6)
        juce::File getStemSeparatorExecutable() const; void setStemSeparatorExecutable (const juce::File&);
        juce::File getTranscriptionModelFolder() const; void setTranscriptionModelFolder (const juce::File&);

        // UTAU (Phase 1 - docs/superpowers/specs/2026-08-26-utau-integration-phase1-design.md)
        juce::StringArray getUtauVoicebankFolders() const;   void setUtauVoicebankFolders (const juce::StringArray&);
        juce::File getUtauResamplerExecutable() const;       void setUtauResamplerExecutable (const juce::File&);

        // Dock layouts (docs/superpowers/specs/2026-08-26-docking-layout-system-design.md)
        juce::StringArray getDockLayoutNames() const;
        juce::var getDockLayout (const juce::String& name) const;
        void setDockLayout (const juce::String& name, const juce::var& state);
        void deleteDockLayout (const juce::String& name);

        /** Audio device state, restored on launch. */
        std::unique_ptr<juce::XmlElement> getAudioDeviceState() const;
        void setAudioDeviceState (const juce::XmlElement*);

        /** Keyboard map preset name: "scoresmith" | "ableton" | "cubase" | "studioone". */
        juce::String getKeymapPreset() const;          void setKeymapPreset (const juce::String&);
        std::unique_ptr<juce::XmlElement> getKeyMappings() const;
        void setKeyMappings (const juce::XmlElement*);

    private:
        // getUserSettings() lazily creates the backing file, so it is not
        // const - which every const getter below needs it to be.
        mutable juce::ApplicationProperties props;
    };
}
