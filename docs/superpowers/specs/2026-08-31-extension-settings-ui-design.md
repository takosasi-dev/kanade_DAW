# Format Extension Settings, Progress & Custom UI — Design

**Status:** Approved by user 2026-08-31. Ready for implementation planning.

## History

Right after the video-audio-mux plugin shipped, the user asked whether
it has a UI ("あー作ったプラグイン自体にUIはない感じ？？UIあると使いやすいねんけど"). It
doesn't — the format extension protocol is a headless CLI contract
(`<exe> --export <in> <out>`, exit code + stdout/stderr text), and the
only feedback during a run is KANADE DAW's own generic
"`<name>` running..." `TaskPanel` (just added the same day to fix the
UI-freeze bug — see `2026-08-31-video-audio-mux-design.md`'s sibling
work).

Asked which of three directions "UI" meant, the user picked all three:
real progress percentage, a pre-run settings screen, and a fully
custom plugin-owned GUI window. Two design approaches were presented:

- **A (plugin-owned GUI):** the exe pops its own WinForms/WPF window
  and handles everything itself. Most flexible, but every plugin
  author reinvents a window.
- **B (host-rendered):** the manifest declares simple settings
  (sliders/checkboxes/dropdowns), KANADE DAW auto-generates the
  dialog, and the plugin reports progress via a `PROGRESS:NN` stdout
  line convention. Consistent look across every System Plugin, no GUI
  framework needed per plugin.

B was recommended (YAGNI — video-audio-mux only needs a volume
balance slider and a progress bar, neither justifies a native GUI
dependency). The user agreed but pushed back on dropping A entirely:
"Bで、ただ自由度を高めるためにDAW側で独自GUIを認めるフレームワークは必須やと思う" — B
as the primary path, but the *capability* for a plugin to take over
with its own window must still exist as a first-class option, even if
nothing uses it yet.

The final shape is both: B is the default (`settings` + progress
protocol), and a `customUI` manifest flag opts an extension out of the
auto-generated dialog and the 120s timeout entirely, handing full
control to the plugin's own window. `video-audio-mux` itself is
updated to use the B path only (one `settings` entry, progress
reporting) — `customUI` ships as working, tested infrastructure with
no concrete consumer yet, the same way `additionalInputs` shipped
with only `userFile`/`mixdownRender` as its two kinds.

This also reopens one line from the video-audio-mux spec's own "Out of
Scope" section: *"A dedicated mix-balance control (superseded by 'just
use the mixer,' per the user's own call earlier in this design's
history)."* That call was made before this settings mechanism existed;
with it in hand, a mix-balance slider is now cheap enough to add back,
and does — see the "video-audio-mux changes" section below.

## Goal

Give format extensions three things, all optional and all backward
compatible with the two existing plain extensions
(`dawproject-dump`, `reaper-bridge`, neither of which changes):

1. A way to declare simple configuration (sliders, checkboxes,
   dropdowns) that KANADE DAW asks the user for, right before running
   the extension, and passes in as environment variables — the same
   channel `additionalInputs` already uses.
2. A way to report real progress (0-100%) back to KANADE DAW's
   existing `TaskPanel` progress bar, instead of an indeterminate
   spinner.
3. An escape hatch (`customUI`) for an extension that wants to own its
   own window instead of the auto-generated dialog — KANADE DAW simply
   runs it with no fixed timeout and lets the user Cancel from the
   `TaskPanel` if it never returns.

A cross-cutting improvement falls out of (2)'s implementation for
free: every extension run (not just ones using these new fields)
becomes cancellable via `TaskPanel`'s existing Cancel button, which
today does nothing for a running extension.

## Precedent already in the codebase

- `docs/superpowers/specs/2026-08-28-format-extension-api-design.md`
  and `2026-08-31-video-audio-mux-design.md` — the extension mechanism
  and its `additionalInputs` extension, both reused and extended here,
  not replaced.
- `Source/UI/UiSupport.h`'s `TaskPanel` — already has
  `setProgress(float)` (atomic, callable from a background thread),
  `isCancelled()`, and an `onCancel` hook explicitly documented "for
  jobs that need more than the flag (killing a child process, say)" —
  this feature is exactly that job, first time it's actually used for
  one.
