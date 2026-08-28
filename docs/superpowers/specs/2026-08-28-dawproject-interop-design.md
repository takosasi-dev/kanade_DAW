# DAWproject Interoperability — Design

**Status:** Approved by user 2026-08-28. Ready for implementation planning.

## Goal

Let a KANADE DAW project move to and from other DAWs (Studio One, Bitwig,
Cubase 14+, Cubasis, VST Live) via `.dawproject`, an open, MIT-licensed
interchange format jointly published by Bitwig and PreSonus. A user should
be able to `File > Export > DAWproject...` a KANADE DAW project and open it
in Studio One with tracks, clips, tempo, automation, and plugin state
intact, and the reverse (`File > Import > DAWproject...`) for a project
started elsewhere.

Full round-trip fidelity is not achievable for anything that has no
equivalent on the other side — that is inherent to any cross-DAW format,
not a shortcut specific to this implementation. The goal is: everything
both KANADE DAW and DAWproject can express, maps cleanly; everything that
can't, is dropped with a visible warning, never silently.

## Tech Stack

- **Container:** `.dawproject` is a ZIP archive holding `project.xml`,
  optionally `metadata.xml`, and referenced media/plugin-state files.
  Read with `juce::ZipFile`, write with `juce::ZipFile::Builder` — both
  already part of `juce_core`. **No new dependency.**
- **XML:** `juce::XmlElement` / `juce::XmlDocument`, already used for
  `exportMusicXml`/`importMusicXml` (`Source/IO/MusicXml.cpp`).
- **Schema (authoritative, fetched and verified 2026-08-28):**
  `https://github.com/bitwig/dawproject`, MIT license.
  - `Project.xsd` — blob `ef4055976e0ea5615567c363a5c83a13e79b768c`
  - `MetaData.xsd` — blob `cc60f065821b54c7daac6ffbeabe93376329b0cf`

  The implementation task should vendor these two files verbatim into
  `docs/superpowers/specs/dawproject-schema/` (pinned to the blobs above,
  where Task 1 actually vendored them — this doc originally said
  `Source/IO/dawproject/`, corrected during the final whole-branch
  review) as the single source
  of truth for element/attribute names — do not re-derive names from this
  spec's prose alone.
- **Support confirmed (as of Nov 2024 / current as of writing):** Bitwig
  Studio 5.0.9+, Studio One 6.5+, Cubase 14, Cubasis 3.7.1, VST Live 2.2.

## Precedent already in the codebase

- `Source/IO/FileIO.h` already has the exact shape this feature needs:
  paired `exportX`/`importX` free functions taking a `juce::File`, a
  `Project&`/`const Project&`, and a `juce::String& errorOut`. DAWproject
  support is a new sibling pair in this same module family, not a new
  pattern.
- `PluginSlot::state` (`Source/Core/Project.h:92`) is already an opaque
  `juce::MemoryBlock` holding VST3 plugin state — exactly what
  DAWproject's `Device/State` file reference wants. No new plugin-state
  capture code needed; it's already captured for `.ssproj` save/load.
- `PluginManager`'s existing "plugin not found" handling (used when a
  `.ssproj` references a plugin identifier that isn't installed) is the
  same failure mode a `.dawproject` with a missing plugin hits on import —
  reuse it rather than inventing new missing-plugin UX.
- `Track::automation` (`AutomationLane { parameterId; points (beat, 0..1) }`,
  `Project.h:159`) already stores automation as normalized 0..1 breakpoints
  keyed by a parameter id string — see Data Model below for why this turns
  out to need no conversion at all.

## Scope

**In scope:**
- Tracks of `TrackType::audio` and `TrackType::midi`, including an
  instrument plugin on a MIDI track.
- `AudioClip`, `MidiClip` (notes), fades, per-clip gain, `playbackRate`.
- `TempoMap` (tempo + time signature events), `Marker`s.
- `Track::automation` (built-in params and plugin params), `Bus`,
  `BuiltinFxSlot` for `eq`/`compressor`/`gate`/`limiter`.
- `Scene` / `sessionSlots` (Session view clip launcher) both ways.
- `PluginSlot` (VST3 only — KANADE DAW hosts no other plugin format) state,
  written as `<Vst3Plugin>` per the schema.
