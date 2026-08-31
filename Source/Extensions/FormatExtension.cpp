#include "Extensions/FormatExtension.h"

namespace ss
{
    namespace
    {
        ExtensionDirection parseDirection (const juce::String& s)
        {
            if (s == "import") return ExtensionDirection::importOnly;
            if (s == "export") return ExtensionDirection::exportOnly;
            return ExtensionDirection::both;
        }

        bool parseAdditionalInputs (const juce::var& value, std::vector<AdditionalInput>& out,
                                    juce::String& warningOut)
        {
            const auto* array = value.getArray();
            if (array == nullptr)
                return true;   // absent/not-an-array: treated as empty, not an error

            for (const auto& entryVar : *array)
            {
                auto* entry = entryVar.getDynamicObject();
                if (entry == nullptr)
                {
                    warningOut = "additionalInputs entry is not a JSON object.";
                    return false;
                }

                const auto kindStr = entry->getProperty ("kind").toString();
                const auto envVar  = entry->getProperty ("envVar").toString();

                if (envVar.isEmpty())
                {
                    warningOut = "additionalInputs entry is missing envVar.";
                    return false;
                }

                AdditionalInput input;
                input.envVar = envVar;

                if (kindStr == "userFile")
                {
                    input.kind = AdditionalInputKind::userFile;
                    input.prompt = entry->getProperty ("prompt").toString();
                    input.fileFilter = entry->getProperty ("fileFilter").toString();
                }
                else if (kindStr == "mixdownRender")
                {
                    input.kind = AdditionalInputKind::mixdownRender;
                }
                else
                {
                    warningOut = "additionalInputs entry has unknown kind \"" + kindStr + "\".";
                    return false;
                }

                out.push_back (input);
            }

            return true;
        }

        bool parseSettings (const juce::var& value, std::vector<ExtensionSetting>& out,
                            juce::String& warningOut)
        {
            const auto* array = value.getArray();
            if (array == nullptr)
                return true;   // absent/not-an-array: treated as empty, not an error

            juce::StringArray seenIds;

            for (const auto& entryVar : *array)
            {
                auto* entry = entryVar.getDynamicObject();
                if (entry == nullptr)
                {
                    warningOut = "settings entry is not a JSON object.";
                    return false;
                }

                const auto id      = entry->getProperty ("id").toString();
                const auto label   = entry->getProperty ("label").toString();
                const auto typeStr = entry->getProperty ("type").toString();
                const auto envVar  = entry->getProperty ("envVar").toString();

                if (id.isEmpty() || label.isEmpty() || envVar.isEmpty())
                {
                    warningOut = "settings entry is missing id/label/envVar.";
                    return false;
                }

                if (seenIds.contains (id))
                {
                    warningOut = "settings entry has a duplicate id \"" + id + "\".";
                    return false;
                }
                seenIds.add (id);

                ExtensionSetting setting;
                setting.id = id;
                setting.label = label;
                setting.envVar = envVar;

                if (typeStr == "slider")
                {
                    setting.type = ExtensionSettingType::slider;
                    if (! entry->hasProperty ("min") || ! entry->hasProperty ("max")
                         || ! entry->hasProperty ("default"))
                    {
                        warningOut = "settings entry \"" + id + "\" (slider) is missing min/max/default.";
                        return false;
                    }
                    setting.sliderMin = (double) entry->getProperty ("min");
                    setting.sliderMax = (double) entry->getProperty ("max");
                    setting.sliderDefault = (double) entry->getProperty ("default");
                }
                else if (typeStr == "checkbox")
                {
                    setting.type = ExtensionSettingType::checkbox;
                    if (! entry->hasProperty ("default"))
                    {
                        warningOut = "settings entry \"" + id + "\" (checkbox) is missing default.";
                        return false;
                    }
                    setting.checkboxDefault = (bool) entry->getProperty ("default");
                }
                else if (typeStr == "dropdown")
                {
                    setting.type = ExtensionSettingType::dropdown;
                    const auto* options = entry->getProperty ("options").getArray();
                    if (options == nullptr || options->isEmpty())
                    {
                        warningOut = "settings entry \"" + id + "\" (dropdown) is missing a non-empty options array.";
                        return false;
                    }
                    for (const auto& option : *options)
                        setting.dropdownOptions.add (option.toString());

                    setting.dropdownDefault = entry->getProperty ("default").toString();
                    if (! setting.dropdownOptions.contains (setting.dropdownDefault))
                    {
                        warningOut = "settings entry \"" + id + "\" (dropdown) default is not one of its own options.";
                        return false;
                    }
                }
                else
                {
                    warningOut = "settings entry \"" + id + "\" has unknown type \"" + typeStr + "\".";
                    return false;
                }

                out.push_back (setting);
            }

            return true;
        }
    }