- `Source/UI/TranscribeView.cpp:376` and `:418` — the established
  pattern for wiring a cancellable background job: `task.onCancel =
  [...] { ... };` to interrupt the work, and `if (! task.isCancelled()
  && ...)` in the completion callback to suppress the error dialog for
  a user-initiated cancel rather than treating it as a failure.
- `Source/UI/ExtensionHelpDialog.h`/`.cpp` — the precedent for a
  `juce::DialogWindow::LaunchOptions` + `launchAsync()` modal dialog
  launched from the System Plugins menu, using `palette()` for
  theming. The new settings dialog follows this shape but, unlike
  `ExtensionHelpDialog`, returns a result via callback instead of just
  showing static text.
- `Source/Extensions/FormatExtensionRunner.cpp`'s existing comment
  block (lines 62-77) already anticipated this exact rewrite: it
  explains *why* the current implementation only reads output after
  the process exits (draining while waiting risked blocking past the
  timeout with the naive approach), and says the proper fix is "drain
  on a dedicated reader thread" — which is exactly what progress
  reporting now requires, so that work is being done for real instead
  of deferred again.

## Manifest Schema Additions: `settings` and `customUI`

Two new optional, independent top-level manifest fields, both
absent/empty by default (so `dawproject-dump` and `reaper-bridge`
need no changes):

```json
{
  "id": "com.kanade-daw-examples.video-audio-mux",
  "name": "Video + Audio Mux",
  "version": "1.1.0",
  "fileExtension": "mp4",
  "direction": "export",
  "executable": "video-audio-mux.exe",
  "customUI": false,
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

**`settings`** — an array of controls KANADE DAW renders as a dialog
immediately before invoking the extension (after `additionalInputs`
gathering, before the export/import runs). Three `type`s for v1:

| type | required fields | env var value |
|------|-----------------|----------------|
| `slider` | `min`, `max`, `default` (numbers) | the chosen number, e.g. `"0.5"` |
| `checkbox` | `default` (bool) | `"1"` or `"0"` |
| `dropdown` | `options` (non-empty string array), `default` (must be one of `options`) | the selected option string, verbatim |

Every entry also requires `id` (unique within the array — used only
for manifest validation, not sent anywhere), `label` (shown next to
the control), and `envVar` (non-empty, same naming rule
`additionalInputs` already enforces).

**`customUI`** — when `true`, KANADE DAW skips the auto-generated
settings dialog entirely and runs the extension with no fixed
timeout (see Components below) instead of the normal 120s. A manifest
that sets `customUI: true` **and** declares a non-empty `settings`
array is rejected at load time — the two are mutually exclusive ways
of asking for the same kind of pre-run interaction, and allowing both
would leave it ambiguous which one wins.

## Components

### `Source/Extensions/FormatExtension.h` (modified)

```cpp
enum class ExtensionSettingType { slider, checkbox, dropdown };

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

struct FormatExtension
{
    // ... existing fields unchanged ...
    bool customUI = false;
    std::vector<ExtensionSetting> settings;   // new, may be empty
};
```

`parseFormatExtensionManifest` gains parsing for both new fields,
following the exact validation-failure convention `additionalInputs`
already established: any defect (unknown `type`, missing required
field for that type, non-unique `id`, `dropdownDefault` not present in
`dropdownOptions`, or `customUI: true` with non-empty `settings`)
skips loading the whole extension with a warning, not a partial load.

### `Source/Extensions/FormatExtensionRunner.h`/`.cpp` (modified)

The `additionalInputs` map's value type widens from `juce::File` to
`juce::String` — settings values (a slider number, a checkbox flag, a
dropdown string) aren't files, and reusing the exact same map/env-var
plumbing for both is simpler than adding a second parallel map type.
Callers that used to hand over a `juce::File` now call
`.getFullPathName()` themselves first; `ScopedEnvVars` and
`runFormatExtensionExport`/`runFormatExtensionImport`'s
`additionalInputs` parameter both become
`std::map<juce::String, juce::String>`.

`FormatExtensionRunner::run` gains three parameters:

```cpp
virtual bool run (const juce::StringArray& args, const juce::File& expectedOutput,
                   int timeoutMs,
                   std::function<void (float)> onProgress,
                   std::function<bool()> shouldCancel,
                   juce::String& errorOut) = 0;
