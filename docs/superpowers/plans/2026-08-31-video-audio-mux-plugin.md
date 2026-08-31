# Video + Audio Mux Plugin Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a general-purpose `additionalInputs` mechanism to KANADE DAW's format extension manifest (environment-variable-based, for inputs the existing `--export <dawproject> <output>` two-path contract has no room for), then build a self-contained example plugin — `video-audio-mux` — that uses it to combine an existing video file with the DAW's rendered mixdown, via a bundled `ffmpeg.exe`.

**Architecture:** Three focused C++ changes inside KANADE DAW's existing format-extension system (manifest parsing, the runner's env-var scope, and the export UI flow that gathers the new inputs), followed by a standalone .NET 8 console app (matching `Extensions-Examples/dawproject-dump/` and `reaper-bridge/`'s established shape exactly) that shells out to a bundled `ffmpeg.exe`.

**Tech Stack:** C++20 / JUCE 8 (existing app), .NET 8 console app + bundled `ffmpeg.exe` (new plugin).

**Spec:** `E:\MIDIDAW\docs\superpowers\specs\2026-08-31-video-audio-mux-design.md`

## Global Constraints

- Junction: always build/run through `E:\MIDIDAW`, never `E:\MIDI&DAW` directly (the `&` breaks MSBuild).
- Build: `cmake --build build --config Debug` from `E:\MIDIDAW`. Test: `build/ScoreSmith_artefacts/Debug/"KANADE DAW.exe" --run-tests`, read `test-results.txt` next to it (no stdout on this GUI-subsystem binary).
- Every task must leave the full test suite green (currently 2,709 tests) before its commit.
- `additionalInputs` is optional and defaults to empty — the two existing example extensions' manifests (`Extensions-Examples/dawproject-dump/manifest.json`, `Extensions-Examples/reaper-bridge/manifest.json`) must keep working unchanged; do not edit them.
- No placeholder/TODO code. No behavior beyond what's specified.

---

### Task 1: `additionalInputs` manifest parsing

**Files:**
- Modify: `Source/Extensions/FormatExtension.h`
- Modify: `Source/Extensions/FormatExtension.cpp`
- Test: `Source/Extensions/FormatExtensionTests.cpp`

**Interfaces:**
- Consumes: nothing new (parses `juce::var`/`juce::DynamicObject`, already a dependency).
- Produces:
  ```cpp
  enum class AdditionalInputKind { userFile, mixdownRender };

  struct AdditionalInput
  {
      AdditionalInputKind kind = AdditionalInputKind::userFile;
      juce::String envVar;
      juce::String prompt;       // userFile only, empty for mixdownRender
      juce::String fileFilter;   // userFile only, empty for mixdownRender
  };

  struct FormatExtension
  {
      // ... existing fields unchanged ...
      std::vector<AdditionalInput> additionalInputs;   // new; empty by default
  };
  ```
  Task 2 and Task 3 both read `extension.additionalInputs`.

- [ ] **Step 1: Write the failing tests**

Add to `Source/Extensions/FormatExtensionTests.cpp`, right after the existing
`"parseFormatExtensionManifest defaults direction to both and version to empty"`
test (after its closing `}` at line 70):

```cpp
        beginTest ("parseFormatExtensionManifest parses additionalInputs of both kinds");
        {
            auto folder = tempRoot.getChildFile ("additional-inputs");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.video-mux",
                "name": "Video Mux",
                "fileExtension": "mp4",
                "direction": "export",
                "executable": "tool.exe",
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
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (parseFormatExtensionManifest (folder, ext, warning), warning);
            expectEquals ((int) ext.additionalInputs.size(), 2);

            expect (ext.additionalInputs[0].kind == AdditionalInputKind::userFile);
            expectEquals (ext.additionalInputs[0].envVar, juce::String ("KANADE_DAW_VIDEO_FILE"));
            expectEquals (ext.additionalInputs[0].prompt, juce::String ("Video file to combine with"));
            expectEquals (ext.additionalInputs[0].fileFilter, juce::String ("*.mp4"));

            expect (ext.additionalInputs[1].kind == AdditionalInputKind::mixdownRender);
            expectEquals (ext.additionalInputs[1].envVar, juce::String ("KANADE_DAW_MIXDOWN_WAV"));
        }

        beginTest ("parseFormatExtensionManifest defaults additionalInputs to empty when absent");
        {
            auto folder = tempRoot.getChildFile ("no-additional-inputs");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.plain",
                "name": "Plain",
                "fileExtension": "xyz",
                "executable": "tool.exe"
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (parseFormatExtensionManifest (folder, ext, warning), warning);
            expect (ext.additionalInputs.empty());
        }

        beginTest ("parseFormatExtensionManifest rejects an additionalInputs entry with an unknown kind");
        {
            auto folder = tempRoot.getChildFile ("bad-kind");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.bad-kind",
                "name": "Bad Kind",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "additionalInputs": [
                    { "kind": "somethingElse", "envVar": "X" }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }

        beginTest ("parseFormatExtensionManifest rejects an additionalInputs entry missing envVar");
        {
            auto folder = tempRoot.getChildFile ("missing-envvar");
            folder.createDirectory();
            folder.getChildFile ("tool.exe").replaceWithText ("");
            folder.getChildFile ("manifest.json").replaceWithText (R"JSON({
                "id": "com.example.missing-envvar",
                "name": "Missing EnvVar",
                "fileExtension": "xyz",
                "executable": "tool.exe",
                "additionalInputs": [
                    { "kind": "mixdownRender" }
                ]
            })JSON");

            FormatExtension ext;
            juce::String warning;
            expect (! parseFormatExtensionManifest (folder, ext, warning));
            expect (warning.isNotEmpty());
        }
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --config Debug` from `E:\MIDIDAW`, then
`build/ScoreSmith_artefacts/Debug/"KANADE DAW.exe" --run-tests` and read
`build/ScoreSmith_artefacts/Debug/test-results.txt`.
Expected: build error (`AdditionalInputKind`/`AdditionalInput`/`additionalInputs`
not declared) — that's the correct "fails" for a type that doesn't exist yet.
If it somehow compiles, expect these 4 new assertions to FAIL.

- [ ] **Step 3: Implement**

In `Source/Extensions/FormatExtension.h`, add right after `enum class ExtensionDirection { ... };`:

```cpp
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
```

Then add the new field to `FormatExtension`:

```cpp
    struct FormatExtension
    {
        juce::String id, name, version, fileExtension;
        ExtensionDirection direction = ExtensionDirection::both;
        juce::File executable;   // resolved absolute path
        juce::File folder;       // the extension's own folder
        std::vector<AdditionalInput> additionalInputs;   // empty by default
    };
```

Add `#include <vector>` at the top of `FormatExtension.h` if not already
present (check first — `juce_core.h` may already pull it in transitively,
but include it explicitly rather than relying on that).

In `Source/Extensions/FormatExtension.cpp`, add a parsing helper next to
the existing `parseDirection` in the anonymous namespace:

```cpp
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
```

Then, in `parseFormatExtensionManifest`, right before the final
`out.id = id; ...` block, add:

```cpp
        std::vector<AdditionalInput> additionalInputs;
        if (! parseAdditionalInputs (obj->getProperty ("additionalInputs"), additionalInputs, warningOut))
        {
            warningOut = "manifest.json in \"" + extensionFolder.getFullPathName()
                          + "\" has an invalid additionalInputs entry: " + warningOut;
            return false;
        }
```

And add `out.additionalInputs = additionalInputs;` alongside the other
`out.xxx = xxx;` assignments at the end of the function.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --config Debug` from `E:\MIDIDAW`, then
`build/ScoreSmith_artefacts/Debug/"KANADE DAW.exe" --run-tests`, check
`test-results.txt` ends with `TOTAL: <N> passed, 0 failed` where N is at
least 4 more than before this task (the whole suite must stay green, not
just the new tests).

- [ ] **Step 5: Commit**

```bash
git add Source/Extensions/FormatExtension.h Source/Extensions/FormatExtension.cpp Source/Extensions/FormatExtensionTests.cpp
git commit -m "Add additionalInputs to the format extension manifest schema"
```

---

### Task 2: Environment-variable scope in `runFormatExtensionExport`

**Files:**
- Modify: `Source/Extensions/FormatExtensionRunner.h`
- Modify: `Source/Extensions/FormatExtensionRunner.cpp`
- Test: `Source/Extensions/FormatExtensionTests.cpp`

**Interfaces:**
- Consumes: `AdditionalInput`/`AdditionalInputKind` from Task 1 (only the
  *shape* — this task doesn't gather them, Task 3 does; this task just
  accepts an already-resolved map and sets/clears the environment).
- Produces:
  ```cpp
  bool runFormatExtensionExport (const FormatExtension& extension, const Project& project,
                                  PluginManager& plugins, const juce::File& outputFile,
                                  const std::map<juce::String, juce::File>& additionalInputs,
                                  FormatExtensionRunner& runner,
                                  juce::String& errorOut, juce::StringArray& warningsOut);
  ```
  Task 3 calls this with the map it gathered from UI/render steps.
  `FormatExtensionRunner::run()`'s own interface is unchanged - existing
  fake runners in tests need no changes to their `run()` override.

- [ ] **Step 1: Write the failing tests**

The existing three calls to `io::runFormatExtensionExport(...)` in
`FormatExtensionTests.cpp` (search for `io::runFormatExtensionExport (ext, project, plugins,`)
each need a `{}` (empty map) inserted as the new 5th argument, e.g. the
first one becomes:

```cpp
            const bool ok = io::runFormatExtensionExport (ext, project, plugins, outputFile, {},
                                                           runner, error, warnings);
```

Apply the same `, {},` insertion (before the runner argument) to the other
two existing calls in the file (search for `io::runFormatExtensionExport (ext, project, plugins,` -
there are 3 total, one per `beginTest` block under "runFormatExtensionExport ...").

Then add a new test, after the existing
`"runFormatExtensionExport writes a temp .dawproject, invokes the runner, and cleans up"`
test:

```cpp
        beginTest ("runFormatExtensionExport sets additionalInputs as environment variables during the run, and clears them after");
        {
            struct EnvCheckingRunner final : public FormatExtensionRunner
            {
                juce::String observedDuringRun;

                bool run (const juce::StringArray&, const juce::File& expectedOutput, juce::String&) override
                {
                    observedDuringRun = juce::SystemStats::getEnvironmentVariable ("KANADE_DAW_TEST_VAR", "<unset>");
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

            const auto outputFile = tempRoot.getChildFile ("export-envvar-out.test");
            outputFile.deleteFile();

            const auto probeFile = tempRoot.getChildFile ("probe-value.txt");
            const std::map<juce::String, juce::File> additionalInputs { { "KANADE_DAW_TEST_VAR", probeFile } };

            juce::String error;
            juce::StringArray warnings;
            const bool ok = io::runFormatExtensionExport (ext, project, plugins, outputFile, additionalInputs,
                                                           runner, error, warnings);

            expect (ok, error);
            expectEquals (runner.observedDuringRun, probeFile.getFullPathName());

            // Cleared once the export call has returned - must not leak into
            // whatever KANADE DAW (or the next extension invocation) does next.
            expectEquals (juce::SystemStats::getEnvironmentVariable ("KANADE_DAW_TEST_VAR", "<unset>"),
                          juce::String ("<unset>"));
        }
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --config Debug` from `E:\MIDIDAW`.
Expected: build error (extra argument to `runFormatExtensionExport` doesn't
match the old 7-parameter signature) on the three existing calls plus the
new test.

- [ ] **Step 3: Implement**

In `Source/Extensions/FormatExtensionRunner.h`, add `#include <map>` near
the top, and change the export declaration to:

```cpp
    /** Export: writes `project` to a temp .dawproject, sets each
        `additionalInputs` entry as an environment variable on the KANADE
        DAW process for the duration of the call (cleared again before
        returning, success or failure), invokes the extension with
        `--export <temp.dawproject> <outputFile>` via `runner`, and reports
        failure via errorOut/warningsOut using the same two-signal contract
        exportDawProject already uses. The temp .dawproject is always
        deleted before returning, success or not. */
    bool runFormatExtensionExport (const FormatExtension& extension, const Project& project,
                                    PluginManager& plugins, const juce::File& outputFile,
                                    const std::map<juce::String, juce::File>& additionalInputs,
                                    FormatExtensionRunner& runner,
                                    juce::String& errorOut, juce::StringArray& warningsOut);
```

In `Source/Extensions/FormatExtensionRunner.cpp`, add `#include <cstdlib>`
near the top, add this to the anonymous namespace (alongside
`extensionTimeoutMs`):

```cpp
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
            explicit ScopedEnvVars (const std::map<juce::String, juce::File>& vars)
            {
                for (const auto& [name, file] : vars)
                {
                    setEnvVar (name, file.getFullPathName());
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
```

Then change `runFormatExtensionExport`'s definition:

```cpp
    bool runFormatExtensionExport (const FormatExtension& extension, const Project& project,
                                    PluginManager& plugins, const juce::File& outputFile,
                                    const std::map<juce::String, juce::File>& additionalInputs,
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

        bool ok = false;
        {
            const ScopedEnvVars scopedVars (additionalInputs);
            ok = runner.run (args, outputFile, errorOut);
        }

        tempDawProject.deleteFile();
        return ok;
    }
```

(`runFormatExtensionImport` is unchanged - `additionalInputs` is an
export-only concept for now, per the spec.)

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --config Debug` from `E:\MIDIDAW`, then
`build/ScoreSmith_artefacts/Debug/"KANADE DAW.exe" --run-tests`, check
`test-results.txt` ends with `TOTAL: <N> passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add Source/Extensions/FormatExtensionRunner.h Source/Extensions/FormatExtensionRunner.cpp Source/Extensions/FormatExtensionTests.cpp
git commit -m "Set additionalInputs as environment variables around a format extension export"
```

---

### Task 3: Gather `additionalInputs` before invoking an export extension

**Files:**
- Modify: `Source/UI/MainComponent.cpp`

**Interfaces:**
- Consumes: `FormatExtension::additionalInputs` (Task 1),
  `runFormatExtensionExport`'s new signature (Task 2),
  `AudioEngine::renderToFile()` (existing, unchanged - same call
  `exportAudio()` already makes: `renderToFile(file, 0.0, project->endBeats(), bitDepth, sampleRate, false, progressFn)`).
- Produces: nothing new for other tasks - this is the UI leaf that ties
  Tasks 1 and 2 into the existing export menu flow.

This task has no new automated test (it's UI glue calling
`juce::FileChooser` and `AudioEngine::renderToFile`, the same category as
`exportAudio()` itself, which also has no dedicated test - covered by
manual verification instead, consistent with how this codebase already
draws that line).

- [ ] **Step 1: Read the current implementation**

Read `Source/UI/MainComponent.cpp` around line 953 (`exportViaExtension`)
before editing - the exact surrounding code (the `chooser`/`task`/`ctx`
member names, `TRANS`, `showDawProjectWarnings`) must match what's
actually there, since this plan was written against a snapshot of it.

- [ ] **Step 2: Implement**

Replace `exportViaExtension`'s body (from `void exportViaExtension (const FormatExtension& extension)`
through its closing `}`) with:

```cpp
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

                gatherAdditionalInputs (extension, 0, {}, [this, extension, file] (std::map<juce::String, juce::File> resolved)
                {
                    finishExportViaExtension (extension, file, std::move (resolved));
                });
            });
        }

        /** Resolves extension.additionalInputs[index..end] one at a time -
            userFile via a chained FileChooser, mixdownRender via a direct
            renderToFile() call - then calls `then` with everything gathered
            so far merged in. Cancelling any userFile chooser silently drops
            the whole export (matches cancelling the output-file chooser
            above). Recursion depth is bounded by additionalInputs.size(),
            which is a handful at most - not a stack-depth concern. */
        void gatherAdditionalInputs (const FormatExtension& extension, size_t index,
                                     std::map<juce::String, juce::File> resolvedSoFar,
                                     std::function<void (std::map<juce::String, juce::File>)> then)
        {
            if (index >= extension.additionalInputs.size())
            {
                then (std::move (resolvedSoFar));
                return;
            }

            const auto& input = extension.additionalInputs[index];

            if (input.kind == AdditionalInputKind::userFile)
            {
                chooser = std::make_unique<juce::FileChooser> (input.prompt, juce::File(), input.fileFilter);
                chooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectFiles,
                                      [this, extension, index, resolvedSoFar, then] (const juce::FileChooser& fc) mutable
                {
                    auto picked = fc.getResult();
                    if (picked.getFullPathName().isEmpty())
                        return;   // cancelled - drop the whole export, same as the output chooser

                    resolvedSoFar[extension.additionalInputs[index].envVar] = picked;
                    gatherAdditionalInputs (extension, index + 1, std::move (resolvedSoFar), std::move (then));
                });
                return;
            }

            // mixdownRender: no user prompt, but rendering can take a moment -
            // same TaskPanel background-task pattern exportAudio() already uses.
            if (ctx.project == nullptr || ctx.engine == nullptr)
                return;

            const auto renderFile = juce::File::createTempFile ("wav");
            const double endBeat = juce::jmax (1.0, ctx.project->endBeats());
            const int bitDepth = ctx.project->bitDepth;
            const double sampleRate = ctx.project->sampleRate;
            auto renderError = std::make_shared<juce::String>();

            task.run (TRANS ("Rendering..."), [this, renderFile, endBeat, bitDepth, sampleRate, renderError] (TaskPanel& t)
            {
                const auto result = ctx.engine->renderToFile (renderFile, 0.0, endBeat, bitDepth, sampleRate,
                                                              false, [&t] (float p) { t.setProgress (p); });
                if (result.failed())
                    *renderError = result.getErrorMessage();
            },
            [this, extension, index, resolvedSoFar, then, renderFile, renderError]() mutable
            {
                if (renderError->isNotEmpty())
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            TRANS ("Render failed"), *renderError,
                                                            TRANS ("OK"), &owner);
                    renderFile.deleteFile();
                    return;
                }

                resolvedSoFar[extension.additionalInputs[index].envVar] = renderFile;
                gatherAdditionalInputs (extension, index + 1, std::move (resolvedSoFar), std::move (then));
            });
        }

        void finishExportViaExtension (const FormatExtension& extension, const juce::File& outputFile,
                                       std::map<juce::String, juce::File> additionalInputs)
        {
            if (ctx.project == nullptr) return;

            juce::String error;
            juce::StringArray warnings;
            ExternalFormatExtensionRunner runner;
            const bool ok = io::runFormatExtensionExport (extension, *ctx.project, *ctx.plugins, outputFile,
                                                           additionalInputs, runner, error, warnings);

            // Any additionalInputs value that was a temp render file (not a
            // user-picked one) should not be left behind. mixdownRender is
            // currently the only kind that creates one, and its envVar names
            // are only known via extension.additionalInputs - clean up every
            // resolved path whose kind was mixdownRender.
            for (const auto& input : extension.additionalInputs)
                if (input.kind == AdditionalInputKind::mixdownRender)
                    if (auto it = additionalInputs.find (input.envVar); it != additionalInputs.end())
                        it->second.deleteFile();

            if (! ok)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                        TRANS ("Export failed"), error, TRANS ("OK"), &owner);
                return;
            }
            showDawProjectWarnings (warnings);
        }