    bool parseFormatExtensionManifest (const juce::File& extensionFolder,
                                        FormatExtension& out, juce::String& warningOut)
    {
        const auto manifestFile = extensionFolder.getChildFile ("manifest.json");
        if (! manifestFile.existsAsFile())
        {
            warningOut = "No manifest.json in \"" + extensionFolder.getFullPathName() + "\".";
            return false;
        }

        const auto parsed = juce::JSON::parse (manifestFile);
        auto* obj = parsed.getDynamicObject();
        if (obj == nullptr)
        {
            warningOut = "manifest.json in \"" + extensionFolder.getFullPathName()
                          + "\" is not valid JSON.";
            return false;
        }

        const auto id             = obj->getProperty ("id").toString();
        const auto name           = obj->getProperty ("name").toString();
        const auto fileExtension  = obj->getProperty ("fileExtension").toString();
        const auto executableName = obj->getProperty ("executable").toString();

        if (id.isEmpty() || name.isEmpty() || fileExtension.isEmpty() || executableName.isEmpty())
        {
            warningOut = "manifest.json in \"" + extensionFolder.getFullPathName()
                          + "\" is missing a required field (id/name/fileExtension/executable).";
            return false;
        }

        const auto executable = extensionFolder.getChildFile (executableName);
        if (! executable.isAChildOf (extensionFolder) || ! executable.existsAsFile())
        {
            warningOut = "manifest.json in \"" + extensionFolder.getFullPathName()
                          + "\" names an executable outside its own folder or one that does not exist: \""
                          + executable.getFullPathName() + "\".";
            return false;
        }

        std::vector<AdditionalInput> additionalInputs;
        if (! parseAdditionalInputs (obj->getProperty ("additionalInputs"), additionalInputs, warningOut))
        {
            warningOut = "manifest.json in \"" + extensionFolder.getFullPathName()
                          + "\" has an invalid additionalInputs entry: " + warningOut;
            return false;
        }

        std::vector<ExtensionSetting> settings;
        if (! parseSettings (obj->getProperty ("settings"), settings, warningOut))
        {
            warningOut = "manifest.json in \"" + extensionFolder.getFullPathName()
                          + "\" has an invalid settings entry: " + warningOut;
            return false;
        }

        // One envVar = one value in the map handed to the extension, so two
        // declarations sharing a name means one silently overwrites the
        // other - and if the loser was a mixdownRender path, its temp file
        // leaks (and something non-path gets treated as a file). Checked
        // across both lists at once, since a collision is equally broken
        // whichever pair of entries causes it.
        {
            juce::StringArray allEnvVars;
            for (const auto& input : additionalInputs)
                allEnvVars.add (input.envVar);
            for (const auto& setting : settings)
                allEnvVars.add (setting.envVar);

            juce::StringArray seenEnvVars;
            for (const auto& envVar : allEnvVars)
            {
                // ignoreCase: Windows environment variable names are
                // case-insensitive, so MUX_A and mux_a really would clobber
                // each other once set.
                if (seenEnvVars.contains (envVar, true))
                {
                    warningOut = "manifest.json in \"" + extensionFolder.getFullPathName()
                                  + "\" declares the envVar \"" + envVar
                                  + "\" more than once across additionalInputs/settings.";
                    return false;
                }
                seenEnvVars.add (envVar);
            }
        }

        const bool customUI = (bool) obj->getProperty ("customUI");
        if (customUI && ! settings.empty())
        {
            warningOut = "manifest.json in \"" + extensionFolder.getFullPathName()
                          + "\" sets customUI: true and also declares settings - these are mutually exclusive.";
            return false;
        }

        out.id = id;
        out.name = name;
        out.version = obj->getProperty ("version").toString();
        out.fileExtension = fileExtension;
        out.direction = parseDirection (obj->getProperty ("direction").toString());
        out.executable = executable;
        out.folder = extensionFolder;
        out.additionalInputs = additionalInputs;
        out.customUI = customUI;
        out.settings = settings;
        return true;
    }
}
