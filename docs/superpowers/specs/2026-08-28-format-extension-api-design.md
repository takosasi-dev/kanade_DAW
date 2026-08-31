# Format Extension API — Design

**Status:** Approved by user 2026-08-28. Ready for implementation planning.

## Goal

Let third-party developers add new import/export file formats to KANADE
DAW without touching its source or compiler toolchain: a "format
extension" is a folder containing a small manifest plus a standalone
executable in any language. KANADE DAW discovers it, offers it in
File > Export / File > Import, and hands it a `.dawproject` file to
convert to/from its own format.

This is the first of four extension-point categories the user's friend
asked about (AI backend swap-in, new UI panels, import/export formats,
macro/scripting). Those three are separate subsystems with different
mechanisms and are explicitly out of scope here — each gets its own
future spec.

## Why route through DAWproject

KANADE DAW already has a fully-built, tested, documented import/export
pair for `.dawproject` (`Source/IO/DawProject.h`,
`docs/superpowers/specs/2026-08-28-dawproject-interop-design.md`). Using
it as the interchange format for extensions means:

- KANADE DAW never has to understand a third-party format at all — it
  only ever converts to/from something it already fully supports.
- Extension authors write against DAWproject's spec (an established,
  open, MIT-licensed standard with its own XSD), not a bespoke KANADE
  DAW-internal schema that was never designed to be a public contract.
- The only new code this feature needs is discovery + process
  invocation — not a second file-format implementation.

## Precedent already in the codebase

- `Source/Vocal/UtauRenderer.h`'s `ResamplerRunner`/
  `ExternalResamplerRunner` split is the exact shape this feature reuses:
  an abstract interface for "run an external tool" so tests never need a
  real executable on disk, with a `juce::ChildProcess`-based production
  implementation. **Tests use a fake implementing the interface, not a
  real spawned process** — this doc originally assumed the existing test
  spawned a small real exe (echo/PowerShell) and said so during
  brainstorming; re-reading `Source/Vocal/VocalTests.cpp` (`FakeRunner`/
  `AlwaysFailRunner`) before writing this spec showed that's wrong, and
  this doc follows the real, better precedent instead.
- `Settings::getPluginScanPaths()`/`setPluginScanPaths()`
  (`Source/Core/Settings.h:60`) is the exact shape for
  `getExtensionScanPaths()`/`setExtensionScanPaths()` — multiple
  user-configured folders, scanned on demand.
- `PluginTab` (`Source/UI/PreferencesDialog.cpp:481`) is the exact shape
  for the new Preferences tab: a path editor + a read-only discovered
  list, minus the blacklist (a malformed extension is just skipped with
  a warning every rescan, not remembered).
- `WhatsNewDialog` (`Source/UI/WhatsNewDialog.h/.cpp`) is the exact shape
  for the new in-app Help dialog: title + scrollable read-only
  `juce::TextEditor`, launched via `juce::DialogWindow::LaunchOptions`.
- `MainComponent`'s `exportDawProject()`/`importDawProject()` handlers
  already show the `errorOut`/`warningsOut` dialog pattern this feature
  reuses verbatim for extension failures.

## Scope

**In scope:**
- Discovering extensions from user-configured folders (multiple, like
  plugin scan paths).
- A manifest schema (`manifest.json`) declaring id/name/version/file
  extension/direction/executable.
- Running an extension via `juce::ChildProcess` with a documented CLI
  contract, using `.dawproject` as the sole interchange format.
- File > Export / File > Import menu integration (dynamic items).
- A Preferences tab (scan paths + discovered list).
- An in-app Help dialog and a matching README section documenting how to
  build one.

**Out of scope:**
- AI backend, UI panel, and macro/scripting extension points (separate
  future specs, per the brainstorming scope-decomposition decision).
- Any format whose data doesn't map onto DAWproject's model at all (the
  same limitation DAWproject interop itself already documents).
- A blacklist/history for malformed extensions — every rescan just skips
  what doesn't parse, no persisted state.
- Sandboxing beyond process isolation (no filesystem/network
  restriction on the extension's own executable — same trust model as
  the UTAU resampler executable the user already points Settings at).

