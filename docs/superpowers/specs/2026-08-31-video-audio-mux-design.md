# Video + Audio Mux — Design

**Status:** Approved by user 2026-08-31 (revised same day — see History).
Ready for implementation planning.

## History

This spec went through two rounds with the user before landing:

1. First round: built in to KANADE DAW's own C++ core using Windows
   Media Foundation directly, specifically to avoid the user needing
   to install a separate tool. Approved, then the user reconsidered:
   "これは前に実装したプラグイン追加機能としての実験やからプラグインとして制作するぞ" —
   this should be built as a **format extension plugin** instead, to
   exercise the extension mechanism the project just shipped (the two
   example extensions, `dawproject-dump` and `reaper-bridge`).
2. That surfaced a real protocol gap: the existing format extension
   CLI contract (`<exe> --export <input.dawproject> <output-file>`)
   only carries two paths, and this feature needs two inputs the
   protocol has no room for — an existing video file, and the DAW's
   own rendered mixdown audio. Resolved by extending the extension
   *manifest* (not the CLI argv shape) with a new, general-purpose
   `additionalInputs` mechanism (environment variables), designed to
   be reusable by any future extension with the same kind of need, not
   special-cased to this one plugin.
3. That, in turn, raised what the plugin should use to actually
   encode video, since a plugin is a standalone executable with no
   access to KANADE DAW's own (now unused, per point 1's reversal)
   Media Foundation code. Resolved: bundle a redistributable `ffmpeg.exe`
   directly in the plugin's own folder, so the end result is still
   "drop one self-contained folder in, nothing else to install" — the
   user's original constraint from point 1 is satisfied by the plugin
   being self-contained, not by KANADE DAW itself doing the encoding.

The final shape below is a KANADE DAW protocol extension
(`additionalInputs`) plus a new example plugin
(`Extensions-Examples/video-audio-mux/`) built on it — not a
Windows-only in-core feature. Windows-only still applies, but now
because the *bundled ffmpeg build* is Windows-only, not because of a
Media Foundation dependency in KANADE DAW's own code.

## Goal

Let a user combine an existing video file with audio from the current
KANADE DAW project, producing a new video file that carries both the
original video's own audio (if any) and the DAW's mixdown, mixed
together. Typical use: dubbing a BGM/vocal track the user produced in
KANADE DAW onto game footage, a vlog, or similar existing video.

This is the first of two video-related features the user asked for
("(A) mux DAW audio into an existing video" and "(B) generate a new
video from audio — waveform/lyric video"). (B) is a separate
subsystem (frame rendering, not just container/audio work) and is
explicitly out of scope here — it gets its own future spec.

## Precedent already in the codebase

- `docs/superpowers/specs/2026-08-28-format-extension-api-design.md` —
  the extension mechanism this feature extends. `FormatExtension.h`'s
  struct, `FormatExtensionManager`'s discovery/rescan, and
  `FormatExtensionRunner.cpp`'s `runFormatExtensionExport` are all
  reused, not replaced.
- `Extensions-Examples/dawproject-dump/` and
  `Extensions-Examples/reaper-bridge/` — the two existing example
  extensions. Both are self-contained .NET 8
  `dotnet publish -p:PublishSingleFile=true` executables sitting
  directly next to their `manifest.json`, with a `src/` sibling folder
  holding the actual project. This plugin follows the identical
  layout and build recipe, with `ffmpeg.exe` added alongside the
  published exe as a third same-folder file.
- `AudioEngine::renderToFile()` (`Source/Engine/AudioEngine.cpp`) is
  what KANADE DAW calls to produce the mixdown WAV for the
  `mixdownRender` additional-input kind — the exact same call
  `MainComponent::Impl::exportAudio()` (`Source/UI/MainComponent.cpp:1018`)
  already makes, at the project's full length (`0.0` to
  `project->endBeats()`), same as that command's own current default.
- `Source/Vocal/UtauRenderer.h`'s external-process pattern (mentioned
  in the format-extension spec too) is the precedent for "an
  external-process step KANADE DAW's own tests fake out rather than
  spawn a real executable for."

## Manifest Schema Addition: `additionalInputs`

A new, optional array on `manifest.json`, empty/absent by default (so
the two existing extensions need no changes):

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

Two kinds for v1 (the two this feature actually needs — the schema
is a plain tagged array so a third kind is a pure addition later, not
a breaking change):

- `"userFile"` — KANADE DAW shows a `juce::FileChooser` (open mode)
  with `prompt` as its title and `fileFilter` as the file pattern,
  before invoking the extension. If the user cancels, the whole export
  is cancelled (same as cancelling the existing output-file chooser
  today).
- `"mixdownRender"` — KANADE DAW renders the current project's full
  mixdown (`AudioEngine::renderToFile()`, `0.0` to
  `project->endBeats()`, project's own bit depth/sample rate — no user
  prompt, no new range-picker UI for v1) to a temp WAV file. Deleted
  after the extension process exits, same lifetime as the temp
  `.dawproject` `runFormatExtensionExport` already creates and cleans
  up.