```

`ExternalFormatExtensionRunner::run` is rewritten around a dedicated
reader thread (the fix the existing code comment already named):

1. `process.start(args)` as today (default `wantStdOut | wantStdErr`
   stream flags — already merges both streams into the one
   `readProcessOutput` reads from, confirmed against
   `juce_ChildProcess.h`; this is why the existing failure path
   already surfaces ffmpeg's stderr text today with no special
   handling).
2. Start a small reader thread that loops
   `process.readProcessOutput(buf, N)`, appends bytes to a
   lock-protected accumulator, and — as each complete line becomes
   available — checks for a `PROGRESS:` prefix. A matching line parses
   the trailing number (clamped 0-100), calls `onProgress(value /
   100.0f)` if non-null, and is excluded from the accumulator used for
   the eventual error message; every other line stays in the
   accumulator exactly as `readAllProcessOutput()` would have
   collected it today.
3. The main thread polls a small loop instead of a single blocking
   `waitForProcessToFinish`: while `process.isRunning()`, check
   `shouldCancel()` (if non-null) and kill+break if true, check
   elapsed time against `timeoutMs` (skipped entirely when `timeoutMs
   < 0`) and kill+break with the existing timeout message if exceeded,
   otherwise sleep ~50ms and loop again. On normal exit the child
   closes its own output handle, the reader thread's blocking read
   returns 0 and stops on its own — no kill needed on that path.
4. Same success/failure checks as today (exit code, `expectedOutput`
   existence) using the accumulated non-progress text as `errorOut`.

`runFormatExtensionImport` adapts to the new five-parameter
`FormatExtensionRunner::run` signature mechanically — it always passes
the existing fixed `120000` timeout and `nullptr` for both
`onProgress`/`shouldCancel`, gaining no new caller-facing capability
itself (see the import scope note under MainComponent.cpp below).

`runFormatExtensionExport` gains two new optional parameters,
`std::function<void (float)> onProgress = nullptr` and
`std::function<bool()> shouldCancel = nullptr`, and computes
`timeoutMs` itself from `extension.customUI` (`-1` when `true`,
`120000` otherwise, the existing constant) — callers never pass a
timeout directly. `runFormatExtensionImport`'s signature is unchanged
by this spec (see the import scope note below) beyond the mechanical
`run(...)` five-parameter update every `FormatExtensionRunner`
implementation needs regardless.

### `Source/UI/ExtensionSettingsDialog.h`/`.cpp` (new)

Same shape as `ExtensionHelpDialog` (a `juce::Component` shown via
`juce::DialogWindow::LaunchOptions::launchAsync()`, themed with
`palette()`), but built dynamically from a
`std::vector<ExtensionSetting>` and returning its result via callback
instead of showing static text:

```cpp
class ExtensionSettingsDialog final : public juce::Component
{
public:
    ExtensionSettingsDialog (const std::vector<ExtensionSetting>& settings,
                              std::function<void (std::optional<std::map<juce::String, juce::String>>)> onComplete);

