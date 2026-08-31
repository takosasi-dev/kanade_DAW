# Format Extension API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let third-party developers add new import/export file formats to KANADE DAW as standalone executables, discovered from user-configured folders and invoked via a documented CLI contract, converting to/from `.dawproject` as the sole interchange format.

**Architecture:** A new `Source/Extensions/` module (manifest parsing, folder discovery, process invocation) sits alongside the existing `Source/Plugins/` and `Source/IO/` modules. It never implements a second file format — it only ever writes/reads `.dawproject` via the already-shipped `Source/IO/DawProject.h`, and calls out to the extension's own executable to do the real format conversion.

**Tech Stack:** C++20 / JUCE 8.0.6, `juce::ChildProcess` for process invocation, `juce::JSON` for the manifest, `juce::UnitTest` for tests.

**Spec:** `docs/superpowers/specs/2026-08-28-format-extension-api-design.md`

## Global Constraints

- Build via the `E:\MIDIDAW` junction, never `E:\MIDI&DAW` directly — the literal `&` in the real path breaks MSBuild's generated batch scripts. `cd E:\MIDIDAW && cmake --build build --config Debug`.
- `Source/*.cpp`/`Source/*.h` are picked up by a `CONFIGURE_DEPENDS` `GLOB_RECURSE` in `CMakeLists.txt`. A **brand-new** `.cpp`/`.h` file's first build after being added needs an explicit `cmake -S . -B build` reconfigure first, or the new file silently isn't linked in.
- Test results are NOT printed to stdout (`KANADE DAW.exe` is a GUI-subsystem binary). After running `KANADE DAW.exe --run-tests`, read `<exe's own folder>\test-results.txt` (e.g. `E:\MIDIDAW\build\ScoreSmith_artefacts\Debug\test-results.txt`) — never trust a bash/PowerShell redirect of stdout, it will be empty.
- New `CommandIDs` enum values go at the true end of the enum (`Source/UI/UiSupport.h`) — inserting one in the middle repoints every saved keyboard shortcut after it.
- No test in this plan spawns a real process. Every test that needs "an extension ran" uses a fake implementing `FormatExtensionRunner`, exactly like `Source/Vocal/VocalTests.cpp`'s `FakeRunner`/`AlwaysFailRunner` do for `ResamplerRunner`.
- `juce::ChildProcess` invocation timeout is 120000ms (120s).
- User-facing prose (README section, Preferences tab labels, Help dialog title/chrome) is Japanese, via `TRANS()`, matching every other string in this app and matching `README.md`, which is entirely Japanese. Only the Help dialog's technical reference block (manifest JSON, CLI syntax, field names) stays as literal English/code — that's the third-party developer contract itself, not app UI chrome.
- `Settings::getExtensionScanPaths()`/`setExtensionScanPaths()` follow the exact `getPluginScanPaths()`/`setPluginScanPaths()` shape (`Source/Core/Settings.h:60`, `Source/Core/Settings.cpp:90-98`) — newline-joined `juce::StringArray`, via the existing private `splitPaths()` helper.

**Ruling (made while writing this plan, not in the spec):** the spec said `MainComponent::Impl` owns the one `FormatExtensionManager`. Writing the Preferences tab task revealed it needs the *same* discovered-extension list `MainComponent`'s File menu uses (so a folder added in Preferences shows up in the menu without restarting), and `PreferencesDialog` is a separate class from `MainComponent` that only ever receives `AppContext&`. Moving `FormatExtensionManager` onto `AppContext` (exactly alongside `plugins`, `settings`, etc. — Task 2 below) is a smaller, more consistent fix than threading a second reference through both classes' constructors. Cost if wrong: a one-line member-location change, nothing downstream depends on which object owns it.

---

### Task 1: FormatExtension data model + manifest parser

**Files:**
- Create: `Source/Extensions/FormatExtension.h`
- Create: `Source/Extensions/FormatExtension.cpp`
- Test: `Source/Extensions/FormatExtensionTests.cpp`

**Interfaces:**
- Produces: `ss::ExtensionDirection` (enum class: `importOnly`, `exportOnly`, `both`), `ss::FormatExtension` (struct: `juce::String id, name, version, fileExtension`; `ExtensionDirection direction`; `juce::File executable, folder`), `ss::parseFormatExtensionManifest (const juce::File& extensionFolder, FormatExtension& out, juce::String& warningOut) -> bool`.

- [ ] **Step 1: Write the failing tests**

Create `Source/Extensions/FormatExtensionTests.cpp`:

```cpp
#include "Extensions/FormatExtension.h"

namespace ss
{

class FormatExtensionUnitTests final : public juce::UnitTest
{
public:
    FormatExtensionUnitTests() : juce::UnitTest ("ScoreSmith Extensions", "ScoreSmith") {}

    void runTest() override
    {
        const auto tempRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("ScoreSmithFormatExtensionTest");
        tempRoot.deleteRecursively();
        tempRoot.createDirectory();

        beginTest ("parseFormatExtensionManifest parses a valid manifest");
        {
            auto folder = tempRoot.getChildFile ("valid");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.reason-export",
                "name": "Reason Project Export",
                "version": "1.0.0",
                "fileExtension": "reason",
                "direction": "export",
                "executable": "tool.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            const bool ok = parseFormatExtensionManifest (folder, ext, warning);

            expect (ok, warning);
            expectEquals (ext.id, juce::String ("com.example.reason-export"));
            expectEquals (ext.name, juce::String ("Reason Project Export"));
            expectEquals (ext.version, juce::String ("1.0.0"));
            expectEquals (ext.fileExtension, juce::String ("reason"));
            expect (ext.direction == ExtensionDirection::exportOnly);
            expectEquals (ext.executable.getFullPathName(), folder.getChildFile ("tool.exe").getFullPathName());
            expectEquals (ext.folder.getFullPathName(), folder.getFullPathName());
        }

        beginTest ("parseFormatExtensionManifest defaults direction to both and version to empty");
        {
            auto folder = tempRoot.getChildFile ("defaults");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.minimal",
                "name": "Minimal",
                "fileExtension": "xyz",
                "executable": "tool.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (parseFormatExtensionManifest (folder, ext, warning), warning);
            expect (ext.direction == ExtensionDirection::both);
            expectEquals (ext.version, juce::String());
        }

        beginTest ("parseFormatExtensionManifest fails with a warning when manifest.json is missing");
        {
            auto folder = tempRoot.getChildFile ("no-manifest");
            folder.createDirectory();

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest fails with a warning on invalid JSON");
        {
            auto folder = tempRoot.getChildFile ("bad-json");
            folder.createDirectory();
            folder.getChildFile ("manifest.json").replaceWithText ("not json at all {{{");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest fails with a warning when a required field is missing");
        {
            auto folder = tempRoot.getChildFile ("missing-field");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "name": "No Id",
                "fileExtension": "xyz",
                "executable": "tool.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest fails with a warning when the executable does not exist");
        {
            auto folder = tempRoot.getChildFile ("missing-exe");
            folder.createDirectory();
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.ghost",
                "name": "Ghost",
                "fileExtension": "xyz",
                "executable": "does-not-exist.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        tempRoot.deleteRecursively();
    }
};

static FormatExtensionUnitTests formatExtensionUnitTests;

}
```

- [ ] **Step 2: Also create the header and an empty .cpp so the project configures**

Create `Source/Extensions/FormatExtension.h`:

```cpp
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
```

Create `Source/Extensions/FormatExtension.cpp` with just the include (implementation comes in Step 4):

```cpp
#include "Extensions/FormatExtension.h"
```

- [ ] **Step 3: Reconfigure CMake and run the tests to verify they fail**

```bash
cd /e/MIDIDAW && cmake -S . -B build
cmake --build build --config Debug
"./build/ScoreSmith_artefacts/Debug/KANADE DAW.exe" --run-tests
```

