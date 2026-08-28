#include "Core/Settings.h"

namespace ss
{
    Settings::Settings()
    {
        juce::PropertiesFile::Options options;
        options.applicationName     = "ScoreSmith";
        options.filenameSuffix      = "settings";
        options.folderName          = "ScoreSmith";
        options.osxLibrarySubFolder = "Application Support";
        options.storageFormat       = juce::PropertiesFile::storeAsXML;
        props.setStorageParameters (options);
    }

    Settings::~Settings()
    {
        flush();
    }

    void Settings::flush()
    {
        props.saveIfNeeded();
    }

    //==============================================================================
    juce::String Settings::getLanguage() const            { return props.getUserSettings()->getValue ("language", juce::SystemStats::getUserLanguage().startsWith ("ja") ? "ja" : "en"); }
    void Settings::setLanguage (const juce::String& v)    { props.getUserSettings()->setValue ("language", v); }

    juce::String Settings::getTheme() const               { return props.getUserSettings()->getValue ("theme", "dark"); }
    void Settings::setTheme (const juce::String& v)       { props.getUserSettings()->setValue ("theme", v); }

    float Settings::getUiScale() const                    { return (float) props.getUserSettings()->getDoubleValue ("uiScale", 1.0); }
    void Settings::setUiScale (float v)                   { props.getUserSettings()->setValue ("uiScale", (double) v); }

    int  Settings::getAutoSaveIntervalSeconds() const     { return props.getUserSettings()->getIntValue ("autoSaveSeconds", 300); }
    void Settings::setAutoSaveIntervalSeconds (int v)     { props.getUserSettings()->setValue ("autoSaveSeconds", v); }

    int  Settings::getBackupGenerations() const           { return props.getUserSettings()->getIntValue ("backupGenerations", 10); }
    void Settings::setBackupGenerations (int v)           { props.getUserSettings()->setValue ("backupGenerations", v); }

    juce::String Settings::getStartupBehaviour() const    { return props.getUserSettings()->getValue ("startup", "picker"); }
    void Settings::setStartupBehaviour (const juce::String& v) { props.getUserSettings()->setValue ("startup", v); }

    juce::String Settings::getStartupDockLayoutName() const { return props.getUserSettings()->getValue ("startupDockLayout", {}); }
    void Settings::setStartupDockLayoutName (const juce::String& v) { props.getUserSettings()->setValue ("startupDockLayout", v); }

    juce::String Settings::getLastSeenVersion() const      { return props.getUserSettings()->getValue ("lastSeenVersion", {}); }
    void Settings::setLastSeenVersion (const juce::String& v) { props.getUserSettings()->setValue ("lastSeenVersion", v); }

    double Settings::getDefaultBpm() const                { return props.getUserSettings()->getDoubleValue ("defaultBpm", 120.0); }
    void Settings::setDefaultBpm (double v)               { props.getUserSettings()->setValue ("defaultBpm", v); }

    double Settings::getDefaultSampleRate() const         { return props.getUserSettings()->getDoubleValue ("defaultSampleRate", 48000.0); }
    void Settings::setDefaultSampleRate (double v)        { props.getUserSettings()->setValue ("defaultSampleRate", v); }

    int  Settings::getDefaultBitDepth() const             { return props.getUserSettings()->getIntValue ("defaultBitDepth", 24); }
    void Settings::setDefaultBitDepth (int v)             { props.getUserSettings()->setValue ("defaultBitDepth", v); }

    bool Settings::getSandboxPlugins() const              { return props.getUserSettings()->getBoolValue ("sandboxPlugins", true); }
    void Settings::setSandboxPlugins (bool v)             { props.getUserSettings()->setValue ("sandboxPlugins", v); }

    bool Settings::getScanPluginsOnStartup() const        { return props.getUserSettings()->getBoolValue ("scanOnStartup", false); }
    void Settings::setScanPluginsOnStartup (bool v)       { props.getUserSettings()->setValue ("scanOnStartup", v); }

    juce::String Settings::getKeymapPreset() const        { return props.getUserSettings()->getValue ("keymapPreset", "scoresmith"); }
    void Settings::setKeymapPreset (const juce::String& v) { props.getUserSettings()->setValue ("keymapPreset", v); }

    //==============================================================================
    static juce::StringArray splitPaths (const juce::String& s)
    {
        juce::StringArray a;
        a.addTokens (s, "\n", {});
        a.removeEmptyStrings();
        return a;
    }

    juce::StringArray Settings::getPluginScanPaths() const
    {
        return splitPaths (props.getUserSettings()->getValue ("pluginScanPaths"));
    }

    void Settings::setPluginScanPaths (const juce::StringArray& v)
    {
        props.getUserSettings()->setValue ("pluginScanPaths", v.joinIntoString ("\n"));
    }

    juce::StringArray Settings::getSampleLibraryFolders() const
    {
        return splitPaths (props.getUserSettings()->getValue ("sampleFolders"));
    }

