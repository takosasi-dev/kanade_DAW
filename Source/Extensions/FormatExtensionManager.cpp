#include "Extensions/FormatExtensionManager.h"

namespace ss
{
    void FormatExtensionManager::rescan (const juce::StringArray& scanPaths, juce::StringArray& warningsOut)
    {
        extensions.clear();

        for (const auto& path : scanPaths)
        {
            const juce::File root (path);
            if (! root.isDirectory())
                continue;

            for (const auto& entry : juce::RangedDirectoryIterator (root, false, "*",
                                                                     juce::File::findDirectories))
            {
                const auto folder = entry.getFile();
                if (! folder.getChildFile ("manifest.json").existsAsFile())
                    continue;   // not every subfolder here has to be an extension

                FormatExtension ext;
                juce::String warning;
                if (parseFormatExtensionManifest (folder, ext, warning))
                    extensions.push_back (std::move (ext));
                else
                    warningsOut.add (warning);
            }
        }
    }

    std::vector<const FormatExtension*> FormatExtensionManager::matching (ExtensionDirection wanted) const
    {
        std::vector<const FormatExtension*> result;
        for (const auto& ext : extensions)
        {
            const bool matches = wanted == ExtensionDirection::both
                                    || ext.direction == ExtensionDirection::both
                                    || ext.direction == wanted;
            if (matches)
                result.push_back (&ext);
        }
        return result;
    }
}