    static void launch (const juce::String& extensionName,
                         const std::vector<ExtensionSetting>& settings,
                         std::function<void (std::optional<std::map<juce::String, juce::String>>)> onComplete);
    // ...
};
```

One row per setting — a `juce::Slider` (linear, `min`/`max`, starting
at `sliderDefault`) for `slider`, a `juce::ToggleButton` for
`checkbox`, a `juce::ComboBox` populated from `dropdownOptions` for
`dropdown` — with `label` as each row's caption. OK reads every
control's current value into a `std::map<juce::String, juce::String>`
keyed by each setting's `envVar` (slider → the number as a string,
checkbox → `"1"`/`"0"`, dropdown → the selected item's text) and calls
`onComplete` with it. Cancel calls `onComplete(std::nullopt)`.

### `Source/UI/MainComponent.cpp` (modified)

`exportViaExtension()`'s existing `gatherAdditionalInputs(...)` step
is unchanged in shape (still resolves `userFile`/`mixdownRender`
entries in order) but its map's value type is now
`juce::String` (each `juce::File` result converted via
`.getFullPathName()` before insertion, per the widened type above).

A new step is inserted between "additional inputs gathered" and
"`finishExportViaExtension` invoked": if `extension.settings` is
non-empty, call `ExtensionSettingsDialog::launch(...)`; on
`std::nullopt` (Cancel), the whole export is cancelled the same way an
`additionalInputs` `userFile` cancel already is today (no error
dialog). On a real result, its entries are merged into the same
`std::map<juce::String, juce::String>` `gatherAdditionalInputs`
produced, and that combined map is what reaches
`finishExportViaExtension`. (`customUI` extensions never reach this
step — their `settings` is always empty by the manifest-load
validation above.)

`finishExportViaExtension`'s `task.run(...)` work lambda now passes
`[&task] (float p) { task.setProgress (p); }` and
`[&task] { return task.isCancelled(); }` into
`runFormatExtensionExport`/`Import` as `onProgress`/`shouldCancel`
(both already have `task` in scope as the lambda's own parameter).
The completion lambda gains one line at its top,
`if (task.isCancelled()) return;`, mirroring
`TranscribeView.cpp:376`'s established idiom, so a user-initiated
Cancel doesn't pop the "Export failed" `AlertWindow`.

**Import scope note:** `importViaExtension()` (`MainComponent.cpp:1090`)
does not gain `settings`/`customUI`/progress/cancel support in this
spec. Discovered while writing this design: it currently has no
`additionalInputs`-style map at all (only export does — the same
asymmetry already exists for `additionalInputs` today, so this isn't
a new gap this feature introduces), and unlike `finishExportViaExtension`
it still runs `runFormatExtensionImport` synchronously via
`performProjectEdit` on the message thread rather than inside
`task.run(...)` — it was not part of the freeze fix made earlier the
same day. A manifest that declares `settings` on an import-capable
extension loads fine but those settings are simply never shown or
populated (same silent-no-op precedent `additionalInputs` already has
for import). Both gaps (import's own freeze, and extending this
feature to import) are real but separate follow-up work — flagged
below in Out of Scope, not fixed here since the user hasn't asked for
either yet.

### `Source/UI/ExtensionHelpDialog.cpp` (modified)

The static help text gets three additions describing `settings`,
`customUI`, and the `PROGRESS:NN` stdout convention, in the same
plain style as its existing field table — this is the only
documentation a third-party extension author has, so it can't fall
out of sync with what `FormatExtension.h` actually parses.

## `video-audio-mux` changes

Uses the `settings` path only — `customUI` stays `false` (unchanged
from today), matching the History section's YAGNI call.

- **manifest.json**: gains the one `settings` entry shown above
  (`mixBalance`, slider, `MUX_MIX_BALANCE`, 0.0-1.0, default 0.5).
- **Mix balance**: `Program.cs` reads `MUX_MIX_BALANCE` (parses as
  `double`, falls back to `0.5` if missing/unparsable — KANADE DAW
  always sets it once the manifest declares it, so this is a defence
  only for someone running the exe by hand outside KANADE DAW) and
  uses it as a linear weight between the two audio inputs in the
  ffmpeg filter graph already built for the amix step: the video's own
  audio gets `1 - balance`, the mixdown gets `balance`. `0.5` is the
  same equal mix the plugin already produces today, so this is
  additive — existing behaviour is the default, not a break.
- **Progress**: the plugin already calls `ffprobe` to read durations
  for its existing pad/trim logic, so the output's total duration is
  already known before ffmpeg's encode pass starts. It runs ffmpeg
  with progress reporting enabled (`-progress pipe:1` or by parsing
  the default `time=` field from ffmpeg's own stderr — implementer's
  choice, both land in the same merged stream KANADE DAW's reader
  thread already drains, confirmed above), converts elapsed/total into
  a 0-100 integer, and writes `PROGRESS:<n>` lines to stdout as
  encoding proceeds.

## Error Handling

- Settings dialog Cancel: export/import cancelled silently, same
  convention as an `additionalInputs` `userFile` cancel today — not
  treated as a failure.
- `TaskPanel` Cancel during the extension's run: the child process is
  killed, `task.isCancelled()` suppresses the failure `AlertWindow`
  (see MainComponent section above) — this is new behaviour today's
  extension runs don't have at all (Cancel currently does nothing
  visible for a running extension).
- `customUI` extension that never exits and is never cancelled: runs
  indefinitely — there is no fallback timeout once `customUI: true` is
  set. This is the accepted trade-off for handing control to the
  plugin's own window; the Cancel button is the only way out, which is
  a reasonable manual escape hatch. If this becomes a real problem in
  practice (a broken `customUI` plugin hanging forever with no obvious
  Cancel-and-move-on path for a confused user), revisit with an actual
  long default timeout for that case instead of none.
- Malformed `settings`/`customUI` in a manifest: whole extension fails
  to load, same as any other manifest defect — a rescan-time warning,
  not a runtime export-time surprise.

## Testing

- `Source/Extensions/FormatExtensionTests.cpp`: manifest-parsing cases
  for `settings` (each of the three `type`s well-formed, each
  type-specific required field missing, non-unique `id`, dropdown
  default not in its own options list) and for `customUI` (parses as
  a plain bool, defaults `false` when absent, and the
  `customUI: true` + non-empty `settings` rejection case). The five
  existing fake `FormatExtensionRunner` subclasses in this file
  (`RecordingRunner`, `EnvCheckingRunner`, `FailingRunner`,
  `ProducesFileRunner`, `MissingOutputRunner`) all need their `run(...)`
  overrides updated to the new five-parameter signature — mechanical,
  same behaviour, new parameters ignored except where a test is
  specifically about progress/cancel/timeout.
- New tests for the reader-thread rewrite: a fake/real short-lived
  process (or `ExternalFormatExtensionRunner` driven against a small
  test helper executable, matching how this file already avoids
  spawning real production executables) that prints one or more
  `PROGRESS:NN` lines confirms `onProgress` is called with the right
  values and those lines don't appear in `errorOut` on a subsequent
  failure; a `shouldCancel` that returns `true` immediately confirms
  the process is killed and `run()` returns `false` promptly rather
  than waiting out `timeoutMs`; `timeoutMs < 0` confirms no timeout
  fires for a process that runs longer than the old 120s constant
  (bounded by a short test-only sleep, not an actual 120s+ wait).
- `video-audio-mux`: same manual verification category as today (not
  part of `--run-tests`) — re-run the existing three duration-matching
  scenarios with the balance slider at `0.0`, `0.5`, and `1.0` and
  confirm the mix audibly shifts; confirm `PROGRESS:` lines appear on
  stdout during a real encode and reach 100 by the time the process
  exits.

## Out of Scope (explicitly deferred)

- **`importViaExtension()`'s own UI freeze** (discovered while writing
  this spec — it runs synchronously on the message thread today,
  unlike `finishExportViaExtension` which was already fixed) **and**
  extending `settings`/`customUI`/progress/cancel to the import path
  at all. Both are real, both are separate from what the user asked
  for here (which was driven entirely by `video-audio-mux`, an
  export-only extension) — worth a follow-up spec if import extensions
  ever grow slow/interactive plugins the way export ones just did.
- `customUI` gaining a concrete consumer — ships as tested,
  documented infrastructure only, per the History section.
- Any `type` beyond `slider`/`checkbox`/`dropdown` (text field,
  colour picker, file picker as a *setting* rather than an
  `additionalInputs` entry) — nothing concrete needs one yet, and the
  schema being a plain tagged array means adding one later isn't a
  breaking change, same reasoning `additionalInputs`' kinds used.
- A default long-but-finite timeout for `customUI` extensions instead
  of none — see Error Handling; revisit only if it's a real problem.
- Persisting a user's settings choices between runs (every run shows
  the dialog at its manifest defaults) — nothing requested this, and
  it's a pure additive feature to bolt on later if asked.
- Mac/Linux — unaffected either way; this is pure C++/JUCE protocol
  work with no Windows-only API used, same portability as
  `additionalInputs` itself.