Read `E:\MIDIDAW\build\ScoreSmith_artefacts\Debug\test-results.txt` — expect the new "ScoreSmith Extensions" tests present and FAILING (link error is also acceptable at this point since `parseFormatExtensionManifest` isn't implemented yet — if so, implement Step 4 before re-running).

- [ ] **Step 4: Implement `parseFormatExtensionManifest`**

Replace the contents of `Source/Extensions/FormatExtension.cpp`:

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
        if (! executable.existsAsFile())
        {
            warningOut = "manifest.json in \"" + extensionFolder.getFullPathName()
                          + "\" names an executable that does not exist: \""
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
```

- [ ] **Step 5: Rebuild and verify all new tests pass**

```bash
cd /e/MIDIDAW && cmake --build build --config Debug
"./build/ScoreSmith_artefacts/Debug/KANADE DAW.exe" --run-tests
```

Read `test-results.txt`. Expect the 6 new "ScoreSmith Extensions" tests passing, and the aggregate `TOTAL:` line's pass count higher than the pre-task baseline by exactly 6 (independently confirm this from the file — do not trust only a self-report).

- [ ] **Step 6: Commit**

```bash
cd "/e/MIDI&DAW"
git add Source/Extensions/FormatExtension.h Source/Extensions/FormatExtension.cpp Source/Extensions/FormatExtensionTests.cpp
git commit -m "Add FormatExtension data model and manifest.json parser"
```

---

### Task 2: FormatExtensionManager (folder discovery) + Settings + AppContext wiring

**Files:**
- Modify: `Source/Core/Settings.h`
- Modify: `Source/Core/Settings.cpp`
- Modify: `Source/Core/AppContext.h`
- Modify: `Source/Core/AppContext.cpp`
- Create: `Source/Extensions/FormatExtensionManager.h`
- Create: `Source/Extensions/FormatExtensionManager.cpp`
- Test: `Source/Extensions/FormatExtensionTests.cpp` (append)

**Interfaces:**
- Consumes: `ss::FormatExtension`, `ss::parseFormatExtensionManifest` (Task 1).
- Produces: `Settings::getExtensionScanPaths() -> juce::StringArray` / `setExtensionScanPaths (const juce::StringArray&)`; `ss::FormatExtensionManager` (methods: `void rescan (const juce::StringArray& scanPaths, juce::StringArray& warningsOut)`, `const std::vector<FormatExtension>& getExtensions() const noexcept`, `std::vector<const FormatExtension*> matching (ExtensionDirection wanted) const`); `AppContext::formatExtensions` (`std::unique_ptr<FormatExtensionManager>`, constructed in `AppContext::AppContext()`, never rescanned automatically there — callers rescan explicitly, see Task 4).

- [ ] **Step 1: Write the failing test for `FormatExtensionManager::rescan`/`matching`**

Append to `Source/Extensions/FormatExtensionTests.cpp`, inside `runTest()` before the final `tempRoot.deleteRecursively();` line, add a new include at the top of the file first:

```cpp
#include "Extensions/FormatExtensionManager.h"
```

Then append these test blocks before `tempRoot.deleteRecursively();`:

```cpp
        beginTest ("FormatExtensionManager::rescan discovers valid extensions across multiple scan paths");
        {
            auto scanRootA = tempRoot.getChildFile ("scanA");
            auto scanRootB = tempRoot.getChildFile ("scanB");
            scanRootA.createDirectory();
            scanRootB.createDirectory();

            auto extA = scanRootA.getChildFile ("ext-a");
            extA.createDirectory();
            extA.getChildFile ("tool.exe").replaceWithText ("");
            extA.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.a", "name": "Ext A", "fileExtension": "aaa",
                "direction": "export", "executable": "tool.exe"
            })JSON");

            auto extB = scanRootB.getChildFile ("ext-b");
            extB.createDirectory();
            extB.getChildFile ("tool.exe").replaceWithText ("");
            extB.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.b", "name": "Ext B", "fileExtension": "bbb",
                "direction": "import", "executable": "tool.exe"
            })JSON");

            // A sibling folder with no manifest.json at all - not an extension,
            // must not produce a warning.
            scanRootA.getChildFile ("just-a-folder").createDirectory();

            // A sibling folder WITH a manifest.json that's broken - must warn
            // but not block discovering ext-a/ext-b.
            auto broken = scanRootA.getChildFile ("broken");
            broken.createDirectory();
            broken.getChildFile ("manifest.json").replaceWithText ("not json {{{");

            FormatExtensionManager manager;
            juce::StringArray warnings;
            manager.rescan ({ scanRootA.getFullPathName(), scanRootB.getFullPathName() }, warnings);

            expectEquals ((int) manager.getExtensions().size(), 2);
            expectEquals (warnings.size(), 1);

            const auto exporters = manager.matching (ExtensionDirection::exportOnly);
            expectEquals ((int) exporters.size(), 1);
            expectEquals (exporters.front()->id, juce::String ("com.example.a"));

            const auto importers = manager.matching (ExtensionDirection::importOnly);
            expectEquals ((int) importers.size(), 1);
            expectEquals (importers.front()->id, juce::String ("com.example.b"));
        }

        beginTest ("FormatExtensionManager::matching treats direction \"both\" as matching either query");
        {
            auto scanRoot = tempRoot.getChildFile ("scanBoth");
            scanRoot.createDirectory();
            auto ext = scanRoot.getChildFile ("ext-both");
            ext.createDirectory();
            ext.getChildFile ("tool.exe").replaceWithText ("");
            ext.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.both", "name": "Ext Both", "fileExtension": "ccc",
                "executable": "tool.exe"
            })JSON");

            FormatExtensionManager manager;
            juce::StringArray warnings;
            manager.rescan ({ scanRoot.getFullPathName() }, warnings);

            expectEquals ((int) manager.matching (ExtensionDirection::exportOnly).size(), 1);
            expectEquals ((int) manager.matching (ExtensionDirection::importOnly).size(), 1);
        }

        beginTest ("FormatExtensionManager::rescan with an empty scan-path list is a no-op");
        {
            FormatExtensionManager manager;
            juce::StringArray warnings;
            manager.rescan ({}, warnings);
            expect (manager.getExtensions().empty());
            expect (warnings.isEmpty());
        }

        beginTest ("FormatExtensionManager::rescan replaces the previous discovery list wholesale");
        {
            auto scanRoot = tempRoot.getChildFile ("scanReplace");
            scanRoot.createDirectory();
            auto ext = scanRoot.getChildFile ("ext-one");
            ext.createDirectory();
            ext.getChildFile ("tool.exe").replaceWithText ("");
            ext.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.one", "name": "One", "fileExtension": "one",
                "executable": "tool.exe"
            })JSON");

            FormatExtensionManager manager;
            juce::StringArray warnings;
            manager.rescan ({ scanRoot.getFullPathName() }, warnings);
            expectEquals ((int) manager.getExtensions().size(), 1);

            // Rescanning an empty list must clear the previous result, not
            // append to it.
            manager.rescan ({}, warnings);
            expect (manager.getExtensions().empty());
        }
```

- [ ] **Step 2: Create the header and an empty .cpp**

Create `Source/Extensions/FormatExtensionManager.h`:

```cpp
#pragma once
#include "Extensions/FormatExtension.h"
#include <vector>

namespace ss
{
    /** Discovers format extensions across a set of user-configured folders.
        See docs/superpowers/specs/2026-08-28-format-extension-api-design.md. */
    class FormatExtensionManager
    {
    public:
        /** Rescans every immediate subfolder of every path in scanPaths for
            a manifest.json. Replaces the previous discovery list wholesale.
            A subfolder with no manifest.json is silently not an extension;
            one with a manifest.json that fails to parse adds one
            warningsOut entry and is otherwise skipped - one bad folder
            never blocks discovering the rest. */
        void rescan (const juce::StringArray& scanPaths, juce::StringArray& warningsOut);