- Both directions: export and import.

**Out of scope (dropped with a `warningsOut` entry, never silently):**
- `TrackType::utau` and `UtauClip` — vocal synthesis has no equivalent in
  any other DAW's data model.
- `BuiltinFxSlot` types `reverb`/`delay` — DAWproject only standardizes
  EQ/Compressor/Gate/Limiter (confirmed against the XSD's `Devices`
  choice — `Equalizer`/`Compressor`/`NoiseGate`/`Limiter` are direct
  alternatives there, **not** nested inside a `<BuiltinDevice>` wrapper;
  an earlier draft of this doc had that backwards, corrected during the
  final whole-branch review after independently re-checking the vendored
  XSD, see Data Model below). These two are skipped on export; if an
  imported file references a foreign built-in device KANADE DAW has no
  type for, it is skipped the same way.
- Complex multi-point warp maps on import collapse to KANADE DAW's single
  `playbackRate` scalar (the average rate implied by the warp map's first
  and last points). Exporting the reverse direction, KANADE DAW writes
  exactly two `Warp` points (start/end) at the constant rate — a correct
  but minimal warp map, not a lossy approximation of one.
- Any element a peer DAW writes that has no KANADE DAW concept at all
  (e.g., Bitwig's Grid / Note FX devices) is skipped on import with a
  warning, never crashes, never silently vanishes from the warning list.
- `Bus::builtinFx` and `Project::masterChain` (effect chains on a bus or
  the master, as opposed to a regular track) are not exported or imported
  in this first pass — the mapping is identical in principle to a track's
  `BuiltinFxSlot` list (same types, same `addEqualizerParams`-family
  helpers apply), but v1 scopes to track-level effects to bound the
  implementation plan's size. `exportDawProject` emits one `warningsOut`
  entry per non-empty bus/master chain so this is never silent. Extending
  Tasks 4/8 to cover them is a natural, small follow-up.

## Data Model

Confirmed against the vendored `Project.xsd` (not guessed from prose):

| KANADE DAW | DAWproject |
|---|---|
| `Project` | root `<Project version="1.0">`, with `<Application name="KANADE DAW" version="…"/>` |
| `TempoMap` events/timeSigs | `<Arrangement><TempoAutomation>`/`<TimeSignatureAutomation>` (`Points` of `RealPoint`/`TimeSignaturePoint`) |
| `Marker` | `<Marker time="…" name="…"/>` under `<Arrangement><Markers>` directly (a sibling of `<Lanes>`, not nested inside it — corrected during the final whole-branch review) |
| `Track` (`audio`) | `<Track contentType="audio">` |
| `Track` (`midi`) | `<Track contentType="notes">`; an instrument `PluginSlot` becomes a `<Device deviceRole="instrument">` child of the track's `<Channel>` |
| `Track` (`utau`) | *(skipped — see Scope)* |
| `Track.gainDb`/`.pan`/`.muted` | `Channel/Volume` (`unit="decibel"`), `Channel/Pan` (`unit="linear"`), `Channel/Mute` |
| `Track.sends` | `Channel/Sends/Send` (`type="post"` fixed — KANADE DAW only has post-fader sends per `Project.h:147`) |
| `Bus` | a `Track`+`Channel` pair with `Channel role="submix"` (confirmed enum value); `outputBus` references become `Channel@destination` IDREFs |
| `AudioClip` | `<Clip>` wrapping `<Audio>`; `gainDb` != 0 wraps `<Audio>` and a `<Points>` (`Target expression="gain"`) together in a `<Lanes>` (see Risks — resolved) instead of `<Audio>` alone; `time`/`duration` from `startBeats`/`lengthBeats`, `fadeInTime`/`fadeOutTime`, `contentTimeUnit="beats"` |
| `MidiClip` | `<Clip>` wrapping `<Notes>` (`<Note time= duration= channel= key= vel=/>`) |
| `AudioClip.playbackRate` | `<Warps contentTimeUnit="beats">` with exactly 2 `<Warp>` points (see Scope) |
| `PluginSlot` (VST3) | `<Vst3Plugin>` device; `identifier` decomposed into `deviceVendor`/`deviceName`/`deviceID`; `state` (MemoryBlock) written verbatim to the file the `<State path="…"/>` reference points at inside the zip |
| `BuiltinFxSlot` (`eq`/`compressor`/`gate`/`limiter`) | `<Equalizer>`/`<Compressor>`/`<NoiseGate>`/`<Limiter>` written directly as a `<Devices>` child (NOT wrapped in `<BuiltinDevice>` — that type's own content model is empty; these four element types extend it directly), `deviceName`/`deviceRole` set (both required by the schema), param-by-param (e.g. `EqBand.Freq/Gain/Q`) |
| `Track.automation` | `<Points unit="normalized">` — **see note below** |
| `Scene`/`sessionSlots` | top-level `<Scenes><Scene>`, referenced from each track's `<ClipSlot>` |

**Automation unit — a simplification found while reading the schema, not
assumed going in:** the schema's `unit` enum includes an explicit
`"normalized"` value. `Track::AutomationLane` already stores 0..1
breakpoints for *every* target, not just plugin parameters — confirmed
against `Source/Mixer/Mixer.cpp:961-973`, where gain resolves via
`-60.0f + 66.0f * v`, pan via `v*2-1`, mute via `v>=0.5`, and builtin
FX/plugin params via `setParameterNormalised`/`setValue(v)`, all consuming
the same normalized `v`. That means every automation lane — gain, pan,
mute, builtin FX, plugin — writes out as
`<Points unit="normalized"><RealPoint value="0..1"/></Points>` with **no
denormalization step anywhere in export, and no dependency on a loaded
plugin instance.** (An earlier draft of this section scoped the
simplification to plugin parameters only and had built-in Volume/Pan
lanes use `unit="decibel"`/`unit="linear"` — that was wrong, corrected
once the Mixer.cpp resolution code was actually read.)

## Export

`Source/IO/DawProject.h`:
```cpp
namespace ss::io
{
    bool exportDawProject (const juce::File&, const Project&, PluginManager&,
                            juce::String& errorOut, juce::StringArray& warningsOut);
    bool importDawProject (const juce::File&, Project&, PluginManager&,
                            juce::String& errorOut, juce::StringArray& warningsOut);
}
```
(`PluginManager&` added versus the signature sketched earlier in this doc:
`PluginSlot::identifier` alone doesn't carry `deviceVendor`/`deviceName` —
those come from `PluginManager::findDescription(identifier)` on export, and
from matching a `PluginDescription` in `PluginManager::getKnownPluginList()`
by vendor+name on import.)

Export walks `Project` once, building a `juce::XmlElement` tree per the
Data Model table above, collecting one `warningsOut` entry per skipped
element (utau track, reverb/delay slot, unmappable foreign data has no
meaning here since this is export — applies to import instead). Plugin
state blobs and audio-clip source files are added to a
`juce::ZipFile::Builder` as separate entries under `plugins/` and
`audio/`; `project.xml` and `metadata.xml` (from `Project.name`, rest
blank — KANADE DAW has no artist/album metadata to fill the optional
`MetaData.xsd` fields) are added last, then the builder writes the `.dawproject`
file.

## Import

Reads the zip via `juce::ZipFile`, parses `project.xml` with
`juce::XmlDocument`, and walks it in the same order the schema declares
(`Application` → `Transport` → `Structure` → `Arrangement` → `Scenes`),
building a fresh `Project`. A `<Device>` whose `deviceID`/`deviceVendor`
doesn't match any installed VST3 plugin becomes a `PluginSlot` with empty
`state` and a `warningsOut` entry — reusing `PluginManager`'s existing
missing-plugin path (see Precedent) rather than a new one. A malformed
archive (bad zip, missing `project.xml`, XML that doesn't parse) returns
`false` with `errorOut` set and never partially mutates the `Project&`
passed in — either the whole import succeeds or the project is untouched.

## Error Handling & Partial Fidelity

Two distinct signals, never conflated:
- `errorOut` (+ `false` return) — the operation failed outright: bad file,
  unreadable zip, malformed XML. Nothing was written/loaded.
- `warningsOut` — the operation succeeded, but specific elements were
  skipped (utau tracks, reverb/delay slots, foreign devices, collapsed
  warp maps). `MainComponent`'s handler shows these in one dialog after a
  successful export/import, so nothing disappears from a project silently.

## UI

Same pattern as the existing `exportMusicXml()`/`importFiles()` handlers
in `Source/UI/MainComponent.cpp`:
- `CommandIDs::exportDawProject` added to the `File > Export` menu next to
  `exportMidi`/`exportAudio`/`exportMusicXml` (`MainComponent.cpp:1177`).
- `CommandIDs::importDawProject` added to `File > Import` next to
  `importAudio`/`importMidi` (`MainComponent.cpp:1176`).
- Handlers open a `juce::FileChooser`, call `io::exportDawProject`/
  `importDawProject`, show an error dialog on failure, and — independent
  of success/failure — show a second dialog listing `warningsOut` if it's
  non-empty.

## Testing Strategy

New file `Source/IO/DawProjectTests.cpp`, one `juce::UnitTest`-derived class
(matching the one-file-per-feature convention already used by
`Source/Mixer/MixerTests.cpp`, `Source/UI/WhatsNewDialogTests.cpp`, etc. —
`Source/IO/IoTests.cpp` itself covers Shift-JIS/UST/oto.ini only, and
MIDI/MusicXML export currently has no dedicated unit tests to pattern-match
against, corrected from this doc's earlier claim):
- Build a `Project` exercising every mapped field (tracks of each in-scope
  type, clips with fades/gain/playbackRate, automation, buses, builtin FX,
  a scene), export, re-import, assert field-by-field equality.
- Plugin state: no real VST3 available in CI, so round-trip a dummy
  `juce::MemoryBlock` through `PluginSlot::state` and assert byte-for-byte
  equality after export→import. Real-plugin fidelity (TH3/Melda) gets a
  manual pass, same as the VST crash-fix verification already done for
  those two plugins this session.
- Feed a truncated/corrupt `.dawproject` to `importDawProject` and assert
  it returns `false` with `errorOut` set and does not crash or partially
  mutate the target `Project`.
- A utau track and a reverb `BuiltinFxSlot` in the source project produce
  the expected `warningsOut` entries on export.

## Risks / things the implementation plan must verify against the vendored XSD

- **`Structure`'s exact Track/Channel nesting — RESOLVED.** Verified
  against the reference implementation's own model source
  (`src/main/java/com/bitwig/dawproject/Track.java`, bitwig/dawproject
  `main`): `Track` has a `public Channel channel` field — `<Channel>` is
  always a direct child of the `<Track>` it belongs to, never a
  `<Structure>`-level sibling. `Track` also has `public List<Track> tracks`
  for nested group/folder tracks, which KANADE DAW never emits (no
  folder-track concept) or needs to read on import beyond flattening.
- **Per-clip static gain — RESOLVED.** `timeline/Clip.java` has no
  gain/volume field at all, confirming the schema reading. The format's
  intended mechanism (per `expressionType`'s `"gain"` enum value):
  wrap the clip's `<Audio>` and a `<Points Target expression="gain">`
  together inside a `<Lanes>` element used as the `Clip`'s single content
  choice (`Lanes.java`: "ability to contain multiple parallel timelines...
  main layering element of the format"). A `gainDb == 0` clip skips the
  `Lanes`/`Points` wrapper entirely and holds `<Audio>` directly — no need
  to pay the extra nesting for the common case.
- **Scene wiring — RESOLVED.** `Scene.java`'s own doc comment gives the
  exact shape:
  ```xml
  <Scene>
    <Lanes>
      <ClipSlot track="...">
         <Clip> ... </Clip>
      </ClipSlot>
      ...
    </Lanes>
  </Scene>
  ```
  One `<ClipSlot track="trackIdRef">` per track that has a session clip in
  that scene (`ClipSlot` has no `scene` back-reference — containment
  inside `<Scene><Lanes>` is the only relationship). Maps directly onto
  `Track::sessionSlots : std::map<SceneId, SessionClip>` by iterating
  `Project::scenes` outermost and, per scene, each track's
  `findSessionClip(scene.id)`.
- **`juce::ZipFile::Builder` API surface** — still open, not a
  DAWproject-repo question. Task 1 below checks it against this project's
  vendored JUCE 8.0.6 headers before any export code is written.
