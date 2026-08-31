#pragma once
#include <juce_core/juce_core.h>

namespace ss
{
    enum class ExtensionDirection { importOnly, exportOnly, both };

    /** One discovered format extension: a folder containing manifest.json
        plus the executable it names. See docs/superpowers/specs/
        2026-08-28-format-extension-api-design.md for the manifest schema
        and the CLI contract FormatExtensionRunner.h invokes it with. */
    struct FormatExtension
    {
        juce::String id, name, version, fileExtension;
        ExtensionDirection direction = ExtensionDirection::both;
        juce::File executable;   // resolved absolute path
        juce::File folder;       // the extension's own folder
    };

    /** Parses `extensionFolder`/manifest.json. Returns false (with
        warningOut set to a human-readable reason) for anything malformed:
        missing manifest.json, invalid JSON, a missing required field, or
        an `executable` that doesn't resolve to an existing file inside
        `extensionFolder`. Never throws. */
    bool parseFormatExtensionManifest (const juce::File& extensionFolder,
                                        FormatExtension& out, juce::String& warningOut);
}