        const std::vector<FormatExtension>& getExtensions() const noexcept { return extensions; }

        /** Extensions usable for `wanted` (importOnly or exportOnly): an
            extension whose own direction is `both` matches either query,
            one whose direction is `importOnly` matches only `importOnly`,
            etc. Passing `both` returns every extension unfiltered. */
        std::vector<const FormatExtension*> matching (ExtensionDirection wanted) const;

    private:
        std::vector<FormatExtension> extensions;
    };
}
```

Create `Source/Extensions/FormatExtensionManager.cpp` with just the include:

```cpp
#include "Extensions/FormatExtensionManager.h"
```

- [ ] **Step 3: Reconfigure, rebuild, and verify the new tests fail**

```bash
cd /e/MIDIDAW && cmake -S . -B build
cmake --build build --config Debug
"./build/ScoreSmith_artefacts/Debug/KANADE DAW.exe" --run-tests
```

Check `test-results.txt` — the new tests should fail or the build should fail to link (expected until Step 4).

- [ ] **Step 4: Implement `FormatExtensionManager`**

Replace `Source/Extensions/FormatExtensionManager.cpp`:

```cpp
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
            const bool matches = ext.direction == ExtensionDirection::both || ext.direction == wanted;
            if (matches)
                result.push_back (&ext);
        }
        return result;
    }
}
```

- [ ] **Step 5: Add `Settings::getExtensionScanPaths`/`setExtensionScanPaths`**

In `Source/Core/Settings.h`, add right after the `getPluginScanPaths`/`setPluginScanPaths` line (line 60):

```cpp
        /** Folders scanned for format extensions (Source/Extensions/) - same
            multi-folder shape as getPluginScanPaths(). */
        juce::StringArray getExtensionScanPaths() const;  void setExtensionScanPaths (const juce::StringArray&);
```

In `Source/Core/Settings.cpp`, add right after `setPluginScanPaths` (after line 98):

```cpp
    juce::StringArray Settings::getExtensionScanPaths() const
    {
        return splitPaths (props.getUserSettings()->getValue ("extensionScanPaths"));
    }

    void Settings::setExtensionScanPaths (const juce::StringArray& v)
    {
        props.getUserSettings()->setValue ("extensionScanPaths", v.joinIntoString ("\n"));
    }
```

- [ ] **Step 6: Add `AppContext::formatExtensions`**

In `Source/Core/AppContext.h`, add a forward declaration and the member. The file becomes:

```cpp
#pragma once
#include <juce_core/juce_core.h>
#include <memory>

namespace ss
{
    class Project;  class Settings;
    class AudioEngine;  class PluginManager;
    class Transcriber;  class Generator;  class StemSeparator;
    class FormatExtensionManager;

    /** Everything the app owns, in one place, handed to the UI by reference.
        Constructed once in Main.cpp; there is no singleton and no service
        locator - views that need a subsystem take a reference to this.        */
    struct AppContext
    {
        AppContext();
        ~AppContext();

        std::unique_ptr<Settings>      settings;
        std::unique_ptr<Project>       project;
        std::unique_ptr<AudioEngine>   engine;
        std::unique_ptr<PluginManager> plugins;
        std::unique_ptr<Transcriber>   transcriber;
        std::unique_ptr<Generator>     generator;
        std::unique_ptr<StemSeparator> stemSeparator;
        std::unique_ptr<FormatExtensionManager> formatExtensions;

        /** Empty when the audio device opened; the driver's message otherwise. */
        juce::String audioDeviceError;

        /** Replaces the current document and re-points every subsystem at it. */
        void setProject (std::unique_ptr<Project>);
    };
}
```

In `Source/Core/AppContext.cpp`, add the include and construct it in the constructor (right after `stemSeparator`'s line):

```cpp
#include "Extensions/FormatExtensionManager.h"
```

Discard the initial-scan warnings here rather than reusing `audioDeviceError` (a single specific error string with its own meaning) as a sink — nothing else in the app has anywhere to show scan warnings at startup, and Task 5's Preferences tab re-scans and displays them the moment it's opened, which is when a user would actually go looking:

```cpp
        stemSeparator = std::make_unique<StemSeparator> (*settings);
        formatExtensions = std::make_unique<FormatExtensionManager>();
        {
            juce::StringArray startupScanWarnings;   // shown in Preferences > Extensions, not at launch
            formatExtensions->rescan (settings->getExtensionScanPaths(), startupScanWarnings);
        }
```

And in the destructor, add `formatExtensions.reset();` right after `stemSeparator.reset();` (it holds no pointers into `project`/`plugins`, so exact ordering relative to those two doesn't matter, but keep it grouped with the other independent subsystems):

```cpp
        engine.reset();
        project.reset();
        stemSeparator.reset();
        formatExtensions.reset();
        generator.reset();
        transcriber.reset();
        plugins.reset();
        settings.reset();