    void Settings::setSampleLibraryFolders (const juce::StringArray& v)
    {
        props.getUserSettings()->setValue ("sampleFolders", v.joinIntoString ("\n"));
    }

    //==============================================================================
    static juce::File folderOrDefault (const juce::PropertiesFile& p, juce::StringRef key,
                                       const juce::File& fallback)
    {
        const auto stored = p.getValue (key);
        return stored.isNotEmpty() ? juce::File (stored) : fallback;
    }

    juce::File Settings::getProjectsFolder() const
    {
        return folderOrDefault (*props.getUserSettings(), "projectsFolder",
                                juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                    .getChildFile ("ScoreSmith"));
    }

    void Settings::setProjectsFolder (const juce::File& f) { props.getUserSettings()->setValue ("projectsFolder", f.getFullPathName()); }

    juce::File Settings::getCacheFolder() const
    {
        return folderOrDefault (*props.getUserSettings(), "cacheFolder",
                                juce::File::getSpecialLocation (juce::File::tempDirectory)
                                    .getChildFile ("ScoreSmith"));
    }

    void Settings::setCacheFolder (const juce::File& f) { props.getUserSettings()->setValue ("cacheFolder", f.getFullPathName()); }

    juce::File Settings::getBackupFolder() const
    {
        return folderOrDefault (*props.getUserSettings(), "backupFolder",
                                getProjectsFolder().getChildFile ("Backups"));
    }

    void Settings::setBackupFolder (const juce::File& f) { props.getUserSettings()->setValue ("backupFolder", f.getFullPathName()); }

    juce::File Settings::getStemSeparatorExecutable() const
    {
        return juce::File (props.getUserSettings()->getValue ("stemSeparatorExe"));
    }

    void Settings::setStemSeparatorExecutable (const juce::File& f) { props.getUserSettings()->setValue ("stemSeparatorExe", f.getFullPathName()); }

    juce::File Settings::getTranscriptionModelFolder() const
    {
        return juce::File (props.getUserSettings()->getValue ("transcriptionModels"));
    }

    void Settings::setTranscriptionModelFolder (const juce::File& f) { props.getUserSettings()->setValue ("transcriptionModels", f.getFullPathName()); }

    juce::StringArray Settings::getUtauVoicebankFolders() const
    {
        return splitPaths (props.getUserSettings()->getValue ("utauVoicebankFolders"));
    }

    void Settings::setUtauVoicebankFolders (const juce::StringArray& v)
    {
        props.getUserSettings()->setValue ("utauVoicebankFolders", v.joinIntoString ("\n"));
    }

    juce::File Settings::getUtauResamplerExecutable() const
    {
        return juce::File (props.getUserSettings()->getValue ("utauResamplerExe"));
    }

    void Settings::setUtauResamplerExecutable (const juce::File& f)
    {
        props.getUserSettings()->setValue ("utauResamplerExe", f.getFullPathName());
    }

    //==============================================================================
    namespace
    {
        juce::String dockLayoutKeyFor (const juce::String& name) { return "dockLayout." + name; }
    }

    juce::StringArray Settings::getDockLayoutNames() const
    {
        juce::StringArray names;
        names.addLines (props.getUserSettings()->getValue ("dockLayoutNames"));
        names.removeEmptyStrings();
        return names;
    }

    juce::var Settings::getDockLayout (const juce::String& name) const
    {
        const auto json = props.getUserSettings()->getValue (dockLayoutKeyFor (name));

        if (json.isEmpty())
            return {};

        return juce::JSON::parse (json);
    }

    void Settings::setDockLayout (const juce::String& name, const juce::var& state)
    {
        props.getUserSettings()->setValue (dockLayoutKeyFor (name), juce::JSON::toString (state));

        auto names = getDockLayoutNames();
        names.addIfNotAlreadyThere (name);
        props.getUserSettings()->setValue ("dockLayoutNames", names.joinIntoString ("\n"));
    }

    void Settings::deleteDockLayout (const juce::String& name)
    {
        props.getUserSettings()->removeValue (dockLayoutKeyFor (name));

        auto names = getDockLayoutNames();
        names.removeString (name);
        props.getUserSettings()->setValue ("dockLayoutNames", names.joinIntoString ("\n"));
    }

    //==============================================================================
    std::unique_ptr<juce::XmlElement> Settings::getAudioDeviceState() const
    {
        return props.getUserSettings()->getXmlValue ("audioDeviceState");
    }

    void Settings::setAudioDeviceState (const juce::XmlElement* xml)
    {
        if (xml != nullptr)
            props.getUserSettings()->setValue ("audioDeviceState", xml);
        else
            props.getUserSettings()->removeValue ("audioDeviceState");
    }

    std::unique_ptr<juce::XmlElement> Settings::getKeyMappings() const
    {
        return props.getUserSettings()->getXmlValue ("keyMappings");
    }

    void Settings::setKeyMappings (const juce::XmlElement* xml)
    {
        if (xml != nullptr)
            props.getUserSettings()->setValue ("keyMappings", xml);
        else
            props.getUserSettings()->removeValue ("keyMappings");
    }
}