Either way, the resolved absolute path is set as the named environment
variable on the KANADE DAW process itself immediately before
`ChildProcess::start()`, and cleared (`SetEnvironmentVariableW(name, nullptr)`)
immediately after the child process exits — `juce::ChildProcess`
has no per-child environment override (checked: `juce_ChildProcess.h`'s
`start()` overloads only take a command/argument list and stream
flags), but a Windows child process inherits its parent's environment
block by default when none is explicitly supplied, which is exactly
what JUCE's own Windows `ChildProcess` implementation does — so
setting it on the KANADE DAW process right before spawning, then
clearing it right after, is sufficient and doesn't require a JUCE API
change.

**Why environment variables and not more CLI arguments:** extending
the argv shape (`--export <dawproject> <extra1> <extra2> ... <output>`)
would break argument-position assumptions for every future extension
kind and needs the count baked into the invocation logic. Environment
variables are additive, self-describing by name, and an extension
that doesn't declare `additionalInputs` is invoked exactly as before
— no protocol version bump, no change to the two existing extensions.

## Components

### `Source/Extensions/FormatExtension.h` (modified)

```cpp
enum class AdditionalInputKind { userFile, mixdownRender };

struct AdditionalInput
{
    AdditionalInputKind kind = AdditionalInputKind::userFile;
    juce::String envVar;                    // both kinds
    juce::String prompt, fileFilter;        // userFile only
};

struct FormatExtension
{
    // ... existing fields unchanged ...
    std::vector<AdditionalInput> additionalInputs;   // new, may be empty
};
```

`parseFormatExtensionManifest` gains parsing for the optional
`additionalInputs` array. A malformed entry (unknown `kind`, missing
`envVar`) is treated the same as any other manifest defect today: the
whole extension is skipped with a warning, not partially loaded.

### `Source/Extensions/FormatExtensionRunner.h`/`.cpp` (modified)

`runFormatExtensionExport` gains one new parameter: a
`std::map<juce::String, juce::File>` of already-resolved
envVar → path pairs (the caller in `MainComponent.cpp` is responsible
for gathering these, since it's the only place that can show UI
dialogs — the runner itself stays UI-free, matching its existing
design). `runFormatExtensionExport` itself sets each one via
`SetEnvironmentVariableW` right before calling `runner.run(args, ...)`,
and clears each one (success or failure) right after — a small
RAII-style scoped helper (`EnvVarScope` or similar) keeps this
exception/early-return-safe the way the existing temp-file cleanup
already is. This keeps the `FormatExtensionRunner` interface itself
unchanged (env vars set on the KANADE DAW process are inherited by
whatever child process `ExternalFormatExtensionRunner::run` spawns
regardless of which layer sets them, and every existing fake runner
in `FormatExtensionTests.cpp` keeps compiling without touching its
`run()` override).

### `Source/UI/MainComponent.cpp` (modified)

`exportViaExtension()` (`MainComponent.cpp:953`) gains a step between
"the output file was chosen" and "invoke the extension": if
`extension.additionalInputs` is non-empty, gather each one in order
before proceeding.