```

- [ ] **Step 7: Reconfigure, rebuild, and verify all tests pass**

```bash
cd /e/MIDIDAW && cmake -S . -B build
cmake --build build --config Debug
"./build/ScoreSmith_artefacts/Debug/KANADE DAW.exe" --run-tests
```

Read `test-results.txt`. Expect the pass count to have grown by exactly 10 more than Task 1's checkpoint (4 new `FormatExtensionManager` tests here — note the manager test file also re-declares `manager` locals per block, all should pass), 0 failures.

- [ ] **Step 8: Commit**

```bash
cd "/e/MIDI&DAW"
git add Source/Core/Settings.h Source/Core/Settings.cpp Source/Core/AppContext.h Source/Core/AppContext.cpp Source/Extensions/FormatExtensionManager.h Source/Extensions/FormatExtensionManager.cpp Source/Extensions/FormatExtensionTests.cpp
git commit -m "Add FormatExtensionManager, extension scan-path setting, and AppContext wiring"
```

---

### Task 3: FormatExtensionRunner (process invocation) + DAWproject-mediated export/import

**Files:**
- Create: `Source/Extensions/FormatExtensionRunner.h`
- Create: `Source/Extensions/FormatExtensionRunner.cpp`
- Test: `Source/Extensions/FormatExtensionTests.cpp` (append)

**Interfaces:**
- Consumes: `ss::FormatExtension` (Task 1); `ss::io::exportDawProject`/`ss::io::importDawProject` (`Source/IO/DawProject.h`: `bool exportDawProject (const juce::File& target, const Project& project, PluginManager& plugins, juce::String& errorOut, juce::StringArray& warningsOut)`, `bool importDawProject (const juce::File& source, Project& project, PluginManager& plugins, juce::String& errorOut, juce::StringArray& warningsOut)`).
- Produces: `ss::FormatExtensionRunner` (abstract: `virtual bool run (const juce::StringArray& args, const juce::File& expectedOutput, juce::String& errorOut) = 0`), `ss::ExternalFormatExtensionRunner` (concrete, `juce::ChildProcess`-backed), `ss::io::runFormatExtensionExport (const FormatExtension&, const Project&, PluginManager&, const juce::File& outputFile, FormatExtensionRunner&, juce::String& errorOut, juce::StringArray& warningsOut) -> bool`, `ss::io::runFormatExtensionImport (const FormatExtension&, Project&, PluginManager&, const juce::File& inputFile, FormatExtensionRunner&, juce::String& errorOut, juce::StringArray& warningsOut) -> bool`.

- [ ] **Step 1: Write the failing tests using a fake runner**

Add this include near the top of `Source/Extensions/FormatExtensionTests.cpp`:

```cpp
#include "Extensions/FormatExtensionRunner.h"
#include "Core/Project.h"
#include "Plugins/PluginManager.h"
#include "Core/Settings.h"
#include "IO/DawProject.h"
```

Append these test blocks before the final `tempRoot.deleteRecursively();`:

```cpp
        beginTest ("runFormatExtensionExport writes a temp .dawproject, invokes the runner, and cleans up");
        {
            struct RecordingRunner final : public FormatExtensionRunner
            {
                juce::StringArray lastArgs;
                juce::File lastExpectedOutput;

                bool run (const juce::StringArray& args, const juce::File& expectedOutput,
                          juce::String&) override
                {
                    lastArgs = args;
                    lastExpectedOutput = expectedOutput;
                    // Simulate a well-behaved extension: create the requested output file.
                    expectedOutput.create();
                    return true;
                }
            } runner;

            Settings settings;
            PluginManager plugins (settings);
            Project project;
            project.addTrack (TrackType::audio, "Audio 1");

            FormatExtension ext;
            ext.id = "com.example.test";
            ext.name = "Test";
            ext.fileExtension = "test";
            ext.direction = ExtensionDirection::exportOnly;
            ext.executable = juce::File ("C:/fake/tool.exe");

            const auto outputFile = tempRoot.getChildFile ("export-out.test");
            outputFile.deleteFile();

            juce::String error;
            juce::StringArray warnings;
            const bool ok = runFormatExtensionExport (ext, project, plugins, outputFile, runner, error, warnings);

            expect (ok, error);
            expectEquals (runner.lastArgs.size(), 4);
            expectEquals (runner.lastArgs[0], ext.executable.getFullPathName());
            expectEquals (runner.lastArgs[1], juce::String ("--export"));
            expect (runner.lastArgs[2].endsWithIgnoreCase (".dawproject"));
            expectEquals (runner.lastArgs[3], outputFile.getFullPathName());
            expectEquals (runner.lastExpectedOutput.getFullPathName(), outputFile.getFullPathName());

            // The intermediate .dawproject was a temp file and must not be left behind.
            expect (! juce::File (runner.lastArgs[2]).existsAsFile());
        }

        beginTest ("runFormatExtensionExport fails and returns the runner's error when the runner reports failure");
        {
            struct FailingRunner final : public FormatExtensionRunner
            {
                bool run (const juce::StringArray&, const juce::File&, juce::String& errorOut) override
                {
                    errorOut = "extension exploded";
                    return false;
                }
            } runner;

            Settings settings;
            PluginManager plugins (settings);
            Project project;

            FormatExtension ext;
            ext.id = "com.example.test";
            ext.name = "Test";
            ext.fileExtension = "test";
            ext.executable = juce::File ("C:/fake/tool.exe");

            juce::String error;
            juce::StringArray warnings;
            const bool ok = runFormatExtensionExport (ext, project, plugins,
                                                       tempRoot.getChildFile ("wont-be-made.test"),
                                                       runner, error, warnings);

            expect (! ok);
            expectEquals (error, juce::String ("extension exploded"));
        }

        beginTest ("runFormatExtensionImport invokes the runner then imports the produced .dawproject");
        {
            // First, build a real .dawproject to hand back as "what the
            // extension produced" - runFormatExtensionImport must import it
            // for real, not just report success.
            Settings settings;
            PluginManager plugins (settings);
            Project sourceProject;
            sourceProject.addTrack (TrackType::audio, "Imported Track");
            const auto realDawProject = tempRoot.getChildFile ("real.dawproject");
            juce::String setupError;
            juce::StringArray setupWarnings;
            expect (io::exportDawProject (realDawProject, sourceProject, plugins, setupError, setupWarnings),
                    setupError);

            struct ProducesFileRunner final : public FormatExtensionRunner
            {
                juce::File sourceToServe;
                bool run (const juce::StringArray&, const juce::File& expectedOutput,
                          juce::String&) override
                {
                    return sourceToServe.copyFileTo (expectedOutput);
                }
            } runner;
            runner.sourceToServe = realDawProject;

            FormatExtension ext;
            ext.id = "com.example.test";
            ext.name = "Test";
            ext.fileExtension = "test";
            ext.direction = ExtensionDirection::importOnly;
            ext.executable = juce::File ("C:/fake/tool.exe");

            Project targetProject;
            juce::String error;
            juce::StringArray warnings;
            const bool ok = runFormatExtensionImport (ext, targetProject, plugins,
                                                       tempRoot.getChildFile ("input.test"),
                                                       runner, error, warnings);

            expect (ok, error);
            expect (targetProject.getTracks().size() >= 1);
            bool foundImportedTrack = false;
            for (const auto& t : targetProject.getTracks())
                if (t->name == "Imported Track")
                    foundImportedTrack = true;
            expect (foundImportedTrack, "the imported project should contain the track from the fake extension's .dawproject");
        }

        beginTest ("runFormatExtensionImport fails when the runner cannot produce a valid .dawproject");
        {
            struct MissingOutputRunner final : public FormatExtensionRunner
            {
                bool run (const juce::StringArray&, const juce::File&, juce::String& errorOut) override
                {
                    errorOut = "did not run";
                    return false;
                }
            } runner;

            Settings settings;
            PluginManager plugins (settings);
            Project project;

            FormatExtension ext;
            ext.id = "com.example.test";
            ext.name = "Test";
            ext.fileExtension = "test";
            ext.executable = juce::File ("C:/fake/tool.exe");

            juce::String error;
            juce::StringArray warnings;
            const bool ok = runFormatExtensionImport (ext, project, plugins,
                                                       tempRoot.getChildFile ("bad-input.test"),
                                                       runner, error, warnings);
            expect (! ok);
            expectEquals (error, juce::String ("did not run"));
        }
```

- [ ] **Step 2: Create the header and an empty .cpp**

Create `Source/Extensions/FormatExtensionRunner.h`:

```cpp
#pragma once
#include "Core/Project.h"
#include "Extensions/FormatExtension.h"
#include "Plugins/PluginManager.h"
#include <juce_core/juce_core.h>

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
            convention). Returns true only if the process exited 0 AND
            expectedOutput now exists; on failure, errorOut carries
            whatever the process wrote to stderr/stdout (or a generic
            timeout/exit-code message when that was empty). */
        virtual bool run (const juce::StringArray& args, const juce::File& expectedOutput,
                           juce::String& errorOut) = 0;
    };

    /** Production FormatExtensionRunner - shells out via juce::ChildProcess,
        exactly like ExternalResamplerRunner does for the UTAU resampler. */
    class ExternalFormatExtensionRunner final : public FormatExtensionRunner
    {
    public:
        bool run (const juce::StringArray& args, const juce::File& expectedOutput,
                   juce::String& errorOut) override;
    };
}

namespace ss::io
{
    /** Export: writes `project` to a temp .dawproject, invokes the
        extension with `--export <temp.dawproject> <outputFile>` via
        `runner`, and reports failure via errorOut/warningsOut using the
        same two-signal contract exportDawProject already uses. The temp
        .dawproject is always deleted before returning, success or not. */
    bool runFormatExtensionExport (const FormatExtension& extension, const Project& project,
                                    PluginManager& plugins, const juce::File& outputFile,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut);

    /** Import: invokes the extension with
        `--import <inputFile> <temp.dawproject>` via `runner`, then imports
        the extension-produced .dawproject into `project` via
        importDawProject. The temp .dawproject is always deleted before
        returning, success or not. */
    bool runFormatExtensionImport (const FormatExtension& extension, Project& project,
                                    PluginManager& plugins, const juce::File& inputFile,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut);
}
```

Create `Source/Extensions/FormatExtensionRunner.cpp` with just the include:

```cpp
#include "Extensions/FormatExtensionRunner.h"
```

- [ ] **Step 3: Reconfigure, rebuild, verify the new tests fail**

```bash
cd /e/MIDIDAW && cmake -S . -B build
cmake --build build --config Debug
"./build/ScoreSmith_artefacts/Debug/KANADE DAW.exe" --run-tests
```

Expect a link failure or failing tests in `test-results.txt` (functions not yet implemented).

- [ ] **Step 4: Implement `ExternalFormatExtensionRunner` and the export/import functions**

Replace `Source/Extensions/FormatExtensionRunner.cpp`:

```cpp
#include "Extensions/FormatExtensionRunner.h"
#include "IO/DawProject.h"