## Manifest Schema

One folder per extension, containing `manifest.json` and the executable
named inside it:

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

| Field | Required | Notes |
|---|---|---|
| `id` | yes | Reverse-DNS-style, must be non-empty; no uniqueness enforcement across extensions in v1 (a duplicate just means two menu entries — not worth the complexity of conflict UI for a first pass) |
| `name` | yes | Shown in the File menu and the Preferences list |
| `version` | no (default `""`) | Display only |
| `fileExtension` | yes | No leading dot, e.g. `"reason"` |
| `direction` | no (default `"both"`) | `"import"` \| `"export"` \| `"both"`, parsed to `ExtensionDirection::importOnly`/`exportOnly`/`both` respectively (the C++ identifiers avoid `import`/`export`, which are contextual keywords in C++20 modules) |
| `executable` | yes | Path relative to the manifest's own folder; resolved to an absolute `juce::File` at parse time and must exist as a file, or the manifest is rejected |

A manifest missing a required field, containing invalid JSON, or naming
an `executable` that doesn't exist is rejected with one warning and
otherwise ignored — one bad extension never blocks discovery of the
rest.

## Components

`Source/Extensions/FormatExtension.h`:
```cpp
namespace ss
{
    enum class ExtensionDirection { importOnly, exportOnly, both };

    struct FormatExtension
    {
        juce::String id, name, version, fileExtension;
        ExtensionDirection direction = ExtensionDirection::both;
        juce::File executable;   // resolved absolute path
        juce::File folder;       // the extension's own folder
    };

    /** Parses one extension folder's manifest.json. Returns false (with
        warningOut set) for anything malformed - never throws. */
    bool parseFormatExtensionManifest (const juce::File& extensionFolder,
                                        FormatExtension& out, juce::String& warningOut);
}
```

`Source/Extensions/FormatExtensionManager.h/.cpp`:
```cpp
namespace ss
{
    class FormatExtensionManager
    {
    public:
        /** Rescans every immediate subfolder of every path in scanPaths for
            a manifest.json. Replaces the previous discovery list wholesale -
            same "rescan is authoritative" contract PluginManager uses.
            Malformed entries go into warningsOut, one bad folder never
            blocks the rest. */
        void rescan (const juce::StringArray& scanPaths, juce::StringArray& warningsOut);

        const std::vector<FormatExtension>& getExtensions() const noexcept;

        /** Extensions usable for the given operation: an extension whose
            own direction is `both` matches either query, one whose
            direction is `importOnly` matches only `importOnly`, etc. -
            i.e. "can this extension do X", not "does its direction field
            literally equal X". `wanted` is only ever importOnly or
            exportOnly here; querying with `both` returns every
            extension unfiltered. */
        std::vector<const FormatExtension*> matching (ExtensionDirection wanted) const;
    };
}
```

`Source/Extensions/FormatExtensionRunner.h/.cpp`:
```cpp
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
            whatever the process wrote to stderr (or a generic timeout/
            exit-code message when stderr was empty). */
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
        extension with `--export <temp.dawproject> <outputFile>`, and
        reports failure via errorOut/warningsOut using the same two-signal
        contract exportDawProject already uses. */
    bool runFormatExtensionExport (const FormatExtension&, const Project&, PluginManager&,
                                    const juce::File& outputFile, FormatExtensionRunner&,
                                    juce::String& errorOut, juce::StringArray& warningsOut);

    /** Import: invokes the extension with
        `--import <inputFile> <temp.dawproject>`, then imports the
        extension-produced .dawproject into `project` via importDawProject. */
    bool runFormatExtensionImport (const FormatExtension&, Project&, PluginManager&,
                                    const juce::File& inputFile, FormatExtensionRunner&,
                                    juce::String& errorOut, juce::StringArray& warningsOut);
}
```

## CLI Contract

```
<executable> --export <input.dawproject> <output-file>
<executable> --import <input-file> <output.dawproject>
```

- Exit code `0` = success. Non-zero = failure.
- Whatever the process wrote to stderr becomes the error dialog text
  KANADE DAW shows; a generic fallback message is used if stderr was
  empty.
