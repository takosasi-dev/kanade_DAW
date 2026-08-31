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

        out.id = id;
        out.name = name;
        out.version = obj->getProperty ("version").toString();
        out.fileExtension = fileExtension;
        out.direction = parseDirection (obj->getProperty ("direction").toString());
        out.executable = executable;
        out.folder = extensionFolder;
        return true;
    }
}