- `userFile` entries are gathered on the message thread, one
  `FileChooser` at a time, chained via each chooser's own async
  callback (KANADE DAW already does this style of sequential async
  UI elsewhere — e.g. chained `FileChooser`s aren't new to this
  codebase's patterns even though this exact multi-step chain is).
  Cancelling any one of them cancels the whole export.
- Once every `userFile` entry has a resolved path, the remaining
  `mixdownRender` entries (rendering audio, potentially slow) run
  inside `task.run(...)` — the same `TaskPanel` background-task helper
  `exportAudio()` already uses — immediately followed by, in the same
  task, calling `runFormatExtensionExport` with the fully-gathered
  map. This keeps every blocking step off the message thread while
  keeping every user-facing dialog on it, the same division
  `exportAudio()` already draws between its `FileChooser` and its
  `task.run(...)`-wrapped render.
- Failure at any stage (a `userFile` cancel, a `mixdownRender` render
  failure, or the extension itself failing) shows the same
  `AlertWindow` pattern already used for every other export failure —
  no new error-presentation code, just new call sites into it.

## The `video-audio-mux` example plugin

`Extensions-Examples/video-audio-mux/` — same layout convention as
the two existing example extensions (`manifest.json` +
published exe directly in the folder, `src/` holding the actual .NET
project).

- **manifest.json**: as shown above — `direction: "export"`,
  `fileExtension: "mp4"`, the two `additionalInputs` entries.
- **video-audio-mux.exe**: a thin .NET 8 console app. It does *not*
  need to read or care about the `.dawproject` input argument at all
  (this plugin's job doesn't touch project structure — the DAW audio
  side already arrives fully rendered via `KANADE_DAW_MIXDOWN_WAV`)
  — it still receives that argument positionally, per the existing
  `--export <input.dawproject> <output>` contract, and simply ignores
  its contents. It reads `KANADE_DAW_VIDEO_FILE` and
  `KANADE_DAW_MIXDOWN_WAV` from the environment, and builds/runs an
  `ffmpeg` command line (via the bundled `ffmpeg.exe` sitting next to
  it — resolved relative to the running exe's own directory, not
  `PATH`) that:
  - Copies the input video's video stream through unchanged
    (`-c:v copy`) — no re-encode, no quality loss.
  - Mixes the input video's own audio stream (if it has one) with the
    mixdown WAV (`amix`/equivalent filter), re-encoded to AAC.
  - Matches the *video's own duration* — pad the shorter audio input
    with silence, trim the longer one, exactly as the original design
    round settled on (this replaces the "user picks a render range to
    match by hand" idea from the in-core version of this design,
    since the mixdown is now always the full project — matching
    lengths is now entirely ffmpeg's job at mux time).
  - Exits 0 with the output file written on success; on any ffmpeg
    failure, forwards ffmpeg's own stderr text and exits 1 (that text
    is what the user sees in KANADE DAW's `AlertWindow`, per the
    existing extension-failure contract — no need to re-word ffmpeg's
    own error output).
- **Bundled `ffmpeg.exe`**: obtained from a well-known Windows build
  provider (BtbN/FFmpeg-Builds on GitHub, or gyan.dev — the same
  BtbN source already named in this project's own conventions
  elsewhere, e.g. the BGM Jukebox addon's README instructs users to
  fetch ffmpeg from there). An LGPL build (not GPL) is required for
  redistribution to stay unencumbered. If the implementer's
  environment can't fetch it directly (no outbound network access),
  fall back to: ship the plugin without `ffmpeg.exe` present, and have
  `video-audio-mux.exe` fail fast at startup with a clear message
  telling the user to place `ffmpeg.exe` next to it themselves — never
  silently do nothing.

## Error Handling

- A `userFile` chooser cancelled: export silently cancelled, same as
  today's output-file chooser cancel — no error dialog (this isn't a
  failure, it's the user changing their mind).
- `mixdownRender`'s underlying `renderToFile()` call fails: existing
  failure path, surfaced via `AlertWindow`, extension is never
  invoked.
- The extension process itself fails (missing `ffmpeg.exe`, ffmpeg
  errors, timeout, no output produced): existing
  `runFormatExtensionExport`/`AlertWindow` failure path — unchanged by
  this feature, just now also covering the new env-var setup/teardown
  as part of "did this export succeed."
- Malformed `additionalInputs` in a manifest (unknown `kind`, missing
  `envVar`): the whole extension fails to load, same as any other
  manifest defect — logged as a warning during rescan, not a runtime
  export-time surprise.

## Testing

- `Source/Extensions/FormatExtensionTests.cpp` (existing file, new
  cases added): manifest parsing of `additionalInputs` — a well-formed
  array of both kinds, an absent array (backward compatibility with
  the two existing example extensions' manifests, verified by keeping
  their actual manifest.json content as literal fixtures if not
  already), and each malformed shape (unknown kind, missing envVar)
  correctly rejecting the whole extension. Env var
  set-before/clear-after behaviour around `ChildProcess::start()` -
  test via `ExternalFormatExtensionRunner`'s existing seam (it's
  already an interface real tests fake out, per the precedent above)
  by asserting the fake runner observed the expected environment
  variable *value* at the moment `run()` was called (no real child
  process needed for this, matching how the existing runner tests
  already avoid spawning real executables).
- The .NET plugin itself (`video-audio-mux.exe`) is tested the same
  way `reaper-bridge.exe` was: build a small sample video +
  a sample WAV, run the exe directly from the command line with the
  environment variables set by hand (`$env:KANADE_DAW_VIDEO_FILE = ...`),
  confirm the output MP4 plays with both audio sources audible, and
  confirm the duration-matching behaviour (shorter/longer mixdown than
  video) with two more sample pairs. Not part of KANADE DAW's own
  `--run-tests` suite — same category as the two existing example
  extensions, verified by hand.

## Out of Scope (explicitly deferred)

- Feature (B) — audio-to-video generation (waveform/lyric video) —
  separate future spec, once this one ships.
- Mac/Linux support (both for the `additionalInputs` mechanism, which
  is portable, and for the bundled `ffmpeg.exe`, which isn't — a Mac
  version of this specific plugin would need its own ffmpeg build,
  not a code change).
- A user-facing render-range picker for `mixdownRender` (always full
  project for v1 — length matching happens at mux time in ffmpeg
  instead).
- A dedicated mix-balance control (superseded by "just use the
  mixer," per the user's own call earlier in this design's history).
- A third `additionalInputs` kind beyond `userFile`/`mixdownRender`
  (the schema supports adding one later without a breaking change,
  but nothing concrete needs one yet).
- Batch processing (multiple videos at once).