```

- [ ] **Step 3: Build**

Run: `cmake --build build --config Debug` from `E:\MIDIDAW`.
Expected: clean build, no errors. (This task adds no new automated tests,
so there is no test-running step here - the build succeeding, plus the
full suite still being green from Task 2, is the bar.)

- [ ] **Step 4: Run the full test suite to confirm no regression**

Run: `build/ScoreSmith_artefacts/Debug/"KANADE DAW.exe" --run-tests`,
check `test-results.txt` ends with `TOTAL: <N> passed, 0 failed` (same N
as the end of Task 2 - this task adds no tests of its own).

- [ ] **Step 5: Commit**

```bash
git add Source/UI/MainComponent.cpp
git commit -m "Gather additionalInputs (file chooser / mixdown render) before invoking an export extension"
```

---

### Task 4: `video-audio-mux` example plugin

**Files:**
- Create: `Extensions-Examples/video-audio-mux/manifest.json`
- Create: `Extensions-Examples/video-audio-mux/video-audio-mux.exe` (published binary)
- Create: `Extensions-Examples/video-audio-mux/ffmpeg.exe` (bundled, LGPL build)
- Create: `Extensions-Examples/video-audio-mux/src/video-audio-mux.csproj`
- Create: `Extensions-Examples/video-audio-mux/src/Program.cs`

**Interfaces:**
- Consumes: `KANADE_DAW_VIDEO_FILE` and `KANADE_DAW_MIXDOWN_WAV`
  environment variables (set by Task 3's `finishExportViaExtension` via
  Task 2's `runFormatExtensionExport`, per the manifest this task itself
  declares). Also receives the standard
  `--export <input.dawproject> <output.mp4>` argv per the base protocol,
  positionally required but not read by this plugin's logic.
- Produces: nothing else in the codebase depends on this - it's a leaf,
  standalone example, discovered at runtime by
  `FormatExtensionManager::rescan()` like the other two.

This task does not touch `Source/`, `CMakeLists.txt`, or run
`cmake --build` - it's an independent standalone folder, same as the two
existing example extensions.

- [ ] **Step 1: manifest.json**

Create `Extensions-Examples/video-audio-mux/manifest.json`:

```json
{
  "id": "com.kanade-daw-examples.video-audio-mux",
  "name": "Video + Audio Mux",
  "version": "1.0.0",
  "fileExtension": "mp4",
  "direction": "export",
  "executable": "video-audio-mux.exe",
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
  ]
}
```

- [ ] **Step 2: .NET console app**

Create `Extensions-Examples/video-audio-mux/src/video-audio-mux.csproj`
(a plain .NET 8 console app - match `Extensions-Examples/reaper-bridge/src/reaper-bridge.csproj`'s
existing shape/target framework exactly, since that project already
builds and publishes cleanly in this environment).

Create `Extensions-Examples/video-audio-mux/src/Program.cs`. Required
behavior:

- Read `args[1]` (`--export`), `args[3]` (output path) - `args[2]`
  (the temp `.dawproject`) is accepted positionally but never opened or
  parsed, since this plugin's job needs no project structure, only the
  two environment-variable inputs.
- Read `KANADE_DAW_VIDEO_FILE` and `KANADE_DAW_MIXDOWN_WAV` from the
  environment (`Environment.GetEnvironmentVariable`). If either is
  missing or doesn't exist on disk, print a clear message to stderr
  (e.g. `"KANADE_DAW_VIDEO_FILE is not set or does not exist - this plugin must be invoked by KANADE DAW, not run standalone without it."`)
  and exit 1.
- Locate `ffmpeg.exe` next to this executable
  (`Path.Combine(AppContext.BaseDirectory, "ffmpeg.exe")`, NOT relying on
  `PATH`). If it's missing, print
  `"ffmpeg.exe was not found next to video-audio-mux.exe - place a Windows ffmpeg.exe build (e.g. from https://github.com/BtbN/FFmpeg-Builds) in this folder."`
  to stderr and exit 1 - never silently do nothing (per the spec's Error
  Handling section).
- Otherwise, build and run an `ffmpeg` command line via `System.Diagnostics.Process`
  that: copies the input video's video stream unchanged (`-c:v copy`),
  mixes the input video's own audio (if any) with the mixdown WAV,
  re-encodes the mixed audio to AAC, and matches the *video's own
  duration* (pad the shorter audio input with silence, trim the longer
  one - `-shortest` alone trims to the shortest of ALL inputs including
  video, which is not quite right if the video is the reference; use an
  explicit filter approach, e.g. `apad` on the shorter audio input plus
  `-t <video-duration>` on the output, where `<video-duration>` comes
  from probing the input video first via `ffprobe.exe` - if bundling
  `ffprobe.exe` alongside `ffmpeg.exe` as a second file is simpler than
  parsing `ffmpeg`'s own stderr for duration, do that; both are
  acceptable, pick whichever is more reliable once you're actually
  testing against a real sample file), writing to the output path from
  argv.
- Capture ffmpeg's stderr; if its exit code is nonzero, write that
  captured text to this program's own stderr and exit 1. If it exits 0
  and the output file exists, exit 0.
- Wrap the whole `Main` in one top-level `try/catch` (matching
  `dawproject-dump`/`reaper-bridge`'s existing pattern) - an unhandled
  .NET exception's raw stack trace in KANADE DAW's error dialog would be
  useless to a user.

- [ ] **Step 3: Obtain and place `ffmpeg.exe`**

Fetch a Windows ffmpeg build from an LGPL (not GPL) release of
BtbN/FFmpeg-Builds (github.com/BtbN/FFmpeg-Builds/releases - pick an
`...-lgpl-...` asset, not a `-gpl-...` one, since an LGPL build is the
one safe to redistribute inside this plugin's folder without pulling in
GPL-only components) or from gyan.dev's "essentials" LGPL build. Extract
`ffmpeg.exe` (and `ffprobe.exe` too, if Step 2 ended up using it) directly
into `Extensions-Examples/video-audio-mux/`, next to `manifest.json`.

If this environment has no outbound network access to actually fetch
it: leave `ffmpeg.exe` absent, and confirm Step 2's "missing ffmpeg.exe"
error path (above) is what a user sees - do not fabricate a fake/stub
binary. Report this limitation clearly rather than silently shipping a
non-functional plugin.

- [ ] **Step 4: Publish the .NET app**

From `Extensions-Examples/video-audio-mux/src/`:

```
dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:PublishTrimmed=false
```

Copy (not move - keep `src/` around) the resulting single exe from the
publish output directory to
`Extensions-Examples/video-audio-mux/video-audio-mux.exe`, sitting
directly next to `manifest.json`.

- [ ] **Step 5: End-to-end test**

Since driving KANADE DAW's own GUI isn't practical here, test directly
from the command line, mirroring exactly how `reaper-bridge.exe` was
verified:

1. Obtain or synthesize a small sample `.mp4` (a few seconds, with an
   audible test tone as its own audio track) and a small sample `.wav`
   (a different test tone, a different length than the video - to
   exercise the pad/trim duration-matching logic both ways: once with a
   WAV shorter than the video, once longer).
2. Set the environment variables and run the exe directly, e.g. (PowerShell):
   ```
   $env:KANADE_DAW_VIDEO_FILE = "C:\path\to\sample.mp4"
   $env:KANADE_DAW_MIXDOWN_WAV = "C:\path\to\sample.wav"
   .\video-audio-mux.exe --export fake.dawproject out.mp4
   ```
   (`fake.dawproject` can be any existing file, or even a nonexistent
   one, since Step 2's implementation never opens it - confirm that's
   actually true by testing with a nonexistent path there too.)
3. Confirm exit code 0 and that `out.mp4` was created.
4. Play `out.mp4` in a normal video player (or inspect via `ffprobe.exe`
   if available) and confirm: the video plays with no visible quality
   loss (it was stream-copied), both the original video's own audio and
   the mixdown's audio are audible together, and the output's total
   duration matches the *input video's* duration in both the
   shorter-WAV and longer-WAV cases.
5. Confirm the missing-env-var and missing-ffmpeg.exe error paths from
   Step 2 each produce a clean one-line stderr message and exit 1, not a
   crash or stack trace (temporarily rename `ffmpeg.exe` aside to test
   the latter, then restore it).

Report the exact commands run and their output in this task's final
report - the same level of evidence `reaper-bridge`'s own end-to-end
test provided.

- [ ] **Step 6: No commit step for this task**

`Extensions-Examples/` is outside KANADE DAW's own git history in the
sense that it's a sibling standalone-tools area, not wired into
`CMakeLists.txt` - but it lives inside the same repository, so it should
still be committed the same way the two existing example extensions
were. Run:

```bash
git add Extensions-Examples/video-audio-mux/manifest.json Extensions-Examples/video-audio-mux/src/
git add Extensions-Examples/video-audio-mux/video-audio-mux.exe
git commit -m "Add video-audio-mux example format extension (bundled ffmpeg)"
```

If `ffmpeg.exe`/`ffprobe.exe` were successfully obtained in Step 3,
include them in the same `git add`/commit too - do not `.gitignore` them
away, since the whole point of this plugin is that it's self-contained
once someone has this folder.

---

## Self-Review Notes

- **Spec coverage:** manifest schema addition (Task 1) - covered.
  Env-var set/clear around the runner call (Task 2) - covered. UI
  gathering flow for both `additionalInputs` kinds (Task 3) - covered.
  The plugin itself, including the bundled-ffmpeg fallback behavior from
  the spec's Components section (Task 4) - covered. Error Handling
  section's four cases (cancel, render failure, extension failure,
  malformed manifest) - the first three are exercised by Tasks 2-4's
  existing error paths (chooser cancel returns early with no dialog per
  Task 3 Step 2's `gatherAdditionalInputs`, render failure shows
  `AlertWindow` per the same step, extension failure is
  `runFormatExtensionExport`'s existing unchanged failure path); the
  fourth (malformed manifest) is Task 1's rejection tests.
- **Type consistency:** `AdditionalInputKind`/`AdditionalInput` defined
  in Task 1 are used with identical field names
  (`kind`/`envVar`/`prompt`/`fileFilter`) in Tasks 2 and 3.
  `runFormatExtensionExport`'s new signature (Task 2) matches exactly
  between its header declaration, its `.cpp` definition, and every call
  site added in Task 3.
- **No user-facing range picker for `mixdownRender`** (always full
  project) is a deliberate spec decision, not a gap - Task 3's
  implementation reflects that directly (`0.0` to `project->endBeats()`,
  no new UI for picking a different range).
