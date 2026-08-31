# Format Extension Settings, Progress & Custom UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a format extension's manifest declare a pre-run settings dialog (sliders/checkboxes/dropdowns), report real progress back to KANADE DAW's `TaskPanel`, and optionally (`customUI: true`) take over with its own window instead — then use the settings path to add a mix-balance control and progress reporting to the existing `video-audio-mux` plugin.

**Architecture:** Two independent, optional manifest fields (`settings`, `customUI`) parsed into `FormatExtension`; `FormatExtensionRunner`'s single blocking wait is replaced with a polling loop plus a dedicated output-reader thread that recognises `PROGRESS:<0-100>` lines and supports cancellation; a new `ExtensionSettingsDialog` auto-builds the pre-run form; `MainComponent.cpp`'s export flow wires all of it together. Export only — import extensions are unaffected (see Task 4's scope note).

**Tech Stack:** C++20 / JUCE 8.0.6 (KANADE DAW core), .NET 8 (the `video-audio-mux` example plugin).

**Spec:** `docs/superpowers/specs/2026-08-31-extension-settings-ui-design.md`

## Global Constraints

- `settings` and `customUI: true` are mutually exclusive on one manifest — a manifest declaring both fails to load with a warning, not a partial load.
- The `additionalInputs` map's value type changes from `juce::File` to `juce::String` everywhere it appears (`ScopedEnvVars`, `runFormatExtensionExport`'s parameter, `MainComponent.cpp`'s `gatherAdditionalInputs`) — settings values aren't files, and both now share one map type.
- Default extension timeout stays 120000ms (the existing `extensionTimeoutMs` constant) for every extension except one with `customUI: true`, which gets `-1` (no timeout) passed to `FormatExtensionRunner::run`.
- Import extensions (`importViaExtension`/`runFormatExtensionImport`) are **out of scope** for `settings`/`customUI`/progress/cancel in this plan — `runFormatExtensionImport`'s own public signature does not change; it only adapts internally to `FormatExtensionRunner::run`'s new parameter list, always passing the fixed 120000ms timeout and null progress/cancel callbacks.
- Progress convention: a line exactly matching `PROGRESS:<number>` (0-100, fractional allowed) on either stdout or stderr (both already merge into the one stream `ExternalFormatExtensionRunner` reads, per `juce::ChildProcess`'s default `wantStdOut | wantStdErr` stream flags) reports progress; every other line is preserved for the failure-path error message exactly as before.
- No test spawns a real child process — this codebase's existing convention (`Source/Extensions/FormatExtensionTests.cpp`, `Source/Vocal/VocalTests.cpp`) always fakes the `FormatExtensionRunner`/`ResamplerRunner` interface instead. `parseProgressLine` is pulled out as a plain, directly-testable function specifically so the new progress convention gets real unit tests without breaking that convention; the reader-thread/cancel/timeout mechanics themselves are verified by manual testing in Task 6 once a real extension (`video-audio-mux`) exercises them end-to-end.
- Do not bump `CMakeLists.txt`'s `PROJECT_VERSION` or publish to GitHub as part of this plan — both happen later, only when the user asks (established pattern this session).
- `video-audio-mux` uses the `settings` path only; `customUI` stays `false` (unchanged) — it ships as tested, documented infrastructure with no consumer yet.

---

### Task 1: Manifest schema — `settings` and `customUI`

**Files:**
- Modify: `Source/Extensions/FormatExtension.h`
- Modify: `Source/Extensions/FormatExtension.cpp`
- Test: `Source/Extensions/FormatExtensionTests.cpp`

**Interfaces:**
- Produces: `enum class ExtensionSettingType { slider, checkbox, dropdown };` and `struct ExtensionSetting { juce::String id, label, envVar; ExtensionSettingType type; double sliderMin, sliderMax, sliderDefault; bool checkboxDefault; juce::StringArray dropdownOptions; juce::String dropdownDefault; };`, both in namespace `ss`. `FormatExtension` gains `bool customUI = false;` and `std::vector<ExtensionSetting> settings;`. Every later task that touches `FormatExtension.h` reads `ExtensionSetting`/`ExtensionSettingType` exactly as named here.

- [ ] **Step 1: Write the failing manifest-parsing tests**

Open `Source/Extensions/FormatExtensionTests.cpp`. Insert the following `beginTest` blocks immediately after the existing `"parseFormatExtensionManifest rejects an additionalInputs entry missing envVar"` block (right before the `"parseFormatExtensionManifest fails with a warning when manifest.json is missing"` block, around line 169):

```cpp
        beginTest ("parseFormatExtensionManifest parses settings of all three types");
        {
            auto folder = tempRoot.getChildFile ("settings-all-types");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.settings",
                "name": "Settings Test",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "settings": [
                    { "id": "balance", "label": "Balance", "type": "slider",
                      "envVar": "BALANCE", "min": 0.0, "max": 1.0, "default": 0.5 },
                    { "id": "matchLength", "label": "Match length", "type": "checkbox",
                      "envVar": "MATCH_LENGTH", "default": true },
                    { "id": "codec", "label": "Codec", "type": "dropdown",
                      "envVar": "CODEC", "options": ["aac", "mp3"], "default": "aac" }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (parseFormatExtensionManifest (folder, ext, warning), warning);
            expectEquals ((int) ext.settings.size(), 3);

            expect (ext.settings[0].type == ExtensionSettingType::slider);
            expectEquals (ext.settings[0].envVar, juce::String ("BALANCE"));
            expectWithinAbsoluteError (ext.settings[0].sliderMin, 0.0, 0.0001);
            expectWithinAbsoluteError (ext.settings[0].sliderMax, 1.0, 0.0001);
            expectWithinAbsoluteError (ext.settings[0].sliderDefault, 0.5, 0.0001);

            expect (ext.settings[1].type == ExtensionSettingType::checkbox);
            expectEquals (ext.settings[1].envVar, juce::String ("MATCH_LENGTH"));
            expect (ext.settings[1].checkboxDefault);

            expect (ext.settings[2].type == ExtensionSettingType::dropdown);
            expectEquals (ext.settings[2].envVar, juce::String ("CODEC"));
            expectEquals (ext.settings[2].dropdownOptions.size(), 2);
            expectEquals (ext.settings[2].dropdownOptions[0], juce::String ("aac"));
            expectEquals (ext.settings[2].dropdownDefault, juce::String ("aac"));
        }

        beginTest ("parseFormatExtensionManifest defaults settings to empty and customUI to false when absent");
        {
            auto folder = tempRoot.getChildFile ("no-settings");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.plain2",
                "name": "Plain2",
                "fileExtension": "xyz",
                "executable": "tool.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (parseFormatExtensionManifest (folder, ext, warning), warning);
            expect (ext.settings.empty());
            expect (! ext.customUI);
        }

        beginTest ("parseFormatExtensionManifest parses customUI: true");
        {
            auto folder = tempRoot.getChildFile ("custom-ui");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.customui",
                "name": "Custom UI",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "customUI": true
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (parseFormatExtensionManifest (folder, ext, warning), warning);
            expect (ext.customUI);
            expect (ext.settings.empty());
        }

        beginTest ("parseFormatExtensionManifest rejects customUI: true combined with a non-empty settings array");
        {
            auto folder = tempRoot.getChildFile ("custom-ui-and-settings");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.conflict",
                "name": "Conflict",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "customUI": true,
                "settings": [
                    { "id": "x", "label": "X", "type": "checkbox", "envVar": "X", "default": false }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest rejects a settings entry with an unknown type");
        {
            auto folder = tempRoot.getChildFile ("settings-bad-type");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.badtype",
                "name": "Bad Type",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "settings": [
                    { "id": "x", "label": "X", "type": "colourPicker", "envVar": "X" }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest rejects a slider settings entry missing min/max/default");
        {
            auto folder = tempRoot.getChildFile ("settings-slider-incomplete");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.incomplete",
                "name": "Incomplete",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "settings": [
                    { "id": "x", "label": "X", "type": "slider", "envVar": "X", "min": 0.0 }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest rejects a dropdown settings entry whose default is not one of its options");
        {
            auto folder = tempRoot.getChildFile ("settings-dropdown-bad-default");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.baddefault",
                "name": "Bad Default",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "settings": [
                    { "id": "x", "label": "X", "type": "dropdown", "envVar": "X",
                      "options": ["a", "b"], "default": "c" }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest rejects settings entries with duplicate ids");
        {
            auto folder = tempRoot.getChildFile ("settings-duplicate-id");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.dupid",
                "name": "Dup Id",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "settings": [
                    { "id": "x", "label": "X1", "type": "checkbox", "envVar": "X1", "default": false },
                    { "id": "x", "label": "X2", "type": "checkbox", "envVar": "X2", "default": false }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }
```

- [ ] **Step 2: Build and run the test target to verify it fails**

```powershell
cmake --build build --config Debug
build\ScoreSmith_artefacts\Debug\"KANADE DAW.exe" --run-tests
```

Expected: a compile error (`ExtensionSettingType`/`ExtensionSetting` not declared, `ext.settings`/`ext.customUI` not a member) — `FormatExtension.h` doesn't have these yet. (If your build output path differs, use whatever `cmake --build` printed as the executable location earlier this session.)

- [ ] **Step 3: Add the new types and fields to `FormatExtension.h`**

Replace the whole file with:

```cpp
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
```

- [ ] **Step 4: Add parsing for both fields to `FormatExtension.cpp`**

Replace the whole file with:

```cpp
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
```

- [ ] **Step 5: Build and run the tests to verify they pass**

```powershell
cmake --build build --config Debug
build\ScoreSmith_artefacts\Debug\"KANADE DAW.exe" --run-tests
```

Expected: `TOTAL: <N> passed, 0 failed`, N greater than before (8 new tests). Every existing extension test must still pass unchanged - this step only added fields, it didn't touch any type existing tests depend on.

- [ ] **Step 6: Commit**

```powershell
git add Source/Extensions/FormatExtension.h Source/Extensions/FormatExtension.cpp Source/Extensions/FormatExtensionTests.cpp
git commit -m "Add settings and customUI fields to the format extension manifest schema"
```

---

### Task 2: `FormatExtensionRunner` — reader thread, progress, cancel, timeout

**Files:**
- Modify: `Source/Extensions/FormatExtensionRunner.h`
- Modify: `Source/Extensions/FormatExtensionRunner.cpp`
- Test: `Source/Extensions/FormatExtensionTests.cpp`

**Interfaces:**
- Consumes: `FormatExtension::customUI` (Task 1).
- Produces: `bool parseProgressLine (const juce::String& line, float& out);` (namespace `ss`) - pure, directly testable. `FormatExtensionRunner::run` gains three parameters and becomes `virtual bool run (const juce::StringArray& args, const juce::File& expectedOutput, int timeoutMs, std::function<void (float)> onProgress, std::function<bool()> shouldCancel, juce::String& errorOut) = 0;`. `runFormatExtensionExport` gains two new *optional* parameters (`std::function<void (float)> onProgress = nullptr, std::function<bool()> shouldCancel = nullptr`) appended after `warningsOut`; its `additionalInputs` parameter type becomes `const std::map<juce::String, juce::String>&`. `runFormatExtensionImport`'s own public signature is unchanged. Task 3 and Task 4 both call these new names/types.

- [ ] **Step 1: Write the failing `parseProgressLine` tests**

Open `Source/Extensions/FormatExtensionTests.cpp`. Insert these `beginTest` blocks right after the last new block from Task 1 (`"parseFormatExtensionManifest rejects settings entries with duplicate ids"`), still before the `"parseFormatExtensionManifest fails with a warning when manifest.json is missing"` block:

```cpp
        beginTest ("parseProgressLine recognises a well-formed PROGRESS: line");
        {
            float value = -1.0f;
            expect (parseProgressLine ("PROGRESS:42", value));
            expectWithinAbsoluteError (value, 0.42f, 0.0001f);
        }

        beginTest ("parseProgressLine clamps a value above 100 to 1.0");
        {
            float value = -1.0f;
            expect (parseProgressLine ("PROGRESS:150", value));
            expectWithinAbsoluteError (value, 1.0f, 0.0001f);
        }

        beginTest ("parseProgressLine accepts a fractional percentage");
        {
            float value = -1.0f;
            expect (parseProgressLine ("PROGRESS:33.5", value));
            expectWithinAbsoluteError (value, 0.335f, 0.0001f);
        }

        beginTest ("parseProgressLine rejects a line without the PROGRESS: prefix");
        {
            float value = -1.0f;
            expect (! parseProgressLine ("frame=120 fps=30 time=00:00:04.00", value));
        }

        beginTest ("parseProgressLine rejects a PROGRESS: line with a non-numeric value");
        {
            float value = -1.0f;
            expect (! parseProgressLine ("PROGRESS:abc", value));
        }

        beginTest ("parseProgressLine rejects an empty value");
        {
            float value = -1.0f;
            expect (! parseProgressLine ("PROGRESS:", value));
        }
```

- [ ] **Step 2: Build to verify it fails to compile**

```powershell
cmake --build build --config Debug
```

Expected: compile error - `parseProgressLine` is not declared yet.

- [ ] **Step 3: Rewrite `FormatExtensionRunner.h`**

Replace the whole file with:

```cpp
#pragma once
#include "Core/Project.h"
#include "Extensions/FormatExtension.h"
#include "Plugins/PluginManager.h"
#include <juce_core/juce_core.h>
#include <functional>
#include <map>

namespace ss
{
    /** Runs one format extension's executable. Abstracted so tests never
        need a real extension executable on disk - mirrors
        Vocal/UtauRenderer.h's ResamplerRunner/ExternalResamplerRunner
        split exactly. */
    class FormatExtensionRunner
    {
    public:
        virtual ~FormatExtensionRunner() = default;

        /** args[0] is the executable itself (juce::ChildProcess::start
            convention). `timeoutMs` < 0 means no timeout (customUI
            extensions - the caller relies on `shouldCancel` instead).
            `onProgress` (0.0-1.0) is called from a background thread
            whenever the process prints a `PROGRESS:<0-100>` line - see
            parseProgressLine below; either callback may be null. Returns
            true only if the process exited 0 AND expectedOutput now
            exists; on failure, errorOut carries whatever the process
            wrote (that wasn't a PROGRESS: line) to stderr/stdout (or a
            generic timeout/exit-code message when that was empty). A
            true `shouldCancel()` result kills the process and returns
            false with an empty errorOut - the caller is expected to
            recognise its own cancellation and not treat that as a
            failure (see TaskPanel::isCancelled()). */
        virtual bool run (const juce::StringArray& args, const juce::File& expectedOutput,
                           int timeoutMs,
                           std::function<void (float)> onProgress,
                           std::function<bool()> shouldCancel,
                           juce::String& errorOut) = 0;
    };

    /** Production FormatExtensionRunner - shells out via juce::ChildProcess,
        exactly like ExternalResamplerRunner does for the UTAU resampler. */
    class ExternalFormatExtensionRunner final : public FormatExtensionRunner
    {
    public:
        bool run (const juce::StringArray& args, const juce::File& expectedOutput,
                   int timeoutMs,
                   std::function<void (float)> onProgress,
                   std::function<bool()> shouldCancel,
                   juce::String& errorOut) override;
    };

    /** Parses one line of an extension's stdout/stderr for the
        `PROGRESS:<0-100>` convention (Format Extension Settings/Progress/
        Custom UI design, docs/superpowers/specs/
        2026-08-31-extension-settings-ui-design.md). Returns true and sets
        `out` (0.0-1.0, clamped) only for a well-formed match; any other
        line - including a malformed "PROGRESS:" line - returns false and
        leaves `out` untouched. Exposed here (not file-local) so
        FormatExtensionTests.cpp can test the parsing contract directly,
        without spawning a real process. */
    bool parseProgressLine (const juce::String& line, float& out);
}

namespace ss::io
{
    /** Export: writes `project` to a temp .dawproject, sets each
        `additionalInputs` entry as an environment variable on the KANADE
        DAW process for the duration of the call (cleared again before
        returning, success or failure), invokes the extension with
        `--export <temp.dawproject> <outputFile>` via `runner`, and reports
        failure via errorOut/warningsOut using the same two-signal contract
        exportDawProject already uses. `onProgress`/`shouldCancel` are
        forwarded to `runner.run` unchanged (both default to null - most
        callers, and every existing test, don't care). The timeout passed
        to the runner is computed here: -1 (no timeout) when
        `extension.customUI` is true, the usual 120s constant otherwise -
        callers never choose it directly. The temp .dawproject is always
        deleted before returning, success or not. */
    bool runFormatExtensionExport (const FormatExtension& extension, const Project& project,
                                    PluginManager& plugins, const juce::File& outputFile,
                                    const std::map<juce::String, juce::String>& additionalInputs,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut,
                                    std::function<void (float)> onProgress = nullptr,
                                    std::function<bool()> shouldCancel = nullptr);

    /** Import: invokes the extension with
        `--import <inputFile> <temp.dawproject>` via `runner` (always with
        the fixed 120s timeout and no progress/cancel callbacks - see the
        Import scope note in docs/superpowers/specs/
        2026-08-31-extension-settings-ui-design.md), then imports the
        extension-produced .dawproject into `project` via importDawProject.
        The temp .dawproject is always deleted before returning, success
        or not. */
    bool runFormatExtensionImport (const FormatExtension& extension, Project& project,
                                    PluginManager& plugins, const juce::File& inputFile,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut);
}
```

- [ ] **Step 4: Rewrite `FormatExtensionRunner.cpp`**

Replace the whole file with:

```cpp
#include "Extensions/FormatExtensionRunner.h"
#include "IO/DawProject.h"
#include <cstdlib>

namespace ss
{
    namespace
    {
        constexpr int extensionTimeoutMs = 120000;
        constexpr int pollIntervalMs = 50;

        void setEnvVar (const juce::String& name, const juce::String& value)
        {
           #if JUCE_WINDOWS
            _wputenv_s (name.toWideCharPointer(), value.toWideCharPointer());
           #else
            setenv (name.toRawUTF8(), value.toRawUTF8(), 1);
           #endif
        }

        void clearEnvVar (const juce::String& name)
        {
           #if JUCE_WINDOWS
            _wputenv_s (name.toWideCharPointer(), L"");
           #else
            unsetenv (name.toRawUTF8());
           #endif
        }

        /** Sets every entry in `vars` as an environment variable on this
            process for its own scope's lifetime, clearing all of them again
            on destruction - RAII so a return/throw partway through
            runFormatExtensionExport can't leak one. */
        class ScopedEnvVars
        {
        public:
            explicit ScopedEnvVars (const std::map<juce::String, juce::String>& vars)
            {
                for (const auto& [name, value] : vars)
                {
                    setEnvVar (name, value);
                    names.push_back (name);
                }
            }

            ~ScopedEnvVars()
            {
                for (const auto& name : names)
                    clearEnvVar (name);
            }

            ScopedEnvVars (const ScopedEnvVars&) = delete;
            ScopedEnvVars& operator= (const ScopedEnvVars&) = delete;

        private:
            std::vector<juce::String> names;
        };

        /** Drains a running juce::ChildProcess's combined stdout/stderr on
            its own thread for as long as the process lives, so a chatty
            extension can never block on a full pipe waiting for someone to
            read it. Splits the stream into lines; a line matching
            parseProgressLine's PROGRESS: convention calls `onProgress` and
            is otherwise discarded, every other line is kept for
            nonProgressText(). */
        class OutputReader final : private juce::Thread
        {
        public:
            OutputReader (juce::ChildProcess& processIn, std::function<void (float)> onProgressIn)
                : juce::Thread ("FormatExtensionOutputReader"),
                  process (processIn), onProgress (std::move (onProgressIn))
            {
                startThread();
            }

            /** Blocks until the reader has drained everything the process
                will ever write (i.e. the process's output handle has
                closed). Callers ensure the process is already dead first -
                that's what makes the underlying blocking read return -
                and must call this, and let it return, before trusting the
                process's exit code or nonProgressText(). */
            void waitUntilDone()
            {
                stopThread (2000);
            }

            juce::String nonProgressText() const
            {
                const juce::ScopedLock sl (lock);
                return accumulated.trim();
            }

        private:
            void run() override
            {
                char buffer[4096];
                juce::String pending;

                for (;;)
                {
                    const auto bytesRead = process.readProcessOutput (buffer, sizeof (buffer));
                    if (bytesRead <= 0)
                        break;   // pipe closed - the process has exited (or is exiting)

                    pending += juce::String::fromUTF8 (buffer, bytesRead);

                    int newline;
                    while ((newline = pending.indexOfChar ('\n')) >= 0)
                    {
                        handleLine (pending.substring (0, newline).trimEnd());
                        pending = pending.substring (newline + 1);
                    }
                }

                if (pending.isNotEmpty())
                    handleLine (pending);
            }

            void handleLine (const juce::String& line)
            {
                float progress = 0.0f;
                if (parseProgressLine (line, progress))
                {
                    if (onProgress)
                        onProgress (progress);
                    return;
                }

                const juce::ScopedLock sl (lock);
                if (accumulated.isNotEmpty())
                    accumulated += "\n";
                accumulated += line;
            }

            juce::ChildProcess& process;
            std::function<void (float)> onProgress;
            mutable juce::CriticalSection lock;
            juce::String accumulated;
        };
    }

    bool parseProgressLine (const juce::String& line, float& out)
    {
        static const juce::String prefix ("PROGRESS:");
        if (! line.startsWith (prefix))
            return false;

        const auto numberText = line.substring (prefix.length()).trim();
        if (numberText.isEmpty() || ! numberText.containsOnly ("0123456789."))
            return false;

        out = juce::jlimit (0.0f, 1.0f, numberText.getFloatValue() / 100.0f);
        return true;
    }

    bool ExternalFormatExtensionRunner::run (const juce::StringArray& args, const juce::File& expectedOutput,
                                              int timeoutMs,
                                              std::function<void (float)> onProgress,
                                              std::function<bool()> shouldCancel,
                                              juce::String& errorOut)
    {
        juce::ChildProcess process;
        if (! process.start (args))
        {
            errorOut = "Could not start \"" + args[0] + "\".";
            return false;
        }

        OutputReader reader (process, std::move (onProgress));

        const bool hasDeadline = timeoutMs >= 0;
        const auto deadline = hasDeadline
                                 ? juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs
                                 : 0u;
        bool cancelled = false;
        bool timedOut = false;

        while (process.isRunning())
        {
            if (shouldCancel && shouldCancel())
            {
                process.kill();
                cancelled = true;
                break;
            }

            if (hasDeadline && juce::Time::getMillisecondCounter() > deadline)
            {
                process.kill();
                timedOut = true;
                break;
            }

            juce::Thread::sleep (pollIntervalMs);
        }

        process.waitForProcessToFinish (2000);
        reader.waitUntilDone();

        if (cancelled)
        {
            errorOut = {};
            return false;
        }

        if (timedOut)
        {
            errorOut = "\"" + args[0] + "\" timed out after "
                       + juce::String (timeoutMs / 1000) + "s.";
            return false;
        }

        const auto exitCode = process.getExitCode();
        const auto output = reader.nonProgressText();

        if (exitCode != 0)
        {
            errorOut = output.isNotEmpty()
                          ? output
                          : ("\"" + args[0] + "\" exited with code " + juce::String (exitCode) + ".");
            return false;
        }

        if (! expectedOutput.existsAsFile())
        {
            errorOut = "\"" + args[0] + "\" exited successfully but did not produce \""
                       + expectedOutput.getFullPathName() + "\".";
            return false;
        }

        return true;
    }
}

namespace ss::io
{
    bool runFormatExtensionExport (const FormatExtension& extension, const Project& project,
                                    PluginManager& plugins, const juce::File& outputFile,
                                    const std::map<juce::String, juce::String>& additionalInputs,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut,
                                    std::function<void (float)> onProgress,
                                    std::function<bool()> shouldCancel)
    {
        const auto tempDawProject = juce::File::createTempFile ("dawproject");

        if (! exportDawProject (tempDawProject, project, plugins, errorOut, warningsOut))
        {
            tempDawProject.deleteFile();
            return false;
        }

        const juce::StringArray args { extension.executable.getFullPathName(), "--export",
                                        tempDawProject.getFullPathName(), outputFile.getFullPathName() };

        const int timeoutMs = extension.customUI ? -1 : extensionTimeoutMs;

        bool ok = false;
        {
            const ScopedEnvVars scopedVars (additionalInputs);
            ok = runner.run (args, outputFile, timeoutMs, std::move (onProgress), std::move (shouldCancel), errorOut);
        }

        tempDawProject.deleteFile();
        return ok;
    }

    bool runFormatExtensionImport (const FormatExtension& extension, Project& project,
                                    PluginManager& plugins, const juce::File& inputFile,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut)
    {
        const auto tempDawProject = juce::File::createTempFile ("dawproject");

        const juce::StringArray args { extension.executable.getFullPathName(), "--import",
                                        inputFile.getFullPathName(), tempDawProject.getFullPathName() };
        if (! runner.run (args, tempDawProject, extensionTimeoutMs, nullptr, nullptr, errorOut))
        {
            tempDawProject.deleteFile();
            return false;
        }

        const bool ok = importDawProject (tempDawProject, project, plugins, errorOut, warningsOut);
        tempDawProject.deleteFile();
        return ok;
    }
}
```

- [ ] **Step 5: Update the five fake `FormatExtensionRunner`s in `FormatExtensionTests.cpp` to the new `run(...)` signature**

Each is a `struct ... final : public FormatExtensionRunner` with one `run(...)` override. Update each override's parameter list to match the new five-value-parameter interface (keep every existing body line unchanged - only the signature widens):

`RecordingRunner` (originally `bool run (const juce::StringArray& args, const juce::File& expectedOutput, juce::String&) override`):
```cpp
                bool run (const juce::StringArray& args, const juce::File& expectedOutput,
                          int, std::function<void (float)>, std::function<bool()>,
                          juce::String&) override
```

`EnvCheckingRunner` (originally `bool run (const juce::StringArray&, const juce::File& expectedOutput, juce::String&) override`):
```cpp
                bool run (const juce::StringArray&, const juce::File& expectedOutput,
                          int, std::function<void (float)>, std::function<bool()>,
                          juce::String&) override
```

`FailingRunner` (originally `bool run (const juce::StringArray&, const juce::File&, juce::String& errorOut) override`):
```cpp
                bool run (const juce::StringArray&, const juce::File&,
                          int, std::function<void (float)>, std::function<bool()>,
                          juce::String& errorOut) override
```

`ProducesFileRunner` (originally `bool run (const juce::StringArray&, const juce::File& expectedOutput, juce::String&) override`):
```cpp
                bool run (const juce::StringArray&, const juce::File& expectedOutput,
                          int, std::function<void (float)>, std::function<bool()>,
                          juce::String&) override
```

`MissingOutputRunner` (originally `bool run (const juce::StringArray&, const juce::File&, juce::String& errorOut) override`):
```cpp
                bool run (const juce::StringArray&, const juce::File&,
                          int, std::function<void (float)>, std::function<bool()>,
                          juce::String& errorOut) override
```

- [ ] **Step 6: Widen the `additionalInputs` map literal in the `EnvCheckingRunner` test**

In the `"runFormatExtensionExport sets additionalInputs as environment variables during the run, and clears them after"` test, change:

```cpp
            const std::map<juce::String, juce::File> additionalInputs { { "KANADE_DAW_TEST_VAR", probeFile } };
```

to:

```cpp
            const std::map<juce::String, juce::String> additionalInputs { { "KANADE_DAW_TEST_VAR", probeFile.getFullPathName() } };
```

- [ ] **Step 7: Build and run the tests to verify everything passes**

```powershell
cmake --build build --config Debug
build\ScoreSmith_artefacts\Debug\"KANADE DAW.exe" --run-tests
```

Expected: `TOTAL: <N> passed, 0 failed`, N now 6 higher than after Task 1 (the six new `parseProgressLine` tests). Every `additionalInputs`/`runFormatExtensionExport`/`runFormatExtensionImport` test from before must still pass unchanged - the only test-side edits were the five signature widenings and one map literal.

- [ ] **Step 8: Commit**

```powershell
git add Source/Extensions/FormatExtensionRunner.h Source/Extensions/FormatExtensionRunner.cpp Source/Extensions/FormatExtensionTests.cpp
git commit -m "Rewrite ExternalFormatExtensionRunner around a reader thread for PROGRESS: parsing and cancellation"
```

---

### Task 3: `ExtensionSettingsDialog` (new)

**Files:**
- Create: `Source/UI/ExtensionSettingsDialog.h`
- Create: `Source/UI/ExtensionSettingsDialog.cpp`

**Interfaces:**
- Consumes: `ss::ExtensionSetting`/`ss::ExtensionSettingType` (Task 1), `palette()`/`TRANS()` (`Source/UI/UiSupport.h`, already used by every other dialog).
- Produces: `class ExtensionSettingsDialog` with `using Result = std::optional<std::map<juce::String, juce::String>>;` and `static void launch (const juce::String& extensionName, const std::vector<ExtensionSetting>& settings, std::function<void (Result)> onComplete);`. Task 4 calls `ExtensionSettingsDialog::launch` and uses `ExtensionSettingsDialog::Result` by name.

New files aren't listed anywhere else - `CMakeLists.txt` collects sources via `file(GLOB_RECURSE SS_SOURCES CONFIGURE_DEPENDS ...)`, so the next `cmake --build` picks them up automatically; no CMakeLists.txt edit is needed.

There is no dedicated unit test for this task - this codebase has no precedent for unit-testing a JUCE dialog's rendering (`ExtensionHelpDialog`, the closest sibling, has none either); `WhatsNewDialogTests.cpp` only tests that dialog's pure logic, and there's no comparable pure logic here beyond what Task 1 already tests (manifest parsing) and Task 4 already tests (nothing new - it's UI wiring). Full manual verification happens in Task 6, once `video-audio-mux` declares a real `settings` entry to open this dialog against.

- [ ] **Step 1: Create `Source/UI/ExtensionSettingsDialog.h`**

```cpp
#pragma once
#include "Extensions/FormatExtension.h"
#include "UI/UiSupport.h"
#include <functional>
#include <map>
#include <optional>

namespace ss
{
    /** Auto-generated from a FormatExtension's `settings` array (see
        docs/superpowers/specs/2026-08-31-extension-settings-ui-design.md) -
        one row per setting (slider/checkbox/dropdown), shown immediately
        before invoking the extension. Same juce::DialogWindow::LaunchOptions
        shape as ExtensionHelpDialog, but returns its result via callback
        instead of showing static text. */
    class ExtensionSettingsDialog final : public juce::Component
    {
    public:
        /** nullopt means the user cancelled (Cancel button, Escape, or the
            window's own close button) - every other case is a map keyed by
            each setting's own envVar, holding the value convention
            FormatExtensionRunner.h documents (a slider's number as a
            string, "1"/"0" for a checkbox, the selected option string for
            a dropdown). */
        using Result = std::optional<std::map<juce::String, juce::String>>;

        ExtensionSettingsDialog (const std::vector<ExtensionSetting>& settings,
                                 std::function<void (Result)> onComplete);
        ~ExtensionSettingsDialog() override;

        void resized() override;
        void paint (juce::Graphics&) override;

        /** Opens the dialog in its own modal window titled `extensionName`. */
        static void launch (const juce::String& extensionName,
                            const std::vector<ExtensionSetting>& settings,
                            std::function<void (Result)> onComplete);

    private:
        void finish (Result);

        struct Row
        {
            juce::String envVar;
            ExtensionSettingType type;
            std::unique_ptr<juce::Label> label;
            std::unique_ptr<juce::Component> control;   // Slider, ToggleButton, or ComboBox
        };

        std::function<void (Result)> onComplete;
        bool completed = false;

        juce::OwnedArray<Row> rows;
        juce::TextButton okButton, cancelButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExtensionSettingsDialog)
    };
}
```

- [ ] **Step 2: Create `Source/UI/ExtensionSettingsDialog.cpp`**

```cpp
#include "UI/ExtensionSettingsDialog.h"

namespace ss
{
    ExtensionSettingsDialog::ExtensionSettingsDialog (const std::vector<ExtensionSetting>& settings,
                                                       std::function<void (Result)> onComplete_)
        : onComplete (std::move (onComplete_))
    {
        for (const auto& setting : settings)
        {
            auto row = std::make_unique<Row>();
            row->envVar = setting.envVar;
            row->type = setting.type;

            row->label = std::make_unique<juce::Label>();
            row->label->setText (setting.label, juce::dontSendNotification);
            row->label->setFont (juce::Font (juce::FontOptions (13.0f)));
            row->label->setColour (juce::Label::textColourId, palette().text);
            row->label->setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (*row->label);

            switch (setting.type)
            {
                case ExtensionSettingType::slider:
                {
                    auto slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal,
                                                                   juce::Slider::TextBoxRight);
                    slider->setRange (setting.sliderMin, setting.sliderMax);
                    slider->setValue (setting.sliderDefault, juce::dontSendNotification);
                    slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
                    addAndMakeVisible (*slider);
                    row->control = std::move (slider);
                    break;
                }
                case ExtensionSettingType::checkbox:
                {
                    auto toggle = std::make_unique<juce::ToggleButton>();
                    toggle->setToggleState (setting.checkboxDefault, juce::dontSendNotification);
                    addAndMakeVisible (*toggle);
                    row->control = std::move (toggle);
                    break;
                }
                case ExtensionSettingType::dropdown:
                {
                    auto combo = std::make_unique<juce::ComboBox>();
                    for (int i = 0; i < setting.dropdownOptions.size(); ++i)
                        combo->addItem (setting.dropdownOptions[i], i + 1);
                    const int defaultIndex = setting.dropdownOptions.indexOf (setting.dropdownDefault);
                    combo->setSelectedId (defaultIndex >= 0 ? defaultIndex + 1 : 1, juce::dontSendNotification);
                    addAndMakeVisible (*combo);
                    row->control = std::move (combo);
                    break;
                }
            }

            rows.add (std::move (row));
        }

        okButton.setButtonText (TRANS ("OK"));
        okButton.onClick = [this]
        {
            std::map<juce::String, juce::String> values;
            for (const auto* row : rows)
            {
                switch (row->type)
                {
                    case ExtensionSettingType::slider:
                        values[row->envVar] = juce::String (
                            static_cast<juce::Slider*> (row->control.get())->getValue());
                        break;
                    case ExtensionSettingType::checkbox:
                        values[row->envVar] = static_cast<juce::ToggleButton*> (row->control.get())
                                                  ->getToggleState() ? "1" : "0";
                        break;
                    case ExtensionSettingType::dropdown:
                        values[row->envVar] = static_cast<juce::ComboBox*> (row->control.get())->getText();
                        break;
                }
            }
            finish (std::move (values));
        };
        addAndMakeVisible (okButton);

        cancelButton.setButtonText (TRANS ("Cancel"));
        cancelButton.onClick = [this] { finish (std::nullopt); };
        addAndMakeVisible (cancelButton);

        setSize (480, 12 + (int) settings.size() * 36 + 12 + 30 + 12);
    }

    ExtensionSettingsDialog::~ExtensionSettingsDialog()
    {
        // The dialog can also close via the window's own close button or
        // Escape, neither of which goes through okButton/cancelButton -
        // treat any of those the same as Cancel.
        if (! completed && onComplete)
            onComplete (std::nullopt);
    }

    void ExtensionSettingsDialog::finish (Result result)
    {
        if (completed) return;
        completed = true;

        if (onComplete)
            onComplete (std::move (result));

        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (0);
    }

    void ExtensionSettingsDialog::paint (juce::Graphics& g)
    {
        g.fillAll (palette().windowBg);
    }

    void ExtensionSettingsDialog::resized()
    {
        auto area = getLocalBounds().reduced (14, 12);

        auto buttonRow = area.removeFromBottom (30);
        okButton.setBounds (buttonRow.removeFromRight (90));
        buttonRow.removeFromRight (8);
        cancelButton.setBounds (buttonRow.removeFromRight (90));
        area.removeFromBottom (8);

        for (auto* row : rows)
        {
            auto r = area.removeFromTop (28);
            row->label->setBounds (r.removeFromLeft (220));
            row->control->setBounds (r);
            area.removeFromTop (8);
        }
    }

    void ExtensionSettingsDialog::launch (const juce::String& extensionName,
                                          const std::vector<ExtensionSetting>& settings,
                                          std::function<void (Result)> onComplete)
    {
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (new ExtensionSettingsDialog (settings, std::move (onComplete)));
        options.dialogTitle            = extensionName;
        options.dialogBackgroundColour = palette().windowBg;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar      = true;
        options.resizable              = true;
        options.launchAsync();
    }
}
```

- [ ] **Step 3: Build to verify it compiles cleanly**

```powershell
cmake --build build --config Debug
```

Expected: builds with no errors. (Nothing calls `ExtensionSettingsDialog` yet - Task 4 wires it in - so there's no runtime behaviour to check until then.)

- [ ] **Step 4: Commit**

```powershell
git add Source/UI/ExtensionSettingsDialog.h Source/UI/ExtensionSettingsDialog.cpp
git commit -m "Add ExtensionSettingsDialog, auto-built from a format extension's settings array"
```

---

### Task 4: Wire `settings`/progress/cancel into `MainComponent.cpp`'s export flow

**Files:**
- Modify: `Source/UI/MainComponent.cpp`

**Interfaces:**
- Consumes: `ExtensionSettingsDialog::launch`/`ExtensionSettingsDialog::Result` (Task 3), `runFormatExtensionExport`'s widened signature (Task 2), `TaskPanel::setProgress`/`isCancelled` (already exists, `Source/UI/UiSupport.h`).
- Produces: nothing new for later tasks - this is the last C++ integration point.

**IMPORTANT - do not touch:** `importViaExtension()` (around line 1090) and `runFormatExtensionImport`'s call site inside it. Per this plan's Global Constraints, import extensions are out of scope - leave that method exactly as it is today.

- [ ] **Step 1: Add the new include**

In the include block near the top of the file, insert a new line directly after `#include "UI/ExtensionHelpDialog.h"` (so the block stays alphabetical):

```cpp
#include "UI/ExtensionSettingsDialog.h"
```

- [ ] **Step 2: Widen `gatherAdditionalInputs`'s map type from `juce::File` to `juce::String`**

Find `gatherAdditionalInputs` (around line 984). Change its signature from:

```cpp
        void gatherAdditionalInputs (const FormatExtension& extension, size_t index,
                                     std::map<juce::String, juce::File> resolvedSoFar,
                                     std::function<void (std::map<juce::String, juce::File>)> then)
```

to:

```cpp
        void gatherAdditionalInputs (const FormatExtension& extension, size_t index,
                                     std::map<juce::String, juce::String> resolvedSoFar,
                                     std::function<void (std::map<juce::String, juce::String>)> then)
```

Inside the same method, the `userFile` branch currently has:

```cpp
                    resolvedSoFar[extension.additionalInputs[index].envVar] = picked;
```

change to:

```cpp
                    resolvedSoFar[extension.additionalInputs[index].envVar] = picked.getFullPathName();
```

and the `mixdownRender` branch currently has:

```cpp
                resolvedSoFar[extension.additionalInputs[index].envVar] = renderFile;
```

change to:

```cpp
                resolvedSoFar[extension.additionalInputs[index].envVar] = renderFile.getFullPathName();
```

No other line in this method changes.

- [ ] **Step 3: Insert the settings-dialog step into `exportViaExtension`**

Find `exportViaExtension` (around line 953). Its `chooser->launchAsync(...)` callback currently ends with:

```cpp
                gatherAdditionalInputs (extension, 0, {}, [this, extension, file] (std::map<juce::String, juce::File> resolved)
                {
                    finishExportViaExtension (extension, file, std::move (resolved));
                });
```

Replace those three lines with:

```cpp
                gatherAdditionalInputs (extension, 0, {}, [this, extension, file] (std::map<juce::String, juce::String> resolved)
                {
                    if (extension.settings.empty())
                    {
                        finishExportViaExtension (extension, file, std::move (resolved));
                        return;
                    }

                    ExtensionSettingsDialog::launch (extension.name, extension.settings,
                        [this, extension, file, resolved] (ExtensionSettingsDialog::Result settingsResult) mutable
                    {
                        if (! settingsResult.has_value())
                            return;   // user cancelled the settings dialog - drop the whole export

                        for (auto& [envVar, value] : *settingsResult)
                            resolved[envVar] = value;

                        finishExportViaExtension (extension, file, std::move (resolved));
                    });
                });
```

- [ ] **Step 4: Widen `finishExportViaExtension`'s parameter type and wire in progress/cancel**

Find `finishExportViaExtension` (around line 1047). Replace the whole method with:

```cpp
        void finishExportViaExtension (const FormatExtension& extension, const juce::File& outputFile,
                                       std::map<juce::String, juce::String> additionalInputs)
        {
            if (ctx.project == nullptr) return;

            auto error = std::make_shared<juce::String>();
            auto warnings = std::make_shared<juce::StringArray>();

            task.run (extension.name + " " + TRANS ("running..."),
                      [this, extension, outputFile, additionalInputs, error, warnings] (TaskPanel& t)
            {
                ExternalFormatExtensionRunner runner;
                io::runFormatExtensionExport (extension, *ctx.project, *ctx.plugins, outputFile,
                                              additionalInputs, runner, *error, *warnings,
                                              [&t] (float p) { t.setProgress (p); },
                                              [&t] { return t.isCancelled(); });
            },
            [this, extension, additionalInputs, error, warnings]
            {
                // Any additionalInputs value that was a temp render file (not a
                // user-picked one) should not be left behind. mixdownRender is
                // currently the only kind that creates one, and its envVar names
                // are only known via extension.additionalInputs - clean up every
                // resolved path whose kind was mixdownRender.
                for (const auto& input : extension.additionalInputs)
                    if (input.kind == AdditionalInputKind::mixdownRender)
                        if (auto it = additionalInputs.find (input.envVar); it != additionalInputs.end())
                            juce::File (it->second).deleteFile();

                if (task.isCancelled())
                    return;   // user hit Cancel - not a failure, nothing more to report

                if (error->isNotEmpty())
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Export failed"), *error, TRANS ("OK"), &owner);
                    return;
                }
                showDawProjectWarnings (*warnings);
            });
        }
```

(The work lambda's `TaskPanel&` parameter is now named `t` instead of left anonymous, since it's needed to build the `onProgress`/`shouldCancel` callbacks; the completion lambda gained the `if (task.isCancelled()) return;` line, mirroring `Source/UI/TranscribeView.cpp:376`'s established idiom, and the cleanup loop's `it->second.deleteFile()` became `juce::File (it->second).deleteFile()` since `it->second` is now a `juce::String`, not a `juce::File`.)

- [ ] **Step 5: Build and run the full test suite**

```powershell
cmake --build build --config Debug
build\ScoreSmith_artefacts\Debug\"KANADE DAW.exe" --run-tests
```

Expected: builds clean, `TOTAL: <N> passed, 0 failed` with the same N as at the end of Task 2 (this task only touches UI wiring code with no dedicated unit tests of its own - `MainComponent.cpp` has none in this codebase, matching every other UI-wiring change made earlier this session).

- [ ] **Step 6: Manual verification**

Copy the freshly built exe over the root-level `E:\MIDIDAW\KANADE DAW.exe` (the path actually launched), following the same convention used throughout this session. Since no extension yet declares a `settings` array (that's Task 6), there's nothing to click through until then - defer full manual verification (including hitting Cancel mid-export to confirm the process dies and no error dialog appears) to Task 6's own manual-testing step, which has a real `settings`-declaring extension to test against.

- [ ] **Step 7: Commit**

```powershell
git add Source/UI/MainComponent.cpp
git commit -m "Show the settings dialog before running an extension, and wire progress/cancel into its TaskPanel"
```

---

### Task 5: `ExtensionHelpDialog` documentation update

**Files:**
- Modify: `Source/UI/ExtensionHelpDialog.cpp`

**Interfaces:**
- Consumes: nothing (pure static text change).
- Produces: nothing (leaf task).

- [ ] **Step 1: Update `helpBodyText()`**

Open `Source/UI/ExtensionHelpDialog.cpp`. Replace the raw string literal passed to `juce::CharPointer_UTF8` inside `helpBodyText()` with:

```cpp
R"RAW(1つのフォルダ = 1つの拡張機能です。manifest.json と、そこで名指しした
実行ファイルを同じフォルダに置いてください。

manifest.json:
{
  "id": "com.example.reason-export",
  "name": "Reason Project Export",
  "version": "1.0.0",
  "fileExtension": "reason",
  "direction": "export",
  "executable": "reason-export.exe"
}

フィールド:
  id            必須。他と重複しないID(逆ドメイン名推奨)
  name          必須。File メニューと設定画面に表示される名前
  version       任意(省略時は空)。表示用のみ
  fileExtension 必須。先頭のドット無し(例: "reason")
  direction     任意(省略時 "both")。"import" | "export" | "both"
  executable    必須。manifest.json と同じフォルダ内の実行ファイル名

呼び出し方(コマンドライン引数):
  <executable> --export <input.dawproject> <output-file>
  <executable> --import <input-file> <output.dawproject>

KANADE DAW は御社独自のフォーマットを一切解釈しません。DAWproject
(Studio One / Bitwig / Cubase 等が対応するオープンな相互運用フォーマット)
との相互変換だけを、拡張機能の実行ファイルに任せます。

終了コード 0 で成功、それ以外は失敗として扱われます。標準出力/標準エラー
出力に書いた内容はそのままエラーダイアログに表示されます。成功終了して
いても出力ファイルが実際に存在しなければ失敗として扱われます。

タイムアウトは120秒です(customUI: true の場合を除く。下記参照)。

実行前の設定ダイアログ(settings, export専用):
  manifest.json に "settings" 配列を書くと、実行前にKANADE DAWが自動で
  ダイアログを出し、選んだ値を環境変数として渡します。

  { "id": "balance", "label": "ミックスバランス", "type": "slider",
    "envVar": "MUX_BALANCE", "min": 0.0, "max": 1.0, "default": 0.5 }

  type は "slider"(min/max/default) | "checkbox"(default) |
  "dropdown"(options配列/default) のいずれか。envVar の値はそれぞれ
  数値の文字列 / "1"か"0" / 選んだ文字列そのまま、として渡されます。

進捗表示:
  標準出力(または標準エラー出力)に "PROGRESS:42" のような行を書くと、
  0-100の数値としてKANADE DAWの進捗バーに反映されます。それ以外の行は
  従来どおり、失敗時のエラーダイアログ用に蓄積されます。

独自GUI(customUI, export専用):
  manifest.json に "customUI": true を書くと、上記の設定ダイアログを
  出さず、120秒のタイムアウトも撤廃してプラグインを起動します。実行
  ファイル自身が好きなだけウィンドウを出してよく、KANADE DAWのキャン
  セルボタンで強制終了できます。"settings" と同時に指定することはでき
  ません。

最小サンプルと詳しい解説は README.md の
「拡張機能(フォーマットプラグイン)の作り方」を参照してください。)RAW"
```

- [ ] **Step 2: Build and run the full test suite**

```powershell
cmake --build build --config Debug
build\ScoreSmith_artefacts\Debug\"KANADE DAW.exe" --run-tests
```

Expected: builds clean, same `TOTAL` pass count as Task 4 (this is a pure string literal change with no test coverage of its own, matching how the original help text also had none).

- [ ] **Step 3: Manual verification**

Copy the exe over `E:\MIDIDAW\KANADE DAW.exe`, launch it, open System Plugins > "How to build a format extension...", and confirm the new sections render without truncation (the dialog's `TextEditor` scrolls, so this is just a sanity check that the text isn't garbled).

- [ ] **Step 4: Commit**

```powershell
git add Source/UI/ExtensionHelpDialog.cpp
git commit -m "Document settings, customUI, and PROGRESS: reporting in the extension help dialog"
```

---

### Task 6: `video-audio-mux` — mix balance setting and progress reporting

**Files:**
- Modify: `Extensions-Examples/video-audio-mux/manifest.json`
- Modify: `Extensions-Examples/video-audio-mux/src/Program.cs`

**Interfaces:**
- Consumes: the `settings` manifest field (Task 1) and the `PROGRESS:<0-100>` stdout convention (Task 2) - both already merged and working in KANADE DAW by this point.
- Produces: nothing (leaf task, end of plan).

This is standalone .NET 8 code with no C++ dependency - it can be developed and `dotnet build`-checked independently, but full end-to-end verification needs Tasks 1-5 already built into the `KANADE DAW.exe` the implementer is testing against.

- [ ] **Step 1: Add the `settings` entry and bump the manifest version**

Open `Extensions-Examples/video-audio-mux/manifest.json`. Replace its whole contents with:

```json
{
  "id": "com.kanade-daw-examples.video-audio-mux",
  "name": "Video + Audio Mux",
  "version": "1.1.0",
  "fileExtension": "mp4",
  "direction": "export",
  "executable": "video-audio-mux.exe",
  "customUI": false,
  "additionalInputs": [
    {
      "kind": "userFile",
      "envVar": "KANADE_DAW_VIDEO_FILE",
      "prompt": "Video file to combine with",
      "fileFilter": "*.mp4"
    },
    {
      "kind": "mixdownRender",
      "envVar": "KANADE_DAW_MIXDOWN_WAV"
    }
  ],
  "settings": [
    {
      "id": "mixBalance",
      "label": "音声ミックスバランス（0=元動画優先 / 1=DAW音声優先）",
      "type": "slider",
      "envVar": "MUX_MIX_BALANCE",
      "min": 0.0,
      "max": 1.0,
      "default": 0.5
    }
  ]
}
```

- [ ] **Step 2: Read `mixBalance` and weight the two audio streams in the ffmpeg filter graph**

Open `Extensions-Examples/video-audio-mux/src/Program.cs`. After the existing `mixdownPath` validation block (right after its closing `}`, before the `ffmpegPath` lookup), insert:

```csharp
    double mixBalance = 0.5;
    string? mixBalanceText = Environment.GetEnvironmentVariable("MUX_MIX_BALANCE");
    if (!string.IsNullOrEmpty(mixBalanceText))
        double.TryParse(mixBalanceText, NumberStyles.Float, CultureInfo.InvariantCulture, out mixBalance);
    mixBalance = Math.Clamp(mixBalance, 0.0, 1.0);
```

Then find the existing `filterComplex` construction:

```csharp
    string filterComplex = videoHasAudio
        ? $"[0:a]apad,atrim=0:{durationArg}[a0];" +
          $"[1:a]apad,atrim=0:{durationArg}[a1];" +
          "[a0][a1]amix=inputs=2:duration=first:dropout_transition=0[aout]"
        : $"[1:a]apad,atrim=0:{durationArg}[aout]";
```

Replace it with:

```csharp
    // amix's own default normalization already scales each input by 1/inputs
    // (i.e. 0.5 each for two inputs, which is exactly what this plugin
    // produced before mixBalance existed) - to make mixBalance a genuine
    // 0..1 weighting rather than stacking a second halving on top of that
    // default, disable amix's normalization (normalize=0) and apply the
    // balance explicitly instead. At mixBalance 0.5 this reproduces the
    // exact same 0.5/0.5 mix as before, so today's default behaviour is
    // unchanged.
    string videoVolumeArg = (1.0 - mixBalance).ToString("0.###", CultureInfo.InvariantCulture);
    string mixdownVolumeArg = mixBalance.ToString("0.###", CultureInfo.InvariantCulture);

    string filterComplex = videoHasAudio
        ? $"[0:a]apad,atrim=0:{durationArg},volume={videoVolumeArg}[a0];" +
          $"[1:a]apad,atrim=0:{durationArg},volume={mixdownVolumeArg}[a1];" +
          "[a0][a1]amix=inputs=2:duration=first:dropout_transition=0:normalize=0[aout]"
        : $"[1:a]apad,atrim=0:{durationArg}[aout]";
```

(When the input video has no audio of its own, there's nothing to balance against, so that branch is left untouched - `mixBalance` simply has no effect in that case, matching today's behaviour.)

- [ ] **Step 3: Add progress reporting to the ffmpeg invocation**

Still in `Program.cs`, find the existing `ffmpegArgs` list:

```csharp
    var ffmpegArgs = new List<string>
    {
        "-y",
        "-i", videoPath,
        "-i", mixdownPath,
        "-filter_complex", filterComplex,
        "-map", "0:v",
        "-map", "[aout]",
        "-c:v", "copy",
        "-c:a", "aac",
        "-t", durationArg,
        outputPath
    };

    var (exitCode, _, stderrText) = RunProcess(ffmpegPath, ffmpegArgs);
```

Replace those two statements with:

```csharp
    var ffmpegArgs = new List<string>
    {
        "-y",
        "-i", videoPath,
        "-i", mixdownPath,
        "-filter_complex", filterComplex,
        "-map", "0:v",
        "-map", "[aout]",
        "-c:v", "copy",
        "-c:a", "aac",
        "-t", durationArg,
        "-nostats",
        "-progress", "pipe:1",
        outputPath
    };

    var (exitCode, stderrText) = RunFfmpegWithProgress(ffmpegPath, ffmpegArgs, duration);
```

Then add a new function alongside the existing `RunProcess` (place it directly after `RunProcess`'s closing brace, at the bottom of the file):

```csharp
// Like RunProcess, but streams ffmpeg's own stdout (populated by
// "-progress pipe:1" as key=value lines) while ffmpeg runs, converting
// each out_time= line into a KANADE_DAW "PROGRESS:<0-100>" line printed
// to THIS process's own stdout - which is what FormatExtensionRunner.cpp's
// reader thread drains into KANADE DAW's TaskPanel progress bar. ffmpeg's
// stderr is still captured in full for the failure-path error message,
// exactly as RunProcess already does.
static (int ExitCode, string Stderr) RunFfmpegWithProgress(string ffmpegPath, List<string> arguments,
    double totalDurationSeconds)
{
    var startInfo = new ProcessStartInfo(ffmpegPath)
    {
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        UseShellExecute = false,
    };
    foreach (var arg in arguments)
        startInfo.ArgumentList.Add(arg);

    using var process = Process.Start(startInfo)
        ?? throw new InvalidOperationException($"could not start \"{ffmpegPath}\"");

    var stderrTask = process.StandardError.ReadToEndAsync();

    var progressTask = Task.Run(async () =>
    {
        string? line;
        while ((line = await process.StandardOutput.ReadLineAsync()) != null)
        {
            if (!line.StartsWith("out_time="))
                continue;

            if (!TimeSpan.TryParse(line.Substring("out_time=".Length), CultureInfo.InvariantCulture,
                    out TimeSpan elapsed))
                continue;

            int percent = totalDurationSeconds > 0
                ? (int)Math.Clamp(elapsed.TotalSeconds / totalDurationSeconds * 100.0, 0, 100)
                : 0;

            Console.WriteLine($"PROGRESS:{percent}");
            Console.Out.Flush();
        }
    });

    Task.WaitAll(stderrTask, progressTask);
    process.WaitForExit();

    return (process.ExitCode, stderrTask.Result);
}
```

- [ ] **Step 4: Build the plugin**

```powershell
cd "E:\MIDIDAW\Extensions-Examples\video-audio-mux\src"
dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:PublishTrimmed=false
```

Expected: builds with no errors. Copy the published `video-audio-mux.exe` over `E:\MIDIDAW\Extensions-Examples\video-audio-mux\video-audio-mux.exe` (same convention used when this plugin was first built).

- [ ] **Step 5: Manual verification (this is also the deferred Task 4 verification - both happen together here)**

Ensure the exe from Tasks 1-5 (KANADE DAW itself) is the one at `E:\MIDIDAW\KANADE DAW.exe` (rebuild and copy it over if it isn't already, per Task 4 Step 6). Then, from KANADE DAW:

1. Open System Plugins > Video + Audio Mux (export). Pick a video file and an output path.
2. Confirm the mixdown render's own progress panel appears first (unchanged from before this plan), then the new settings dialog opens with one "音声ミックスバランス" slider at 0.5.
3. Click OK. Confirm the "Video + Audio Mux running..." `TaskPanel` now shows a moving progress bar (not just an indeterminate spinner) as ffmpeg encodes, and that the output plays with both audio sources audible.
4. Re-run with the slider dragged to 0.0 (video-only) and 1.0 (DAW-only); confirm the output's audio balance shifts accordingly in each case.
5. Re-run once more and click Cancel on the `TaskPanel` mid-encode; confirm the ffmpeg process disappears from Task Manager promptly (not after waiting out any timeout) and no "Export failed" dialog appears.

- [ ] **Step 6: Commit**

```powershell
git add Extensions-Examples/video-audio-mux/manifest.json Extensions-Examples/video-audio-mux/src/Program.cs Extensions-Examples/video-audio-mux/video-audio-mux.exe
git commit -m "Add a mix-balance setting and progress reporting to the video-audio-mux plugin"
```