namespace ss
{
    namespace
    {
        constexpr int extensionTimeoutMs = 120000;
    }

    bool ExternalFormatExtensionRunner::run (const juce::StringArray& args, const juce::File& expectedOutput,
                                              juce::String& errorOut)
    {
        juce::ChildProcess process;
        if (! process.start (args))
        {
            errorOut = "Could not start \"" + args[0] + "\".";
            return false;
        }

        if (! process.waitForProcessToFinish (extensionTimeoutMs))
        {
            process.kill();
            errorOut = "\"" + args[0] + "\" timed out after "
                       + juce::String (extensionTimeoutMs / 1000) + "s.";
            return false;
        }

        const auto exitCode = process.getExitCode();
        const auto output = process.readAllProcessOutput().trim();

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
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut)
    {
        const auto tempDawProject = juce::File::createTempFile ("dawproject");

        if (! exportDawProject (tempDawProject, project, plugins, errorOut, warningsOut))
        {
            tempDawProject.deleteFile();
            return false;
        }

        const juce::StringArray args { extension.executable.getFullPathName(), "--export",
                                        tempDawProject.getFullPathName(), outputFile.getFullPathName() };
        const bool ok = runner.run (args, outputFile, errorOut);
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
        if (! runner.run (args, tempDawProject, errorOut))
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

- [ ] **Step 5: Rebuild and verify all new tests pass**

```bash
cd /e/MIDIDAW && cmake --build build --config Debug
"./build/ScoreSmith_artefacts/Debug/KANADE DAW.exe" --run-tests
```

Read `test-results.txt`. Expect 4 more passing tests than Task 2's checkpoint, 0 failures. If `runFormatExtensionImport invokes the runner then imports the produced .dawproject` fails on the track-name assertion, check that `Project::getTracks()`/`Track::name` match the real signatures in `Source/Core/Project.h` (used here from memory of the DAWproject feature — verify before assuming the test itself is wrong).

- [ ] **Step 6: Commit**

```bash
cd "/e/MIDI&DAW"
git add Source/Extensions/FormatExtensionRunner.h Source/Extensions/FormatExtensionRunner.cpp Source/Extensions/FormatExtensionTests.cpp
git commit -m "Add FormatExtensionRunner and DAWproject-mediated export/import"
```

---

### Task 4: MainComponent — dynamic File > Export/Import menu items

**Files:**
- Modify: `Source/UI/MainComponent.cpp`

**Interfaces:**
- Consumes: `AppContext::formatExtensions` (Task 2), `ss::FormatExtension`, `ss::ExtensionDirection` (Task 1), `ss::ExternalFormatExtensionRunner`, `ss::io::runFormatExtensionExport`/`runFormatExtensionImport` (Task 3).
- Produces: `MainComponent::Impl::exportViaExtension (const FormatExtension&)`, `MainComponent::Impl::importViaExtension (const FormatExtension&)`, `MainComponent::Impl::rescanFormatExtensions()` — all private to this file, no other task depends on their exact names, but Task 5 depends on `ctx.formatExtensions->rescan(...)` being safe to call repeatedly (already guaranteed by Task 2's "replaces wholesale" contract).

- [ ] **Step 1: Add the include**

In `Source/UI/MainComponent.cpp`, add near the other `#include`s at the top of the file:

```cpp
#include "Extensions/FormatExtensionRunner.h"
```

- [ ] **Step 2: Add the menu-id constants**

`Source/UI/MainComponent.cpp` already has a file-local `namespace ss { namespace { ... } }` block at lines 27-32 holding small constants (`menuBarHeight`, `toolbarHeight`, etc.). Add these two lines inside that same block, right after `constexpr int resizerWidth = 5;`:

```cpp
        // Hand-rolled PopupMenu ids for the dynamically-discovered format
        // extensions (their count varies at runtime, so they can't live in the
        // fixed CommandIDs enum). 1000-wide bands comfortably outlive any
        // realistic number of installed extensions and never collide with the
        // existing hand-rolled ids (20001-20003) used elsewhere in this file.
        constexpr int importExtensionMenuIdBase = 21000;
        constexpr int exportExtensionMenuIdBase = 22000;
```

- [ ] **Step 3: Add `rescanFormatExtensions()`, `exportViaExtension()`, `importViaExtension()` to `MainComponent::Impl`**

Find `void exportDawProject()` inside `MainComponent::Impl` (around line 854) and add these three new methods directly after `void importDawProject()` (around line 911, right before `void exportAudio()`):

```cpp
        void rescanFormatExtensions()
        {
            if (ctx.formatExtensions == nullptr)
                return;
            juce::StringArray warnings;   // shown in Preferences > Extensions, not here
            ctx.formatExtensions->rescan (ctx.settings->getExtensionScanPaths(), warnings);
        }

        void exportViaExtension (const FormatExtension& extension)
        {
            if (ctx.project == nullptr || ctx.plugins == nullptr)
                return;

            chooser = std::make_unique<juce::FileChooser> (extension.name,
                                                            ctx.settings->getProjectsFolder(),
                                                            "*." + extension.fileExtension);
            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                    | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this, extension] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file.getFullPathName().isEmpty() || ctx.project == nullptr) return;
                file = file.withFileExtension (extension.fileExtension);

                juce::String error;
                juce::StringArray warnings;
                ExternalFormatExtensionRunner runner;
                if (! io::runFormatExtensionExport (extension, *ctx.project, *ctx.plugins, file,
                                                    runner, error, warnings))
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Export failed"), error, TRANS ("OK"), &owner);
                    return;
                }
                showDawProjectWarnings (warnings);
            });
        }

        void importViaExtension (const FormatExtension& extension)
        {
            if (ctx.project == nullptr || ctx.plugins == nullptr)
                return;

            chooser = std::make_unique<juce::FileChooser> (extension.name, juce::File(),
                                                            "*." + extension.fileExtension);
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles,
                                  [this, extension] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file.getFullPathName().isEmpty() || ctx.project == nullptr) return;

                auto error = std::make_shared<juce::String>();
                auto warnings = std::make_shared<juce::StringArray>();
                bool ok = false;
                ExternalFormatExtensionRunner runner;

                performProjectEdit (*ctx.project, TRANS ("Import") + " " + extension.name,
                                    [this, file, extension, error, warnings, &ok, &runner]
                {
                    ok = io::runFormatExtensionImport (extension, *ctx.project, *ctx.plugins, file,
                                                       runner, *error, *warnings);
                });

                if (! ok)
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Import failed"), *error, TRANS ("OK"), &owner);
                else
                    showDawProjectWarnings (*warnings);
            });
        }
```

- [ ] **Step 4: Call `rescanFormatExtensions()` from `settingsChanged()`**

Find `void settingsChanged()` (around line 989) and add the call right after the undo-history-limit block, before the language check:

```cpp
            if (ctx.project != nullptr)
                ctx.project->getUndoManager().setMaxNumberOfStoredUnits (
                    ctx.settings->getUndoHistoryLimit() * projectSnapshotActionUnitsPerStep, 1);

            rescanFormatExtensions();

            if (ctx.settings->getLanguage() != currentLanguage)
```

- [ ] **Step 5: Add the dynamic menu items in `getMenuForIndex`**

In `getMenuForIndex`, `case 0:` (File menu), replace:

```cpp
                menu.addCommandItem (&commands, CommandIDs::importDawProject);
                menu.addSeparator();
                {
                    juce::PopupMenu exportMenu;
                    exportMenu.addCommandItem (&commands, CommandIDs::exportMidi);
                    exportMenu.addCommandItem (&commands, CommandIDs::exportAudio);
                    exportMenu.addCommandItem (&commands, CommandIDs::exportMusicXml);
                    exportMenu.addCommandItem (&commands, CommandIDs::exportDawProject);
                    menu.addSubMenu (TRANS ("Export"), exportMenu);
                }
```

with:

```cpp
                menu.addCommandItem (&commands, CommandIDs::importDawProject);

                if (impl->ctx.formatExtensions != nullptr)
                {
                    const auto importExtensions = impl->ctx.formatExtensions->matching (ExtensionDirection::importOnly);
                    for (size_t i = 0; i < importExtensions.size(); ++i)
                        menu.addItem ((int) (importExtensionMenuIdBase + i),
                                      importExtensions[i]->name + " (." + importExtensions[i]->fileExtension + ")...");
                }

                menu.addSeparator();
                {
                    juce::PopupMenu exportMenu;
                    exportMenu.addCommandItem (&commands, CommandIDs::exportMidi);
                    exportMenu.addCommandItem (&commands, CommandIDs::exportAudio);
                    exportMenu.addCommandItem (&commands, CommandIDs::exportMusicXml);
                    exportMenu.addCommandItem (&commands, CommandIDs::exportDawProject);

                    if (impl->ctx.formatExtensions != nullptr)
                    {
                        const auto exportExtensions = impl->ctx.formatExtensions->matching (ExtensionDirection::exportOnly);
                        for (size_t i = 0; i < exportExtensions.size(); ++i)
                            exportMenu.addItem ((int) (exportExtensionMenuIdBase + i),
                                                exportExtensions[i]->name + " (." + exportExtensions[i]->fileExtension + ")...");
                    }

                    menu.addSubMenu (TRANS ("Export"), exportMenu);
                }
```

- [ ] **Step 6: Dispatch the dynamic ids in `menuItemSelected`**

`menuItemSelected` currently starts with:

```cpp
    void MainComponent::menuItemSelected (int menuItemID, int)
    {
        // Command items invoke themselves through the command manager; only the
        // hand-rolled ids need handling here.
        if (menuItemID == 20001)
```

Change it to dispatch the new ranges first:

```cpp
    void MainComponent::menuItemSelected (int menuItemID, int)
    {
        if (menuItemID >= importExtensionMenuIdBase && menuItemID < exportExtensionMenuIdBase)
        {
            if (impl->ctx.formatExtensions != nullptr)
            {
                const auto extensions = impl->ctx.formatExtensions->matching (ExtensionDirection::importOnly);
                const auto index = (size_t) (menuItemID - importExtensionMenuIdBase);
                if (index < extensions.size())
                    impl->importViaExtension (*extensions[index]);
            }
            return;
        }

        if (menuItemID >= exportExtensionMenuIdBase)
        {
            if (impl->ctx.formatExtensions != nullptr)
            {
                const auto extensions = impl->ctx.formatExtensions->matching (ExtensionDirection::exportOnly);
                const auto index = (size_t) (menuItemID - exportExtensionMenuIdBase);
                if (index < extensions.size())
                    impl->exportViaExtension (*extensions[index]);
            }
            return;
        }

        // Command items invoke themselves through the command manager; only the
        // hand-rolled ids below here need handling.
        if (menuItemID == 20001)
```

(Leave the rest of the existing `if`/`else if` chain for `20001`/`20002`/`20003` exactly as it is.)

- [ ] **Step 7: Rebuild and run the full test suite (no new automated tests in this task — it's UI wiring)**

```bash
cd /e/MIDIDAW && cmake --build build --config Debug
"./build/ScoreSmith_artefacts/Debug/KANADE DAW.exe" --run-tests
```

Read `test-results.txt`. Expect the same pass count as Task 3's checkpoint (this task adds no tests, but must not break any existing ones), 0 failures.

- [ ] **Step 8: Commit**

```bash
cd "/e/MIDI&DAW"
git add Source/UI/MainComponent.cpp
git commit -m "Wire discovered format extensions into File > Export/Import menus"
```

---

### Task 5: Preferences — Extensions tab

**Files:**
- Modify: `Source/UI/PreferencesDialog.cpp`
- Modify: `Resources/lang/en.txt`
- Modify: `Resources/lang/ja.txt`

**Interfaces:**
- Consumes: `AppContext::formatExtensions` (Task 2), `Settings::getExtensionScanPaths`/`setExtensionScanPaths` (Task 2), `ss::FormatExtension`, `ss::ExtensionDirection` (Task 1).
- Produces: nothing other tasks depend on (a new `ExtensionsTab` class, registered as a new Preferences tab).

- [ ] **Step 1: Add the `ExtensionsTab` class**

In `Source/UI/PreferencesDialog.cpp`, find the end of `PluginTab` (search for the closing of that class - it ends right before the `//======================================================================\n// 6. Shortcuts` comment, i.e. right before `class ShortcutTab`). Insert a new section immediately after `PluginTab`'s closing `};` and before `ShortcutTab`:

```cpp
        //======================================================================
        // 6. Format extensions
        //======================================================================
        class ExtensionsTab final : public juce::Component
        {
        public:
            explicit ExtensionsTab (AppContext& c) : ctx (c)
            {
                setUpLabel (*this, pathsLabel, TRANS ("Scan paths"));
                pathsEditor.setMultiLine (true, false);
                pathsEditor.setReturnKeyStartsNewLine (true);
                pathsEditor.setText (ctx.settings->getExtensionScanPaths().joinIntoString ("\n"),
                                     juce::dontSendNotification);
                pathsEditor.onFocusLost = [this]
                {
                    juce::StringArray paths;
                    paths.addLines (pathsEditor.getText());
                    paths.removeEmptyStrings();
                    ctx.settings->setExtensionScanPaths (paths);
                    rescan();
                };
                addAndMakeVisible (pathsEditor);

                addPathButton.setButtonText (TRANS ("Add folder..."));
                addPathButton.onClick = [this]
                {
                    chooser = std::make_unique<juce::FileChooser> (TRANS ("Add an extensions folder"));
                    chooser->launchAsync (juce::FileBrowserComponent::openMode
                                            | juce::FileBrowserComponent::canSelectDirectories,
                                          [this] (const juce::FileChooser& fc)
                    {
                        const auto folder = fc.getResult();
                        if (! folder.isDirectory()) return;

                        auto paths = ctx.settings->getExtensionScanPaths();
                        paths.addIfNotAlreadyThere (folder.getFullPathName());
                        ctx.settings->setExtensionScanPaths (paths);
                        pathsEditor.setText (paths.joinIntoString ("\n"), juce::dontSendNotification);
                        rescan();
                    });
                };
                addAndMakeVisible (addPathButton);

                setUpNote (*this, helpNote,
                           TRANS ("See Help > \"How to build a format extension...\" for the manifest.json "
                                  "schema and command-line contract an extension must follow."));

                setUpLabel (*this, listLabel, TRANS ("Discovered extensions"));
                list.setMultiLine (true, true);
                list.setReadOnly (true);
                list.setCaretVisible (false);
                addAndMakeVisible (list);

                rescan();
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (14, 12);

                auto pathsRow = area.removeFromTop (84);
                pathsLabel.setBounds (pathsRow.removeFromLeft (labelWidth).removeFromTop (rowHeight));
                addPathButton.setBounds (pathsRow.removeFromRight (130).removeFromTop (26));
                pathsEditor.setBounds (pathsRow.withTrimmedRight (8));

                area.removeFromTop (8);
                helpNote.setBounds (area.removeFromTop (32));
                area.removeFromTop (6);
                listLabel.setBounds (area.removeFromTop (20));
                list.setBounds (area);
            }

        private:
            void rescan()
            {
                juce::StringArray warnings;
                if (ctx.formatExtensions != nullptr)
                    ctx.formatExtensions->rescan (ctx.settings->getExtensionScanPaths(), warnings);

                juce::String text;
                if (ctx.formatExtensions != nullptr)
                    for (const auto& ext : ctx.formatExtensions->getExtensions())
                        text += ext.name + " v" + (ext.version.isEmpty() ? juce::String ("-") : ext.version)
                                + " (." + ext.fileExtension + ") - " + directionLabel (ext.direction) + "\n";

                if (! warnings.isEmpty())
                {
                    text += "\n" + TRANS ("Warnings") + ":\n";
                    for (const auto& w : warnings)
                        text += w + "\n";
                }

                list.setText (text.trimEnd(), false);
            }

            static juce::String directionLabel (ExtensionDirection d)
            {
                switch (d)
                {
                    case ExtensionDirection::importOnly: return TRANS ("Import");
                    case ExtensionDirection::exportOnly: return TRANS ("Export");
                    default:                             return TRANS ("Import") + " / " + TRANS ("Export");
                }
            }

            AppContext& ctx;
            juce::Label pathsLabel, listLabel, helpNote;
            juce::TextEditor pathsEditor, list;
            juce::TextButton addPathButton;
            std::unique_ptr<juce::FileChooser> chooser;
        };
```

- [ ] **Step 2: Register the tab**

Find the block of `tabs.addTab (...)` calls (around line 1251-1258). Add the new tab right after the `"Plugins"` one:

```cpp
        tabs.addTab (TRANS ("Plugins"),        tabColour, new PluginTab (ctx), true);
        tabs.addTab (TRANS ("Extensions"),     tabColour, new ExtensionsTab (ctx), true);
```

- [ ] **Step 3: Add the new localization strings**

In `Resources/lang/en.txt`, add (matching the existing identity-mapping convention — find where `"Scan paths"`, `"Add folder..."` etc. already live near the Plugins-tab strings, and add the new ones nearby, in whatever order the file already groups its Plugins-tab block):

```
"Add an extensions folder" = "Add an extensions folder"
"Discovered extensions" = "Discovered extensions"
"Warnings" = "Warnings"
"See Help > \"How to build a format extension...\" for the manifest.json schema and command-line contract an extension must follow." = "See Help > \"How to build a format extension...\" for the manifest.json schema and command-line contract an extension must follow."
```

Check first whether `"Import"` and `"Export"` already exist as standalone entries anywhere in `en.txt`/`ja.txt` (search for `^"Import" =` / `^"Export" =`) — if they do, reuse them (do not add duplicates); if not, add:

```
"Import" = "Import"
"Export" = "Export"
```

In `Resources/lang/ja.txt`, add the matching translations in the same relative location:

```
"Add an extensions folder" = "拡張機能フォルダを追加"
"Discovered extensions" = "発見した拡張機能"
"Warnings" = "警告"
"See Help > \"How to build a format extension...\" for the manifest.json schema and command-line contract an extension must follow." = "manifest.json の書式とコマンドライン契約は Help > 「拡張機能の作り方...」を参照してください。"
```

(Only add `"Import"`/`"Export"` to `ja.txt` too if you had to add them to `en.txt` in the previous step — translate as `"インポート"` / `"エクスポート"`.)

- [ ] **Step 4: Reconfigure (language files are compiled in as binary data), rebuild, and run the full test suite**

```bash
cd /e/MIDIDAW && cmake -S . -B build
cmake --build build --config Debug
"./build/ScoreSmith_artefacts/Debug/KANADE DAW.exe" --run-tests
```

Read `test-results.txt`. Expect the same pass count as Task 4's checkpoint, 0 failures (no new automated tests — `PreferencesDialog`'s tabs have no existing unit-test precedent to follow here, matching how `PluginTab`/`ProjectDefaultsTab` etc. are also untested UI classes).

- [ ] **Step 5: Commit**

```bash
cd "/e/MIDI&DAW"
git add Source/UI/PreferencesDialog.cpp Resources/lang/en.txt Resources/lang/ja.txt
git commit -m "Add Preferences > Extensions tab (scan paths + discovered list)"
```

---

### Task 6: Help dialog + CommandIDs::showExtensionHelp + README

**Files:**
- Create: `Source/UI/ExtensionHelpDialog.h`
- Create: `Source/UI/ExtensionHelpDialog.cpp`
- Modify: `Source/UI/UiSupport.h`
- Modify: `Source/UI/MainComponent.cpp`
- Modify: `Resources/lang/en.txt`
- Modify: `Resources/lang/ja.txt`
- Modify: `README.md`

**Interfaces:**
- Consumes: nothing from earlier tasks (this dialog is self-contained static reference text).
- Produces: `ss::ExtensionHelpDialog`, `ExtensionHelpDialog::launch()`, `CommandIDs::showExtensionHelp`.

- [ ] **Step 1: Add `CommandIDs::showExtensionHelp`**

In `Source/UI/UiSupport.h`, the `CommandIDs` enum currently ends:

```cpp
            toggleAutomation,
            exportDawProject, importDawProject
        };
```

Change to:

```cpp
            toggleAutomation,
            exportDawProject, importDawProject,
            showExtensionHelp
        };
```

- [ ] **Step 2: Create `ExtensionHelpDialog`**

Create `Source/UI/ExtensionHelpDialog.h`:

```cpp
#pragma once
#include "UI/UiSupport.h"

namespace ss
{
    /** Static reference for third-party developers: the format extension
        manifest schema and CLI contract (spec:
        docs/superpowers/specs/2026-08-28-format-extension-api-design.md).
        Opened from Help > "How to build a format extension...". Unlike
        WhatsNewDialog this never changes per version - one fixed body. */
    class ExtensionHelpDialog final : public juce::Component
    {
    public:
        ExtensionHelpDialog();

        void resized() override;
        void paint (juce::Graphics&) override;

        static void launch();

    private:
        juce::Label titleLabel;
        juce::TextEditor body;
        juce::TextButton okButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExtensionHelpDialog)
    };
}
```

Create `Source/UI/ExtensionHelpDialog.cpp`:

```cpp
#include "UI/ExtensionHelpDialog.h"

namespace ss
{
    namespace
    {
        // The manifest JSON, field table, and CLI syntax stay literal
        // English/code - this is the technical contract a third-party
        // extension author builds against, the same way manifest.json's own
        // keys and the DAWproject XSD are English-only. The surrounding
        // prose is Japanese, matching every other user-facing string in
        // this app (and README.md, which is entirely Japanese).
        juce::String helpBodyText()
        {
            return juce::String (juce::CharPointer_UTF8 (
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

タイムアウトは120秒です。

最小サンプルと詳しい解説は README.md の
「拡張機能(フォーマットプラグイン)の作り方」を参照してください。)RAW"
            ));
        }
    }

    ExtensionHelpDialog::ExtensionHelpDialog()
    {
        titleLabel.setText (TRANS ("How to build a format extension"), juce::dontSendNotification);
        titleLabel.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        titleLabel.setColour (juce::Label::textColourId, palette().text);
        addAndMakeVisible (titleLabel);

        body.setMultiLine (true, true);
        body.setReadOnly (true);
        body.setCaretVisible (false);
        body.setScrollbarsShown (true);
        body.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f,
                                                      juce::Font::plain)));
        body.setColour (juce::TextEditor::backgroundColourId, palette().panelAltBg);
        body.setColour (juce::TextEditor::textColourId, palette().text);
        body.setColour (juce::TextEditor::outlineColourId, palette().outline);
        body.setText (helpBodyText(), false);
        addAndMakeVisible (body);

        okButton.setButtonText (TRANS ("OK"));
        okButton.onClick = [this]
        {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState (0);
        };
        addAndMakeVisible (okButton);

        setSize (560, 480);
    }

    void ExtensionHelpDialog::paint (juce::Graphics& g)
    {
        g.fillAll (palette().windowBg);
    }

    void ExtensionHelpDialog::resized()
    {
        auto area = getLocalBounds().reduced (14, 12);
        titleLabel.setBounds (area.removeFromTop (28));
        area.removeFromTop (8);

        auto buttonRow = area.removeFromBottom (30);
        okButton.setBounds (buttonRow.removeFromRight (90));
        area.removeFromBottom (8);

        body.setBounds (area);
    }

    void ExtensionHelpDialog::launch()
    {
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (new ExtensionHelpDialog());
        options.dialogTitle            = TRANS ("How to build a format extension");
        options.dialogBackgroundColour = palette().windowBg;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar      = true;
        options.resizable              = true;
        options.launchAsync();
    }
}
```

- [ ] **Step 3: Wire the command into `MainComponent`**

In `Source/UI/MainComponent.cpp`:

1. Add the include near the top:

```cpp
#include "UI/ExtensionHelpDialog.h"
```

2. In `getAllCommands`, add `CommandIDs::showExtensionHelp` to the `initializer_list` (anywhere in the list; append at the end for minimal diff):

```cpp
            CommandIDs::showPreferences, CommandIDs::rescanPlugins, CommandIDs::showExtensionHelp });
```

3. In `getCommandInfo`, add a new case (anywhere among the other `case CommandIDs::...` blocks — put it right after the `CommandIDs::rescanPlugins` case if one exists, otherwise near `CommandIDs::showPreferences`):

```cpp
            case CommandIDs::showExtensionHelp:
                info.setInfo (TRANS ("How to build a format extension..."),
                              TRANS ("Shows the manifest schema and command-line contract for format extensions"),
                              TRANS ("Help"), 0);
                break;
```

4. In `perform`, add:

```cpp
            case CommandIDs::showExtensionHelp: ExtensionHelpDialog::launch(); return true;
```

5. In `getMenuForIndex`, `case 6:` (Help menu) currently reads:

```cpp
            case 6:
                menu.addItem (20001, TRANS ("About KANADE DAW"));
                break;
```

Change to:

```cpp
            case 6:
                menu.addItem (20001, TRANS ("About KANADE DAW"));
                menu.addCommandItem (&commands, CommandIDs::showExtensionHelp);
                break;
```

- [ ] **Step 4: Add the new localization strings**

In `Resources/lang/en.txt`, add near the other Help/menu-adjacent strings:

```
"How to build a format extension..." = "How to build a format extension..."
"Shows the manifest schema and command-line contract for format extensions" = "Shows the manifest schema and command-line contract for format extensions"
"How to build a format extension" = "How to build a format extension"
```

In `Resources/lang/ja.txt`, add the matching translations:

```
"How to build a format extension..." = "拡張機能の作り方..."
"Shows the manifest schema and command-line contract for format extensions" = "拡張機能のマニフェスト書式とコマンドライン契約を表示します"
"How to build a format extension" = "拡張機能の作り方"
```

- [ ] **Step 5: Add the README section**

Append to the end of `README.md`:

```markdown

## 拡張機能(フォーマットプラグイン)の作り方

KANADE DAW は import/export フォーマットをサードパーティが追加できます。
1フォルダ = 1拡張機能で、`manifest.json` と実行ファイル(言語は問いません)
を同じフォルダに置くだけです。アプリ内では Help > 「拡張機能の作り方...」
でも同じ内容を確認できます。

### manifest.json

```json
{
  "id": "com.example.reason-export",
  "name": "Reason Project Export",
  "version": "1.0.0",
  "fileExtension": "reason",
  "direction": "export",
  "executable": "reason-export.exe"
}
```

| フィールド | 必須 | 説明 |
|---|---|---|
| `id` | ○ | 一意なID(逆ドメイン名推奨) |
| `name` | ○ | File メニュー・設定画面に表示される名前 |
| `version` | - | 表示用(省略可) |
| `fileExtension` | ○ | 先頭のドット無し(例: `"reason"`) |
| `direction` | - | `"import"` \| `"export"` \| `"both"`(省略時 `"both"`) |
| `executable` | ○ | 同じフォルダ内の実行ファイル名 |

### 呼び出し方

```
<executable> --export <input.dawproject> <output-file>
<executable> --import <input-file> <output.dawproject>
```

KANADE DAW は御社独自のフォーマットを一切解釈しません。相互運用フォーマット
[DAWproject](https://github.com/bitwig/dawproject)(Studio One / Bitwig /
Cubase 等が対応)との変換だけを拡張機能の実行ファイルに任せます。

終了コード `0` で成功。それ以外は失敗として扱われ、標準出力/標準エラー
出力の内容がそのまま KANADE DAW 側のエラーダイアログに表示されます。
成功終了しても出力ファイルが実際に存在しなければ失敗扱いです。
タイムアウトは120秒です。

### 最小サンプル(Windowsバッチファイル、恒等変換)

`identity.cmd`:
```batch
@echo off
copy "%2" "%3" >nul
exit /b 0
```

`manifest.json`の`executable`に`"identity.cmd"`と書けば、そのまま
`--export <in.dawproject> <out-file>` / `--import <in-file> <out.dawproject>`
の呼び出しで動きます(バッチファイルの引数は実行ファイル自身を含めず`%1`
から始まるため、`%1`が`--export`/`--import`、`%2`が入力、`%3`が出力に
なります)。実運用では`copy`の代わりに実際のフォーマット変換処理を
書いてください。

### 発見のされ方

設定 > Extensions タブでスキャン対象フォルダを登録すると、その直下の
各サブフォルダが1拡張機能として走査されます。`manifest.json` が壊れて
いる場合は警告付きでスキップされ、他の拡張機能の発見をブロックしません。
```

- [ ] **Step 6: Reconfigure, rebuild, and run the full test suite**

```bash
cd /e/MIDIDAW && cmake -S . -B build
cmake --build build --config Debug
"./build/ScoreSmith_artefacts/Debug/KANADE DAW.exe" --run-tests
```

Read `test-results.txt`. Expect the same pass count as Task 5's checkpoint, 0 failures.

- [ ] **Step 7: Commit**

```bash
cd "/e/MIDI&DAW"
git add Source/UI/ExtensionHelpDialog.h Source/UI/ExtensionHelpDialog.cpp Source/UI/UiSupport.h Source/UI/MainComponent.cpp Resources/lang/en.txt Resources/lang/ja.txt README.md
git commit -m "Add format extension Help dialog, CommandIDs::showExtensionHelp, and README docs"
```

---

## Final Verification (after all 6 tasks)

- [ ] Build both Debug and Release via the `E:\MIDIDAW` junction.
- [ ] Run `--run-tests` on both and confirm `test-results.txt` shows `0 failed` on each, with a pass count matching the sum of the per-task deltas actually observed in `test-results.txt` at each task's own verification step (Tasks 1-3 each add new assertions; Tasks 4-6 add none) — read the real numbers from those checkpoints rather than recomputing from the test code, since JUCE's `UnitTestRunner` counts individual `expect`/`expectEquals` calls, not `beginTest` blocks.
- [ ] Manually sanity-check in a text editor: `Source/UI/UiSupport.h`'s `CommandIDs` enum has `showExtensionHelp` as its last entry, nothing was inserted before it.
- [ ] Confirm neither `Source/UI/MainComponent.cpp`'s `importExtensionMenuIdBase`/`exportExtensionMenuIdBase` (21000/22000) nor `showExtensionHelp`'s `0x31xx`-range enum value collide with any existing `CommandIDs` value (they're a different numbering scheme entirely - hand-rolled PopupMenu ids vs. the `0x3100`-based enum - so this is a sanity check, not an expected fix).
