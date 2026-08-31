#pragma once
#include <juce_core/juce_core.h>
#include <vector>

namespace ss
{
    enum class ExtensionDirection { importOnly, exportOnly, both };

    enum class AdditionalInputKind { userFile, mixdownRender };

    /** One extra input a format extension's manifest can declare beyond the
        base --export/--import <dawproject> <output> contract - resolved by
        KANADE DAW before the extension is invoked and handed over as an
        environment variable (see FormatExtensionRunner.h). `userFile` shows
        the user a file chooser; `mixdownRender` renders the current
        project's own mixdown with no user prompt. `prompt`/`fileFilter`
        only apply to `userFile`. */
    struct AdditionalInput
    {
        AdditionalInputKind kind = AdditionalInputKind::userFile;
        juce::String envVar;
        juce::String prompt, fileFilter;
    };

    enum class ExtensionSettingType { slider, checkbox, dropdown };

    /** One pre-run setting a format extension's manifest can declare -
        KANADE DAW shows a dialog built from these immediately before
        invoking the extension (see ExtensionSettingsDialog.h), and passes
        the user's chosen value in as `envVar`, the same environment-variable
        channel `additionalInputs` uses. Only the fields for `type` are
        meaningful; the others are left at their default. See
        docs/superpowers/specs/2026-08-31-extension-settings-ui-design.md. */
    struct ExtensionSetting
    {
        juce::String id, label, envVar;
        ExtensionSettingType type = ExtensionSettingType::slider;

        // slider only
        double sliderMin = 0.0, sliderMax = 1.0, sliderDefault = 0.0;
        // checkbox only
        bool checkboxDefault = false;
        // dropdown only
        juce::StringArray dropdownOptions;
        juce::String dropdownDefault;
    };

    /** One discovered format extension: a folder containing manifest.json
        plus the executable it names. See docs/superpowers/specs/
        2026-08-28-format-extension-api-design.md for the manifest schema
        and the CLI contract FormatExtensionRunner.h invokes it with,
        docs/superpowers/specs/2026-08-31-video-audio-mux-design.md for
        additionalInputs, and docs/superpowers/specs/
        2026-08-31-extension-settings-ui-design.md for settings/customUI. */
    struct FormatExtension
    {
        juce::String id, name, version, fileExtension;
        ExtensionDirection direction = ExtensionDirection::both;
        juce::File executable;   // resolved absolute path
        juce::File folder;       // the extension's own folder
        std::vector<AdditionalInput> additionalInputs;   // empty by default

        bool customUI = false;
        std::vector<ExtensionSetting> settings;   // empty by default; mutually exclusive with customUI == true
    };

    /** Parses `extensionFolder`/manifest.json. Returns false (with
        warningOut set to a human-readable reason) for anything malformed:
        missing manifest.json, invalid JSON, a missing required field, an
        `executable` that doesn't resolve to an existing file inside
        `extensionFolder`, a malformed additionalInputs/settings entry, or
        `customUI: true` combined with a non-empty `settings` array. Never
        throws. */
    bool parseFormatExtensionManifest (const juce::File& extensionFolder,
                                        FormatExtension& out, juce::String& warningOut);
}
