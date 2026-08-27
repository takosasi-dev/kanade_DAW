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

        // project defaults
        double getDefaultBpm() const;                  void setDefaultBpm (double);
        double getDefaultSampleRate() const;           void setDefaultSampleRate (double);
        int    getDefaultBitDepth() const;             void setDefaultBitDepth (int);

        // plugins (spec 9.6, 8.4.2)
        juce::StringArray getPluginScanPaths() const;  void setPluginScanPaths (const juce::StringArray&);
        bool  getSandboxPlugins() const;               void setSandboxPlugins (bool);
        bool  getScanPluginsOnStartup() const;         void setScanPluginsOnStartup (bool);

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