- After a `0` exit, the expected output file must exist on disk or the
  call is still treated as a failure (mirrors
  `ExternalResamplerRunner`'s `expectedOutputWav.existsAsFile()` check).
- Timeout: 120 seconds (`ExternalResamplerRunner` uses 30s for a single
  note; a whole-project conversion needs more headroom). On timeout the
  process is killed and treated as failure with a "timed out" message.

## UI

**File menu:** File > Export and File > Import each gain one dynamic
item per discovered extension whose direction matches, appended after
the existing static items (MIDI/Audio/MusicXML/DAWproject). Because the
count is dynamic, these use hand-rolled `PopupMenu` ids (the same
pattern `20001`-`20003` already use for About/Save-layout/Reset-layout in
`MainComponent::menuItemSelected`) rather than the fixed `CommandIDs`
enum, which can't grow at runtime. `MainComponent::Impl` owns one
`FormatExtensionManager`, rescanned once at startup and again whenever
the user saves scan-path changes in Preferences (same "on demand, not
continuous" rescan trigger `PluginTab`'s rescan button already uses).

**Preferences:** a new "Extensions" tab — a folder-path editor identical
in shape to `PluginTab`'s (`pathsEditor` + "Add folder..." button), a
read-only list of currently-discovered extensions (name, version,
direction), and one line of pointer text to the Help dialog below. No
blacklist UI (see Scope).

**Help dialog:** a new `CommandIDs::showExtensionHelp` (appended at the
true end of the enum, per its own documented constraint) opens an
`ExtensionHelpDialog`, structurally identical to `WhatsNewDialog`
(title + scrollable read-only `TextEditor` + OK button), showing the
manifest schema table and CLI contract from this doc. This dialog's
body text is technical/code-facing (field names, JSON, CLI syntax) and
is **not** wrapped in `TRANS()` — consistent with how this spec itself,
`manifest.json`, and the DAWproject XSD stay English-only; the dialog
chrome (title, OK button) still uses `TRANS()` like every other dialog.

## Documentation

A new "## Building a format extension" section in `README.md` is the
canonical, complete reference: manifest schema, CLI contract, and one
minimal worked example (a trivial identity-style converter script).
The in-app Help dialog is intentionally shorter — schema table + CLI
contract only, with a closing line pointing to the README for the full
example — rather than duplicating a complete walkthrough in two places
that would drift out of sync.

## Error Handling

Two distinct signals, matching the convention `DawProject.h` already
established:
- Manifest-parse failures during `rescan` — always non-fatal, always a
  `warningsOut` entry, never block discovering the rest.
- Runtime failures (`runFormatExtensionExport`/`Import` returning
  `false`) — shown via the same `errorOut`-driven `juce::AlertWindow`
  dialog pattern `MainComponent`'s existing `exportDawProject()`/
  `importDawProject()` handlers already use.

## Testing

`Source/Extensions/FormatExtensionTests.cpp` (new), using real temp
folders/files for manifest parsing (no process spawning anywhere in the
suite, matching `VocalTests.cpp`'s `FakeRunner`/`AlwaysFailRunner`
precedent):
- `parseFormatExtensionManifest`: valid manifest parses correctly;
  missing required field, malformed JSON, and a nonexistent `executable`
  path each fail with a warning and don't throw.
- `FormatExtensionManager::rescan`: discovers valid extensions across
  multiple scan paths; a malformed sibling folder doesn't block
  discovery of the rest; an empty scan-path list is a no-op.
- `runFormatExtensionExport`/`runFormatExtensionImport`: driven entirely
  through a `FakeFormatExtensionRunner` (success, non-zero exit,
  missing-output-file, and the two-signal error/warning contract) —
  never spawns a real process, per the corrected precedent above.

## Out of Scope (explicitly deferred)

- AI backend, UI panel, and macro/scripting extension categories — each
  is its own future spec.
- Extension id collision handling.
- Any sandboxing beyond OS process isolation.
- A way for an extension to report progress mid-conversion (the 120s
  timeout is a flat ceiling, not a progress-aware one).
